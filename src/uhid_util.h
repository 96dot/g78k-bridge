/* Small helpers around /dev/uhid shared by the bridge and the fake-controller test tool. */
#pragma once
#include <errno.h>
#include <linux/uhid.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Write only the part of struct uhid_event that the event needs.
 * The kernel accepts short writes, and this keeps 8000 writes a second cheap. */
static inline int uhid_write_ev(int fd, const struct uhid_event *ev, size_t len)
{
	for (;;) {
		ssize_t n = write(fd, ev, len);
		if (n == (ssize_t)len)
			return 0;
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
}

static inline int uhid_send_input(int fd, const uint8_t *data, uint16_t size)
{
	struct uhid_event ev;
	if (size > UHID_DATA_MAX)
		return -1;
	ev.type = UHID_INPUT2;
	ev.u.input2.size = size;
	memcpy(ev.u.input2.data, data, size);
	return uhid_write_ev(fd, &ev, offsetof(struct uhid_event, u.input2.data) + size);
}

static inline int uhid_reply_get_report(int fd, uint32_t id, uint16_t err, const uint8_t *data, uint16_t size)
{
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	if (size > UHID_DATA_MAX)
		size = UHID_DATA_MAX;
	ev.type = UHID_GET_REPORT_REPLY;
	ev.u.get_report_reply.id = id;
	ev.u.get_report_reply.err = err;
	ev.u.get_report_reply.size = err ? 0 : size;
	if (!err && data)
		memcpy(ev.u.get_report_reply.data, data, size);
	return uhid_write_ev(fd, &ev, sizeof(ev));
}

static inline int uhid_reply_set_report(int fd, uint32_t id, uint16_t err)
{
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_SET_REPORT_REPLY;
	ev.u.set_report_reply.id = id;
	ev.u.set_report_reply.err = err;
	return uhid_write_ev(fd, &ev, sizeof(ev));
}

static inline void uhid_destroy(int fd)
{
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;
	uhid_write_ev(fd, &ev, sizeof(ev));
}

static inline uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
