/* Минимальный клиент ioctl; команды и структуры общие с модулем. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../include/virtual_board_uapi.h"

static int parse_int32(const char *text, __s32 *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || end == text || *end || parsed < INT32_MIN ||
	    parsed > INT32_MAX)
		return -1;
	*value = (__s32)parsed;
	return 0;
}

static int parse_period(const char *text, __u32 *value)
{
	char *end;
	unsigned long parsed;

	/* Период без знака; диапазон платы окончательно проверяет драйвер. */
	if (text[0] < '0' || text[0] > '9')
		return -1;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *end || parsed > UINT32_MAX)
		return -1;
	*value = (__u32)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	struct vb_status status = { 0 };
	struct vb_limits limits;
	struct vb_period period;
	unsigned long cmd;
	void *arg;
	int flags;
	int fd;
	int result = EXIT_SUCCESS;

	if (argc == 2 && !strcmp(argv[1], "status")) {
		cmd = VB_IOC_GET_STATUS;
		arg = &status;
		flags = O_RDONLY;
	} else if (argc == 4 && !strcmp(argv[1], "limits")) {
		if (parse_int32(argv[2], &limits.low_decic) ||
		    parse_int32(argv[3], &limits.high_decic))
			goto usage;
		cmd = VB_IOC_SET_LIMITS;
		arg = &limits;
		flags = O_WRONLY;
	} else if (argc == 3 && !strcmp(argv[1], "period")) {
		if (parse_period(argv[2], &period.period_ms))
			goto usage;
		cmd = VB_IOC_SET_PERIOD;
		arg = &period;
		flags = O_WRONLY;
	} else {
		goto usage;
	}

	fd = open("/dev/virtual_board", flags);
	if (fd < 0) {
		perror("open /dev/virtual_board");
		return EXIT_FAILURE;
	}
	if (ioctl(fd, cmd, arg) < 0) {
		perror("ioctl");
		result = EXIT_FAILURE;
	} else if (cmd == VB_IOC_GET_STATUS) {
		const char *state = "unknown";
		const char *temp_status = "unknown";

		if (status.state == VB_STATE_STOPPED)
			state = "stopped";
		else if (status.state == VB_STATE_RUNNING)
			state = "running";
		switch (status.temp_status) {
		case VB_TEMP_NORMAL:
			temp_status = "normal";
			break;
		case VB_TEMP_UNDERTEMP:
			temp_status = "undertemp";
			break;
		case VB_TEMP_OVERHEAT:
			temp_status = "overheat";
			break;
		}
		printf(
		    "temp=%d low=%d high=%d period_ms=%u state=%s temp_status=%s\n",
		    status.temperature_decic, status.low_decic,
		    status.high_decic, status.period_ms, state, temp_status);
	}
	if (close(fd) < 0) {
		perror("close");
		result = EXIT_FAILURE;
	}
	return result;

usage:
	fprintf(stderr, "Usage: %s status | limits LOW HIGH | period MS\n",
		argv[0]);
	return EXIT_FAILURE;
}
