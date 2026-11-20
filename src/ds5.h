/* DualSense Edge (USB) report layout, as presented by the virtual controller.
 * Offsets are into the 64-byte input report, byte 0 = report ID 0x01.
 * Layout cross-checked against Linux hid-playstation.c and InputPlumber. */
#pragma once
#include <stdint.h>

#define DS5_VID 0x054C
#define DS5_EDGE_PID 0x0DF2
#define DS5_EDGE_VERSION 0x0100
#define DS5_EDGE_NAME "Sony Interactive Entertainment DualSense Edge Wireless Controller"

#define DS5_IN_REPORT_ID 0x01
#define DS5_IN_SIZE 64
#define DS5_OUT_REPORT_ID 0x02
#define DS5_OUT_SHORT_SIZE 48 /* SDL/Steam's output report length over USB */

#define DS5_FEATURE_CALIBRATION 0x05
#define DS5_FEATURE_PAIRING 0x09
#define DS5_FEATURE_FIRMWARE 0x20

enum {
	DS5_OFF_LX = 1, DS5_OFF_LY, DS5_OFF_RX, DS5_OFF_RY,
	DS5_OFF_L2 = 5, DS5_OFF_R2 = 6,
	DS5_OFF_SEQ = 7,
	DS5_OFF_BTN0 = 8, DS5_OFF_BTN1 = 9, DS5_OFF_BTN2 = 10,
	DS5_OFF_GYRO = 16,  /* pitch, yaw, roll: int16 LE */
	DS5_OFF_ACCEL = 22, /* x, y, z: int16 LE */
	DS5_OFF_SENSOR_TS = 28, /* uint32 LE, units of 1/3 microsecond */
	DS5_OFF_TOUCH0 = 33, /* 4 bytes per finger, bit 7 of byte 0 set = not touching */
	DS5_OFF_TOUCH1 = 37,
	DS5_OFF_BATTERY = 53, /* high nibble status, low nibble level 0..10 */
};

/* BTN0: low nibble = hat (0 up .. 7 up-left, 8 none) */
#define DS5_SQUARE 0x10
#define DS5_CROSS 0x20
#define DS5_CIRCLE 0x40
#define DS5_TRIANGLE 0x80
/* BTN1 */
#define DS5_L1 0x01
#define DS5_R1 0x02
#define DS5_L2 0x04
#define DS5_R2 0x08
#define DS5_CREATE 0x10
#define DS5_OPTIONS 0x20
#define DS5_L3 0x40
#define DS5_R3 0x80
/* BTN2 */
#define DS5_PS 0x01
#define DS5_TOUCHPAD 0x02
#define DS5_MUTE 0x04
#define DS5_EDGE_FN_LEFT 0x10
#define DS5_EDGE_FN_RIGHT 0x20
#define DS5_EDGE_PADDLE_LEFT 0x40
#define DS5_EDGE_PADDLE_RIGHT 0x80

#define DS5_HAT_NONE 8
