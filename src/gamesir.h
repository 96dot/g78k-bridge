/* GameSir G7 Pro 8K "extended" HID protocol.
 *
 * Everything here comes from SDL's driver, src/joystick/hidapi/SDL_hidapi_gamesir.c
 * (libsdl-org/SDL, zlib licence). Confirmed on a real G7 Pro 8K (firmware 1.35).
 */
#pragma once
#include <stdint.h>

#define GS_VID 0x3537
#define GS_PID_G7_PRO_8K 0x10B8 /* plain-HID mode, wired or 2.4 GHz dongle (the bridge only matches USB) */

/* Output report: 0xA2 <cmd> ...  first byte is the report ID. Its length comes from the
 * interface's report descriptor (37 bytes on firmware 1.35); 64 is the maximum. */
#define GS_OUT_REPORT_ID 0xA2
#define GS_CMD_MODE 0x01   /* A2 01 00 -> switch to extended reports */
#define GS_CMD_RUMBLE 0x03 /* A2 03 <low freq> <high freq> */

/* Extended input packets: [0x43] 0xA1 0xC8 <payload...> */
#define GS_IN_REPORT_ID 0x43
#define GS_HDR0 0xA1
#define GS_HDR1 0xC8

/* Payload byte 0 */
#define GS_A 0x01
#define GS_B 0x02
#define GS_X 0x08
#define GS_Y 0x10
#define GS_L1 0x40
#define GS_R1 0x80
/* Payload byte 1 */
#define GS_L2 0x01
#define GS_R2 0x02
#define GS_SELECT 0x04
#define GS_START 0x08
#define GS_HOME 0x10
#define GS_L3 0x20
#define GS_R3 0x40
#define GS_CAPTURE 0x80
/* Payload byte 2: low nibble = D-pad (0 none, 1 up, 2 up-right ... 8 up-left, clockwise) */
#define GS_DPAD_MASK 0x0F
#define GS_L4 0x40
#define GS_R4 0x80
/* Payload byte 3 */
#define GS_L5 0x01
#define GS_R5 0x02

/* Payload layout (all 16-bit values big-endian, signed) */
#define GS_OFF_LX 4  /* 4-5 */
#define GS_OFF_LY 6  /* 6-7, up = positive */
#define GS_OFF_RX 8
#define GS_OFF_RY 10
#define GS_OFF_L2 12 /* 0-255 */
#define GS_OFF_R2 13
#define GS_OFF_ACCEL 14 /* x,y,z: 14-19, 8192 counts per g (+-4 g) */
#define GS_OFF_GYRO 20  /* x,y,z: 20-25, 16 counts per deg/s */
#define GS_MIN_PAYLOAD 14
#define GS_IMU_PAYLOAD 26

/* "Simple" 9-byte USB report: sent before the mode switch is acknowledged,
 * and again after a reconnect or dongle wake-up. Layout (SDL):
 *   0: buttons like payload byte 0   1: buttons like payload byte 1
 *   2: hat 0..7 clockwise from up, 8+ = none
 *   3,4,5,6: LX LY RX RY (0x80 centre, Y down = larger)   7,8: L2 R2 */
#define GS_SIMPLE_USB_SIZE 9
