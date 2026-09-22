#ifndef VIRTUAL_BOARD_H
#define VIRTUAL_BOARD_H

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/pid.h>
#include <linux/types.h>
#include <linux/wait.h>
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

/* Максимальное число одновременно существующих контекстов процессов. */
#define VB_MAX_CLIENTS 8U

/* Ёмкость персонального FIFO в записях struct board_event. */
#define VB_EVENT_FIFO_CAPACITY 64U

/* Буфер одной текстовой записи read, включая завершающий ноль. */
#define VB_EVENT_LINE_MAX 256U

/* Общие операции управления через write и ioctl. */
enum vb_command_type {
	VB_CMD_SET_TEMP,
	VB_CMD_SET_LIMITS,
	VB_CMD_SET_PERIOD,
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
	unsigned int period_ms;
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

/* Причина появления записи в персональных очередях читателей. */
enum vb_event_type {
	VB_EVENT_TEMP_TRANSITION,
	VB_EVENT_STARTED,
	VB_EVENT_STOPPED,
	VB_EVENT_CAN_ERROR,
};

/*
 * Снимок одного события. Каждый подписанный процесс получает свою копию.
 * Последующие изменения платы не изменяют уже сохранённую запись.
 */
struct board_event {
	enum vb_event_type type;
	struct board_state snapshot;

	/* Для CAN_ERROR — отрицательный код ошибки; иначе 0. */
	int error;
};

/*
 * Общий контекст открытий одного процесса.
 * Потоки и повторные открытия владельца используют один FIFO.
 */
struct board_client {
	/* Связи в списке клиентов платы. */
	struct list_head node;

	/* Удерживаемая ссылка на идентификатор группы потоков владельца. */
	struct pid *owner;

	/*
	 * Число открытых файловых объектов и читающих среди них.
	 * dup не создаёт новый файловый объект.
	 */
	unsigned int opens;
	unsigned int readers;

	/* Защищает записи FIFO и счётчик потерь. */
	struct mutex fifo_lock;

	/* Хранилище событий выделим отдельно при создании клиента. */
	DECLARE_KFIFO_PTR(fifo, struct board_event);

	/* Число новых событий, отброшенных из-за заполнения FIFO. */
	u64 lost_events;

	/* Сериализует чтения и защищает строку с её текущей позицией. */
	struct mutex read_lock;

	/* Здесь читатели будут ждать появления доступных данных. */
	wait_queue_head_t read_wait;

	/* События в FIFO плюс событие с ещё не дочитанной строкой. */
	atomic_t pending_events;

	/* Сформированная строка остаётся здесь до полного прочтения. */
	char read_buf[VB_EVENT_LINE_MAX];

	/* Длина строки без завершающего нуля и число отданных байтов. */
	size_t read_len;
	size_t read_pos;
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

	/*
	 * Адрес CAN-сокета модуля.
	 * Освобождаем сокет после завершения использующих его работ.
	 */
	struct socket *can_sock;

	/* Защищает согласованное чтение и изменение текущего состояния. */
	struct mutex state_lock;

	/* Единственное хранилище текущих значений, включая параметры модуля. */
	struct board_state status;

	/*
	 * Контексты процессов, открывших устройство.
	 * Список и число клиентов защищает state_lock.
	 */
	struct list_head clients;
	unsigned int client_count;

	/* Выделенный номер major/minor; тот же номер будет у узла в /dev. */
	dev_t devno;

	/* Связывает номер устройства с таблицей наших файловых операций. */
	struct cdev cdev;

	/* Адрес объекта класса в ядре, представляющего
	 * /sys/class/virtual_board. */
	struct class *class;

	/* Адрес объекта устройства в ядре; это не указатель на файл в /dev. */
	struct device *device;

	/* Таймер назначает периодическую работу в общей очереди. */
	struct hrtimer tx_timer;

	/* Один объект работы повторно используется для отправки статуса. */
	struct work_struct tx_work;

	/*
	 * Интервал для обработчика таймера.
	 * Изменяем только при остановленном таймере.
	 */
	ktime_t tx_interval;
};

/* Единственный экземпляр определён в main.c; остальные файлы используют его. */
extern struct board_ctx board;

/*
 * Параметры загрузки определены в params.c.
 * Используются для начального заполнения рабочего состояния платы.
 */
extern char can_iface[]; /* Имя CAN-интерфейса из параметров загрузки; массив
			    определён в params.c. */

/* Подготавливает mutex и состояние платы после проверки параметров. */
void vb_board_init(void);

/* Применяет команду; при ошибке оставляет состояние платы прежним. */
int vb_board_apply_command(const struct board_command *command);

/* Передаёт команду рабочему потоку и ждёт результата её выполнения. */
int vb_submit_command(const struct board_command *command);

/* Создаёт очередь работ; возвращает 0 или отрицательный код ошибки. */
int vb_worker_init(void);

/* Освобождает очередь после исключения новых постановок работ. */
void vb_worker_exit(void);

/* Возвращает 0 при допустимых параметрах или отрицательный код ошибки. */
int vb_validate_params(void);

/* Разрешает/запрещает чтение параметров из готового состояния платы. */
void vb_params_set_ready(bool ready);

/* Связывает открытие с контекстом процесса и учитывает его закрытие. */
int vb_client_open(struct inode *inode, struct file *file);
int vb_client_release(struct inode *inode, struct file *file);

/* Читает событие или остаток ранее сформированной строки. */
ssize_t vb_client_read(struct file *file, char __user *user_buf, size_t count,
		       loff_t *ppos);

/* Копирует событие читателям; вызывающий удерживает board.state_lock. */
void vb_clients_publish_locked(const struct board_event *event);

/* Подготавливает символьное устройство; возвращает 0 или код ошибки. */
int vb_chardev_init(void);

/* Освобождает ресурсы после успешного vb_chardev_init(). */
void vb_chardev_exit(void);

/* Создаёт и настраивает CAN-сокет; возвращает 0 или код ошибки. */
int vb_can_init(void);

/* Освобождает сокет после успешного init и завершения отправителей. */
void vb_can_exit(void);

/*
 * Отправляют согласованный снимок платы через подготовленный сокет.
 * Вызываются из рабочего потока только в состоянии running.
 * Возвращают 0 при принятии кадра стеком или отрицательный код ошибки.
 */
int vb_can_send_status(const struct board_state *status);
int vb_can_send_temp_event(const struct board_state *status);

#endif /* VIRTUAL_BOARD_H */
