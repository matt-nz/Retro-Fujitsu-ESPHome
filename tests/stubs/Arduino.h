#pragma once
// Minimal host stand-ins so FujiHeatPump.cpp can be compiled and exercised on a desktop compiler.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t byte;
unsigned long millis();
void delay(unsigned long ms);
#define SERIAL_8E1 0
