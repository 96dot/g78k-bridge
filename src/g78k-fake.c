/* g78k-fake: a pretend GameSir G7 Pro 8K for testing the bridge without the real controller.
 *
 * Creates a virtual 3537:10B8 device through /dev/uhid that behaves like the
 * protocol in SDL_hidapi_gamesir.c: it stays silent until it receives the
 * A2 01 00 mode switch, then streams extended reports (43 A1 C8 ...) while
 * pressing every button in turn, moving the sticks and turning the gyro.
 *
 * Usage: g78k-fake [--rate HZ] [--loops N] [--step-ms MS] | --print-steps
 * Needs root (/dev/uhid). Prints each step so you can watch Steam react.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamesir.h"
#include "uhid_util.h"

/* Vendor-defined: input report 0x43 (63 bytes), output report 0xA2 (63 bytes).
 * No standard usages, so the kernel makes no joystick of its own for it. */
static const uint8_t FAKE_DESCRIPTOR[] = {
	0x06, 0x00, 0xFF, /* Usage Page (Vendor 0xFF00) */
	0x09, 0x01,       /* Usage (1) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, 0x43,       /*   Report ID 0x43 */
	0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F,
	0x81, 0x02,       /*   Input (63 bytes) */
	0x85, 0xA2,       /*   Report ID 0xA2 */
	0x09, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F,
	0x91, 0x02,       /*   Output (63 bytes) */
	0xC0,
};

struct step {
	const char *name;
	uint8_t b0, b1, b2, b3;
	uint8_t l2, r2;
	int16_t lx, ly, rx, ry; /* up = positive, like the real controller */
	int16_t gyro_yaw;       /* counts, 16 per deg/s */
};

static const struct step STEPS[] = {
	{"Get ready (nothing pressed)", .b0 = 0},
	{"A (Cross)", .b0 = GS_A},
	{"B (Circle)", .b0 = GS_B},
	{"X (Square)", .b0 = GS_X},
	{"Y (Triangle)", .b0 = GS_Y},
	{"LB (L1)", .b0 = GS_L1},
	{"RB (R1)", .b0 = GS_R1},
	{"LT fully pressed (L2)", .b1 = GS_L2, .l2 = 255},
	{"RT fully pressed (R2)", .b1 = GS_R2, .r2 = 255},
	{"View/Select (Create)", .b1 = GS_SELECT},
	{"Menu/Start (Options)", .b1 = GS_START},
	{"Home (PS / Steam button)", .b1 = GS_HOME},
	{"Left stick click (L3)", .b1 = GS_L3},
	{"Right stick click (R3)", .b1 = GS_R3},
	{"Capture (touchpad click)", .b1 = GS_CAPTURE},
	{"D-pad up", .b2 = 1},
	{"D-pad right", .b2 = 3},
	{"D-pad down", .b2 = 5},
	{"D-pad left", .b2 = 7},
	{"L4 back paddle (left paddle)", .b2 = GS_L4},
	{"R4 back paddle (right paddle)", .b2 = GS_R4},
	{"L5 top button (left Fn)", .b3 = GS_L5},
	{"R5 top button (right Fn)", .b3 = GS_R5},
	{"Left stick right", .lx = 32767},
	{"Left stick up", .ly = 32767},
	{"Right stick left", .rx = -32768},
	{"Right stick down", .ry = -32768},
	{"Gyro: turning at 90 deg/s", .gyro_yaw = 90 * 16},
	{"Idle", .b0 = 0},
};
#define NSTEPS (sizeof(STEPS) / sizeof(STEPS[0]))

static volatile sig_atomic_t g_stop;
static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void put_be16(uint8_t *p, int16_t v)
{
	p[0] = (uint8_t)((uint16_t)v >> 8);
	p[1] = (uint8_t)v;
}

static void build_report(const struct step *s, uint8_t *r)
{
	memset(r, 0, 64);
	r[0] = GS_IN_REPORT_ID;
	r[1] = GS_HDR0;
	r[2] = GS_HDR1;
	uint8_t *p = r + 3;
	p[0] = s->b0;
	p[1] = s->b1;
	p[2] = s->b2;
	p[3] = s->b3;
	put_be16(p + GS_OFF_LX, s->lx);
	put_be16(p + GS_OFF_LY, s->ly);
	put_be16(p + GS_OFF_RX, s->rx);
	put_be16(p + GS_OFF_RY, s->ry);
	p[GS_OFF_L2] = s->l2;
	p[GS_OFF_R2] = s->r2;
	put_be16(p + GS_OFF_ACCEL + 2, 8192); /* 1 g resting */
	put_be16(p + GS_OFF_GYRO + 2, s->gyro_yaw);
}

