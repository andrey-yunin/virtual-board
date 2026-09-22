#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "board.h"

/*
 * Вызывается с удерживаемым board.state_lock.
 * Возвращённый контекст остаётся защищённым этой же блокировкой.
 */
static struct board_client *vb_client_find(struct pid *owner)
{
	struct board_client *client;

	list_for_each_entry(client, &board.clients, node)
	{
		/* Сравниваем идентичность владельца, а не номер открытия. */
		if (client->owner == owner)
			return client;
	}

	/* Контекст этого процесса ещё не создан. */
	return NULL;
}

/*
 * Создаёт пустой контекст, ещё не связанный с владельцем и списком.
 * Возвращает адрес контекста либо ошибку в формате ERR_PTR.
 */
static struct board_client *vb_client_alloc(void)
{
	struct board_client *client;
	int ret;

	/* Обнуляем также счётчики и позиции частичного чтения. */
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	ret = kfifo_alloc(&client->fifo, VB_EVENT_FIFO_CAPACITY, GFP_KERNEL);
	if (ret) {
		/* FIFO не создан: освобождаем ранее выделенный контекст. */
		kfree(client);
		return ERR_PTR(ret);
	}

	/* Готовим объекты до появления доступа из других потоков. */
	INIT_LIST_HEAD(&client->node);
	mutex_init(&client->fifo_lock);
	mutex_init(&client->read_lock);
	init_waitqueue_head(&client->read_wait);

	atomic_set(&client->pending_events, 0);

	return client;
}

/*
 * Вызывается после исключения клиента из списка и завершения
 * всех обращений к нему. FIFO должен быть успешно создан.
 */
static void vb_client_free(struct board_client *client)
{
	/* Описание FIFO находится внутри ещё существующего контекста. */
	kfifo_free(&client->fifo);

	/* У пустого, ещё не привязанного контекста владельца нет. */
	if (client->owner)
		put_pid(client->owner);

	/* Последнее действие: поля client больше недоступны. */
	kfree(client);
}

/*
 * Формирует строку из сохранённого события.
 * Вызывающий удерживает read_lock и уже выдал предыдущую строку целиком.
 */
static void vb_client_format_event(struct board_client *client,
				   const struct board_event *event)
{
	const struct board_state *snapshot = &event->snapshot;
	const char *event_name;
	const char *state_name;
	const char *temp_name;

	switch (event->type) {
	case VB_EVENT_TEMP_TRANSITION:
		event_name = "temp_transition";
		break;
	case VB_EVENT_STARTED:
		event_name = "started";
		break;
	case VB_EVENT_STOPPED:
		event_name = "stopped";
		break;
	case VB_EVENT_CAN_ERROR:
		event_name = "can_error";
		break;
	default:
		event_name = "unknown";
		break;
	}
	state_name =
	    snapshot->state == VB_STATE_RUNNING ? "running" : "stopped";

	switch (snapshot->temp_status) {
	case VB_TEMP_NORMAL:
		temp_name = "normal";
		break;
	case VB_TEMP_UNDERTEMP:
		temp_name = "undertemp";
		break;
	case VB_TEMP_OVERHEAT:
		temp_name = "overheat";
		break;
	default:
		temp_name = "unknown";
		break;
	}

	client->read_len = scnprintf(
	    client->read_buf, sizeof(client->read_buf),
	    "event=%s temp=%d low=%d high=%d state=%s temp_status=%s error=%d\n",
	    event_name, snapshot->temperature_decic, snapshot->low_decic,
	    snapshot->high_decic, state_name, temp_name, event->error);

	client->read_pos = 0;
}

/*
 * Повторные открытия одного процесса разделяют контекст.
 * Вызывается ядром после подключения к file_operations.open.
 */
int vb_client_open(struct inode *inode, struct file *file)
{
	struct board_client *client;
	struct pid *owner;
	int ret = 0;

	/* Удерживаем идентичность процесса на время поиска или жизни клиента.
	 */
	owner = get_task_pid(current, PIDTYPE_TGID);

	/* Поиск и добавление выполняем вместе: дубликаты недопустимы. */
	mutex_lock(&board.state_lock);

	client = vb_client_find(owner);
	if (!client) {
		if (board.client_count >= VB_MAX_CLIENTS) {
			ret = -EMFILE;
			goto out;
		}

		client = vb_client_alloc();
		if (IS_ERR(client)) {
			ret = PTR_ERR(client);
			goto out;
		}

		/*
		 * Передаём полученную ссылку контексту.
		 * Теперь её освободит vb_client_free().
		 */
		client->owner = owner;
		owner = NULL;

		list_add(&client->node, &board.clients);
		board.client_count++;
	}

	/* Считаем файловые объекты от open, а не копии fd от dup. */
	client->opens++;
	if (file->f_mode & FMODE_READ)
		client->readers++;

	/* Сохраняем адрес; сам контекст никуда не копируется. */
	file->private_data = client;

out:
	mutex_unlock(&board.state_lock);

	/* При повторном открытии или ошибке временная ссылка не нужна. */
	if (owner)
		put_pid(owner);

	return ret;
}

