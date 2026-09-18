#ifndef VIRTUAL_BOARD_H
#define VIRTUAL_BOARD_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "../include/virtual_board_uapi.h"


/* Температура и оба предела задаются в десятых долях градуса. */
#define VB_TEMP_MIN_DECIC (-400)
#define VB_TEMP_MAX_DECIC 1250

/* Допустимый период передачи CAN-статуса в миллисекундах. */
#define VB_PERIOD_MIN_MS 10U
#define VB_PERIOD_MAX_MS 60000U

/* Максимум байтов в одной команде write, включая перевод строки. */
#define VB_COMMAND_MAX_LEN 64

/* Операции, которые пользователь может запросить через write. */
enum vb_command_type {
    VB_CMD_SET_TEMP,
  	VB_CMD_SET_LIMITS,
  	VB_CMD_START,
  	VB_CMD_STOP,
};

/*
 * Результат разбора одной команды.
 * Поля температуры используются только для соответствующей операции.
 */
struct board_command {
    enum vb_command_type type;
  	int temperature_decic;
  	int low_decic;
  	int high_decic;
};

/*
 * Текущее состояние единственной платы.
 * Температура и пределы хранятся в десятых долях градуса.
 */
struct board_state {
    int temperature_decic;
  	int low_decic;
  	int high_decic;
  	unsigned int period_ms;
  	enum vb_state state;
  	enum vb_temp_status temp_status;
  };


/*
 * Объекты регистрации единственного символьного устройства.
 * Сохраняем их, чтобы использовать при работе и освободить при завершении.
 */
struct board_ctx {
    /*
  	 * Сериализует передачу команд: следующий отправитель ждёт
  	 * завершения предыдущего. Рабочий поток этот mutex не берёт.
  	 */
  	struct mutex command_lock;

  	/*
  	 * Адрес общей последовательной очереди команд и периодических работ.
  	 * Сам объект очереди создадим при инициализации модуля.
  	 */
  	struct workqueue_struct *wq;

    /* Защищает согласованное чтение и изменение текущего состояния. */
  	struct mutex state_lock;

  	/* Рабочие значения; при загрузке получат проверенные параметры. */
  	struct board_state status;

	/* Выделенный номер major/minor; тот же номер будет у узла в /dev. */
	dev_t devno;

	/* Связывает номер устройства с таблицей наших файловых операций. */
	struct cdev cdev;

	/* Адрес объекта класса в ядре, представляющего
	 * /sys/class/virtual_board. */
	struct class *class;

	/* Адрес объекта устройства в ядре; это не указатель на файл в /dev. */
	struct device *device;
};

/* Единственный экземпляр определён в main.c; остальные файлы используют его. */
extern struct board_ctx board;

/*
 * Параметры загрузки определены в params.c.
 * Используются для начального заполнения рабочего состояния платы.
 */
extern int temperature_decic;
extern int low_decic;
extern int high_decic;
extern unsigned int period_ms;

/* Подготавливает mutex и состояние платы после проверки параметров. */
void vb_board_init(void);

/* Возвращает 0 при допустимых параметрах или отрицательный код ошибки. */
int vb_validate_params(void);

/* Подготавливает символьное устройство; возвращает 0 или код ошибки. */
int vb_chardev_init(void);

/* Освобождает ресурсы после успешного vb_chardev_init(). */
void vb_chardev_exit(void);

#endif /* VIRTUAL_BOARD_H */
