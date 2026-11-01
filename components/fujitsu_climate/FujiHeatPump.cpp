// ---------------------------------------------------------------------------
// Vendored from unreality/FujiHeatPump, MIT licensed, Copyright 2021 Raal Goff.
// See FujiHeatPump.h's file-header comment for what's deliberately different from
// upstream here (the sendPendingFrame() reply-gate constant, and the passive
// lastRawControllerPresent capture below) and why.
// ---------------------------------------------------------------------------

#include "FujiHeatPump.h"
#include "esphome/core/log.h"
#include <cstdio>

#ifdef ESP32
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#endif

namespace {
static const char *const TAG = "fujitsu.heatpump";

static_assert(fuji_passive::kModeFan == static_cast<byte>(FujiMode::FAN), "passive mode codes must match FujiMode");
static_assert(fuji_passive::kModeCool == static_cast<byte>(FujiMode::COOL), "passive mode codes must match FujiMode");
static_assert(fuji_passive::kModeHeat == static_cast<byte>(FujiMode::HEAT), "passive mode codes must match FujiMode");
static_assert(fuji_passive::kModeAuto == static_cast<byte>(FujiMode::AUTO), "passive mode codes must match FujiMode");
static_assert(fuji_passive::kFanAuto == static_cast<byte>(FujiFanMode::FAN_AUTO), "fan codes must match FujiFanMode");
static_assert(fuji_passive::kFanLow == static_cast<byte>(FujiFanMode::FAN_LOW), "fan codes must match FujiFanMode");
static_assert(fuji_passive::kFanMedium == static_cast<byte>(FujiFanMode::FAN_MEDIUM), "fan codes must match FujiFanMode");
static_assert(fuji_passive::kFanHigh == static_cast<byte>(FujiFanMode::FAN_HIGH), "fan codes must match FujiFanMode");

bool same_observed_state(const FujiObservedControllerState &a, const FujiObservedControllerState &b) {
  return a.valid == b.valid && a.on == b.on && a.mode_valid == b.mode_valid && a.mode == b.mode &&
         a.fan_valid == b.fan_valid && a.fan_mode == b.fan_mode && a.temperature_valid == b.temperature_valid &&
         a.temperature == b.temperature;
}
}  // namespace

// The AOT30L observer reads 0x7F-delimited messages (see FujiPassiveStream.h), not the
// 8-byte frames the secondary-controller engine below assumes. The observer remains passive;
// a separately gated, one-shot captured-frame experiment is implemented below.
void FujiHeatPump::inspectObservedControllerByte(byte value) {
  fuji_passive::Message message;
  if (!passiveParser.feed(value, &message)) {
    return;
  }

  if (message.kind != fuji_passive::Kind::UNKNOWN) {
    lastPassiveMessage = millis();
  }

  switch (message.kind) {
    case fuji_passive::Kind::SETTING: {
      fuji_passive::Setting setting;
      if (!fuji_passive::decode_setting(message.value, &setting)) {
        if (debugPrint) {
          ESP_LOGD(TAG, "Unmapped controller state byte: 0x%02X", message.value);
        }
        return;
      }
      FujiObservedControllerState decoded;
      decoded.valid = true;
      decoded.on = true;
      decoded.mode_valid = true;
      decoded.mode = setting.mode;
      decoded.fan_valid = setting.fan_valid;
      decoded.fan_mode = setting.fan;
      decoded.temperature_valid = setting.temperature_valid;
      decoded.temperature = setting.temperature;
      // The remote's message and the unit's echo carry the same state; publish once.
      if (!same_observed_state(decoded, observedControllerState)) {
        observedControllerState = decoded;
        observedControllerStateRevision++;
        if (debugPrint) {
          ESP_LOGD(TAG, "Observed controller ON state: value=0x%02X mode=%u fan_valid=%u fan=%u temp_valid=%u temp=%u",
                   message.value, static_cast<unsigned>(decoded.mode), static_cast<unsigned>(decoded.fan_valid),
                   static_cast<unsigned>(decoded.fan_mode), static_cast<unsigned>(decoded.temperature_valid),
                   static_cast<unsigned>(decoded.temperature));
        }
      }
      break;
    }
    case fuji_passive::Kind::POWER_OFF:
      setObservedOff();
      break;
    case fuji_passive::Kind::STATUS:
      // A running -> idle change also means off; this covers a missed power-off message.
      // The idle -> running change is ignored because the setting message already set On.
      if (passiveRunningKnown && passiveRunning && !message.running) {
        setObservedOff();
      }
      passiveRunningKnown = true;
      passiveRunning = message.running;
      break;
    default:
      break;
  }
}

