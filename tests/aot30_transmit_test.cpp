#include "FujiAot30Transmit.h"

#include <cstdio>

int main() {
  constexpr std::array<uint8_t, 7> expected_message = {0x7F, 0x22, 0x03, 0x96, 0x02, 0x5F, 0x7E};
  constexpr std::array<uint8_t, 7> expected_uart = {0x80, 0xDD, 0xFC, 0x69, 0xFD, 0xA0, 0x81};
  if (fuji_aot30::kCapturedHeat20On != expected_message ||
      fuji_aot30::kCapturedHeat20OnUart != expected_uart) {
    std::fprintf(stderr, "AOT30 captured HEAT 20 ON message encoding mismatch\n");
    return 1;
  }
  return 0;
}
