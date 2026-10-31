#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fuji_aot30 {

// Candidate startup sequence observed in AOT30L captures; its role is not confirmed.
constexpr std::array<uint8_t, 7> kCandidateStartupSequence = {0x7F, 0x28, 0x13, 0x08, 0x7F, 0x12, 0x01};
// Approximate capture cadence only; the startup-to-setting interval is unconfirmed.
constexpr uint32_t kStartupToSettingDelayMs = 200;

// Captured when the wired remote was switched on in HEAT at 20 C with ECO off.
// This is a one-frame experiment, not a decoded general-purpose command format.
constexpr std::array<uint8_t, 7> kCapturedHeat20On = {0x7F, 0x22, 0x03, 0x96, 0x02, 0x5F, 0x7E};

constexpr std::array<uint8_t, 7> invert_for_uart(const std::array<uint8_t, 7> &message) {
  std::array<uint8_t, 7> encoded{};
  for (std::size_t i = 0; i < message.size(); i++) {
    encoded[i] = message[i] ^ 0xFF;
  }
  return encoded;
}

constexpr auto kCapturedHeat20OnUart = invert_for_uart(kCapturedHeat20On);
constexpr auto kCandidateStartupSequenceUart = invert_for_uart(kCandidateStartupSequence);

}  // namespace fuji_aot30
