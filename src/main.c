#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include "board.h"

/*
 * Единственный контекст платы хранится в памяти загруженного модуля.
 * Значения по умолчанию сразу хранятся в рабочем состоянии.
 */
struct board_ctx board = {
	.status = {
		.temperature_decic = 250,
		.low_decic = 100,
		.high_decic = 700,
		.period_ms = 100,
	},
};

static int __init vb_init(void)
{
	int ret;

	/* Неверные параметры отклоняем до выделения ресурсов. */
	ret = vb_validate_params();
	if (ret)
		return ret;

	vb_board_init();

	/* Сокет готовим до появления работ, которые смогут отправлять кадры. */
	ret = vb_can_init();
	if (ret)
		return ret;

	ret = vb_worker_init();
	if (ret)
		goto err_can;

	/* Доступ к устройству открываем после подготовки общих ресурсов. */
	ret = vb_chardev_init();
	if (ret)
		goto err_worker;

	vb_params_set_ready(true);

	pr_info("virtual_board: module loaded\n");
	return 0;

err_worker:
	/* Регистрация устройства уже откатила свои ресурсы. */
	vb_worker_exit();
err_can:
	/* Очередь либо не создана, либо уже завершена и освобождена. */
	vb_can_exit();
	return ret;
}

static void __exit vb_exit(void)
{
	/* Ждём текущий callback параметра и запрещаем новые команды sysfs. */
	vb_params_set_ready(false);
	vb_chardev_exit();

	/* Сокет остаётся доступным до завершения всех работ очереди. */
	vb_worker_exit();
	vb_can_exit();

	pr_info("virtual_board: module unloaded\n");
}

/* Указываем ядру функции загрузки и завершения нашего модуля. */
module_init(vb_init);
module_exit(vb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrey Yunin");
MODULE_DESCRIPTION("Virtual CAN temperature control board");