void FujiHeatPump::setObservedOff() {
  if (observedControllerState.valid && !observedControllerState.on) {
    return;
  }
  observedControllerState.valid = true;
  observedControllerState.on = false;
  observedControllerState.mode_valid = false;
  observedControllerState.fan_valid = false;
  observedControllerState.temperature_valid = false;
  observedControllerStateRevision++;
  if (debugPrint) {
    ESP_LOGD(TAG, "Observed controller OFF");
  }
}

FujiFrame FujiHeatPump::decodeFrame() {
  FujiFrame ff;

  ff.messageSource = readBuf[0];
  ff.messageDest = readBuf[1] & 0b01111111;
  ff.messageType = (readBuf[2] & 0b00110000) >> 4;

  ff.acError = (readBuf[kErrorIndex] & kErrorMask) >> kErrorOffset;
  ff.temperature = (readBuf[kTemperatureIndex] & kTemperatureMask) >> kTemperatureOffset;
  ff.acMode = (readBuf[kModeIndex] & kModeMask) >> kModeOffset;
  ff.fanMode = (readBuf[kFanIndex] & kFanMask) >> kFanOffset;
  ff.economyMode = (readBuf[kEconomyIndex] & kEconomyMask) >> kEconomyOffset;
  ff.swingMode = (readBuf[kSwingIndex] & kSwingMask) >> kSwingOffset;
  ff.swingStep = (readBuf[kSwingStepIndex] & kSwingStepMask) >> kSwingStepOffset;
  ff.controllerPresent = (readBuf[kControllerPresentIndex] & kControllerPresentMask) >> kControllerPresentOffset;
  ff.updateMagic = (readBuf[kUpdateMagicIndex] & kUpdateMagicMask) >> kUpdateMagicOffset;
  ff.onOff = (readBuf[kEnabledIndex] & kEnabledMask) >> kEnabledOffset;
  ff.controllerTemp = (readBuf[kControllerTempIndex] & kControllerTempMask) >> kControllerTempOffset;

  ff.writeBit = (readBuf[2] & 0b00001000) != 0;
  ff.loginBit = (readBuf[1] & 0b00100000) != 0;
  ff.unknownBit = (readBuf[1] & 0b10000000) > 0;

  return ff;
}

void FujiHeatPump::encodeFrame(FujiFrame ff) {
  memset(writeBuf, 0, 8);

  writeBuf[0] = ff.messageSource;

  writeBuf[1] &= 0b10000000;
  writeBuf[1] |= ff.messageDest & 0b01111111;

  writeBuf[2] &= 0b11001111;
  writeBuf[2] |= ff.messageType << 4;

  if (ff.writeBit) {
    writeBuf[2] |= 0b00001000;
  } else {
    writeBuf[2] &= 0b11110111;
  }

  writeBuf[1] &= 0b01111111;
  if (ff.unknownBit) {
    writeBuf[1] |= 0b10000000;
  }

  if (ff.loginBit) {
    writeBuf[1] |= 0b00100000;
  } else {
    writeBuf[1] &= 0b11011111;
  }

  writeBuf[kModeIndex] = (writeBuf[kModeIndex] & ~kModeMask) | (ff.acMode << kModeOffset);
  writeBuf[kModeIndex] = (writeBuf[kEnabledIndex] & ~kEnabledMask) | (ff.onOff << kEnabledOffset);
  writeBuf[kFanIndex] = (writeBuf[kFanIndex] & ~kFanMask) | (ff.fanMode << kFanOffset);
  writeBuf[kErrorIndex] = (writeBuf[kErrorIndex] & ~kErrorMask) | (ff.acError << kErrorOffset);
  writeBuf[kEconomyIndex] = (writeBuf[kEconomyIndex] & ~kEconomyMask) | (ff.economyMode << kEconomyOffset);
  writeBuf[kTemperatureIndex] = (writeBuf[kTemperatureIndex] & ~kTemperatureMask) | (ff.temperature << kTemperatureOffset);
  writeBuf[kSwingIndex] = (writeBuf[kSwingIndex] & ~kSwingMask) | (ff.swingMode << kSwingOffset);
  writeBuf[kSwingStepIndex] = (writeBuf[kSwingStepIndex] & ~kSwingStepMask) | (ff.swingStep << kSwingStepOffset);
  writeBuf[kControllerPresentIndex] =
      (writeBuf[kControllerPresentIndex] & ~kControllerPresentMask) | (ff.controllerPresent << kControllerPresentOffset);
  writeBuf[kUpdateMagicIndex] = (writeBuf[kUpdateMagicIndex] & ~kUpdateMagicMask) | (ff.updateMagic << kUpdateMagicOffset);
  writeBuf[kControllerTempIndex] =
      (writeBuf[kControllerTempIndex] & ~kControllerTempMask) | (ff.controllerTemp << kControllerTempOffset);
}

