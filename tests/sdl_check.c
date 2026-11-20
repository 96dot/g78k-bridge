/* sdl_check: watch the bridge's virtual controller (Xbox Elite or DualSense Edge) the way SDL sees it.
 *
 * Prints every button press by name, big stick/trigger moves and gyro, then a
 * summary of which buttons were seen. Run as your normal user, not root, so it
 * also proves the virtual controller is accessible to Steam.
 *
 * Usage: sdl_check [seconds] [vid:pid]   (default 30 s, 054c:0df2 = DualSense Edge)
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static const SDL_GamepadButton WANTED[] = {
	SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
	SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_GUIDE,
	SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_TOUCHPAD,
	SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
	SDL_GAMEPAD_BUTTON_DPAD_LEFT,
	SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
	SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
};
#define NWANTED (sizeof(WANTED) / sizeof(WANTED[0]))

static const char *button_label(SDL_GamepadButton b)
{
	switch (b) {
	/* SDL's names; which physical Edge button each one is depends on SDL's mapping. */
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "right paddle 1";
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "left paddle 1";
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "right paddle 2";
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "left paddle 2";
	default: return SDL_GetGamepadStringForButton(b);
	}
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 30;
	unsigned want_vid = 0x054c, want_pid = 0x0df2;
	if (argc > 2 && sscanf(argv[2], "%x:%x", &want_vid, &want_pid) != 2) {
		fprintf(stderr, "bad vid:pid %s\n", argv[2]);
		return 2;
	}
	bool seen[SDL_GAMEPAD_BUTTON_COUNT] = {0};
	bool gyro_seen = false, sticks_seen = false, triggers_seen = false;
	bool has_gyro = false, has_button[SDL_GAMEPAD_BUTTON_COUNT] = {0};

	if (!SDL_Init(SDL_INIT_GAMEPAD)) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Gamepad *pad = NULL;
	Uint64 end = SDL_GetTicks() + (Uint64)seconds * 1000, last_gyro_print = 0;
	printf("waiting up to %d s for a gamepad %04x:%04x...\n", seconds, want_vid, want_pid);

	while (SDL_GetTicks() < end) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_EVENT_GAMEPAD_ADDED:
				if (!pad && SDL_GetGamepadVendorForID(e.gdevice.which) == want_vid &&
				    SDL_GetGamepadProductForID(e.gdevice.which) == want_pid) {
					pad = SDL_OpenGamepad(e.gdevice.which);
					if (pad) {
						printf("found: \"%s\" (type %s, path %s)\n", SDL_GetGamepadName(pad),
						       SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)),
						       SDL_GetGamepadPath(pad) ? SDL_GetGamepadPath(pad) : "?");
						printf("paddles reported by SDL: %s\n",
						       SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) ? "yes" : "NO");
						for (int bi = 0; bi < SDL_GAMEPAD_BUTTON_COUNT; bi++)
							has_button[bi] = SDL_GamepadHasButton(pad, (SDL_GamepadButton)bi);
						if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
							has_gyro = true;
							SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
							printf("gyro: available\n");
						} else {
							printf("gyro: NOT available\n");
						}
					}
				}
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				if (pad && e.gbutton.which == SDL_GetGamepadID(pad)) {
					printf("  pressed: %s\n", button_label((SDL_GamepadButton)e.gbutton.button));
					if (e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT)
						seen[e.gbutton.button] = true;
				}
				break;
			case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				if (pad && e.gaxis.which == SDL_GetGamepadID(pad) && SDL_abs(e.gaxis.value) > 30000) {
					bool trig = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
						    e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
					if (trig) triggers_seen = true; else sticks_seen = true;
					printf("  axis %s = %d\n", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e.gaxis.axis),
					       e.gaxis.value);
				}
				break;
			case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
				if (pad && e.gsensor.sensor == SDL_SENSOR_GYRO && SDL_GetTicks() - last_gyro_print > 500) {
					float yaw = e.gsensor.data[1] * 57.29578f;
					if (SDL_fabsf(yaw) > 5.0f) {
						gyro_seen = true;
						printf("  gyro yaw %.1f deg/s\n", yaw);
						last_gyro_print = SDL_GetTicks();
					}
				}
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				if (pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
					printf("controller removed\n");
					SDL_CloseGamepad(pad);
					pad = NULL;
				}
				break;
			default:
				break;
			}
		}
		SDL_Delay(2);
	}

	int missing = 0;
	printf("\nsummary:\n");
	/* Only count what this controller type has (an Xbox Elite has no touchpad or gyro). */
	for (size_t i = 0; i < NWANTED; i++) {
		if (!has_button[WANTED[i]]) {
			printf("  n/a  %s\n", button_label(WANTED[i]));
			continue;
		}
		printf("  %-4s %s\n", seen[WANTED[i]] ? "ok" : "--", button_label(WANTED[i]));
		missing += !seen[WANTED[i]];
	}
	printf("  %-4s sticks\n  %-4s triggers\n  %-4s gyro\n", sticks_seen ? "ok" : "--",
	       triggers_seen ? "ok" : "--", !has_gyro ? "n/a" : gyro_seen ? "ok" : "--");
	missing += !sticks_seen + !triggers_seen + (has_gyro && !gyro_seen);
	printf(missing ? "RESULT: %d item(s) not seen\n" : "RESULT: everything seen\n", missing);
	if (pad)
		SDL_CloseGamepad(pad);
	SDL_Quit();
	return missing ? 3 : 0;
}
