/* g78k-bridge: make every GameSir G7 Pro 8K button bindable in Steam Input.
 *
 * Reads the controller's extended HID reports (all buttons, including the
 * L4/R4 back paddles and L5/R5 top buttons, plus gyro) from its hidraw nodes
 * and presents a virtual controller Steam fully supports:
 *
 *   --output elite (default): an Xbox Elite Series 2 (uinput), four back paddles
 *     R5 -> P1, R4 -> P2, L5 -> P3, L4 -> P4. No gyro, no Share (the Elite has neither).
 *   --output dualsense-edge: a DualSense Edge (uhid), with gyro
 *     L4/R4 -> back paddles, L5/R5 -> Fn buttons, Capture -> touchpad click.
 *
 * Rumble from games or Steam is passed back to the controller.
 *
 * Usage:
 *   g78k-bridge [--output elite|dualsense-edge] [--max-rate HZ] --usb <usb device, e.g. 3-1.4>
 *       (started by udev/systemd; --output elite is the default)
 *   g78k-bridge [--max-rate HZ] --hidraw /dev/hidrawN [--any-device]   (testing)
 *   g78k-bridge --restore|--hide <usb device>   (give the raw controller back / hide it again)
 *
 * --max-rate 0 (default) forwards every report. In the mode the bridge uses the
 * controller connects at USB Full Speed, so that is at most 1000 a second anyway.
 * --max-rate 1000 sends at most 1000 a second; button changes still go out at once.
 *
 * Hiding: a udev rule makes the controller's own input nodes root-only, so Steam
 * doesn't show it twice. systemd runs "--hide" before the bridge starts and
 * "--restore" after it stops for any reason (including the fail-safes below), so the
 * controller never ends up dead. A bridge started by hand does neither.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ds5.h"
#include "ds5_edge_descriptor.h"
#include "gamesir.h"
#include "uhid_util.h"

#define MAX_SOURCES 8
#define ELITE_FF_MAX 16
#define MODE_SWITCH_RETRY_US 500000u
#define FAILSAFE_US 10000000u      /* unknown reports for this long -> give the controller back */
#define FAILSAFE_MIN_PACKETS 500  /* a gamepad stream, not an idle dongle's occasional status report */
#define RESCAN_US 2000000u         /* look for new hidraw nodes while none is producing input */
#define ACTIVE_SILENT_US 200000u  /* gamepad interface quiet this long -> listen to all */
#define START_TIMEOUT_US 5000000u /* kernel must accept the virtual pad within this */
#define RUMBLE_MIN_GAP_US 8000u   /* at most ~125 rumble writes a second */
#define GYRO_SCALE_NUM 1024        /* GameSir 16 counts per deg/s -> DualSense 16.384 */
#define GYRO_SCALE_DEN 1000

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------------- sysfs */

static bool name_is(const char *name, const char *prefix)
{
	size_t n = strlen(prefix);
	if (strncmp(name, prefix, n) != 0 || !name[n])
		return false;
	for (const char *p = name + n; *p; p++)
		if (*p < '0' || *p > '9')
			return false;
	return true;
}

/* USB device names look like "3-1.4": starts with a digit, then digits, '-', '.'. */
static bool valid_usb_name(const char *s)
{
	if (*s < '0' || *s > '9' || strlen(s) > 32 || !strchr(s, '-'))
		return false;
	for (const char *p = s; *p; p++)
		if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '.'))
			return false;
	return true;
}

static bool read_sysfs_line(const char *path, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, (int)outlen, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\r\n")] = '\0';
	return ok;
}

/* Resolve /sys/bus/usb/devices/<name> to its real directory. */
static bool usb_dir(const char *usb, char *out)
{
	char link[PATH_MAX];
	snprintf(link, sizeof(link), "/sys/bus/usb/devices/%s", usb);
	return realpath(link, out) != NULL;
}

#define FLAG_DIR "/run/g78k-bridge"

enum node_action { NODE_HIDE, NODE_RESTORE, NODE_LIST_HIDRAW };

static enum node_action g_action;
static char (*g_list)[32];
static int g_list_n;

static int node_walk(const char *fpath, const struct stat *sb, int type, struct FTW *ftw)
{
	(void)sb;
	if (type != FTW_D)
		return FTW_CONTINUE;
	const char *name = fpath + ftw->base;
	if (ftw->level > 0) {
		/* Never descend into another USB device (e.g. something on a hub port). */
		char probe[PATH_MAX];
		snprintf(probe, sizeof(probe), "%s/idVendor", fpath);
		if (access(probe, F_OK) == 0)
			return FTW_SKIP_SUBTREE;
	}
	bool is_hidraw = name_is(name, "hidraw");
	if (g_action == NODE_LIST_HIDRAW) {
		if (is_hidraw && g_list_n < MAX_SOURCES)
			snprintf(g_list[g_list_n++], 32, "%s", name);
		return FTW_CONTINUE;
	}
	if (name_is(name, "event")) {
		/* Only gamepad-like nodes (they have axes). A keyboard or mouse interface
		 * the controller may also expose for macros is left alone. */
		char caps[PATH_MAX], abs[256];
		snprintf(caps, sizeof(caps), "%.*s/capabilities/abs", (int)ftw->base - 1, fpath);
		if (!read_sysfs_line(caps, abs, sizeof(abs)) || strcmp(abs, "0") == 0)
			return FTW_CONTINUE;
	} else if (!is_hidraw && !name_is(name, "js")) {
		return FTW_CONTINUE;
	}
	/* Per-node flag /run/g78k-bridge/<node>: while it exists our udev rule leaves the
	 * node alone, so the system's own rules give it to the logged-in user exactly as
	 * if the bridge were not installed. Then ask udev to re-apply its rules. */
	char flag[96];
	snprintf(flag, sizeof(flag), FLAG_DIR "/%s", name);
	if (g_action == NODE_RESTORE) {
		int ffd = open(flag, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (ffd < 0) {
			logmsg("cannot create %s: %s", flag, strerror(errno));
			return FTW_CONTINUE;
		}
		close(ffd);
	} else if (unlink(flag) != 0) {
		return FTW_CONTINUE; /* was never released: already hidden, nothing to refresh */
	}
	char uevent[PATH_MAX];
	snprintf(uevent, sizeof(uevent), "%s/uevent", fpath);
	int fd = open(uevent, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0 || write(fd, "change", 6) != 6)
		logmsg("could not refresh %s: %s", name, strerror(errno));
	if (fd >= 0)
		close(fd);
	return FTW_CONTINUE;
}

/* Is this USB device directory a GameSir G7 Pro 8K? */
static bool is_g7_8k(const char *dir)
{
	char path[PATH_MAX + 16], v[16], p[16];
	snprintf(path, sizeof(path), "%s/idVendor", dir);
	if (!read_sysfs_line(path, v, sizeof(v)))
		return false;
	snprintf(path, sizeof(path), "%s/idProduct", dir);
	if (!read_sysfs_line(path, p, sizeof(p)))
		return false;
	return strtoul(v, NULL, 16) == GS_VID && strtoul(p, NULL, 16) == GS_PID_G7_PRO_8K;
}

static bool walk_usb(const char *usb, enum node_action action)
{
	char dir[PATH_MAX];
	if (!usb_dir(usb, dir))
		return false; /* unplugged */
	if (!is_g7_8k(dir)) {
		logmsg("%s is not a GameSir G7 Pro 8K; not touching it", usb);
		return false;
	}
	g_action = action;
	nftw(dir, node_walk, 16, FTW_PHYS | FTW_ACTIONRETVAL);
	return true;
}

/* Both run from systemd outside the sandbox (ExecStartPre=+ / ExecStopPost=+),
 * so the bridge itself never needs write access to /sys. */
static void restore_raw(const char *usb)
{
	char dir[PATH_MAX];
	if (!usb_dir(usb, dir) || !is_g7_8k(dir))
		return; /* unplugged: nothing to give back */
	if (mkdir(FLAG_DIR, 0755) != 0 && errno != EEXIST) {
		logmsg("cannot create %s: %s", FLAG_DIR, strerror(errno));
		return;
	}
	if (walk_usb(usb, NODE_RESTORE))
		logmsg("%s: raw controller handed back to games", usb);
}

static void hide_raw(const char *usb)
{
	/* Drop flags left by nodes that no longer exist, so a future node that reuses
	 * the name isn't left visible. */
	DIR *d = opendir(FLAG_DIR);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			const char *n = e->d_name;
			bool hid = name_is(n, "hidraw");
			if ((!hid && !name_is(n, "event") && !name_is(n, "js")) || strlen(n) > 32)
				continue;
			char sys[96], flag[96];
			snprintf(sys, sizeof(sys), "/sys/class/%s/%.32s", hid ? "hidraw" : "input", n);
			snprintf(flag, sizeof(flag), FLAG_DIR "/%.32s", n);
			if (access(sys, F_OK) != 0)
				unlink(flag);
		}
		closedir(d);
	}
	walk_usb(usb, NODE_HIDE); /* only touches nodes that had been given back */
}