void FujiHeatPump::connect(HardwareSerial *serial, bool secondary) { return this->connect(serial, secondary, -1, -1); }

void FujiHeatPump::connect(HardwareSerial *serial, bool secondary, int rxPin = -1, int txPin = -1) {
  _serial = serial;
  rxPin_ = rxPin;
  txPin_ = txPin;
  lastRxByteMs = 0;
  if (rxPin != -1 && txPin != -1) {
#ifdef ESP32
    _serial->begin(500, SERIAL_8E1, rxPin, txPin);
#else
    Serial.print("Setting RX/TX pin unsupported, using defaults.\n");
    _serial->begin(500, SERIAL_8E1);
#endif
  } else {
    _serial->begin(500, SERIAL_8E1);
  }
  _serial->setTimeout(200);

  if (secondary) {
    controllerIsPrimary = false;
    controllerAddress = static_cast<byte>(FujiAddress::SECONDARY);
  } else {
    controllerIsPrimary = true;
    controllerAddress = static_cast<byte>(FujiAddress::PRIMARY);
  }

  lastFrameReceived = 0;
}

void FujiHeatPump::printFrame(const char *prefix, byte buf[8], FujiFrame ff) {
  ESP_LOGI(TAG,
           "%s %02X %02X %02X %02X %02X %02X %02X %02X  mSrc:%u mDst:%u mType:%u write:%u login:%u "
           "unknown:%u onOff:%u temp:%u mode:%u cP:%u uM:%u cTemp:%u acError:%u",
           prefix, static_cast<unsigned>(buf[0]), static_cast<unsigned>(buf[1]), static_cast<unsigned>(buf[2]),
           static_cast<unsigned>(buf[3]), static_cast<unsigned>(buf[4]), static_cast<unsigned>(buf[5]),
           static_cast<unsigned>(buf[6]), static_cast<unsigned>(buf[7]), static_cast<unsigned>(ff.messageSource),
           static_cast<unsigned>(ff.messageDest), static_cast<unsigned>(ff.messageType),
           static_cast<unsigned>(ff.writeBit), static_cast<unsigned>(ff.loginBit), static_cast<unsigned>(ff.unknownBit),
           static_cast<unsigned>(ff.onOff), static_cast<unsigned>(ff.temperature), static_cast<unsigned>(ff.acMode),
           static_cast<unsigned>(ff.controllerPresent), static_cast<unsigned>(ff.updateMagic),
           static_cast<unsigned>(ff.controllerTemp), static_cast<unsigned>(ff.acError));
}

void FujiHeatPump::sendPendingFrame() {
  // Deviation #1 from upstream: `> 50` replaced with `>= kMinReplyGapMs` (0) -- see
  // FujiHeatPump.h's file-header comment and kMinReplyGapMs's own comment.
  if (pendingFrame && (millis() - lastFrameReceived) >= kMinReplyGapMs) {
    _serial->write(writeBuf, 8);
    _serial->flush();
    pendingFrame = false;
    updateFields = 0;

    _serial->readBytes(writeBuf, 8);  // read back our own frame so we dont process it again
  }
}

// ---------------------------------------------------------------------------------------------
// AOT30L/ART30RL wired-remote emulation. EXPERIMENTAL; read the comment in FujiHeatPump.h first.
// ---------------------------------------------------------------------------------------------
namespace {
// Command mailbox layout (main thread -> bus task): kind in bits 0-3, mode 4-6, fan 7-9, temp 10-14, eco 15.
constexpr uint32_t kEmuStart = 1;
constexpr uint32_t kEmuSetting = 2;
constexpr uint32_t kEmuOff = 3;
// The bus must have been silent this long before the emulator starts; a connected remote polls every ~247 ms.
constexpr uint32_t kEmuStartQuietMs = 1500;
// Do not start a transmission within this long of the last received byte (the unit's reply is still coming).
constexpr uint32_t kEmuQuietBeforeTxMs = 12;
// Abort a transmission after this many cells whose read-back differs from what was driven.
constexpr uint32_t kEmuMaxMismatches = 3;
}  // namespace

void FujiHeatPump::aot30EmulatorSetEnabled(bool enabled) {
  if (enabled) {
    emuCommand_.store(kEmuStart);
    emuEnabled_.store(true);
  } else {
    emuEnabled_.store(false);
    emuCommand_.store(0);
  }
}