int main(int argc, char **argv)
{
	unsigned rate = 1000, loops = 0, step_ms = 700;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--print-steps") == 0) {
			printf("%zu\n", NSTEPS); /* lets the test script size its wait */
			return 0;
		}
		if (i + 1 < argc && strcmp(argv[i], "--rate") == 0)
			rate = (unsigned)strtoul(argv[++i], NULL, 10);
		else if (i + 1 < argc && strcmp(argv[i], "--loops") == 0)
			loops = (unsigned)strtoul(argv[++i], NULL, 10);
		else if (i + 1 < argc && strcmp(argv[i], "--step-ms") == 0)
			step_ms = (unsigned)strtoul(argv[++i], NULL, 10);
		else {
			fprintf(stderr, "usage: g78k-fake [--rate HZ] [--loops N (0 = forever)] [--step-ms MS]\n");
			return 2;
		}
	}
	if (rate == 0 || rate > 8000 || step_ms == 0) {
		fprintf(stderr, "rate must be 1..8000 and step-ms above 0\n");
		return 2;
	}

	int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/uhid (run with sudo)");
		return 1;
	}
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "GameSir-G7 Pro 8K (g78k test fake)");
	snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "g78k-fake");
	ev.u.create2.rd_size = sizeof(FAKE_DESCRIPTOR);
	memcpy(ev.u.create2.rd_data, FAKE_DESCRIPTOR, sizeof(FAKE_DESCRIPTOR));
	ev.u.create2.bus = BUS_USB;
	ev.u.create2.vendor = GS_VID;
	ev.u.create2.product = GS_PID_G7_PRO_8K;
	ev.u.create2.version = 0x0100;
	if (uhid_write_ev(fd, &ev, sizeof(ev)) != 0) {
		perror("create fake controller");
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	printf("fake GameSir G7 Pro 8K created; waiting for the bridge's mode switch...\n");
	fflush(stdout);

	bool extended = false;
	uint64_t period = 1000000u / rate, next = now_us(), step_start = 0;
	size_t step = 0;
	unsigned loop = 0, sent = 0;
	uint8_t report[64];

	while (!g_stop) {
		uint64_t t = now_us();
		uint64_t wait = extended ? (next > t ? next - t : 0) : 1000000u;
		struct timespec to = {.tv_sec = (time_t)(wait / 1000000u), .tv_nsec = (long)(wait % 1000000u) * 1000};
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (ppoll(&pfd, 1, &to, NULL) > 0) {
			if (read(fd, &ev, sizeof(ev)) > 0 && ev.type == UHID_OUTPUT && ev.u.output.size >= 2 &&
			    ev.u.output.data[0] == GS_OUT_REPORT_ID) {
				const uint8_t *d = ev.u.output.data;
				if (d[1] == GS_CMD_MODE && !extended) {
					extended = true;
					step_start = next = now_us();
					printf("mode switch received: streaming extended reports at %u Hz\n", rate);
					printf("step 1/%zu: %s\n", NSTEPS, STEPS[0].name);
					fflush(stdout);
				} else if (d[1] == GS_CMD_RUMBLE && ev.u.output.size >= 4) {
					printf("rumble: low %u, high %u\n", d[2], d[3]);
					fflush(stdout);
				}
			} else if (ev.type == UHID_GET_REPORT) {
				uhid_reply_get_report(fd, ev.u.get_report.id, EIO, NULL, 0);
			} else if (ev.type == UHID_SET_REPORT) {
				uhid_reply_set_report(fd, ev.u.set_report.id, 0);
			}
		}
		if (!extended)
			continue;
		t = now_us();
		if (t - step_start >= (uint64_t)step_ms * 1000u) {
			step_start = t;
			if (++step == NSTEPS) {
				step = 0;
				if (loops && ++loop >= loops)
					break;
			}
			printf("step %zu/%zu: %s\n", step + 1, NSTEPS, STEPS[step].name);
			fflush(stdout);
		}
		/* Send every report that is due (catches up after a late wake-up). */
		while (next <= t) {
			build_report(&STEPS[step], report);
			uhid_send_input(fd, report, 64);
			sent++;
			next += period;
		}
	}
	printf("done: %u reports sent\n", sent);
	uhid_destroy(fd);
	close(fd);
	return 0;
}
