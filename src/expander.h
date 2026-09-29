// expander.h - PCA9539 16-bit I/O expander driver.
//
// The PCA9539 sits on I2C1 (GP2=SDA, GP3=SCL) at address 0x74 (A0/A1 to GND).
// It holds all Atari controller / paddle-fire / joycon-button inputs:
//
//   P0.0  Player 1  UP
//   P0.1  Player 1  DOWN
//   P0.2  Player 1  LEFT
//   P0.3  Player 1  RIGHT
//   P0.4  Player 1  FIRE
//   P0.5  spare / reserved for BTN_A
//   P0.6  spare / reserved for BTN_B
//   P0.7  spare
//   P1.0  Player 2  UP
//   P1.1  Player 2  DOWN
//   P1.2  Player 2  LEFT
//   P1.3  Player 2  RIGHT
//   P1.4  Player 2  FIRE
//   P1.5  JoyCon button
//   P1.6  spare
//   P1.7  spare
//
// All bits are ACTIVE LOW (button pressed = 0). INT pin (open-drain) on
// GP36 pulses low whenever any input state changes.

#ifndef EXPANDER_H
#define EXPANDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXPANDER_I2C_ADDR   0x74
#define EXPANDER_INT_PIN    36

// Configure I2C1, probe the expander, and set all 16 pins as inputs.
// Returns true if the chip ACKed.
bool expander_init(void);

// True if init detected the chip.
bool expander_isPresent(void);

// Read all 16 inputs in one transaction. Bit layout (LSB first):
//   bits 0..7  = Port 0 (P0.0..P0.7)
//   bits 8..15 = Port 1 (P1.0..P1.7)
// Active low: pressed = 0, released = 1. Returns 0xFFFF if the chip is
// absent (so caller's active-low checks gracefully report "not pressed").
uint16_t expander_readAll(void);

// True if the INT line has pulsed low since the last readAll (inputs changed).
bool expander_hasInterrupt(void);

// Debug: read a single register directly. reg = 0..7 (Input0/1, Output0/1,
// Invert0/1, Config0/1). Returns 0xFF if the chip is absent.
uint8_t expander_readReg(uint8_t reg);

// Convenience bit masks
#define EXP_P1_UP      0x0001
#define EXP_P1_DOWN    0x0002
#define EXP_P1_LEFT    0x0004
#define EXP_P1_RIGHT   0x0008
#define EXP_P1_FIRE    0x0010
#define EXP_BTN_B      0x0020   // P0.5
#define EXP_BTN_A      0x0040   // P0.6
#define EXP_P0_7       0x0080
#define EXP_P2_UP      0x0100
#define EXP_P2_DOWN    0x0200
#define EXP_P2_LEFT    0x0400
#define EXP_P2_RIGHT   0x0800
#define EXP_P2_FIRE    0x1000
#define EXP_JOYCON_BTN 0x2000
#define EXP_P1_6       0x4000
#define EXP_P1_7       0x8000

#ifdef __cplusplus
}
#endif

#endif
