#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "board.h"

/*
 * Одна команда и объекты для её выполнения рабочим потоком.
 * Отправитель сохраняет заявку до получения результата.
 */
struct board_request {
	struct work_struct work;
	struct board_command command;
	int result;
	struct completion done;
};

/* Рабочий поток ядра вызывает эту функцию для одной поставленной заявки. */
static void vb_command_work(struct work_struct *work)
{
	struct board_request *request;

	/* Получаем адрес заявки по адресу её вложенного поля work. */
	request = container_of(work, struct board_request, work);

	/* Записываем результат до того, как разрешим отправителю продолжить. */
	request->result = vb_board_apply_command(&request->command);

	/*
	 * Сообщаем о завершении также при ошибке команды.
	 * После этого вызова к заявке больше не обращаемся.
	 */
	complete(&request->done);
}

/*
 * Вызывается отправителем из контекста процесса при работающей очереди.
 * Из обработчика этой же очереди вызывать нельзя: он будет ждать сам себя.
 */
int vb_submit_command(const struct board_command *command)
{
	struct board_request *request;
	int ret;

	/* До завершения этой команды другие отправители ждут здесь. */
	mutex_lock(&board.command_lock);

	request = kmalloc(sizeof(*request), GFP_KERNEL);
	if (!request) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	/* Сохраняем собственную копию команды в памяти заявки. */
	request->command = *command;
	init_completion(&request->done);
	INIT_WORK(&request->work, vb_command_work);

	/* Новая работа ставится ровно один раз. */
	queue_work(board.wq, &request->work);

	/* Сохраняем заявку, пока обработчик не сообщит результат. */
	wait_for_completion(&request->done);
	ret = request->result;

	/* После уведомления обработчик больше не обращается к заявке. */
	kfree(request);

out_unlock:
	mutex_unlock(&board.command_lock);
	return ret;
}

/* Вызывается до регистрации устройства и появления отправителей команд. */
int vb_worker_init(void)
{
	mutex_init(&board.command_lock);

	/* Команды и будущие периодические работы выполняются по очереди. */
	board.wq = alloc_ordered_workqueue("virtual_board_wq", 0);
	if (!board.wq) {
		pr_err("virtual_board: failed to create workqueue\n");
		return -ENOMEM;
	}

	return 0;
}

/* Вызывается после успешного init, когда новые работы уже исключены. */
void vb_worker_exit(void)
{
	/* Ждём завершения принятых работ перед освобождением очереди. */
	destroy_workqueue(board.wq);

	/* Освобождённый объект больше нельзя использовать через этот адрес. */
	board.wq = NULL;
}
