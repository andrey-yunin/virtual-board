#include <asm/unaligned.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/socket.h>
#include <linux/sockptr.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <net/net_namespace.h>

#include "board.h"

/* Записываем снимок платы в общий формат статуса и уведомления. */
static void vb_can_fill_frame(struct can_frame *frame, canid_t can_id,
			      const struct board_state *status)
{
	/* Обнуляем также служебные поля, передаваемые SocketCAN. */
	memset(frame, 0, sizeof(*frame));

	frame->can_id = can_id;
	frame->len = CAN_MAX_DLEN;

	/*
	 * Записываем снимок платы в стандартную struct can_frame из
	 * linux/can.h. Объект кадра предоставляет вызывающая функция; память
	 * здесь не выделяем.
	 *
	 * Поля Classical CAN:
	 *   can_id — 32 бита: идентификатор и служебные флаги.
	 *            В нашем протоколе используем стандартный 11-битный ID
	 *            (0x000..0x7FF), без флагов EFF, RTR и ERR:
	 *            0x123 — статус, 0x124 — температурный переход.
	 *   len    — 8-битное поле, допустимая длина данных 0..8 байт.
	 *            Наш протокол всегда использует 8 байт.
	 *   data   — массив из 8 байтов, допустимые индексы 0..7.
	 *
	 * Раскладка data по нашему протоколу:
	 *   [0..1] — температура;
	 *   [2..3] — нижний предел;
	 *   [4..5] — верхний предел;
	 *   [6]    — temp_status: 0 — норма, 1 — ниже, 2 — выше диапазона;
	 *   [7]    — state: 0 — stopped, 1 — running.
	 *
	 * Температуры — знаковые 16-битные числа в десятых долях градуса,
	 * младший байт первым. Отрицательные значения — дополнительный код.
	 * CRC физического CAN-кадра в этой структуре не хранится.
	 *
	 * Функция только записывает поля кадра. Вызывающий код передаёт
	 * согласованный снимок и разрешает отправку только в состоянии running.
	 */
	put_unaligned_le16((u16)status->temperature_decic, &frame->data[0]);
	put_unaligned_le16((u16)status->low_decic, &frame->data[2]);
	put_unaligned_le16((u16)status->high_decic, &frame->data[4]);

	frame->data[6] = (u8)status->temp_status;
	frame->data[7] = (u8)status->state;
}

/*
 * Отправляем один кадр через подготовленный CAN-сокет.
 * Вызывающий код передаёт согласованный снимок в состоянии running.
 */
static int vb_can_send_frame(canid_t can_id, const struct board_state *status)
{
	struct can_frame frame;
	struct msghdr msg = {
		.msg_flags = MSG_DONTWAIT,
	};
	struct kvec iov = {
		.iov_base = &frame,
		.iov_len = sizeof(frame),
	};
	int ret;

	/* Записываем данные в локальный объект до передачи его ядру. */
	vb_can_fill_frame(&frame, can_id, status);

	/* SocketCAN получает всю структуру, а не только восемь байтов data. */
	ret = kernel_sendmsg(board.can_sock, &msg, &iov, 1, sizeof(frame));
	if (ret < 0)
		return ret;

	/* Частичный результат не подтверждает отправку полного кадра. */
	if (ret != sizeof(frame))
		return -EIO;

	return 0;
}

/* Периодический статус и температурный переход имеют разные CAN ID. */
int vb_can_send_status(const struct board_state *status)
{
	return vb_can_send_frame(0x123, status);
}

int vb_can_send_temp_event(const struct board_state *status)
{
	return vb_can_send_frame(0x124, status);
}

/* Возвращает индекс выбранного CAN-интерфейса или отрицательный код ошибки. */
static int vb_can_get_ifindex(void)
{
	struct net_device *dev;
	int ret;

	dev = dev_get_by_name(&init_net, can_iface);
	if (!dev) {
		pr_err("virtual_board: interface %s not found\n", can_iface);
		return -ENODEV;
	}

	if (dev->type != ARPHRD_CAN) {
		pr_err("virtual_board: interface %s is not CAN\n", can_iface);
		ret = -EINVAL;
	} else {
		/* Сохраняем число до освобождения нашей ссылки на объект. */
		ret = dev->ifindex;
	}

	/* Снимаем ссылку и при успехе, и при неподходящем типе интерфейса. */
	dev_put(dev);
	return ret;
}

/* Создаём сокет для отправки и привязываем его к выбранному интерфейсу. */
int vb_can_init(void)
{
	struct sockaddr_can addr = {
		.can_family = AF_CAN,
	};
	int ret;

	ret = vb_can_get_ifindex();
	if (ret < 0)
		return ret;

	/* Положительный результат поиска — индекс CAN-интерфейса. */
	addr.can_ifindex = ret;

	ret = sock_create_kern(&init_net, PF_CAN, SOCK_RAW, CAN_RAW,
			       &board.can_sock);
	if (ret) {
		pr_err("virtual_board: failed to create CAN socket: %d\n", ret);
		return ret;
	}

	/*
	 * Модуль не читает CAN-сокет: пустой список фильтров
	 * исключает накопление непрочитанных входящих кадров.
	 */
	ret = board.can_sock->ops->setsockopt(board.can_sock, SOL_CAN_RAW,
					      CAN_RAW_FILTER,
					      KERNEL_SOCKPTR(NULL), 0);
	if (ret) {
		pr_err("virtual_board: failed to disable CAN reception: %d\n",
		       ret);
		goto err_socket;
	}

	ret =
	    kernel_bind(board.can_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret) {
		pr_err("virtual_board: failed to bind CAN socket to %s: %d\n",
		       can_iface, ret);
		goto err_socket;
	}

	return 0;

/* При ошибке настройки или привязки освобождаем созданный сокет. */
err_socket:
	/* Сокет создан, но использовать его без нужной привязки нельзя. */
	sock_release(board.can_sock);
	board.can_sock = NULL;
	return ret;
}

/* Вызывается после успешной инициализации и завершения отправителей. */
void vb_can_exit(void)
{
	sock_release(board.can_sock);

	/* Адрес освобождённого сокета больше нельзя использовать. */
	board.can_sock = NULL;
}
