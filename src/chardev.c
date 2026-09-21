#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "board.h"

/*
 * command должен указывать на массив размером VB_COMMAND_MAX_LEN + 1.
 * После успеха он содержит полную копию ввода с завершающим нулём.
 */
static int vb_copy_command(char *command, const char __user *user_buf,
			   size_t count)
{
	/* Длинную команду отклоняем целиком, не обрезая её смысл. */
	if (count > VB_COMMAND_MAX_LEN)
		return -E2BIG;

	/* Не выполняем команду, если удалось получить лишь часть данных. */
	if (copy_from_user(command, user_buf, count))
		return -EFAULT;

	/* Пользовательский NUL не должен скрывать оставшуюся часть ввода. */
	if (memchr(command, '\0', count))
		return -EINVAL;

	command[count] = '\0';
	return 0;
}

static int vb_parse_command(char *command, struct board_command *parsed)
{
	char *args[3]; /* Название команды и не более двух аргументов. */
	char *cursor = command;
	char *token;
	size_t len = strlen(command);
	int argc = 0;
	int ret;

	/* Неиспользуемые поля команды остаются нулевыми. */
	*parsed = (struct board_command){ 0 };

	/* Разрешаем один завершающий перевод строки от echo. */
	if (len && command[len - 1] == '\n')
		command[len - 1] = '\0';

	/* Оставшийся перевод строки означает недопустимый многострочный ввод.
	 */
	if (strchr(command, '\n'))
		return -EINVAL;

	while ((token = strsep(&cursor, " \t")) != NULL) {
		if (!*token)
			continue;

		if (argc == 3)
			return -EINVAL;

		args[argc++] = token;
	}

	if (!argc)
		return -EINVAL;

	if (!strcmp(args[0], "temp")) {
		if (argc != 2)
			return -EINVAL;

		parsed->type = VB_CMD_SET_TEMP;
		return kstrtoint(args[1], 10, &parsed->temperature_decic);
	}

	if (!strcmp(args[0], "limits")) {
		if (argc != 3)
			return -EINVAL;

		parsed->type = VB_CMD_SET_LIMITS;

		ret = kstrtoint(args[1], 10, &parsed->low_decic);
		if (ret)
			return ret;

		return kstrtoint(args[2], 10, &parsed->high_decic);
	}

	/* У start и stop аргументов быть не должно. */
	if (argc != 1)
		return -EINVAL;

	if (!strcmp(args[0], "start"))
		parsed->type = VB_CMD_START;
	else if (!strcmp(args[0], "stop"))
		parsed->type = VB_CMD_STOP;
	else
		return -EINVAL;

	return 0;
}

/* Один вызов принимает одну полную команду и ждёт её выполнения. */
static ssize_t vb_write(struct file *file, const char __user *user_buf,
			size_t count, loff_t *ppos)
{
	char command[VB_COMMAND_MAX_LEN + 1];
	struct board_command parsed;
	int ret;

	/* Пустая запись не создаёт заявку и не изменяет состояние. */
	if (!count)
		return 0;

	ret = vb_copy_command(command, user_buf, count);
	if (ret)
		return ret;

	ret = vb_parse_command(command, &parsed);
	if (ret)
		return ret;

	/* В рабочую очередь передаётся команда в памяти ядра. */
	ret = vb_submit_command(&parsed);
	if (ret)
		return ret;

	/* Подтверждаем все исходные байты только после выполнения команды. */
	return count;
}

/*
 * Таблица операций принадлежит нашему модулю.
 * Обработчики добавим по мере реализации; owner позволяет ядру
 * удерживать модуль, пока используются операции открытого устройства.
 */
static const struct file_operations vb_fops = {
	.owner = THIS_MODULE,
	.write = vb_write,
};

/* Регистрируем номер, файловые операции, класс и объект устройства. */
int vb_chardev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&board.devno, 0, 1, "virtual_board");
	if (ret) {
		pr_err("virtual_board: alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	pr_info("virtual_board: reserved device number %u:%u\n",
		MAJOR(board.devno), MINOR(board.devno));

	/* Сохраняем связь с таблицей операций до регистрации cdev. */
	cdev_init(&board.cdev, &vb_fops);
	board.cdev.owner = THIS_MODULE;

	ret = cdev_add(&board.cdev, board.devno, 1);
	if (ret) {
		pr_err("virtual_board: cdev_add failed: %d\n", ret);
		goto err_region;
	}

	board.class = class_create("virtual_board");
	if (IS_ERR(board.class)) {
		ret = PTR_ERR(board.class);
		pr_err("virtual_board: class_create failed: %d\n", ret);
		goto err_cdev;
	}

	/* Сохраняем адрес контекста в объекте устройства без копирования. */
	board.device = device_create(board.class, NULL, board.devno, &board,
				     "virtual_board");
	if (IS_ERR(board.device)) {
		ret = PTR_ERR(board.device);
		pr_err("virtual_board: device_create failed: %d\n", ret);
		goto err_class;
	}

	return 0;

	/* Каждая метка освобождает успешно полученные ранее ресурсы. */
err_class:
	class_destroy(board.class);
err_cdev:
	cdev_del(&board.cdev);
err_region:
	unregister_chrdev_region(board.devno, 1);
	return ret;
}

/* Вызывается только после успешной регистрации всех объектов. */
void vb_chardev_exit(void)
{
	/* Сначала удаляем устройство, затем обеспечивавшие его объекты. */
	device_destroy(board.class, board.devno);
	class_destroy(board.class);
	cdev_del(&board.cdev);
	unregister_chrdev_region(board.devno, 1);
}
