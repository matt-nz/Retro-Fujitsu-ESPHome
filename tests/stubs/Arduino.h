#pragma once
// Minimal host stand-ins so FujiHeatPump.cpp can be compiled and exercised on a desktop compiler.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t byte;
unsigned long millis();
void delay(unsigned long ms);
#define SERIAL_8E1 0
// Arduino.h defines these as macros; keeping them here makes the host build catch identifiers that clash.
#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x01
#define OUTPUT 0x03
