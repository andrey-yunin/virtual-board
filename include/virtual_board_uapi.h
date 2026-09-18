#ifndef VIRTUAL_BOARD_UAPI_H
#define VIRTUAL_BOARD_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum vb_state {
	VB_STATE_STOPPED = 0,
	VB_STATE_RUNNING = 1,
};

enum vb_temp_status {
	VB_TEMP_NORMAL = 0,
	VB_TEMP_UNDERTEMP = 1,
	VB_TEMP_OVERHEAT = 2,
};

struct vb_limits {
	__s32 low_decic;
	__s32 high_decic;
};

struct vb_period {
	__u32 period_ms;
};

/* Native-endian ioctl data; this is NOT the CAN wire format. */
struct vb_status {
	__s32 temperature_decic;
	__s32 low_decic;
	__s32 high_decic;
	__u32 period_ms;
	__u32 state;
	__u32 temp_status;
};

/* Direction is relative to userspace. Pass a pointer to the named type. */
#define VB_IOC_MAGIC 'V'
#define VB_IOC_SET_LIMITS _IOW(VB_IOC_MAGIC, 1, struct vb_limits)
#define VB_IOC_SET_PERIOD _IOW(VB_IOC_MAGIC, 2, struct vb_period)
#define VB_IOC_GET_STATUS _IOR(VB_IOC_MAGIC, 3, struct vb_status)

#endif /* VIRTUAL_BOARD_UAPI_H */
