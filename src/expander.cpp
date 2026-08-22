// expander.cpp - PCA9539 driver over I2C1.

#include "expander.h"

#include <Arduino.h>
#include <Wire.h>

static const uint8_t I2C_SDA_PIN = 2;
static const uint8_t I2C_SCL_PIN = 3;
static const uint32_t I2C_FREQ   = 400000;   // 400 kHz — plenty for buttons

// PCA9539 registers
static const uint8_t REG_INPUT0    = 0x00;
static const uint8_t REG_INPUT1    = 0x01;
static const uint8_t REG_OUTPUT0   = 0x02;
static const uint8_t REG_OUTPUT1   = 0x03;
static const uint8_t REG_INVERT0   = 0x04;
static const uint8_t REG_INVERT1   = 0x05;
static const uint8_t REG_CONFIG0   = 0x06;
static const uint8_t REG_CONFIG1   = 0x07;

static bool initialized = false;
static bool present     = false;
static volatile bool intFlag = false;

// Falling-edge ISR on the PCA9539 INT pin. Just sets a flag — the
// I2C transaction to clear the interrupt happens in expander_readAll().
static void expanderIntISR(void) {
    intFlag = true;
}

static void writeReg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(EXPANDER_I2C_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
    Wire1.beginTransmission(EXPANDER_I2C_ADDR);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return 0xFF;
    if (Wire1.requestFrom(EXPANDER_I2C_ADDR, (uint8_t)1) != 1) return 0xFF;
    return Wire1.read();
}

bool expander_init(void) {
    if (initialized) return present;
    initialized = true;

    Wire1.setSDA(I2C_SDA_PIN);
    Wire1.setSCL(I2C_SCL_PIN);
    Wire1.setClock(I2C_FREQ);
    Wire1.begin();

    // Probe
    Wire1.beginTransmission(EXPANDER_I2C_ADDR);
    present = (Wire1.endTransmission() == 0);
    if (!present) {
        Serial.printf("PCA9539: not found at 0x%02X\n", EXPANDER_I2C_ADDR);
        return false;
    }
    Serial.printf("PCA9539: detected at 0x%02X\n", EXPANDER_I2C_ADDR);

    // Configure both ports as inputs (config bit = 1 -> input)
    writeReg(REG_CONFIG0, 0xFF);
    writeReg(REG_CONFIG1, 0xFF);
    // Non-inverting (invert = 0)
    writeReg(REG_INVERT0, 0x00);
    writeReg(REG_INVERT1, 0x00);

    // Wire the INT pin: PCA9539's INT is open-drain, active low, self-clears
    // on read of the input registers.
    pinMode(EXPANDER_INT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(EXPANDER_INT_PIN),
                    expanderIntISR, FALLING);

    return true;
}

bool expander_isPresent(void) {
    return present;
}

uint16_t expander_readAll(void) {
    if (!present) return 0xFFFF;
    uint8_t p0 = readReg(REG_INPUT0);
    uint8_t p1 = readReg(REG_INPUT1);
    intFlag = false;
    return (uint16_t)p0 | ((uint16_t)p1 << 8);
}

// Debug read — direct access to a single register, callable from main
// to inspect what the chip is actually returning.
uint8_t expander_readReg(uint8_t reg) {
    if (!present) return 0xFF;
    return readReg(reg);
}

bool expander_hasInterrupt(void) {
    // Also treat direct read of INT pin low as "pending" — belt-and-suspenders
    // in case an edge was missed.
    if (intFlag) return true;
    if (digitalRead(EXPANDER_INT_PIN) == LOW) {
        intFlag = true;
        return true;
    }
    return false;
}
