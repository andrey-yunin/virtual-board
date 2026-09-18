#include <linux/errno.h>
#include <linux/if.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/printk.h>

#include "board.h"

/*
 * Имя интерфейса хранится в данных модуля.
 * Размер включает завершающий нулевой байт.
 */
char can_iface[IFNAMSIZ] = "vcan0";

/* При загрузке имя можно задать; после загрузки — только прочитать. */
module_param_string(can_iface, can_iface, sizeof(can_iface), 0444);
MODULE_PARM_DESC(can_iface, "CAN interface name (default: vcan0)");

/*
 * Начальные настройки платы.
 * Температуры задаются в десятых долях градуса, период — в миллисекундах.
 */
int temperature_decic = 250;
module_param(temperature_decic, int, 0444);
MODULE_PARM_DESC(temperature_decic,
		 "Initial temperature in 0.1 degrees C (default: 250)");

int low_decic = 100;
module_param(low_decic, int, 0444);
MODULE_PARM_DESC(
    low_decic,
    "Initial lower temperature limit in 0.1 degrees C (default: 100)");

int high_decic = 700;
module_param(high_decic, int, 0444);
MODULE_PARM_DESC(
    high_decic,
    "Initial upper temperature limit in 0.1 degrees C (default: 700)");

unsigned int period_ms = 100;
module_param(period_ms, uint, 0444);
MODULE_PARM_DESC(
    period_ms,
    "Initial CAN transmission period in milliseconds (default: 100)");

int vb_validate_params(void)
{
	/* Проверяем допустимость имени; сам интерфейс найдём в CAN-части. */
	if (!dev_valid_name(can_iface)) {
		pr_err("virtual_board: invalid CAN interface name\n");
		return -EINVAL;
	}

	/* Проверяем общий диапазон платы, а не пороги температурной аварии. */
	if (temperature_decic < VB_TEMP_MIN_DECIC ||
	    temperature_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: temperature_decic is out of range\n");
		return -ERANGE;
	}

	if (low_decic < VB_TEMP_MIN_DECIC || low_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: low_decic is out of range\n");
		return -ERANGE;
	}

	if (high_decic < VB_TEMP_MIN_DECIC || high_decic > VB_TEMP_MAX_DECIC) {
		pr_err("virtual_board: high_decic is out of range\n");
		return -ERANGE;
	}

	/* Равные и переставленные пределы не задают допустимый интервал. */
	if (low_decic >= high_decic) {
		pr_err(
		    "virtual_board: low_decic must be less than high_decic\n");
		return -EINVAL;
	}

	if (period_ms < VB_PERIOD_MIN_MS || period_ms > VB_PERIOD_MAX_MS) {
		pr_err("virtual_board: period_ms is out of range\n");
		return -ERANGE;
	}

	return 0;
}