bool FujiHeatPump::aot30EmulatorQueueSetting(fuji_aot30::Mode mode, fuji_aot30::Fan fan, uint8_t temp, bool eco) {
  if (!emuEnabled_.load() || (mode != fuji_aot30::Mode::FAN && !fuji_aot30::temp_in_range(mode, temp))) {
    return false;
  }
  const uint32_t command = kEmuSetting | (static_cast<uint32_t>(mode) << 4) | (static_cast<uint32_t>(fan) << 7) |
                           (static_cast<uint32_t>(temp & 0x1F) << 10) | (eco ? (1u << 15) : 0u);
  uint32_t expected = 0;
  return emuCommand_.compare_exchange_strong(expected, command);
}

bool FujiHeatPump::aot30EmulatorQueueOff() {
  if (!emuEnabled_.load()) {
    return false;
  }
  uint32_t expected = 0;
  return emuCommand_.compare_exchange_strong(expected, kEmuOff);
}

void FujiHeatPump::emuDrainRx() {
  if (_serial == nullptr) {
    return;
  }
  uint8_t chunk[8];
  size_t chunk_len = 0;
  auto flush_chunk = [&]() {
    if (debugPrint && chunk_len > 0) {
      // Same line format as waitForFrame() so tools/decode_timeline.py reads these logs too.
      char raw[25] = {};
      for (size_t i = 0; i < chunk_len; i++) {
        snprintf(raw + i * 3, sizeof(raw) - i * 3, " %02X", static_cast<unsigned>(chunk[i]));
      }
      ESP_LOGI(TAG, "RX raw (%u/8, before XOR):%s", static_cast<unsigned>(chunk_len), raw);
    }
    chunk_len = 0;
  };
  while (_serial->available() > 0) {
    const int value = _serial->read();
    if (value < 0) {
      break;
    }
    lastRxByteMs = millis();
    chunk[chunk_len++] = static_cast<uint8_t>(value);
    this->inspectObservedControllerByte(static_cast<byte>(value) ^ 0xFF);
    if (chunk_len == sizeof(chunk)) {
      flush_chunk();
    }
  }
  flush_chunk();
}

void FujiHeatPump::emuHandleCommand(uint32_t command) {
  const uint32_t kind = command & 0xF;
  if (kind == kEmuStart) {
    emuScheduler_.stop();
    emuWaitingQuiet_ = true;
    emuStartRequestedMs_ = millis();
    ESP_LOGW(TAG, "AOT30 emulator: start requested (%s); checking that the bus is silent for %u ms",
             emuDryRun_.load() ? "DRY RUN, nothing will be driven" : "LIVE", static_cast<unsigned>(kEmuStartQuietMs));
  } else if (kind == kEmuSetting) {
    const auto mode = static_cast<fuji_aot30::Mode>((command >> 4) & 0x7);
    const auto fan = static_cast<fuji_aot30::Fan>((command >> 7) & 0x7);
    const uint8_t temp = (command >> 10) & 0x1F;
    const bool eco = (command >> 15) & 1;
    if (emuScheduler_.queue_setting(mode, fan, temp, eco)) {
      ESP_LOGW(TAG, "AOT30 emulator: setting queued (mode %u, fan %u, %u C, eco %u)", static_cast<unsigned>(mode),
               static_cast<unsigned>(fan), static_cast<unsigned>(temp), static_cast<unsigned>(eco));
    } else {
      ESP_LOGW(TAG, "AOT30 emulator: setting rejected (not running yet, or another change is in progress)");
    }
  } else if (kind == kEmuOff) {
    if (emuScheduler_.queue_off()) {
      ESP_LOGW(TAG, "AOT30 emulator: power-off queued");
    } else {
      ESP_LOGW(TAG, "AOT30 emulator: power-off rejected (not running yet, or another change is in progress)");
    }
  }
}