/* Stable fake MAC address per controller (USB serial, else USB port name). */
static void make_mac(const char *usb, const char *fallback, uint8_t mac[6])
{
	char dir[PATH_MAX], key[PATH_MAX + 64] = "", path[PATH_MAX + 16];
	if (usb && usb_dir(usb, dir)) {
		/* Serial plus port: two controllers with the same serial still differ,
		 * which matters because the kernel refuses two DualSenses with one MAC. */
		char serial[128] = "";
		snprintf(path, sizeof(path), "%s/serial", dir);
		read_sysfs_line(path, serial, sizeof(serial));
		snprintf(key, sizeof(key), "%s@%s", serial, usb);
	} else {
		snprintf(key, sizeof(key), "%s", fallback);
	}
	uint32_t h = 2166136261u; /* FNV-1a */
	for (const char *p = key; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	mac[0] = 0x02; /* locally administered */
	mac[1] = 0x0e;
	mac[2] = 0xd6;
	mac[3] = (uint8_t)(h >> 16);
	mac[4] = (uint8_t)(h >> 8);
	mac[5] = (uint8_t)h;
}

/* -------------------------------------------------------------- reports */

/* Controller-neutral state: what the GameSir reported, before it is dressed up as
 * whichever controller the bridge presents (Xbox Elite 2 or DualSense Edge). */
enum {
	PB_A = 1u << 0, PB_B = 1u << 1, PB_X = 1u << 2, PB_Y = 1u << 3,
	PB_L1 = 1u << 4, PB_R1 = 1u << 5, PB_L2 = 1u << 6, PB_R2 = 1u << 7,
	PB_SELECT = 1u << 8, PB_START = 1u << 9, PB_HOME = 1u << 10,
	PB_L3 = 1u << 11, PB_R3 = 1u << 12, PB_CAPTURE = 1u << 13,
	PB_L4 = 1u << 14, PB_R4 = 1u << 15, PB_L5 = 1u << 16, PB_R5 = 1u << 17,
};

struct pad_state {
	uint32_t buttons;            /* PB_* */
	uint8_t hat;                 /* 0 up .. 7 up-left clockwise, 8 = centred */
	int16_t lx, ly, rx, ry;      /* evdev/SDL convention: right and DOWN are positive */
	uint8_t l2, r2;              /* 0..255 */
	int16_t gyro[3];             /* DualSense units: pitch, yaw, roll */
	int16_t accel[3];            /* 8192 per g */
};

static inline int16_t be16(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline int16_t clamp16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static inline void put_le16(uint8_t *p, int32_t v)
{
	v = clamp16(v);
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((uint16_t)v >> 8);
}

/* Signed 16-bit stick value -> DualSense 8-bit (0x80 centre). */
static inline uint8_t stick8(int32_t v)
{
	int32_t r = (v + 32768 + 128) >> 8; /* rounded, so 8-bit -> 16-bit -> 8-bit is exact */
	return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

/* 8-bit stick (0x80 centre) -> signed 16-bit: 257 = 65535 / 255 spreads 0..255 over the full range. */
static inline int16_t stick16(uint8_t v)
{
	return v == 0x80 ? 0 : clamp16(((int32_t)v - 128) * 257);
}

/* GameSir D-pad nibble (0 none, 1 up .. 8 up-left, clockwise) -> hat (0 up .. 7, 8 none). */
static inline uint8_t hat_from_gs(uint8_t v)
{
	v &= GS_DPAD_MASK;
	return (v >= 1 && v <= 8) ? (uint8_t)(v - 1) : DS5_HAT_NONE;
}

static uint32_t common_buttons(uint8_t b0, uint8_t b1)
{
	uint32_t o = 0;
	if (b0 & GS_A) o |= PB_A;
	if (b0 & GS_B) o |= PB_B;
	if (b0 & GS_X) o |= PB_X;
	if (b0 & GS_Y) o |= PB_Y;
	if (b0 & GS_L1) o |= PB_L1;
	if (b0 & GS_R1) o |= PB_R1;
	if (b1 & GS_L2) o |= PB_L2;
	if (b1 & GS_R2) o |= PB_R2;
	if (b1 & GS_SELECT) o |= PB_SELECT;
	if (b1 & GS_START) o |= PB_START;
	if (b1 & GS_HOME) o |= PB_HOME;
	if (b1 & GS_L3) o |= PB_L3;
	if (b1 & GS_R3) o |= PB_R3;
	if (b1 & GS_CAPTURE) o |= PB_CAPTURE;
	return o;
}

static void pad_init(struct pad_state *st)
{
	memset(st, 0, sizeof(*st));
	st->hat = DS5_HAT_NONE;
	st->accel[1] = 8192; /* resting: 1 g on Y */
}

/* Extended payload (after the A1 C8 header). */
static void map_extended(const uint8_t *p, size_t len, struct pad_state *st)
{
	st->buttons = common_buttons(p[0], p[1]);
	if (p[2] & GS_L4) st->buttons |= PB_L4;
	if (p[2] & GS_R4) st->buttons |= PB_R4;
	if (p[3] & GS_L5) st->buttons |= PB_L5;
	if (p[3] & GS_R5) st->buttons |= PB_R5;
	st->hat = hat_from_gs(p[2]);
	st->lx = be16(p + GS_OFF_LX);
	st->ly = clamp16(-(int32_t)be16(p + GS_OFF_LY)); /* GameSir: up positive */
	st->rx = be16(p + GS_OFF_RX);
	st->ry = clamp16(-(int32_t)be16(p + GS_OFF_RY));
	st->l2 = p[GS_OFF_L2];
	st->r2 = p[GS_OFF_R2];
	if (len >= GS_IMU_PAYLOAD) {
		for (int i = 0; i < 3; i++) {
			st->gyro[i] = clamp16((int32_t)be16(p + GS_OFF_GYRO + 2 * i) * GYRO_SCALE_NUM / GYRO_SCALE_DEN);
			st->accel[i] = be16(p + GS_OFF_ACCEL + 2 * i);
		}
	}
}

/* Simple 9-byte report (no paddles, no gyro). */
static void map_simple(const uint8_t *p, struct pad_state *st)
{
	uint8_t hat = p[2] & 0x0F;
	st->buttons = common_buttons(p[0], p[1]);
	st->hat = hat <= 7 ? hat : DS5_HAT_NONE;
	st->lx = stick16(p[3]);
	st->ly = stick16(p[4]);
	st->rx = stick16(p[5]);
	st->ry = stick16(p[6]);
	st->l2 = p[7];
	st->r2 = p[8];
	/* No motion data in simple reports: report "still" so the camera can't drift. */
	memset(st->gyro, 0, sizeof(st->gyro));
	st->accel[0] = st->accel[2] = 0;
	st->accel[1] = 8192;
}

static void ds5_init_report(uint8_t *ds)
{
	memset(ds, 0, DS5_IN_SIZE);
	ds[0] = DS5_IN_REPORT_ID;
	ds[DS5_OFF_LX] = ds[DS5_OFF_LY] = ds[DS5_OFF_RX] = ds[DS5_OFF_RY] = 0x80;
	ds[DS5_OFF_BTN0] = DS5_HAT_NONE;
	ds[DS5_OFF_TOUCH0] = 0x80; /* no finger on the touchpad */
	ds[DS5_OFF_TOUCH1] = 0x80;
	ds[DS5_OFF_BATTERY] = 0x0A; /* not charging, 100% (the GameSir doesn't report it) */
	put_le16(ds + DS5_OFF_ACCEL + 2, 8192); /* resting: 1 g on Y, like a real DualSense */
}

/* Pad state -> DualSense Edge report (L4/R4 = back paddles, L5/R5 = Fn, Capture = touchpad click). */
static void ds5_encode(const struct pad_state *st, uint8_t *ds)
{
	uint32_t b = st->buttons;
	uint8_t o0 = st->hat & 0x0F, o1 = 0, o2 = 0;
	if (b & PB_A) o0 |= DS5_CROSS;
	if (b & PB_B) o0 |= DS5_CIRCLE;
	if (b & PB_X) o0 |= DS5_SQUARE;
	if (b & PB_Y) o0 |= DS5_TRIANGLE;
	if (b & PB_L1) o1 |= DS5_L1;
	if (b & PB_R1) o1 |= DS5_R1;
	if (b & PB_L2) o1 |= DS5_L2;
	if (b & PB_R2) o1 |= DS5_R2;
	if (b & PB_SELECT) o1 |= DS5_CREATE;
	if (b & PB_START) o1 |= DS5_OPTIONS;
	if (b & PB_L3) o1 |= DS5_L3;
	if (b & PB_R3) o1 |= DS5_R3;
	if (b & PB_HOME) o2 |= DS5_PS;
	if (b & PB_CAPTURE) o2 |= DS5_TOUCHPAD;
	if (b & PB_L4) o2 |= DS5_EDGE_PADDLE_LEFT;
	if (b & PB_R4) o2 |= DS5_EDGE_PADDLE_RIGHT;
	if (b & PB_L5) o2 |= DS5_EDGE_FN_LEFT;
	if (b & PB_R5) o2 |= DS5_EDGE_FN_RIGHT;
	ds[DS5_OFF_BTN0] = o0;
	ds[DS5_OFF_BTN1] = o1;
	ds[DS5_OFF_BTN2] = o2;
	ds[DS5_OFF_LX] = stick8(st->lx);
	ds[DS5_OFF_LY] = stick8(st->ly);
	ds[DS5_OFF_RX] = stick8(st->rx);
	ds[DS5_OFF_RY] = stick8(st->ry);
	ds[DS5_OFF_L2] = st->l2;
	ds[DS5_OFF_R2] = st->r2;
	for (int i = 0; i < 3; i++) {
		put_le16(ds + DS5_OFF_GYRO + 2 * i, st->gyro[i]);
		put_le16(ds + DS5_OFF_ACCEL + 2 * i, st->accel[i]);
	}
}

/* ------------------------------------------------------------ the bridge */

struct source {
	int fd;
	char name[32];
	uint8_t out_len;    /* length of the A2 command report incl. ID; 0 = no command channel here */
	bool has_simple;    /* has the ordinary gamepad report (ID 0x01, 9 bytes) */
	bool extended;      /* sends A1 C8 reports */
	bool dumped;        /* first unknown report logged */
	bool write_failed;  /* mode switch not accepted here */
};

/* Writes to the controller (rumble, mode switch) are slow USB transfers that
 * block, so they run on their own thread and never hold up input. Each slot
 * keeps only the newest request; the thread owns the dup()ed fd it is handed. */
struct writer {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	bool running, stop;
	bool want_rumble;
	int mode_fds[MAX_SOURCES], nmode; /* interfaces still to ask for extended reports */
	uint8_t mode_lens[MAX_SOURCES];
	int rumble_fd;
	uint8_t rumble_len;
	uint8_t lo, hi;
	uint64_t last_rumble_us;
	bool fail_logged;
};

struct bridge {
	struct source src[MAX_SOURCES];
	int nsrc;
	int active; /* source the gamepad data comes from, -1 until known */
	const char *usb; /* NULL when testing with --hidraw */
	enum { OUT_ELITE, OUT_EDGE } mode; /* which controller the bridge presents */
	int uhid;   /* virtual DualSense Edge (OUT_EDGE), -1 until the controller sends data */
	int uin;    /* virtual Xbox Elite 2 (OUT_ELITE, uinput), -1 until the controller sends data */
	uint8_t mac[6];
	struct pad_state st, sent; /* current state, and what the virtual pad last got */
	uint8_t ds[DS5_IN_SIZE];
	struct { uint16_t strong, weak, length_ms; bool used; } ff[ELITE_FF_MAX];
	uint64_t ff_until_us; /* running rumble effect ends here (0 = no timed effect) */
	int ff_playing;       /* effect id playing now, -1 = none */
	uint8_t want_lo, want_hi;      /* rumble level the game asked for */
	bool rumble_pending;           /* a change waits for the rumble gap below */
	uint64_t last_rumble_post_us;
	uint8_t seq;
	bool dirty;
	bool out_fail_logged;
	uint64_t t0_us, last_send_us, last_mode_switch_us, first_packet_us, last_scan_us;
	uint64_t last_active_us, created_us, no_sources_since_us;
	bool started; /* the kernel accepted the virtual device (UHID_START) */
	sigset_t run_sigmask;
	uint32_t min_interval_us, unknown_packets, fallbacks;
	int last_rumble_lo, last_rumble_hi;
	struct writer w;
};

/* One attempt (plus EINTR/EAGAIN retries); no retry on timeouts, which take seconds. */
static int gs_write_fd(int fd, uint8_t len, uint8_t cmd, uint8_t a, uint8_t c)
{
	uint8_t buf[64] = {GS_OUT_REPORT_ID, cmd, a, c};
	if (len < 4 || len > sizeof(buf))
		return -1;
	for (int attempt = 0; attempt < 3; attempt++) {
		ssize_t n = write(fd, buf, len);
		if (n == (ssize_t)len)
			return 0;
		if (n >= 0 || (errno != EINTR && errno != EAGAIN))
			return -1;
		usleep(1000);
	}
	return -1;
}

static void *writer_main(void *arg)
{
	struct writer *w = arg;
	pthread_mutex_lock(&w->lock);
	while (!w->stop) {
		if (!w->nmode && !w->want_rumble) {
			pthread_cond_wait(&w->wake, &w->lock);
			continue;
		}
		int fd;
		uint8_t cmd, a = 0, c = 0, len;
		if (w->nmode) {
			--w->nmode;
			fd = w->mode_fds[w->nmode];
			len = w->mode_lens[w->nmode];
			cmd = GS_CMD_MODE;
		} else {
			uint64_t since = now_us() - w->last_rumble_us;
			if (since < RUMBLE_MIN_GAP_US) {
				/* Too soon: wait, then send whatever the newest value is by then. */
				uint64_t left = RUMBLE_MIN_GAP_US - since;
				pthread_mutex_unlock(&w->lock);
				usleep((useconds_t)left);
				pthread_mutex_lock(&w->lock);
				continue;
			}
			fd = w->rumble_fd;
			len = w->rumble_len;
			w->rumble_fd = -1;
			w->want_rumble = false;
			cmd = GS_CMD_RUMBLE;
			a = w->lo;
			c = w->hi;
			w->last_rumble_us = now_us();
		}
		pthread_mutex_unlock(&w->lock);
		int rc = gs_write_fd(fd, len, cmd, a, c);
		int err = errno;
		close(fd);
		pthread_mutex_lock(&w->lock);
		if (rc != 0 && !w->fail_logged) {
			logmsg("write to controller failed (%s); further failures not logged", strerror(err));
			w->fail_logged = true;
		}
	}
	pthread_mutex_unlock(&w->lock);
	return NULL;
}

static void writer_start(struct writer *w)
{
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->wake, NULL);
	w->rumble_fd = -1;
	w->running = pthread_create(&w->thread, NULL, writer_main, w) == 0;
	if (!w->running)
		logmsg("cannot start writer thread; rumble disabled");
}

static void writer_stop(struct writer *w)
{
	if (!w->running)
		return;
	pthread_mutex_lock(&w->lock);
	w->stop = true;
	pthread_cond_signal(&w->wake);
	pthread_mutex_unlock(&w->lock);
	pthread_join(w->thread, NULL);
	while (w->nmode)
		close(w->mode_fds[--w->nmode]);
	if (w->rumble_fd >= 0)
		close(w->rumble_fd);
	w->running = false;
}

/* Queue a write. fd is the source's fd; the writer gets its own copy so the
 * main loop can close the source at any time. */
static void writer_post(struct writer *w, int fd, uint8_t len, uint8_t cmd, uint8_t lo, uint8_t hi)
{
	if (!w->running || !len)
		return;
	int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (copy < 0)
		return;
	pthread_mutex_lock(&w->lock);
	if (cmd == GS_CMD_MODE) {
		if (w->nmode < MAX_SOURCES) {
			w->mode_lens[w->nmode] = len;
			w->mode_fds[w->nmode++] = copy;
		}
		else
			close(copy); /* already a request queued for every interface */
	} else {
		if (w->rumble_fd >= 0)
			close(w->rumble_fd);
		w->rumble_fd = copy;
		w->rumble_len = len;
		w->want_rumble = true;
		w->lo = lo;
		w->hi = hi;
	}
	pthread_cond_signal(&w->wake);
	pthread_mutex_unlock(&w->lock);
}

/* Ask for extended reports. At start-up (nothing to hold up yet) this writes
 * directly to every interface; later it goes through the writer thread. */
static void send_mode_switch(struct bridge *b, bool direct)
{
	b->last_mode_switch_us = now_us();
	for (int i = 0; i < b->nsrc; i++) {
		struct source *s = &b->src[i];
		/* Once extended reports flow, only that interface matters; before that, ask them all. */
		if (b->active >= 0 && b->src[b->active].extended && i != b->active)
			continue;
		if (!s->out_len)
			continue; /* no command channel on this interface */
		if (!direct) {
			writer_post(&b->w, s->fd, s->out_len, GS_CMD_MODE, 0, 0);
			continue;
		}
		bool ok = gs_write_fd(s->fd, s->out_len, GS_CMD_MODE, 0x00, 0x00) == 0;
		if (!ok && !s->write_failed)
			logmsg("%s: mode switch not accepted on this interface (%s)", s->name, strerror(errno));
		s->write_failed = !ok;
	}
}

/* Read the interface's HID report descriptor (sysfs, readable by anyone) to learn
 * the real size of the A2 command report and whether the ordinary gamepad report
 * lives here. Unreadable or unusual descriptor: fall back to SDL's assumptions. */
static void parse_descriptor(struct source *s)
{
	char path[96];
	uint8_t rd[4096];
	snprintf(path, sizeof(path), "/sys/class/hidraw/%.31s/device/report_descriptor", s->name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t len = fd >= 0 ? read(fd, rd, sizeof(rd)) : -1;
	if (fd >= 0)
		close(fd);
	if (len <= 0) {
		s->out_len = 64;
		s->has_simple = true;
		return;
	}
	uint32_t rsize = 0, rcount = 0, out_bits = 0, simple_bits = 0;
	uint8_t rid = 0;
	bool any_id = false;
	for (ssize_t i = 0; i < len;) {
		uint8_t p = rd[i];
		if (p == 0xFE) { /* long item: skip */
			if (i + 1 >= len)
				break;
			i += 3 + rd[i + 1];
			continue;
		}
		int sz = (p & 3) == 3 ? 4 : (p & 3);
		if (i + 1 + sz > len)
			break;
		uint32_t v = 0;
		for (int k = 0; k < sz; k++)
			v |= (uint32_t)rd[i + 1 + k] << (8 * k);
		switch (p & 0xFC) {
		case 0x74: rsize = v; break;                       /* Report Size */
		case 0x94: rcount = v; break;                      /* Report Count */
		case 0x84: rid = (uint8_t)v; any_id = true; break; /* Report ID */
		case 0x90:                                          /* Output */
			if (rid == GS_OUT_REPORT_ID)
				out_bits += rsize * rcount;
			break;
		case 0x80:                                          /* Input */
			if (rid == (any_id ? 0x01 : 0x00))
				simple_bits += rsize * rcount;
			break;
		default:
			break;
		}
		i += 1 + sz;
	}
	uint32_t out_bytes = (out_bits + 7) / 8 + 1; /* + report ID */
	s->out_len = out_bits && out_bytes <= 64 ? (uint8_t)out_bytes : 0;
	s->has_simple = simple_bits == GS_SIMPLE_USB_SIZE * 8;
}

static int open_source(struct bridge *b, const char *devpath, bool any_device)
{
	if (b->nsrc >= MAX_SOURCES)
		return -1;
	const char *base = strrchr(devpath, '/');
	base = base ? base + 1 : devpath;
	if (!name_is(base, "hidraw")) {
		logmsg("not a hidraw device: %s", devpath);
		return -1;
	}
	for (int i = 0; i < b->nsrc; i++)
		if (strcmp(b->src[i].name, base) == 0)
			return 0; /* already open */
	int fd = open(devpath, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		logmsg("cannot open %s: %s", devpath, strerror(errno));
		return -1;
	}
	struct hidraw_devinfo info;
	if (ioctl(fd, HIDIOCGRAWINFO, &info) != 0 ||
	    (!any_device && ((uint16_t)info.vendor != GS_VID || (uint16_t)info.product != GS_PID_G7_PRO_8K))) {
		logmsg("%s is not a GameSir G7 Pro 8K; leaving it alone", devpath);
		close(fd);
		return -1;
	}
	struct source *s = &b->src[b->nsrc++];
	memset(s, 0, sizeof(*s));
	s->fd = fd;
	snprintf(s->name, sizeof(s->name), "%s", base);
	parse_descriptor(s);
	logmsg("%s: listening (%04x:%04x; command channel %s, gamepad report %s)", s->name,
	       (uint16_t)info.vendor, (uint16_t)info.product, s->out_len ? "yes" : "no",
	       s->has_simple ? "yes" : "no");
	return 1;
}

/* Open any hidraw nodes of the USB device not open yet. Returns how many were added. */
static int scan_usb(struct bridge *b, bool direct)
{
	char dir[PATH_MAX], names[MAX_SOURCES][32];
	b->last_scan_us = now_us();
	if (!usb_dir(b->usb, dir))
		return -1; /* unplugged */
	if (!is_g7_8k(dir))
		return -1; /* something else is on this port now */
	g_action = NODE_LIST_HIDRAW;
	g_list = names;
	g_list_n = 0;
	nftw(dir, node_walk, 16, FTW_PHYS | FTW_ACTIONRETVAL);
	int added = 0;
	for (int i = 0; i < g_list_n; i++) {
		char dev[64];
		snprintf(dev, sizeof(dev), "/dev/%.31s", names[i]);
		if (open_source(b, dev, false) > 0) {
			added++;
			/* Ask the new interface for extended reports too: directly at start-up,
			 * through the writer thread once the loop is running (writes can block). */
			struct source *s = &b->src[b->nsrc - 1];
			if (!s->out_len)
				continue; /* no command channel on this interface */
			if (direct)
				s->write_failed = gs_write_fd(s->fd, s->out_len, GS_CMD_MODE, 0, 0) != 0;
			else
				writer_post(&b->w, s->fd, s->out_len, GS_CMD_MODE, 0, 0);
		}
	}
	return added;
}

#ifndef BTN_GRIPL
#define BTN_GRIPL 0x224
#define BTN_GRIPR 0x225
#define BTN_GRIPL2 0x226
#define BTN_GRIPR2 0x227
#endif

/* Virtual Xbox Elite Series 2, with exactly the identity Linux's xpad driver gives a
 * real one (name, 045e:0b00, axis ranges, BTN_GRIP* paddles), so Steam and SDL treat
 * it as an Elite with four bindable back paddles:
 *   R5 -> P1 (BTN_GRIPR)   R4 -> P2 (BTN_GRIPR2)   L5 -> P3 (BTN_GRIPL)   L4 -> P4 (BTN_GRIPL2)
 * The Elite has no Share button and no gyro, so Capture and motion are not passed on. */
static int create_elite(struct bridge *b)
{
	int fd = open("/dev/uinput", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		logmsg("cannot open /dev/uinput: %s", strerror(errno));
		return -1;
	}
	static const int keys[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR,
				   BTN_TL, BTN_TR, BTN_MODE, BTN_GRIPR, BTN_GRIPR2, BTN_GRIPL, BTN_GRIPL2};
	static const struct { int code, min, max, fuzz, flat; } axes[] = {
		{ABS_X, -32768, 32767, 16, 128}, {ABS_Y, -32768, 32767, 16, 128},
		{ABS_RX, -32768, 32767, 16, 128}, {ABS_RY, -32768, 32767, 16, 128},
		{ABS_Z, 0, 1023, 0, 0}, {ABS_RZ, 0, 1023, 0, 0},
		{ABS_HAT0X, -1, 1, 0, 0}, {ABS_HAT0Y, -1, 1, 0, 0}};
	bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 &&
		  ioctl(fd, UI_SET_EVBIT, EV_FF) == 0 && ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) == 0;
	for (size_t i = 0; ok && i < sizeof(keys) / sizeof(keys[0]); i++)
		ok = ioctl(fd, UI_SET_KEYBIT, keys[i]) == 0;
	for (size_t i = 0; ok && i < sizeof(axes) / sizeof(axes[0]); i++) {
		struct uinput_abs_setup a;
		memset(&a, 0, sizeof(a));
		a.code = (uint16_t)axes[i].code;
		a.absinfo.minimum = axes[i].min;
		a.absinfo.maximum = axes[i].max;
		a.absinfo.fuzz = axes[i].fuzz;
		a.absinfo.flat = axes[i].flat;
		ok = ioctl(fd, UI_SET_ABSBIT, axes[i].code) == 0 && ioctl(fd, UI_ABS_SETUP, &a) == 0;
	}
	struct uinput_setup us;
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x045e;
	us.id.product = 0x0b00;
	us.id.version = 0x0500;
	us.ff_effects_max = ELITE_FF_MAX;
	snprintf(us.name, sizeof(us.name), "Microsoft X-Box One Elite 2 pad");
	if (ok)
		ok = ioctl(fd, UI_DEV_SETUP, &us) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
	if (!ok) {
		logmsg("cannot create virtual Xbox Elite 2: %s", strerror(errno));
		close(fd);
		return -1;
	}
	b->uin = fd;
	b->started = true; /* uinput has no separate start step */
	logmsg("virtual Xbox Elite 2 created");
	return 0;
}

/* Send only what changed since the last report, as one write. */
static void elite_emit(struct bridge *b)
{
	struct input_event ev[40];
	int n = 0;
	const struct pad_state *a = &b->sent, *c = &b->st;
#define EV(t, cd, v) do { memset(&ev[n], 0, sizeof(ev[n])); ev[n].type = (t); ev[n].code = (cd); ev[n].value = (v); n++; } while (0)
	static const struct { uint32_t bit; int code; } km[] = {
		{PB_A, BTN_A}, {PB_B, BTN_B}, {PB_X, BTN_X}, {PB_Y, BTN_Y}, {PB_L1, BTN_TL}, {PB_R1, BTN_TR},
		{PB_SELECT, BTN_SELECT}, {PB_START, BTN_START}, {PB_HOME, BTN_MODE}, {PB_L3, BTN_THUMBL},
		{PB_R3, BTN_THUMBR}, {PB_R5, BTN_GRIPR}, {PB_R4, BTN_GRIPR2}, {PB_L5, BTN_GRIPL}, {PB_L4, BTN_GRIPL2}};
	uint32_t diff = a->buttons ^ c->buttons;
	for (size_t i = 0; i < sizeof(km) / sizeof(km[0]); i++)
		if (diff & km[i].bit)
			EV(EV_KEY, km[i].code, (c->buttons & km[i].bit) != 0);
	if (a->lx != c->lx) EV(EV_ABS, ABS_X, c->lx);
	if (a->ly != c->ly) EV(EV_ABS, ABS_Y, c->ly);
	if (a->rx != c->rx) EV(EV_ABS, ABS_RX, c->rx);
	if (a->ry != c->ry) EV(EV_ABS, ABS_RY, c->ry);
	if (a->l2 != c->l2) EV(EV_ABS, ABS_Z, c->l2 * 1023 / 255);
	if (a->r2 != c->r2) EV(EV_ABS, ABS_RZ, c->r2 * 1023 / 255);
	if (a->hat != c->hat) {
		/* hat 0 up .. 7 up-left clockwise, 8 centred */
		static const int8_t hx[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
		static const int8_t hy[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
		int h = c->hat <= 8 ? c->hat : 8;
		EV(EV_ABS, ABS_HAT0X, hx[h]);
		EV(EV_ABS, ABS_HAT0Y, hy[h]);
	}
	if (!n)
		return;
	EV(EV_SYN, SYN_REPORT, 0);
#undef EV
	ssize_t want = (ssize_t)(n * sizeof(ev[0]));
	if (write(b->uin, ev, (size_t)want) != want) {
		if (!b->out_fail_logged)
			logmsg("uinput write failed: %s; further failures not logged", strerror(errno));
		b->out_fail_logged = true;
	} else {
		b->out_fail_logged = false;
	}
}

static int create_virtual(struct bridge *b)
{
	if (b->mode == OUT_ELITE)
		return create_elite(b);
	int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		logmsg("cannot open /dev/uhid: %s", strerror(errno));
		return -1;
	}
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s", DS5_EDGE_NAME);
	snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "g78k-bridge/%s",
		 b->usb ? b->usb : b->src[0].name);
	snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "%02x:%02x:%02x:%02x:%02x:%02x",
		 b->mac[0], b->mac[1], b->mac[2], b->mac[3], b->mac[4], b->mac[5]);
	ev.u.create2.rd_size = sizeof(DS5_EDGE_USB_DESCRIPTOR);
	memcpy(ev.u.create2.rd_data, DS5_EDGE_USB_DESCRIPTOR, sizeof(DS5_EDGE_USB_DESCRIPTOR));
	ev.u.create2.bus = BUS_USB;
	ev.u.create2.vendor = DS5_VID;
	ev.u.create2.product = DS5_EDGE_PID;
	ev.u.create2.version = DS5_EDGE_VERSION;
	if (uhid_write_ev(fd, &ev, sizeof(ev)) != 0) {
		logmsg("cannot create virtual DualSense Edge: %s", strerror(errno));
		close(fd);
		return -1;
	}
	b->uhid = fd;
	b->created_us = now_us();
	logmsg("virtual DualSense Edge created");
	return 0;
}

static void send_report(struct bridge *b, uint64_t t)
{
	if (b->mode == OUT_ELITE) {
		if (b->uin < 0)
			return;
		elite_emit(b);
		b->sent = b->st;
		b->last_send_us = t;
		b->dirty = false;
		return;
	}
	if (b->uhid < 0)
		return;
	ds5_encode(&b->st, b->ds);
	b->ds[DS5_OFF_SEQ] = b->seq++;
	uint32_t ts = (uint32_t)((t - b->t0_us) * 3u); /* 1/3 us units, wraps like the real thing */
	b->ds[DS5_OFF_SENSOR_TS + 0] = (uint8_t)ts;
	b->ds[DS5_OFF_SENSOR_TS + 1] = (uint8_t)(ts >> 8);
	b->ds[DS5_OFF_SENSOR_TS + 2] = (uint8_t)(ts >> 16);
	b->ds[DS5_OFF_SENSOR_TS + 3] = (uint8_t)(ts >> 24);
	if (uhid_send_input(b->uhid, b->ds, DS5_IN_SIZE) != 0) {
		if (!b->out_fail_logged)
			logmsg("uhid write failed: %s; further failures not logged", strerror(errno));
		b->out_fail_logged = true;
	} else {
		b->out_fail_logged = false;
	}
	b->sent = b->st;
	b->last_send_us = t;
	b->dirty = false;
}

static inline bool buttons_changed(const struct bridge *b)
{
	return b->st.buttons != b->sent.buttons || b->st.hat != b->sent.hat;
}

static void dump_unknown(struct source *s, const uint8_t *buf, ssize_t n)
{
	char hex[3 * 24 + 1];
	int len = 0;
	for (ssize_t i = 0; i < n && i < 24; i++)
		len += snprintf(hex + len, sizeof(hex) - (size_t)len, "%02x ", buf[i]);
	logmsg("%s: unrecognised report (%zd bytes): %s", s->name, n, hex);
	s->dumped = true;
}

/* The gamepad interface has gone quiet (e.g. the controller switched interface after a reconnect). */
static inline bool active_silent(const struct bridge *b, uint64_t t)
{
	return b->active >= 0 && t - b->last_active_us > ACTIVE_SILENT_US;
}

static void set_active(struct bridge *b, int idx)
{
	b->active = idx;
	b->last_rumble_lo = b->last_rumble_hi = -1;
	if (b->want_lo || b->want_hi)
		b->rumble_pending = true; /* resend the current rumble to the new interface */
}

/* Translate one report into b->st. Returns true if it was gamepad state. */
static bool handle_packet(struct bridge *b, int idx, const uint8_t *buf, ssize_t n, uint64_t t)
{
	struct source *s = &b->src[idx];
	const uint8_t *payload = NULL;
	size_t plen = 0;
	if (n >= 3 && buf[0] == GS_IN_REPORT_ID && buf[1] == GS_HDR0 && buf[2] == GS_HDR1) {
		payload = buf + 3;
		plen = (size_t)n - 3;
	} else if (n >= 2 && buf[0] == GS_HDR0 && buf[1] == GS_HDR1) {
		payload = buf + 2;
		plen = (size_t)n - 2;
	}

	if (payload && plen >= GS_MIN_PAYLOAD) {
		if (!s->extended)
			logmsg("%s: extended reports active (all buttons + gyro)", s->name);
		s->extended = true;
		/* Prefer the interface carrying extended reports, or take over from a silent one. */
		if (b->active != idx && (b->active < 0 || !b->src[b->active].extended || active_silent(b, t)))
			set_active(b, idx);
		if (b->active != idx)
			return false;
		map_extended(payload, plen, &b->st);
		b->last_active_us = t;
		return true;
	}
	const uint8_t *simple = NULL;
	if (s->has_simple && n == GS_SIMPLE_USB_SIZE + 1 && buf[0] == 0x01)
		simple = buf + 1; /* Linux hidraw keeps the report ID */
	else if (s->has_simple && n == GS_SIMPLE_USB_SIZE)
		simple = buf;
	if (simple) {
		if (s->extended && b->fallbacks++ < 3)
			logmsg("%s: controller fell back to simple reports, asking again", s->name);
		s->extended = false;
		if (b->active < 0 || (b->active != idx && active_silent(b, t)))
			set_active(b, idx);
		if (b->active != idx)
			return false;
		map_simple(simple, &b->st);
		b->last_active_us = t;
		if (t - b->last_mode_switch_us >= MODE_SWITCH_RETRY_US)
			send_mode_switch(b, false);
		return true;
	}
	b->unknown_packets++;
	if (!s->dumped)
		dump_unknown(s, buf, n);
	return false; /* battery, acks, other interfaces: ignore */
}

/* Read everything waiting on a source. Button changes go out at once, so a quick
 * press and release can't be lost; stick and gyro updates are merged into one
 * report per wake-up (or per --max-rate interval). Returns false if the source is gone. */
static bool handle_source(struct bridge *b, int idx)
{
	struct source *s = &b->src[idx];
	uint8_t buf[128];
	bool got = false;
	uint64_t t = now_us();
	/* Uncapped, one read per wake-up (poll wakes again at once if more is queued);
	 * capped, drain so reports are merged. */
	int max_reads = b->min_interval_us ? 64 : 1;
	for (int i = 0; i < max_reads; i++) {
		ssize_t n = read(s->fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				break;
			logmsg("%s: disconnected (%s)", s->name, strerror(errno));
			return false;
		}
		if (n == 0)
			break;
		if (!b->first_packet_us)
			b->first_packet_us = t;
		if (!handle_packet(b, idx, buf, n, t))
			continue;
		if (b->uhid < 0 && b->uin < 0 && create_virtual(b) != 0) {
			g_stop = 1;
			return true;
		}
		got = true;
		if (buttons_changed(b))
			send_report(b, t);
		else
			b->dirty = true;
	}
	if (got && b->dirty && (b->min_interval_us == 0 || t - b->last_send_us >= b->min_interval_us))
		send_report(b, t);
	return true;
}

/* Where commands go: the gamepad interface if it has the command channel, else any that does. */
static struct source *command_source(struct bridge *b)
{
	if (b->active >= 0 && b->src[b->active].out_len)
		return &b->src[b->active];
	for (int i = 0; i < b->nsrc; i++)
		if (b->src[i].out_len)
			return &b->src[i];
	return NULL;
}

/* Pass a rumble level (0..255 per motor) to the controller, skipping repeats. */
static void rumble_flush(struct bridge *b, uint64_t t)
{
	b->rumble_pending = false;
	uint8_t lo = b->want_lo, hi = b->want_hi;
	if (b->active < 0 || (hi == b->last_rumble_hi && lo == b->last_rumble_lo))
		return;
	b->last_rumble_hi = hi;
	b->last_rumble_lo = lo;
	b->last_rumble_post_us = t;
	struct source *out = command_source(b);
	if (out)
		writer_post(&b->w, out->fd, out->out_len, GS_CMD_RUMBLE, lo, hi);
}

/* Remember the level asked for; hand it to the writer at most once per rumble gap,
 * so a flood of changes costs the input loop nothing (the main loop flushes the rest). */
static void rumble_set(struct bridge *b, uint8_t lo, uint8_t hi)
{
	b->want_lo = lo;
	b->want_hi = hi;
	uint64_t t = now_us();
	if (t - b->last_rumble_post_us >= RUMBLE_MIN_GAP_US)
		rumble_flush(b, t);
	else
		b->rumble_pending = true;
}

static void handle_rumble(struct bridge *b, const uint8_t *d, size_t len)
{
	/* DualSense USB output report: 02 <flags0> <flags1> <right/high motor> <left/low motor> ...
	 * flags0 bits 0/1 mean "motor values are valid". SDL and Steam send the rumble *stop*
	 * for a DualSense Edge with flags0 = 0 (see SDL_hidapi_ps5.c UpdateEffects), so a
	 * 48-byte report with no flags0 bits at all counts as rumble too, as InputPlumber does. */
	if (b->active < 0 || len < 5 || d[0] != DS5_OUT_REPORT_ID)
		return;
	/* (The kernel's own LED-only reports are 63 bytes with flags0 = 0: not rumble.) */
	if (!(d[1] & 0x03) && !(d[1] == 0 && len == DS5_OUT_SHORT_SIZE))
		return;
	rumble_set(b, d[4], d[3]);
}

/* Virtual Elite force feedback: games upload rumble effects, then play/stop them.
 * uinput leaves effect timing to us, so a timed effect is stopped from the main loop
 * (id -1 = the running effect's time is up). Stopping an effect that isn't the one
 * playing does nothing. Replay delay and repeat counts aren't modelled: SDL and Steam
 * use neither. */
static void elite_ff_play(struct bridge *b, int id, bool on, uint64_t t)
{
	bool valid = id >= 0 && id < ELITE_FF_MAX && b->ff[id].used;
	if (!on || !valid) {
		if (id < 0 || id == b->ff_playing || (on && !valid)) {
			rumble_set(b, 0, 0);
			b->ff_until_us = 0;
			b->ff_playing = -1;
		}
		return;
	}
	b->ff_playing = id;
	rumble_set(b, (uint8_t)(b->ff[id].strong >> 8), (uint8_t)(b->ff[id].weak >> 8));
	b->ff_until_us = b->ff[id].length_ms ? t + (uint64_t)b->ff[id].length_ms * 1000u : 0;
}

/* Read every queued uinput event (force-feedback requests). Every upload and erase
 * request must be answered, or the game asking for rumble blocks. */
static void handle_uinput(struct bridge *b)
{
	struct input_event ev;
	/* Drain completely: uinput's queue is small and shared by play events and upload
	 * requests, and a lost upload request would block the game for 30 s. */
	for (int i = 0; i < 4096; i++) {
		if (read(b->uin, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
			return;
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up;
			memset(&up, 0, sizeof(up));
			up.request_id = (uint32_t)ev.value;
			if (ioctl(b->uin, UI_BEGIN_FF_UPLOAD, &up) != 0)
				continue;
			int id = up.effect.id;
			if (up.effect.type == FF_RUMBLE && id >= 0 && id < ELITE_FF_MAX) {
				b->ff[id].strong = up.effect.u.rumble.strong_magnitude;
				b->ff[id].weak = up.effect.u.rumble.weak_magnitude;
				b->ff[id].length_ms = up.effect.replay.length;
				b->ff[id].used = true;
				up.retval = 0;
				if (id == b->ff_playing) /* new values for the effect that's playing now */
					elite_ff_play(b, id, true, now_us());
			} else {
				up.retval = -EINVAL;
			}
			ioctl(b->uin, UI_END_FF_UPLOAD, &up);
		} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
			struct uinput_ff_erase er;
			memset(&er, 0, sizeof(er));
			er.request_id = (uint32_t)ev.value;
			if (ioctl(b->uin, UI_BEGIN_FF_ERASE, &er) != 0)
				continue;
			if (er.effect_id < ELITE_FF_MAX) {
				if ((int)er.effect_id == b->ff_playing)
					elite_ff_play(b, (int)er.effect_id, false, now_us());
				b->ff[er.effect_id].used = false;
			}
			er.retval = 0;
			ioctl(b->uin, UI_END_FF_ERASE, &er);
		} else if (ev.type == EV_FF && ev.code < ELITE_FF_MAX) {
			elite_ff_play(b, ev.code, ev.value > 0, now_us());
		}
	}
}

static void handle_get_report(struct bridge *b, uint32_t id, uint8_t rnum)
{
	uint8_t r[64];
	memset(r, 0, sizeof(r));
	switch (rnum) {
	case DS5_FEATURE_CALIBRATION: {
		/* Identity calibration: gyro 16.384 counts per deg/s, accel 8192 counts per g. */
		static const int16_t v[17] = {0, 0, 0,                               /* gyro bias */
					      2048, -2048, 2048, -2048, 2048, -2048, /* gyro +/- per axis */
					      125, 125,                              /* gyro speed +/- */
					      8192, -8192, 8192, -8192, 8192, -8192}; /* accel +/- */
		r[0] = DS5_FEATURE_CALIBRATION;
		for (int i = 0; i < 17; i++)
			put_le16(r + 1 + 2 * i, v[i]);
		uhid_reply_get_report(b->uhid, id, 0, r, 41);
		return;
	}
	case DS5_FEATURE_PAIRING: {
		static const uint8_t tail[] = {0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc, 0, 0, 0, 0};
		r[0] = DS5_FEATURE_PAIRING;
		for (int i = 0; i < 6; i++)
			r[1 + i] = b->mac[5 - i]; /* stored least-significant byte first */
		memcpy(r + 7, tail, sizeof(tail));
		uhid_reply_get_report(b->uhid, id, 0, r, 20);
		return;
	}
	case DS5_FEATURE_FIRMWARE: {
		/* Values from InputPlumber's emulated DualSense Edge (build date "Jun 19 2023 14:47:34"). */
		static const uint8_t fw[64] = {
			0x20, 0x4a, 0x75, 0x6e, 0x20, 0x31, 0x39, 0x20, 0x32, 0x30, 0x32, 0x33, 0x31, 0x34, 0x3a, 0x34,
			0x37, 0x3a, 0x33, 0x34, 0x03, 0x00, 0x44, 0x00, 0x08, 0x02, 0x00, 0x01, 0x36, 0x00, 0x00, 0x01,
			0xc1, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x01, 0x00, 0x00,
			0x14, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		uhid_reply_get_report(b->uhid, id, 0, fw, sizeof(fw));
		return;
	}
	default:
		uhid_reply_get_report(b->uhid, id, EIO, NULL, 0);
	}
}

/* Returns false if the virtual device failed. */
static bool handle_uhid_event(struct bridge *b, struct uhid_event *evp);

/* Read every queued event (the kernel's queue is small). */
static bool handle_uhid(struct bridge *b)
{
	struct uhid_event ev;
	for (int i = 0; i < 32; i++) {
		ssize_t n = read(b->uhid, &ev, sizeof(ev));
		if (n < 0)
			return errno == EINTR || errno == EAGAIN;
		if (n >= (ssize_t)sizeof(ev.type) && !handle_uhid_event(b, &ev))
			return false;
	}
	return true;
}

static bool handle_uhid_event(struct bridge *b, struct uhid_event *evp)
{
	struct uhid_event ev = *evp;
	switch (ev.type) {
	case UHID_OUTPUT:
		handle_rumble(b, ev.u.output.data, ev.u.output.size < UHID_DATA_MAX ? ev.u.output.size : UHID_DATA_MAX);
		break;
	case UHID_GET_REPORT:
		handle_get_report(b, ev.u.get_report.id, ev.u.get_report.rnum);
		break;
	case UHID_SET_REPORT:
		uhid_reply_set_report(b->uhid, ev.u.set_report.id, 0);
		break;
	case UHID_START:
		b->started = true;
		break;
	case UHID_STOP:
		/* The kernel's DualSense driver starts the device before setting it up and
		 * stops it if setup fails; no new START within the timeout = give up. */
		b->started = false;
		b->created_us = now_us();
		break;
	default: /* OPEN, CLOSE */
		break;
	}
	return true;
}

static void close_source(struct bridge *b, int idx)
{
	close(b->src[idx].fd);
	b->src[idx] = b->src[--b->nsrc];
	if (b->nsrc == 0)
		b->no_sources_since_us = now_us();
	if (b->active == idx) {
		b->active = -1;
		/* Release everything that was held, and start the fail-safe clock afresh. */
		pad_init(&b->st);
		send_report(b, now_us());
		b->first_packet_us = 0;
		b->unknown_packets = 0;
		b->last_rumble_lo = b->last_rumble_hi = -1;
	} else if (b->active == b->nsrc) {
		b->active = idx; /* the last entry moved into idx */
	}
}

/* Fail-safes: give the raw controller back rather than leave it hidden and dead.
 * Returns true when the bridge should stop. */
static bool failsafe(struct bridge *b, uint64_t t)
{
	/* Fail-safe: the kernel never accepted the virtual pad (it would be hidden and dead). */
	if (b->uhid >= 0 && !b->started && t - b->created_us >= START_TIMEOUT_US) {
		logmsg("the kernel did not accept the virtual DualSense Edge; handing the controller back");
		return true;
	}
	/* Fail-safe: the controller has no interface the bridge can read at all. */
	if (b->usb && b->nsrc == 0 && t - b->no_sources_since_us >= FAILSAFE_US) {
		logmsg("no readable interface on this controller; handing it back to games");
		return true;
	}
	/* Fail-safe: reports keep arriving but none are understood. */
	if (b->usb && b->active < 0 && b->first_packet_us &&
	    t - b->first_packet_us >= FAILSAFE_US && b->unknown_packets >= FAILSAFE_MIN_PACKETS) {
		logmsg("no recognisable GameSir reports after %u s; handing the controller back to games",
		       FAILSAFE_US / 1000000u);
		return true;
	}
	return false;
}

static int run(struct bridge *b)
{
	ds5_init_report(b->ds);
	pad_init(&b->st);
	b->sent = b->st;
	b->t0_us = b->no_sources_since_us = now_us();
	if (!b->usb)
		send_mode_switch(b, true); /* --usb mode already asked each interface in scan_usb() */

	while (!g_stop) {
		struct pollfd pfd[MAX_SOURCES + 2];
		int nfds = 0;
		/* All interfaces stay watched: unread ones would queue stale reports that
		 * could replay old button states after a switch-over. Non-gamepad data is
		 * dropped cheaply in handle_packet(). */
		for (int i = 0; i < b->nsrc; i++)
			pfd[nfds++] = (struct pollfd){.fd = b->src[i].fd, .events = POLLIN};
		int uhid_idx = -1, uin_idx = -1;
		if (b->uhid >= 0) {
			uhid_idx = nfds;
			pfd[nfds++] = (struct pollfd){.fd = b->uhid, .events = POLLIN};
		}
		if (b->uin >= 0) {
			uin_idx = nfds;
			pfd[nfds++] = (struct pollfd){.fd = b->uin, .events = POLLIN};
		}

		uint64_t wait = UINT64_MAX;
		uint64_t t = now_us();
		if (b->dirty) {
			uint64_t el = t - b->last_send_us;
			wait = el >= b->min_interval_us ? 0 : b->min_interval_us - el;
		}
		if (b->usb && b->active < 0) {
			uint64_t el = t - b->last_scan_us;
			uint64_t w = el >= RESCAN_US ? 0 : RESCAN_US - el;
			if (w < wait)
				wait = w;
		}
		if (b->rumble_pending) {
			uint64_t el = t - b->last_rumble_post_us;
			uint64_t w = el >= RUMBLE_MIN_GAP_US ? 0 : RUMBLE_MIN_GAP_US - el;
			if (w < wait)
				wait = w;
		}
		if (b->ff_until_us) { /* end of a timed rumble effect */
			uint64_t w = b->ff_until_us > t ? b->ff_until_us - t : 0;
			if (w < wait)
				wait = w;
		}
		if (b->uhid >= 0 && !b->started) { /* wake up for the start-up fail-safe */
			uint64_t el = t - b->created_us;
			uint64_t w = el >= START_TIMEOUT_US ? 0 : START_TIMEOUT_US - el;
			if (w < wait)
				wait = w;
		}
		struct timespec to, *top = NULL;
		if (wait != UINT64_MAX) {
			to.tv_sec = (time_t)(wait / 1000000u);
			to.tv_nsec = (long)(wait % 1000000u) * 1000;
			top = &to;
		}

		int r = ppoll(pfd, (nfds_t)nfds, top, &b->run_sigmask); /* SIGTERM only lands here */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll failed: %s", strerror(errno));
			break;
		}
		t = now_us();
		if (r == 0 && failsafe(b, t)) /* timeouts only; with events, checked after handling them */
			break;
		if (b->ff_until_us && t >= b->ff_until_us)
			elite_ff_play(b, -1, false, t); /* timed rumble finished */
		if (b->rumble_pending && t - b->last_rumble_post_us >= RUMBLE_MIN_GAP_US)
			rumble_flush(b, t);
		if (b->dirty && t - b->last_send_us >= b->min_interval_us)
			send_report(b, t);
		if (b->usb && b->active < 0 && t - b->last_scan_us >= RESCAN_US) {
			if (scan_usb(b, false) < 0)
				break; /* unplugged */
			if (b->nsrc == 0)
				continue;
		}
		if (r == 0)
			continue;

		if (uin_idx >= 0 && (pfd[uin_idx].revents & POLLIN))
			handle_uinput(b);
		if (uhid_idx >= 0 && (pfd[uhid_idx].revents & POLLIN) && !handle_uhid(b)) {
			logmsg("virtual device error: %s", strerror(errno));
			break;
		}
		/* Walk backwards so close_source() can swap entries safely. */
		for (int i = b->nsrc - 1; i >= 0; i--) {
			if (i >= nfds || pfd[i].fd != b->src[i].fd)
				continue;
			bool gone = (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
			if (!gone && (pfd[i].revents & POLLIN))
				gone = !handle_source(b, i);
			if (gone)
				close_source(b, i);
		}
		if (b->nsrc == 0 && !b->usb)
			break;
		if (b->nsrc == 0 && b->usb) {
			char dir[PATH_MAX];
			if (!usb_dir(b->usb, dir))
				break; /* unplugged */
		}
		if (failsafe(b, now_us())) /* again, now that this wake-up's events are handled */
			break;
	}

	/* Stop the writer first so a queued rumble can't follow the "off" below. */
	writer_stop(&b->w);
	struct source *out = command_source(b);
	if (out && (b->last_rumble_lo > 0 || b->last_rumble_hi > 0))
		gs_write_fd(out->fd, out->out_len, GS_CMD_RUMBLE, 0, 0); /* don't leave the motors running */
	if (b->uhid >= 0) {
		uhid_destroy(b->uhid);
		close(b->uhid);
		logmsg("virtual DualSense Edge removed");
	}
	if (b->uin >= 0) {
		ioctl(b->uin, UI_DEV_DESTROY);
		close(b->uin);
		logmsg("virtual Xbox Elite 2 removed");
	}
	for (int i = 0; i < b->nsrc; i++)
		close(b->src[i].fd);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: g78k-bridge [--output elite|dualsense-edge] [--max-rate HZ] --usb <usb device, e.g. 3-1.4>\n"
		"       g78k-bridge [--output ...] --hidraw /dev/hidrawN [--any-device]   (testing)\n"
		"       g78k-bridge --restore <usb device>   (give the raw controller back to games)\n"
		"       g78k-bridge --hide <usb device>      (undo --restore)\n");
}

int main(int argc, char **argv)
{
	uint32_t max_rate = 0;
	int mode = OUT_ELITE;
	bool any_device = false;
	const char *usb = NULL, *hidraw = NULL, *restore = NULL, *hide = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--restore") == 0 && i + 1 < argc) {
			restore = argv[++i];
		} else if (strcmp(argv[i], "--hide") == 0 && i + 1 < argc) {
			hide = argv[++i];
		} else if (strcmp(argv[i], "--usb") == 0 && i + 1 < argc) {
			usb = argv[++i];
		} else if (strcmp(argv[i], "--hidraw") == 0 && i + 1 < argc) {
			hidraw = argv[++i];
		} else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
			const char *o = argv[++i];
			if (strcmp(o, "elite") == 0)
				mode = OUT_ELITE;
			else if (strcmp(o, "dualsense-edge") == 0)
				mode = OUT_EDGE;
			else {
				usage();
				return 2;
			}
		} else if (strcmp(argv[i], "--any-device") == 0) {
			any_device = true; /* testing with the fake controller */
		} else if (strcmp(argv[i], "--max-rate") == 0 && i + 1 < argc) {
			char *end;
			unsigned long v = strtoul(argv[++i], &end, 10);
			if (*end || v > 100000) {
				usage();
				return 2;
			}
			max_rate = (uint32_t)v;
		} else {
			usage();
			return 2;
		}
	}
	if (restore) {
		if (!valid_usb_name(restore)) {
			usage();
			return 2;
		}
		restore_raw(restore);
		return 0;
	}
	if (hide) {
		if (!valid_usb_name(hide)) {
			usage();
			return 2;
		}
		hide_raw(hide);
		return 0;
	}
	if (!!usb == !!hidraw || (usb && !valid_usb_name(usb))) {
		usage();
		return 2;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	static struct bridge b;
	/* Block the stop signals except inside ppoll(), so one can't slip in between
	 * the g_stop check and the wait (and the writer thread never receives them). */
	sigset_t stopsigs;
	sigemptyset(&stopsigs);
	sigaddset(&stopsigs, SIGINT);
	sigaddset(&stopsigs, SIGTERM);
	sigprocmask(SIG_BLOCK, &stopsigs, &b.run_sigmask);
	sigdelset(&b.run_sigmask, SIGINT);
	sigdelset(&b.run_sigmask, SIGTERM);
	b.active = -1;
	b.uhid = -1;
	b.uin = -1;
	b.ff_playing = -1;
	b.mode = mode;
	b.usb = usb;
	b.last_rumble_lo = b.last_rumble_hi = -1;
	b.min_interval_us = max_rate ? 1000000u / max_rate : 0;

	if (usb) {
		if (scan_usb(&b, true) < 0) {
			logmsg("%s: USB device not found", usb);
			return 0;
		}
		logmsg("%s: waiting for GameSir G7 Pro 8K input (%s)", usb,
		       max_rate ? "rate capped" : "full rate");
	} else if (open_source(&b, hidraw, any_device) <= 0) {
		return 1;
	}
	make_mac(usb, b.nsrc ? b.src[0].name : "g78k", b.mac);

	writer_start(&b.w);
	run(&b); /* stops the writer thread itself */
	/* Stopped on purpose (not unplugged): let games see the raw controller again. */
	/* Whatever stopped the loop, systemd runs "--restore" afterwards (ExecStopPost=+),
	 * which hands the raw controller back; it does nothing if it was unplugged. */
	return 0;
}
