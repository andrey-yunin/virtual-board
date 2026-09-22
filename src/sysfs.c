#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "board.h"

/* Показывает состояние передачи на момент чтения файла. */
static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	enum vb_state state;

	/* Получаем значение согласованно с обработчиком команд. */
	mutex_lock(&board.state_lock);
	state = board.status.state;
	mutex_unlock(&board.state_lock);

	return sysfs_emit(buf, "%s\n",
			  state == VB_STATE_RUNNING ? "running" : "stopped");
}

/* Описывает атрибут state: чтение разрешено, обработчика записи нет. */
static DEVICE_ATTR_RO(state);

/* Показывает результат температурного контроля на момент чтения. */
static ssize_t temp_status_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	enum vb_temp_status status;
	const char *name;

	mutex_lock(&board.state_lock);
	status = board.status.temp_status;
	mutex_unlock(&board.state_lock);

	switch (status) {
	case VB_TEMP_NORMAL:
		name = "normal";
		break;
	case VB_TEMP_UNDERTEMP:
		name = "undertemp";
		break;
	case VB_TEMP_OVERHEAT:
		name = "overheat";
		break;
	default:
		name = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", name);
}

/* Связывает имя temp_status с обработчиком temp_status_show(). */
static DEVICE_ATTR_RO(temp_status);

/*
 * Вызывается после создания board.device.
 * При ошибке удаляет уже созданные этой функцией атрибуты.
 */
int vb_sysfs_init(void)
{
	int ret;

	ret = device_create_file(board.device, &dev_attr_state);
	if (ret)
		return ret;

	ret = device_create_file(board.device, &dev_attr_temp_status);
	if (ret) {
		/* Второй файл не создан: отменяем создание первого. */
		device_remove_file(board.device, &dev_attr_state);
		return ret;
	}

	return 0;
}

/*
 * Вызывается после успешного vb_sysfs_init(),
 * до удаления самого board.device.
 */
void vb_sysfs_exit(void)
{
	device_remove_file(board.device, &dev_attr_temp_status);
	device_remove_file(board.device, &dev_attr_state);
}
