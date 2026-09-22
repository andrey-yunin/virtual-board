#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "board.h"

/*
 * Резерв для общей строки состояния и строки каждого клиента.
 * При реализации вывода проверим, что все поля помещаются.
 */
#define VB_PROC_HEADER_MAX 256U
#define VB_PROC_CLIENT_LINE_MAX 128U
#define VB_PROC_TEXT_MAX                                                       \
	(VB_PROC_HEADER_MAX + VB_MAX_CLIENTS * VB_PROC_CLIENT_LINE_MAX)

/* Отдельная сводка для одного открытого proc-файла. */
struct vb_proc_snapshot {
	/* Сериализует чтения через одно открытие файла. */
	struct mutex lock;

	/* Длина подготовленного текста без завершающего нуля. */
	size_t len;

	/* После формирования текст сохраняется до закрытия файла. */
	char text[VB_PROC_TEXT_MAX];
};

/* Формирует неизменяемый текст для одного открытия proc-файла. */
static int vb_proc_open(struct inode *inode, struct file *file)
{
	struct vb_proc_snapshot *snapshot;
	struct board_client *client;
	const char *temp_name;
	u64 lost;
	size_t remaining;
	int len;

	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	mutex_init(&snapshot->lock);

	/* Также удерживаем клиентов в списке до завершения обхода. */
	mutex_lock(&board.state_lock);

	switch (board.status.temp_status) {
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

	len = snprintf(snapshot->text, sizeof(snapshot->text),
		       "temp=%d low=%d high=%d period_ms=%u "
		       "state=%s temp_status=%s clients=%u\n",
		       board.status.temperature_decic, board.status.low_decic,
		       board.status.high_decic, board.status.period_ms,
		       board.status.state == VB_STATE_RUNNING ? "running"
							      : "stopped",
		       temp_name, board.client_count);
	if (len < 0 || len >= sizeof(snapshot->text))
		goto err_overflow;

	snapshot->len = len;

	list_for_each_entry(client, &board.clients, node)
	{
		/* Порядок блокировок тот же, что при публикации события. */
		mutex_lock(&client->fifo_lock);
		lost = client->lost_events;
		mutex_unlock(&client->fifo_lock);

		remaining = sizeof(snapshot->text) - snapshot->len;
		len = snprintf(snapshot->text + snapshot->len, remaining,
			       "pid=%d opens=%u readers=%u lost_events=%llu\n",
			       pid_vnr(client->owner), client->opens,
			       client->readers, (unsigned long long)lost);
		if (len < 0 || len >= remaining)
			goto err_overflow;

		snapshot->len += len;
	}

	mutex_unlock(&board.state_lock);

	/* Сохраняем адрес; сам текст остаётся в выделенной памяти. */
	file->private_data = snapshot;
	/* Сводку читаем последовательно, без операций lseek. */
	return nonseekable_open(inode, file);

err_overflow:
	mutex_unlock(&board.state_lock);
	kfree(snapshot);
	return -EOVERFLOW;
}

/* Выдаёт подготовленный при открытии текст, сохраняя позицию чтения. */
static ssize_t vb_proc_read(struct file *file, char __user *user_buf,
			    size_t count, loff_t *ppos)
{
	struct vb_proc_snapshot *snapshot = file->private_data;
	size_t available;
	size_t to_copy;
	size_t copied;
	ssize_t ret;

	if (!count)
		return 0;

	/* Сигнал позволяет прервать ожидание другого чтения. */
	if (mutex_lock_interruptible(&snapshot->lock))
		return -ERESTARTSYS;

	if (*ppos < 0) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* Весь снимок прочитан: сообщаем EOF, чтобы cat завершился. */
	if (*ppos >= snapshot->len) {
		ret = 0;
		goto out_unlock;
	}

	available = snapshot->len - (size_t)*ppos;
	to_copy = min(count, available);

	/*
	 * Блокировку платы здесь не удерживаем.
	 * Читаем собственный неизменяемый снимок этого открытия.
	 */
	copied =
	    to_copy -
	    copy_to_user(user_buf, snapshot->text + (size_t)*ppos, to_copy);

	/* Недоставленные байты остаются доступны следующему read. */
	*ppos += copied;
	ret = copied ? (ssize_t)copied : -EFAULT;

out_unlock:
	mutex_unlock(&snapshot->lock);
	return ret;
}

/* Освобождает снимок после завершения использования открытия. */
static int vb_proc_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

/* Ядро вызывает эти функции при работе с нашим proc-файлом. */
static const struct proc_ops vb_proc_ops = {
	.proc_open = vb_proc_open,
	.proc_read = vb_proc_read,
	.proc_release = vb_proc_release,
};

/* Адрес регистрации; это не буфер снимка конкретного открытия. */
static struct proc_dir_entry *vb_proc_entry;

/* Вызывается после подготовки состояния платы и списка клиентов. */
int vb_proc_init(void)
{
	vb_proc_entry = proc_create("virtual_board", 0444, NULL, &vb_proc_ops);
	if (!vb_proc_entry)
		return -ENOMEM;

	return 0;
}

/* Удаляет интерфейс до освобождения используемых им ресурсов платы. */
void vb_proc_exit(void)
{
	proc_remove(vb_proc_entry);
	vb_proc_entry = NULL;
}