/*
 * Закрывает одно учтённое файловое открытие.
 * Используем сохранённый контекст, даже если файл закрывает другой процесс.
 */
int vb_client_release(struct inode *inode, struct file *file)
{
	struct board_client *client = file->private_data;
	bool last_open;

	mutex_lock(&board.state_lock);

	if (file->f_mode & FMODE_READ) {
		client->readers--;

		if (!client->readers) {
			/*
			 * Читающих открытий больше нет: активных read тоже нет.
			 * state_lock исключает новое открытие и рассылку.
			 */
			mutex_lock(&client->fifo_lock);
			kfifo_reset(&client->fifo);
			mutex_unlock(&client->fifo_lock);

			/* Следующая подписка начнётся без старого остатка. */
			client->read_len = 0;
			client->read_pos = 0;
			atomic_set(&client->pending_events, 0);
		}
	}

	client->opens--;
	last_open = !client->opens;

	if (last_open) {
		/* После удаления рассылка больше не найдёт этот контекст. */
		list_del(&client->node);
		board.client_count--;
	}

	file->private_data = NULL;
	mutex_unlock(&board.state_lock);

	/* Контекст уже недоступен через список и открытые файлы. */
	if (last_open)
		vb_client_free(client);

	return 0;
}

/*
 * Вызывается с удерживаемым board.state_lock:
 * список клиентов и их подписки не меняются во время рассылки.
 */
void vb_clients_publish_locked(const struct board_event *event)
{
	struct board_client *client;
	unsigned int copied;

	list_for_each_entry(client, &board.clients, node)
	{
		if (!client->readers)
			continue;

		mutex_lock(&client->fifo_lock);

		copied = kfifo_in(&client->fifo, event, 1);
		if (copied == 1)
			atomic_inc(&client->pending_events);
		else
			client->lost_events++;

		mutex_unlock(&client->fifo_lock);

		/* Сначала сохраняем событие и условие, затем уведомляем. */
		if (copied == 1)
			wake_up_interruptible(&client->read_wait);
	}
}

/*
 * Выдаёт не больше одной строки или её остатка.
 * Позицию ppos не используем: устройство предоставляет поток событий.
 */
ssize_t vb_client_read(struct file *file, char __user *user_buf, size_t count,
		       loff_t *ppos)
{
	struct board_client *client = file->private_data;
	struct board_event event;
	unsigned int taken;
	size_t amount;
	size_t not_copied;
	size_t copied;
	int ret;

	if (!count)
		return 0;

	/* Унаследованное открытие не даёт права читать очередь родителя. */
	if (client->owner != task_tgid(current))
		return -EXDEV;

	for (;;) {
		ret = mutex_lock_interruptible(&client->read_lock);
		if (ret)
			return ret;

		/* Сначала дочитываем строку, уже извлечённую из FIFO. */
		if (client->read_pos < client->read_len)
			break;

		mutex_lock(&client->fifo_lock);
		taken = kfifo_out(&client->fifo, &event, 1);
		mutex_unlock(&client->fifo_lock);

		if (taken == 1) {
			vb_client_format_event(client, &event);
			break;
		}

		/* Во время ожидания другие читатели могут работать. */
		mutex_unlock(&client->read_lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(
		    client->read_wait,
		    atomic_read(&client->pending_events) > 0);
		if (ret)
			return ret;
	}

	/* Здесь read_lock удерживается, а fifo_lock уже освобождён. */
	amount = min(count, client->read_len - client->read_pos);
	not_copied =
	    copy_to_user(user_buf, client->read_buf + client->read_pos, amount);
	copied = amount - not_copied;
	client->read_pos += copied;

	if (client->read_pos == client->read_len) {
		/* Событие учтено как прочитанное только после полной выдачи. */
		client->read_pos = 0;
		client->read_len = 0;
		atomic_dec(&client->pending_events);
	}

	mutex_unlock(&client->read_lock);

	/* При частичном успехе возвращаем длину доставленной части. */
	return copied ? (ssize_t)copied : -EFAULT;
}
