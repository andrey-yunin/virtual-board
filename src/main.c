#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include "board.h"

/*
 * Единственный контекст платы хранится в памяти загруженного модуля.
 * Изначально поля нулевые; объекты регистрации подготовим отдельно.
 */
struct board_ctx board;

static int __init vb_init(void)
{
	int ret;

	/* Неверные параметры отклоняем до резервирования номера устройства. */
	ret = vb_validate_params();
	if (ret)
		return ret;

    /* Подготавливаем состояние и mutex до регистрации устройства. */
    vb_board_init();

	/* Продолжаем загрузку только после успешного получения ресурса. */
	ret = vb_chardev_init();
	if (ret)
		return ret;

	pr_info("virtual_board: module loaded\n");
	return 0;
}

static void __exit vb_exit(void)
{
	/* Возвращаем ядру номер, полученный при успешной загрузке. */
	vb_chardev_exit();

	pr_info("virtual_board: module unloaded\n");
}

/* Указываем ядру функции загрузки и завершения нашего модуля. */
module_init(vb_init);
module_exit(vb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrey Yunin");
MODULE_DESCRIPTION("Virtual CAN temperature control board");