bool FujiHeatPump::emuTransmit(const uint8_t *cells, size_t count, const char *what) {
  const unsigned long duration_us = static_cast<unsigned long>(count * fuji_aot30::kRemoteCellTenthsUs / 10);
  if (emuDryRun_.load()) {
    ESP_LOGW(TAG, "AOT30 emulator DRY RUN would send %s (%u cells, %lu us)", what, static_cast<unsigned>(count),
             duration_us);
    return true;
  }
#ifdef ESP32
  if (rxPin_ < 0 || txPin_ < 0) {
    ESP_LOGE(TAG, "AOT30 emulator: TX/RX pins unknown");
    return false;
  }
  const gpio_num_t tx = static_cast<gpio_num_t>(txPin_);
  const gpio_num_t rx = static_cast<gpio_num_t>(rxPin_);

  // Take the TX pin away from the UART and drive it as a plain GPIO, idle high (recessive).
  esp_rom_gpio_connect_out_signal(tx, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(tx, GPIO_MODE_OUTPUT);
  gpio_set_level(tx, 1);

  uint32_t mismatches = 0;
  int first_bad = -1;
  int last_level = 1;
  const int64_t t0 = esp_timer_get_time();
  for (size_t i = 0; i < count; i++) {
    const int64_t cell_start = t0 + static_cast<int64_t>(i * fuji_aot30::kRemoteCellTenthsUs / 10);
    const int64_t cell_mid = cell_start + (fuji_aot30::kRemoteCellTenthsUs / 20);
    const int level = cells[i] ? 0 : 1;  // line level is the inverse of the bus bit
    while (esp_timer_get_time() < cell_start) {
    }
    if (level != last_level) {
      gpio_set_level(tx, level);
      last_level = level;
    }
    while (esp_timer_get_time() < cell_mid) {
    }
    if (gpio_get_level(rx) != level) {
      if (first_bad < 0) {
        first_bad = static_cast<int>(i);
      }
      if (++mismatches >= kEmuMaxMismatches) {
        break;
      }
    }
  }
  if (mismatches < kEmuMaxMismatches) {
    const int64_t end = t0 + static_cast<int64_t>(count * fuji_aot30::kRemoteCellTenthsUs / 10);
    while (esp_timer_get_time() < end) {
    }
  }
  gpio_set_level(tx, 1);

  // Give the pin back to the UART.
  uart_set_pin(static_cast<uart_port_t>(2), txPin_, rxPin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  if (mismatches >= kEmuMaxMismatches) {
    ESP_LOGE(TAG, "AOT30 emulator: %s aborted, %u read-back mismatches (first at cell %d); line released", what,
             static_cast<unsigned>(mismatches), first_bad);
    return false;
  }
  if (first_bad >= 0) {
    ESP_LOGW(TAG, "AOT30 emulator: %s sent with %u read-back mismatch (cell %d)", what,
             static_cast<unsigned>(mismatches), first_bad);
  }
  return true;
#else
  (void) cells;
  ESP_LOGE(TAG, "AOT30 emulator needs an ESP32");
  return false;
#endif
}

void FujiHeatPump::aot30EmulatorPump() {
  emuDrainRx();
  const uint32_t command = emuCommand_.exchange(0);
  if (command != 0) {
    emuHandleCommand(command);
  }

  uint32_t now = millis();
  if (emuWaitingQuiet_) {
    const bool traffic_before = lastRxByteMs != 0 && static_cast<uint32_t>(emuStartRequestedMs_ - lastRxByteMs) < kEmuStartQuietMs;
    const bool traffic_since = lastRxByteMs != 0 && static_cast<int32_t>(lastRxByteMs - emuStartRequestedMs_) > 0;
    if (traffic_before || traffic_since) {
      ESP_LOGE(TAG, "AOT30 emulator: refused, bus traffic seen within %u ms of starting. Is the wired remote "
                    "still connected? Disconnect it first.",
               static_cast<unsigned>(kEmuStartQuietMs));
      emuWaitingQuiet_ = false;
      emuEnabled_.store(false);
      return;
    }
    if (now - emuStartRequestedMs_ >= kEmuStartQuietMs) {
      emuWaitingQuiet_ = false;
      emuScheduler_.start(now);
      emuTxCount_ = 0;
      ESP_LOGW(TAG, "AOT30 emulator: bus silent, polling started (unit assumed OFF)");
    }
    return;
  }

  fuji_aot30::Action action;
  if (!emuScheduler_.active() || !emuScheduler_.due(now, &action)) {
    return;
  }
  if (_serial->available() > 0 || (lastRxByteMs != 0 && now - lastRxByteMs < kEmuQuietBeforeTxMs)) {
    return;  // the unit is still talking, try again next time round
  }

  uint8_t cells[fuji_aot30::kSettingCells];
  size_t count = 0;
  const char *what = "poll";
  switch (action.kind) {
    case fuji_aot30::Action::Kind::POLL:
      fuji_aot30::build_poll(action.running, action.pending, cells);
      count = fuji_aot30::kPollCells;
      what = action.pending ? "poll (pending)" : (action.running ? "poll (running)" : "poll (idle)");
      break;
    case fuji_aot30::Action::Kind::SETTING:
      if (!fuji_aot30::build_setting(action.mode, action.fan, action.temp, action.eco, action.repeat, cells)) {
        ESP_LOGE(TAG, "AOT30 emulator: invalid setting, stopping");
        emuEnabled_.store(false);
        return;
      }
      count = fuji_aot30::kSettingCells;
      what = action.repeat ? "setting (round 2)" : "setting (round 1)";
      break;
    case fuji_aot30::Action::Kind::OFF:
      fuji_aot30::build_off(cells);
      count = fuji_aot30::kOffCells;
      what = "power-off";
      break;
    default:
      return;
  }

  now = millis();
  const bool ok = emuTransmit(cells, count, what);
  if (!ok) {
    emuEnabled_.store(false);
    emuScheduler_.stop();
    ESP_LOGE(TAG, "AOT30 emulator stopped. Reconnect the wired remote.");
    return;
  }
  emuScheduler_.sent(now);
  emuTxCount_++;
  if (action.kind != fuji_aot30::Action::Kind::POLL) {
    ESP_LOGW(TAG, "AOT30 emulator: sent %s", what);
  }
}

bool FujiHeatPump::sendCapturedHeat20OnMessage() {
  if (_serial == nullptr) {
    ESP_LOGE(TAG, "Cannot send AOT30 test message: UART is not connected");
    return false;
  }
  if (_serial->available() > 0) {
    ESP_LOGW(TAG, "AOT30 test TX blocked: UART receive data is pending");
    return false;
  }

  const size_t written = _serial->write(fuji_aot30::kCapturedHeat20OnUart.data(),
                                        fuji_aot30::kCapturedHeat20OnUart.size());
  _serial->flush();
  if (written != fuji_aot30::kCapturedHeat20OnUart.size()) {
    ESP_LOGE(TAG, "AOT30 test message write incomplete: wrote %u of %u bytes",
             static_cast<unsigned>(written),
             static_cast<unsigned>(fuji_aot30::kCapturedHeat20OnUart.size()));
    return false;
  }

  ESP_LOGW(TAG, "AOT30 experimental TX sent captured HEAT 20 ON frame once "
                "(UART: 80 DD FC 69 FD A0 81; decoded: 7F 22 03 96 02 5F 7E)");
  return true;
}

bool FujiHeatPump::sendCandidateStartupAndHeat20On() {
  if (_serial == nullptr) {
    ESP_LOGE(TAG, "Cannot send AOT30 startup experiment: UART is not connected");
    return false;
  }
  if (_serial->available() > 0) {
    ESP_LOGW(TAG, "AOT30 startup experiment blocked: UART receive data is pending");
    return false;
  }

  const size_t startup_written = _serial->write(fuji_aot30::kCandidateStartupSequenceUart.data(),
                                                fuji_aot30::kCandidateStartupSequenceUart.size());
  _serial->flush();
  if (startup_written != fuji_aot30::kCandidateStartupSequenceUart.size()) {
    ESP_LOGE(TAG, "AOT30 startup sequence write incomplete: wrote %u of %u bytes",
             static_cast<unsigned>(startup_written),
             static_cast<unsigned>(fuji_aot30::kCandidateStartupSequenceUart.size()));
    return false;
  }

  ESP_LOGW(TAG, "AOT30 experimental startup sequence sent once "
                "(UART: 80 D7 EC F7 80 ED FE; decoded: 7F 28 13 08 7F 12 01)");
  delay(fuji_aot30::kStartupToSettingDelayMs);

  const size_t setting_written = _serial->write(fuji_aot30::kCapturedHeat20OnUart.data(),
                                                fuji_aot30::kCapturedHeat20OnUart.size());
  _serial->flush();
  if (setting_written != fuji_aot30::kCapturedHeat20OnUart.size()) {
    ESP_LOGE(TAG, "AOT30 HEAT 20 setting write incomplete: wrote %u of %u bytes",
             static_cast<unsigned>(setting_written),
             static_cast<unsigned>(fuji_aot30::kCapturedHeat20OnUart.size()));
    return false;
  }

  ESP_LOGW(TAG, "AOT30 experimental HEAT 20 ON setting sent once after %u ms "
                "(UART: 80 DD FC 69 FD A0 81; decoded: 7F 22 03 96 02 5F 7E)",
           static_cast<unsigned>(fuji_aot30::kStartupToSettingDelayMs));
  return true;
}

bool FujiHeatPump::waitForFrame() {
  FujiFrame ff;

  if (_serial->available()) {
    memset(readBuf, 0, 8);
    int bytesRead = _serial->readBytes(readBuf, 8);
    if (bytesRead > 0) {
      lastRxByteMs = millis();
    }

    if (debugPrint && bytesRead > 0) {
      char rawBytes[25] = {};
      for (int i = 0; i < bytesRead; i++) {
        snprintf(rawBytes + i * 3, sizeof(rawBytes) - i * 3, " %02X", static_cast<unsigned>(readBuf[i]));
      }
      ESP_LOGI(TAG, "RX raw (%d/8, before XOR):%s", bytesRead, rawBytes);
    }

    for (int i = 0; i < bytesRead; i++) {
      this->inspectObservedControllerByte(readBuf[i] ^ 0xFF);
    }

    if (bytesRead < 8) {
      return false;
    }

    for (int i = 0; i < 8; i++) {
      readBuf[i] ^= 0xFF;
    }

    ff = decodeFrame();

    // Deviation #3 -- see FujiHeatPump.h's file-header comment. Captured here, right
    // after decode and before any of the branches below reuse/overwrite `ff` to build
    // our own outgoing reply -- this is the only point where `ff.controllerPresent`
    // still reflects what was actually received, not what we're about to send back.
    lastRawControllerPresent = ff.controllerPresent;

    // 21 Aug 2026 -- same treatment for unknownBit (readBuf[1] bit 7). See
    // FujiHeatPump.h's lastRawUnknownBit comment for why this was added.
    lastRawUnknownBit = ff.unknownBit;

    // 21 Aug 2026 -- see FujiHeatPump.h's lastRawFrame comment. readBuf is still
    // exactly the just-decoded incoming frame at this point in the function.
    memcpy(lastRawFrame, readBuf, 8);

    // 4 Oct 2026 (4A.9) -- same bytes, split by whether the frame is addressed to us.
    if (ff.messageDest == controllerAddress) {
      memcpy(lastToUsFrame, readBuf, 8);
      toUsSeen = true;
    } else {
      memcpy(lastOtherFrame, readBuf, 8);
      otherSeen = true;
    }

    if (debugPrint) {
      printFrame("<--", readBuf, ff);
    }

    if (ff.messageDest == controllerAddress) {
      lastFrameReceived = millis();

      if (ff.messageType == static_cast<byte>(FujiMessageType::STATUS)) {
        if (ff.controllerPresent == 1) {
          // we have logged into the indoor unit
          // this is what most frames are
          ff.messageSource = controllerAddress;

          if (seenSecondaryController) {
            ff.messageDest = static_cast<byte>(FujiAddress::SECONDARY);
            ff.loginBit = true;
            ff.controllerPresent = 0;
          } else {
            ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
            ff.loginBit = false;
            ff.controllerPresent = 1;
          }

          ff.updateMagic = 0;
          ff.unknownBit = true;
          ff.writeBit = 0;
          ff.messageType = static_cast<byte>(FujiMessageType::STATUS);

        } else {
          if (controllerIsPrimary) {
            // if this is the first message we have received, announce ourselves to the indoor unit
            ff.messageSource = controllerAddress;
            ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
            ff.loginBit = false;
            ff.controllerPresent = 0;
            ff.updateMagic = 0;
            ff.unknownBit = true;
            ff.writeBit = 0;
            ff.messageType = static_cast<byte>(FujiMessageType::LOGIN);

            ff.onOff = 0;
            ff.temperature = 0;
            ff.acMode = 0;
            ff.fanMode = 0;
            ff.swingMode = 0;
            ff.swingStep = 0;
            ff.acError = 0;
          } else {
            // secondary controller never seems to get any other message types, only status with controllerPresent == 0
            // the secondary controller seems to send the same flags no matter which message type

            ff.messageSource = controllerAddress;
            ff.messageDest = static_cast<byte>(FujiAddress::UNIT);
            ff.loginBit = false;
            ff.controllerPresent = 1;
            ff.updateMagic = 2;
            ff.unknownBit = true;
            ff.writeBit = 0;
          }
        }

        // if we have any updates, set the flags
        if (updateFields) {
          ff.writeBit = 1;
        }

        if (updateFields & kOnOffUpdateMask) {
          ff.onOff = updateState.onOff;
        }

        if (updateFields & kTempUpdateMask) {
          ff.temperature = updateState.temperature;
        }

        if (updateFields & kModeUpdateMask) {
          ff.acMode = updateState.acMode;
        }

        if (updateFields & kFanModeUpdateMask) {
          ff.fanMode = updateState.fanMode;
        }

        if (updateFields & kSwingModeUpdateMask) {
          ff.swingMode = updateState.swingMode;
        }

        if (updateFields & kSwingStepUpdateMask) {
          ff.swingStep = updateState.swingStep;
        }

        if (updateFields & kEconomyModeUpdateMask) {
          ff.economyMode = updateState.economyMode;
        }

        memcpy(&currentState, &ff, sizeof(FujiFrame));

      } else if (ff.messageType == static_cast<byte>(FujiMessageType::LOGIN)) {
        // received a login frame OK frame
        // the primary will send packet to a secondary controller to see if it exists
        ff.messageSource = controllerAddress;
        ff.messageDest = static_cast<byte>(FujiAddress::SECONDARY);
        ff.loginBit = true;
        ff.controllerPresent = 1;
        ff.updateMagic = 0;
        ff.unknownBit = true;
        ff.writeBit = 0;

        ff.onOff = currentState.onOff;
        ff.temperature = currentState.temperature;
        ff.acMode = currentState.acMode;
        ff.fanMode = currentState.fanMode;
        ff.swingMode = currentState.swingMode;
        ff.swingStep = currentState.swingStep;
        ff.acError = currentState.acError;
      } else if (ff.messageType == static_cast<byte>(FujiMessageType::ERROR)) {
        printFrame("AC ERROR RECV:", readBuf, ff);
        // handle errors here
        return false;
      }

      // 4 Oct 2026 (4A.9) -- EXPERIMENTAL Thermo Sensor write test (default off). See
      // thermoWriteTest in FujiHeatPump.h. updateMagic occupies byte 5 bits 7:4, so
      // |= 0x08 sets byte 5 bit 7 on the wire.
      // 17 Oct 2026 (4A.11) -- thermoExp* experiment (select in HA) takes precedence over
      // the plain write-test switch when active; see FujiHeatPump.h.
      if (ff.messageType == static_cast<byte>(FujiMessageType::STATUS)) {
        uint8_t expMode = thermoExpMode;
        if (expMode != 0) {
          uint8_t v = thermoExpValue & 0x0F;
          if (expMode == 1) {
            ff.updateMagic |= v;
          } else {
            ff.updateMagic = v;
          }
          uint8_t pulse = thermoExpPulse;
          if (pulse > 0) {
            pulse = pulse - 1;
            thermoExpPulse = pulse;
            if (pulse == 0) {
              thermoExpMode = 0;
              thermoExpFinished = true;
            }
          }
        } else if (thermoWriteTest) {
          ff.updateMagic |= 0x08;
        }
      }

      encodeFrame(ff);

      if (debugPrint) {
        printFrame("-->", writeBuf, ff);
      }

      for (int i = 0; i < 8; i++) {
        writeBuf[i] ^= 0xFF;
      }

      pendingFrame = true;

    } else if (ff.messageDest == static_cast<byte>(FujiAddress::SECONDARY)) {
      seenSecondaryController = true;
      currentState.controllerTemp = ff.controllerTemp;  // we dont have a temp sensor, use the temp reading from the secondary controller
    }

    return true;
  }

  return false;
}

bool FujiHeatPump::isBound() {
  if (millis() - lastFrameReceived < 1000) {
    return true;
  }
  return false;
}

bool FujiHeatPump::updatePending() {
  if (updateFields) {
    return true;
  }
  return false;
}

void FujiHeatPump::setOnOff(bool o) {
  updateFields |= kOnOffUpdateMask;
  updateState.onOff = o ? 1 : 0;
}
void FujiHeatPump::setTemp(byte t) {
  updateFields |= kTempUpdateMask;
  updateState.temperature = t;
}
void FujiHeatPump::setMode(byte m) {
  updateFields |= kModeUpdateMask;
  updateState.acMode = m;
}
void FujiHeatPump::setFanMode(byte fm) {
  updateFields |= kFanModeUpdateMask;
  updateState.fanMode = fm;
}
void FujiHeatPump::setEconomyMode(byte em) {
  updateFields |= kEconomyModeUpdateMask;
  updateState.economyMode = em;
}
void FujiHeatPump::setSwingMode(byte sm) {
  updateFields |= kSwingModeUpdateMask;
  updateState.swingMode = sm;
}
void FujiHeatPump::setSwingStep(byte ss) {
  updateFields |= kSwingStepUpdateMask;
  updateState.swingStep = ss;
}

bool FujiHeatPump::getOnOff() { return currentState.onOff == 1 ? true : false; }
byte FujiHeatPump::getTemp() { return currentState.temperature; }
byte FujiHeatPump::getMode() { return currentState.acMode; }
byte FujiHeatPump::getFanMode() { return currentState.fanMode; }
byte FujiHeatPump::getEconomyMode() { return currentState.economyMode; }
byte FujiHeatPump::getSwingMode() { return currentState.swingMode; }
byte FujiHeatPump::getSwingStep() { return currentState.swingStep; }
byte FujiHeatPump::getControllerTemp() { return currentState.controllerTemp; }

FujiFrame *FujiHeatPump::getCurrentState() { return &currentState; }

FujiFrame *FujiHeatPump::getUpdateState() { return &updateState; }

byte FujiHeatPump::getUpdateFields() { return updateFields; }
