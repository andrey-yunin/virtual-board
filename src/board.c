#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/printk.h>

#include "board.h"

/* Только определяем температурное состояние, не изменяя переданные поля. */
static enum vb_temp_status
vb_calc_temp_status(const struct board_state *status)
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

  	/* Копируем числа из параметров загрузки в отдельное рабочее состояние. */
  	board.status.temperature_decic = temperature_decic;
  	board.status.low_decic = low_decic;
  	board.status.high_decic = high_decic;
  	board.status.period_ms = period_ms;

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
  		if (command->low_decic < VB_TEMP_MIN_DECIC ||
  		    command->low_decic > VB_TEMP_MAX_DECIC ||
  		    command->high_decic < VB_TEMP_MIN_DECIC ||
  		    command->high_decic > VB_TEMP_MAX_DECIC) {
  			ret = -ERANGE;
  			break;
  		}

  		if (command->low_decic >= command->high_decic) {
  			ret = -EINVAL;
  			break;
  		}

  		next.low_decic = command->low_decic;
  		next.high_decic = command->high_decic;
  		break;

  	case VB_CMD_START:
    case VB_CMD_STOP:
  		/* Управление передачей подключим вместе с CAN и таймером. */
  		ret = -EOPNOTSUPP;
  		break;

  	default:
  		ret = -EINVAL;
  		break;
  	}

  	if (!ret) {
  		/* Пересчитываем статус и сохраняем все поля под одной блокировкой. */
  		next.temp_status = vb_calc_temp_status(&next);
  		board.status = next;

  		/* Позволяет проверить результат до реализации интерфейсов чтения. */
  		pr_info("virtual_board: temp=%d low=%d high=%d temp_status=%u\n",
  			next.temperature_decic, next.low_decic, next.high_decic,
  			(unsigned int)next.temp_status);
  	}

  	/* Общий выход освобождает mutex и при успехе, и при ошибке. */
  	mutex_unlock(&board.state_lock);
  	return ret;
}


