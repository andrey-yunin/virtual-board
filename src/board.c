#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/printk.h>

#include "board.h"

/* Только определяем температурное состояние, не изменяя переданные поля. */
static enum vb_temp_status vb_calc_temp_status(const struct board_state *status)
{
	if (status->temperature_decic < status->low_decic)
		return VB_TEMP_UNDERTEMP;

	if (status->temperature_decic > status->high_decic)
		return VB_TEMP_OVERHEAT;

	/* Значения ровно на обоих пределах входят в нормальный диапазон. */
	return VB_TEMP_NORMAL;
}

/* Вызывается после проверки параметров, до открытия доступа к устройству. */
void vb_board_init(void)
{
	/* Подготавливаем блокировку до появления параллельных обращений. */
	mutex_init(&board.state_lock);

	/* До первого открытия список пуст, контексты ещё не выделены. */
	INIT_LIST_HEAD(&board.clients);
	board.client_count = 0;

	/* Передача после загрузки выключена независимо от температуры. */
	board.status.state = VB_STATE_STOPPED;

	/* Начальная температура уже может находиться вне заданных пределов. */
	board.status.temp_status = vb_calc_temp_status(&board.status);
}

/*
 * Применяем одну разобранную команду.
 * При ошибке рабочее состояние остаётся прежним.
 */
int vb_board_apply_command(const struct board_command *command)
{
	struct board_state next;
	bool send_temp_event = false;
	int ret = 0;

	mutex_lock(&board.state_lock);
	next = board.status;

	switch (command->type) {
	case VB_CMD_SET_TEMP:
		if (command->temperature_decic < VB_TEMP_MIN_DECIC ||
		    command->temperature_decic > VB_TEMP_MAX_DECIC) {
			ret = -ERANGE;
			break;
		}

		next.temperature_decic = command->temperature_decic;
		break;

	case VB_CMD_SET_LIMITS:
		/* Оба предела проверяем и применяем как одну операцию. */
		next.low_decic = command->low_decic;
		next.high_decic = command->high_decic;

		if (next.low_decic < VB_TEMP_MIN_DECIC ||
		    next.low_decic > VB_TEMP_MAX_DECIC ||
		    next.high_decic < VB_TEMP_MIN_DECIC ||
		    next.high_decic > VB_TEMP_MAX_DECIC) {
			ret = -ERANGE;
			break;
		}
		if (next.low_decic >= next.high_decic)
			ret = -EINVAL;
		break;

	case VB_CMD_SET_PERIOD:
		if (command->period_ms < VB_PERIOD_MIN_MS ||
		    command->period_ms > VB_PERIOD_MAX_MS) {
			ret = -ERANGE;
			break;
		}
		/* Callback читает интервал: меняем его после отмены таймера. */
		hrtimer_cancel(&board.tx_timer);
		next.period_ms = command->period_ms;
		board.tx_interval = ms_to_ktime(next.period_ms);
		if (next.state == VB_STATE_RUNNING)
			hrtimer_start(&board.tx_timer, board.tx_interval,
				      HRTIMER_MODE_REL);
		break;

	case VB_CMD_START:
		/* Повторный start сохраняет уже назначенный срок. */
		if (next.state == VB_STATE_RUNNING)
			break;

		next.state = VB_STATE_RUNNING;

		/* Первый вызов callback произойдёт через заданный интервал. */
		hrtimer_start(&board.tx_timer, board.tx_interval,
			      HRTIMER_MODE_REL);
		break;

	case VB_CMD_STOP:
		/*
		 * Ждём завершения callback, чтобы он больше
		 * не мог поставить периодическую работу.
		 */
		hrtimer_cancel(&board.tx_timer);

		/* Ожидающая работа пропустит отправку после сохранения stopped.
		 */
		next.state = VB_STATE_STOPPED;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (!ret) {
		struct board_event event = { 0 };
		bool state_changed;
		bool temp_changed;

		next.temp_status = vb_calc_temp_status(&next);

		/* Сравниваем до перезаписи прежнего состояния. */
		state_changed = next.state != board.status.state;
		temp_changed = next.temp_status != board.status.temp_status;

		send_temp_event =
		    next.state == VB_STATE_RUNNING && temp_changed;

		board.status = next;

		/* Сохраняем значения события, а не адрес изменяемой платы. */
		event.snapshot = next;

		if (state_changed) {
			if (next.state == VB_STATE_RUNNING)
				event.type = VB_EVENT_STARTED;
			else
				event.type = VB_EVENT_STOPPED;

			vb_clients_publish_locked(&event);
		}

		/* Локальный контроль действует и при остановленной передаче. */
		if (temp_changed) {
			event.type = VB_EVENT_TEMP_TRANSITION;
			vb_clients_publish_locked(&event);
		}

		pr_info(
		    "virtual_board: temp=%d low=%d high=%d temp_status=%u\n",
		    next.temperature_decic, next.low_decic, next.high_decic,
		    (unsigned int)next.temp_status);
	}

	/* Общий выход освобождает mutex и при успехе, и при ошибке. */
	mutex_unlock(&board.state_lock);

	/*
	 * Отправляем сохранённый снимок вне блокировки состояния.
	 * Ошибка CAN не отменяет уже применённую команду.
	 */
	if (send_temp_event) {
		vb_can_send_temp_event(&next);
	}
	return ret;
}
