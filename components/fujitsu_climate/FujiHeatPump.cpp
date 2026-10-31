// ---------------------------------------------------------------------------
// Vendored from unreality/FujiHeatPump, MIT licensed, Copyright 2021 Raal Goff.
// See FujiHeatPump.h's file-header comment for what's deliberately different from
// upstream here (the sendPendingFrame() reply-gate constant, and the passive
// lastRawControllerPresent capture below) and why.
// ---------------------------------------------------------------------------

#include "FujiHeatPump.h"
#include "esphome/core/log.h"
#include <cstdio>

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

// Receive-only. The AOT30L bus carries 0x7F-delimited messages (see FujiPassiveStream.h), not
// the 8-byte frames the secondary-controller engine below assumes, so state is taken from the
// wired remote's setting/power messages here. Nothing is transmitted.
void FujiHeatPump::inspectObservedControllerByte(byte value) {
  fuji_passive::Message message;
  if (!passiveParser.feed(value, &message)) {
    return;
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

bool FujiHeatPump::waitForFrame() {
  FujiFrame ff;

  if (_serial->available()) {
    memset(readBuf, 0, 8);
    int bytesRead = _serial->readBytes(readBuf, 8);

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
