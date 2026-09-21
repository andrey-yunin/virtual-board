#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
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

/*
 * Выполняется в общей последовательной очереди вместе с командами.
 * Отправляет актуальный снимок только при разрешённой передаче.
 */
static void vb_periodic_work(struct work_struct *work)
{
	struct board_ctx *ctx;
	struct board_state snapshot;
	int ret;

	/* Ядро передаёт адрес tx_work, вложенного в контекст платы. */
	ctx = container_of(work, struct board_ctx, tx_work);

	/* Копируем все поля под блокировкой, отправляем после её снятия. */
	mutex_lock(&ctx->state_lock);
	snapshot = ctx->status;
	mutex_unlock(&ctx->state_lock);

	/* Поставленная ранее работа могла дождаться выполнения stop. */
	if (snapshot.state != VB_STATE_RUNNING)
		return;

	ret = vb_can_send_status(&snapshot);
	if (ret)
		pr_err("virtual_board: periodic CAN status failed: %d\n", ret);
}

/*
 * По истечении интервала поручаем отправку рабочему потоку.
 * Здесь не берём mutex и не вызываем отправку через сокет.
 */
static enum hrtimer_restart vb_timer_callback(struct hrtimer *timer)
{
	struct board_ctx *ctx;

	ctx = container_of(timer, struct board_ctx, tx_timer);

	/* Уже ожидающая работа не добавляется в очередь повторно. */
	queue_work(ctx->wq, &ctx->tx_work);

	/* Переносим срок вперёд, не воспроизводя пропущенные периоды. */
	hrtimer_forward_now(timer, ctx->tx_interval);

	/* Просим ядро повторно поставить таймер на обновлённый срок. */
	return HRTIMER_RESTART;
}

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

	/* Сохраняем обработчик; работа пока не поставлена в очередь. */
	INIT_WORK(&board.tx_work, vb_periodic_work);

	/* Начальное состояние уже подготовлено, команд ещё нет. */
	board.tx_interval = ms_to_ktime(board.status.period_ms);

	/* Подготавливаем таймер, но не назначаем срок срабатывания. */
	hrtimer_init(&board.tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	board.tx_timer.function = vb_timer_callback;

	return 0;
}

/* Вызывается после успешного init, когда новые работы уже исключены. */
void vb_worker_exit(void)
{
	/* Прекращаем срабатывания и ждём текущий обработчик таймера. */
	hrtimer_cancel(&board.tx_timer);

	/* Завершаем работы, включая поставленную последним срабатыванием. */
	destroy_workqueue(board.wq);

	/* Освобождённый объект больше нельзя использовать через этот адрес. */
	board.wq = NULL;
}
