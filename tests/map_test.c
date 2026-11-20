/* Unit tests for the GameSir translation and both outputs (DualSense Edge, Xbox Elite). No hardware or root needed. */
#define main bridge_main
#include "../src/g78k-bridge.c"
#undef main

static int failures;
#define CHECK(cond, ...)                                     \
	do {                                                 \
		if (!(cond)) {                               \
			failures++;                          \
			fprintf(stderr, "FAIL line %d: ", __LINE__); \
			fprintf(stderr, __VA_ARGS__);        \
			fputc('\n', stderr);                 \
		}                                            \
	} while (0)

static void payload_reset(uint8_t *p)
{
	memset(p, 0, 32);
}

static void ext(const uint8_t *p, uint8_t *ds)
{
	static struct pad_state st;
	pad_init(&st);
	map_extended(p, 32, &st);
	ds5_init_report(ds);
	ds5_encode(&st, ds);
}

int main(void)
{
	uint8_t p[32], ds[DS5_IN_SIZE];

	/* Idle: centred sticks, hat neutral, no buttons */
	payload_reset(p);
	ext(p, ds);
	CHECK(ds[DS5_OFF_LX] == 128 && ds[DS5_OFF_LY] == 128, "idle sticks %d %d", ds[DS5_OFF_LX], ds[DS5_OFF_LY]);
	CHECK(ds[DS5_OFF_BTN0] == DS5_HAT_NONE, "idle hat %#x", ds[DS5_OFF_BTN0]);
	CHECK(ds[DS5_OFF_BTN1] == 0 && ds[DS5_OFF_BTN2] == 0, "idle buttons");

	struct { int byte; uint8_t gs; int dsoff; uint8_t dsbit; const char *name; } cases[] = {
		{0, GS_A, DS5_OFF_BTN0, DS5_CROSS, "A"},
		{0, GS_B, DS5_OFF_BTN0, DS5_CIRCLE, "B"},
		{0, GS_X, DS5_OFF_BTN0, DS5_SQUARE, "X"},
		{0, GS_Y, DS5_OFF_BTN0, DS5_TRIANGLE, "Y"},
		{0, GS_L1, DS5_OFF_BTN1, DS5_L1, "L1"},
		{0, GS_R1, DS5_OFF_BTN1, DS5_R1, "R1"},
		{1, GS_L2, DS5_OFF_BTN1, DS5_L2, "L2"},
		{1, GS_R2, DS5_OFF_BTN1, DS5_R2, "R2"},
		{1, GS_SELECT, DS5_OFF_BTN1, DS5_CREATE, "Select"},
		{1, GS_START, DS5_OFF_BTN1, DS5_OPTIONS, "Start"},
		{1, GS_L3, DS5_OFF_BTN1, DS5_L3, "L3"},
		{1, GS_R3, DS5_OFF_BTN1, DS5_R3, "R3"},
		{1, GS_HOME, DS5_OFF_BTN2, DS5_PS, "Home"},
		{1, GS_CAPTURE, DS5_OFF_BTN2, DS5_TOUCHPAD, "Capture"},
		{2, GS_L4, DS5_OFF_BTN2, DS5_EDGE_PADDLE_LEFT, "L4"},
		{2, GS_R4, DS5_OFF_BTN2, DS5_EDGE_PADDLE_RIGHT, "R4"},
		{3, GS_L5, DS5_OFF_BTN2, DS5_EDGE_FN_LEFT, "L5"},
		{3, GS_R5, DS5_OFF_BTN2, DS5_EDGE_FN_RIGHT, "R5"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		payload_reset(p);
		p[cases[i].byte] = cases[i].gs;
		ext(p, ds);
		uint8_t want0 = DS5_HAT_NONE, want1 = 0, want2 = 0;
		if (cases[i].dsoff == DS5_OFF_BTN0) want0 |= cases[i].dsbit;
		if (cases[i].dsoff == DS5_OFF_BTN1) want1 |= cases[i].dsbit;
		if (cases[i].dsoff == DS5_OFF_BTN2) want2 |= cases[i].dsbit;
		CHECK(ds[DS5_OFF_BTN0] == want0 && ds[DS5_OFF_BTN1] == want1 && ds[DS5_OFF_BTN2] == want2,
		      "%s -> %02x %02x %02x, want %02x %02x %02x", cases[i].name, ds[DS5_OFF_BTN0],
		      ds[DS5_OFF_BTN1], ds[DS5_OFF_BTN2], want0, want1, want2);
	}

	/* All buttons at once: nothing lost or bleeding into the hat */
	payload_reset(p);
	p[0] = 0xFF; p[1] = 0xFF; p[2] = GS_L4 | GS_R4 | 3; p[3] = GS_L5 | GS_R5;
	ext(p, ds);
	CHECK(ds[DS5_OFF_BTN0] == (0xF0 | 2), "all buttons btn0 %#x", ds[DS5_OFF_BTN0]);
	CHECK(ds[DS5_OFF_BTN1] == 0xFF, "all buttons btn1 %#x", ds[DS5_OFF_BTN1]);
	CHECK(ds[DS5_OFF_BTN2] == 0xF3, "all buttons btn2 %#x", ds[DS5_OFF_BTN2]);

	/* D-pad: GameSir 1..8 clockwise from up -> DualSense 0..7 */
	for (int v = 0; v <= 15; v++) {
		payload_reset(p);
		p[2] = (uint8_t)v;
		ext(p, ds);
		int want = (v >= 1 && v <= 8) ? v - 1 : 8;
		CHECK((ds[DS5_OFF_BTN0] & 0x0F) == want, "dpad %d -> %d want %d", v, ds[DS5_OFF_BTN0] & 0x0F, want);
	}

	/* Sticks: GameSir signed BE with up = positive -> DualSense u8 with up = 0 */
	payload_reset(p);
	p[GS_OFF_LX] = 0x7F; p[GS_OFF_LX + 1] = 0xFF;  /* full right */
	p[GS_OFF_LY] = 0x7F; p[GS_OFF_LY + 1] = 0xFF;  /* full up */
	p[GS_OFF_RX] = 0x80; p[GS_OFF_RX + 1] = 0x00;  /* full left */
	p[GS_OFF_RY] = 0x80; p[GS_OFF_RY + 1] = 0x00;  /* full down */
	p[GS_OFF_L2] = 200; p[GS_OFF_R2] = 255;
	ext(p, ds);
	CHECK(ds[DS5_OFF_LX] == 255, "LX right %d", ds[DS5_OFF_LX]);
	CHECK(ds[DS5_OFF_LY] <= 1, "LY up %d", ds[DS5_OFF_LY]);
	CHECK(ds[DS5_OFF_RX] == 0, "RX left %d", ds[DS5_OFF_RX]);
	CHECK(ds[DS5_OFF_RY] == 255, "RY down %d", ds[DS5_OFF_RY]);
	CHECK(ds[DS5_OFF_L2] == 200 && ds[DS5_OFF_R2] == 255, "triggers");

	/* Gyro and accel: big-endian in, little-endian out, gyro x1.024, order gyro then accel */
	payload_reset(p);
	p[GS_OFF_ACCEL + 2] = 0x20; p[GS_OFF_ACCEL + 3] = 0x00; /* accel y = 8192 */
	p[GS_OFF_GYRO + 2] = 0x05; p[GS_OFF_GYRO + 3] = 0xA0;   /* gyro y = 1440 = 90 deg/s */
	p[GS_OFF_GYRO + 4] = 0x80; p[GS_OFF_GYRO + 5] = 0x00;   /* gyro z = -32768 (clamps) */
	ext(p, ds);
	int16_t gy = (int16_t)(ds[DS5_OFF_GYRO + 2] | ds[DS5_OFF_GYRO + 3] << 8);
	int16_t gz = (int16_t)(ds[DS5_OFF_GYRO + 4] | ds[DS5_OFF_GYRO + 5] << 8);
	int16_t ay = (int16_t)(ds[DS5_OFF_ACCEL + 2] | ds[DS5_OFF_ACCEL + 3] << 8);
	CHECK(gy == 1474, "gyro yaw %d want 1474", gy);
	CHECK(gz == -32768, "gyro roll clamp %d", gz);
	CHECK(ay == 8192, "accel y %d", ay);

	/* Short extended packet (no IMU) leaves the resting accel alone */
	payload_reset(p);
	struct pad_state st;
	pad_init(&st);
	map_extended(p, GS_MIN_PAYLOAD, &st);
	ds5_init_report(ds);
	ds5_encode(&st, ds);
	ay = (int16_t)(ds[DS5_OFF_ACCEL + 2] | ds[DS5_OFF_ACCEL + 3] << 8);
	CHECK(ay == 8192, "short packet accel %d", ay);

	/* Simple 9-byte report */
	uint8_t s[9] = {GS_A, GS_START | GS_HOME, 2, 0, 255, 128, 128, 10, 20};
	pad_init(&st);
	st.buttons |= PB_L4; /* left over from an extended report */
	map_simple(s, &st);
	ds5_encode(&st, ds);
	CHECK(ds[DS5_OFF_BTN0] == (DS5_CROSS | 2), "simple btn0 %#x", ds[DS5_OFF_BTN0]);
	CHECK(ds[DS5_OFF_BTN1] == DS5_OPTIONS, "simple btn1 %#x", ds[DS5_OFF_BTN1]);
	CHECK(ds[DS5_OFF_BTN2] == DS5_PS, "simple btn2 %#x (paddle must clear)", ds[DS5_OFF_BTN2]);
	CHECK(ds[DS5_OFF_LX] == 0 && ds[DS5_OFF_LY] == 255 && ds[DS5_OFF_L2] == 10 && ds[DS5_OFF_R2] == 20, "simple axes");
	/* Simple reports carry no motion data: gyro must read "still", not the last value */
	st.gyro[1] = 5000;
	map_simple(s, &st);
	ds5_encode(&st, ds);
	gy = (int16_t)(ds[DS5_OFF_GYRO + 2] | ds[DS5_OFF_GYRO + 3] << 8);
	ay = (int16_t)(ds[DS5_OFF_ACCEL + 2] | ds[DS5_OFF_ACCEL + 3] << 8);
	CHECK(gy == 0 && ay == 8192, "simple report gyro %d accel %d", gy, ay);
	s[2] = 0x12; /* upper nibble set, hat 2 */
	map_simple(s, &st);
	ds5_encode(&st, ds);
	CHECK((ds[DS5_OFF_BTN0] & 0x0F) == 2, "simple hat masks upper bits");
	s[2] = 8;
	map_simple(s, &st);
	ds5_encode(&st, ds);
	CHECK((ds[DS5_OFF_BTN0] & 0x0F) == DS5_HAT_NONE, "simple hat none");

	/* Xbox Elite output: only changed values are sent, with xpad's codes and ranges */
	{
		int pfd[2];
		if (pipe(pfd) != 0) {
			perror("pipe");
			return 1;
		}
		static struct bridge eb;
		memset(&eb, 0, sizeof(eb));
		eb.uin = pfd[1];
		pad_init(&eb.st);
		eb.sent = eb.st;
		elite_emit(&eb);                  /* nothing changed: nothing written */
		eb.st.buttons = PB_R4 | PB_L5 | PB_A;
		eb.st.ly = -32768;                /* full up */
		eb.st.l2 = 255;
		eb.st.hat = 2;                    /* right */
		elite_emit(&eb);
		close(pfd[1]);
		struct input_event ev[32];
		ssize_t n = read(pfd[0], ev, sizeof(ev));
		close(pfd[0]);
		int got_gripr = 0, got_gripl2 = 0, got_gripr2 = 0, got_gripl = 0, got_a = 0, y = 0, z = 0, hx = 9, hy = 9, syn = 0, n_ev = (int)(n / (ssize_t)sizeof(ev[0]));
		for (int i = 0; i < n_ev; i++) {
			if (ev[i].type == EV_KEY && ev[i].code == BTN_GRIPR) got_gripr = ev[i].value;
			if (ev[i].type == EV_KEY && ev[i].code == BTN_GRIPL2) got_gripl2 = ev[i].value;
			if (ev[i].type == EV_KEY && ev[i].code == BTN_GRIPR2) got_gripr2 = ev[i].value;
			if (ev[i].type == EV_KEY && ev[i].code == BTN_GRIPL) got_gripl = ev[i].value;
			if (ev[i].type == EV_KEY && ev[i].code == BTN_A) got_a = ev[i].value;
			if (ev[i].type == EV_ABS && ev[i].code == ABS_Y) y = ev[i].value;
			if (ev[i].type == EV_ABS && ev[i].code == ABS_Z) z = ev[i].value;
			if (ev[i].type == EV_ABS && ev[i].code == ABS_HAT0X) hx = ev[i].value;
			if (ev[i].type == EV_ABS && ev[i].code == ABS_HAT0Y) hy = ev[i].value;
			if (ev[i].type == EV_SYN) syn++;
		}
		CHECK(n_ev == 8, "elite: %d events, want 8 (3 keys, Y, Z, hat x/y, SYN)", n_ev);
		/* R4 -> P2 (GRIPR2), L5 -> P3 (GRIPL); GRIPR (P1) and GRIPL2 (P4) must stay untouched */
		CHECK(got_gripr2 == 1 && got_gripl == 1 && got_gripr == 0 && got_gripl2 == 0 && got_a == 1,
		      "elite: R4 should be P2 (GRIPR2) and L5 P3 (GRIPL)");
		CHECK(y == -32768 && z == 1023 && hx == 1 && hy == 0 && syn == 1, "elite axes y=%d z=%d hat %d,%d", y, z, hx, hy);
	}

	/* Elite paddle layout, one paddle at a time: R5->P1, R4->P2, L5->P3, L4->P4 */
	{
		static const struct { uint32_t bit; int code; const char *name; } lay[] = {
			{PB_R5, BTN_GRIPR, "R5->P1"}, {PB_R4, BTN_GRIPR2, "R4->P2"},
			{PB_L5, BTN_GRIPL, "L5->P3"}, {PB_L4, BTN_GRIPL2, "L4->P4"}};
		for (size_t k = 0; k < 4; k++) {
			int pfd[2];
			if (pipe(pfd) != 0)
				return 1;
			static struct bridge eb;
			memset(&eb, 0, sizeof(eb));
			eb.uin = pfd[1];
			pad_init(&eb.st);
			eb.sent = eb.st;
			eb.st.buttons = lay[k].bit;
			elite_emit(&eb);
			close(pfd[1]);
			struct input_event ev[4];
			ssize_t n = read(pfd[0], ev, sizeof(ev));
			close(pfd[0]);
			CHECK(n == 2 * (ssize_t)sizeof(ev[0]) && ev[0].type == EV_KEY && ev[0].code == lay[k].code &&
			      ev[0].value == 1, "elite layout %s", lay[k].name);
		}
	}

	/* Simple-report stick conversion keeps full range */
	CHECK(stick16(0x80) == 0 && stick16(0) == -32768 && stick16(255) == 32639, "stick16");

	/* Name validation used for the systemd instance */
	CHECK(valid_usb_name("3-1.4") && valid_usb_name("1-2"), "usb names");
	CHECK(!valid_usb_name("../x") && !valid_usb_name("") && !valid_usb_name("3-1;rm"), "bad usb names");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("map_test: all checks passed\n");
	return 0;
}
