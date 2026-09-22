#include <linux/errno.h>
#include <linux/if.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/sysfs.h>

#include "board.h"

/* Интерфейс привязываем при загрузке; менять привязку на ходу не разрешаем. */
char can_iface[IFNAMSIZ] = "vcan0";
module_param_string(can_iface, can_iface, sizeof(can_iface), 0444);
MODULE_PARM_DESC(can_iface, "CAN interface name (default: vcan0)");

/*
 * Как в ДЗ29: фазу защищает штатная блокировка параметров модуля.
 * Ядро уже удерживает её при вызове set/get; внутри callback не берём
 * её повторно. Из init/exit берём явно до изменения фазы.
 * LOADING разрешает записать аргументы insmod до готовности ресурсов;
 * OFFLINE запрещает доступ во время init, его ошибки и завершения.
 */
static enum {
	VB_PARAMS_LOADING,
	VB_PARAMS_OFFLINE,
	VB_PARAMS_READY,
} vb_params_phase;

/* Идентификаторы полей для начальной настройки и чтения через sysfs. */
enum vb_param_id {
	VB_PARAM_TEMP,
	VB_PARAM_LOW,
	VB_PARAM_HIGH,
	VB_PARAM_PERIOD,
};

/* set используется только для аргументов загрузки; sysfs имеет права 0444. */
static int vb_param_set(const char *val, const struct kernel_param *kp)
{
	enum vb_param_id field = *(enum vb_param_id *)kp->arg;
	unsigned int period;
	int value;
	int ret;

	if (vb_params_phase != VB_PARAMS_LOADING)
		return -EPERM;

	if (field == VB_PARAM_PERIOD) {
		ret = kstrtouint(val, 10, &period);
		if (ret)
			return ret;
		if (period < VB_PERIOD_MIN_MS || period > VB_PERIOD_MAX_MS)
			return -ERANGE;
		board.status.period_ms = period;
		return 0;
	}

	ret = kstrtoint(val, 10, &value);
	if (ret)
		return ret;
	if (value < VB_TEMP_MIN_DECIC || value > VB_TEMP_MAX_DECIC)
		return -ERANGE;

	/* Соотношение пределов проверим после разбора всех аргументов. */
	switch (field) {
	case VB_PARAM_TEMP:
		board.status.temperature_decic = value;
		break;
	case VB_PARAM_LOW:
		board.status.low_decic = value;
		break;
	case VB_PARAM_HIGH:
		board.status.high_decic = value;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vb_param_get(char *buffer, const struct kernel_param *kp)
{
	struct board_state snapshot;
	enum vb_param_id type = *(enum vb_param_id *)kp->arg;
	int ret;

	if (vb_params_phase == VB_PARAMS_OFFLINE)
		return -ENODEV;
	if (vb_params_phase == VB_PARAMS_READY) {
		mutex_lock(&board.state_lock);
		snapshot = board.status;
		mutex_unlock(&board.state_lock);
	} else {
		snapshot = board.status;
	}

	switch (type) {
	case VB_PARAM_TEMP:
		ret = sysfs_emit(buffer, "%d\n", snapshot.temperature_decic);
		break;
	case VB_PARAM_LOW:
		ret = sysfs_emit(buffer, "%d\n", snapshot.low_decic);
		break;
	case VB_PARAM_HIGH:
		ret = sysfs_emit(buffer, "%d\n", snapshot.high_decic);
		break;
	case VB_PARAM_PERIOD:
		ret = sysfs_emit(buffer, "%u\n", snapshot.period_ms);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static const struct kernel_param_ops vb_param_ops = {
	.set = vb_param_set,
	.get = vb_param_get,
};

/* arg выбирает поле; само значение хранится только в board.status. */
static enum vb_param_id temperature_field = VB_PARAM_TEMP;
static enum vb_param_id low_field = VB_PARAM_LOW;
static enum vb_param_id high_field = VB_PARAM_HIGH;
static enum vb_param_id period_field = VB_PARAM_PERIOD;

module_param_cb(temperature_decic, &vb_param_ops, &temperature_field, 0444);
MODULE_PARM_DESC(temperature_decic,
		 "Current temperature in 0.1 degrees C (default: 250)");
module_param_cb(low_decic, &vb_param_ops, &low_field, 0444);
MODULE_PARM_DESC(low_decic,
		 "Current lower limit in 0.1 degrees C (default: 100)");
module_param_cb(high_decic, &vb_param_ops, &high_field, 0444);
MODULE_PARM_DESC(high_decic,
		 "Current upper limit in 0.1 degrees C (default: 700)");
module_param_cb(period_ms, &vb_param_ops, &period_field, 0444);
MODULE_PARM_DESC(period_ms,
		 "Current CAN period in milliseconds (default: 100)");

void vb_params_set_ready(bool ready)
{
	kernel_param_lock(THIS_MODULE);
	vb_params_phase = ready ? VB_PARAMS_READY : VB_PARAMS_OFFLINE;
	kernel_param_unlock(THIS_MODULE);
}

static int vb_check_initial_params(void)
{
	/* Проверяем допустимость имени; сам интерфейс найдём в CAN-части. */
	if (!dev_valid_name(can_iface)) {
		pr_err("virtual_board: invalid CAN interface name\n");
		return -EINVAL;
	}

	/* Проверяем общий диапазон платы, а не пороги температурной аварии. */
	if (board.status.temperature_decic < VB_TEMP_MIN_DECIC ||
	    board.status.temperature_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: temperature_decic is out of range\n");
		return -ERANGE;
	}

	if (board.status.low_decic < VB_TEMP_MIN_DECIC ||
	    board.status.low_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: low_decic is out of range\n");
		return -ERANGE;
	}

	if (board.status.high_decic < VB_TEMP_MIN_DECIC ||
	    board.status.high_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: high_decic is out of range\n");
		return -ERANGE;
	}

	/* Равные и переставленные пределы не задают допустимый интервал. */
	if (board.status.low_decic >= board.status.high_decic) {
		pr_err(
		    "virtual_board: low_decic must be less than high_decic\n");
		return -EINVAL;
	}

	if (board.status.period_ms < VB_PERIOD_MIN_MS ||
	    board.status.period_ms > VB_PERIOD_MAX_MS) {
		pr_err("virtual_board: period_ms is out of range\n");
		return -ERANGE;
	}

	return 0;
}

/* Закрываем фазу загрузочных записей до инициализации общих объектов. */
int vb_validate_params(void)
{
	int ret;

	kernel_param_lock(THIS_MODULE);
	vb_params_phase = VB_PARAMS_OFFLINE;
	ret = vb_check_initial_params();
	kernel_param_unlock(THIS_MODULE);
	return ret;
}
