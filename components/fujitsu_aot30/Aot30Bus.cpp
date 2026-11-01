#include "Aot30Bus.h"

#include "esphome/core/log.h"

#include <cstdio>

#ifdef ESP32
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#endif

namespace fuji_aot30 {

namespace {
const char *const TAG = "aot30.bus";

constexpr uint32_t kCmdStart = 1;
constexpr uint32_t kCmdStop = 2;
// The bus must have been silent this long before starting; a connected remote polls every ~247 ms.
constexpr uint32_t kQuietMs = 1500;
// Polling time before the first command, so the unit sees a normal cycle first.
constexpr uint32_t kLeadMs = 2000;
// Our own transmission comes back on RX and takes up to this long to finish arriving. The unit never replies
// sooner than 30 ms after the end of a message.
constexpr uint32_t kOwnEchoMs = 25;
// Wired-remote messages heard while running before the master controller gives the bus back.
constexpr uint32_t kRemoteMessagesToStop = 2;
// Do not start a transmission within this long of the last received byte (the unit's reply is still coming).
constexpr uint32_t kQuietBeforeTxMs = 12;
// Abort a transmission after this many cells whose read-back differs from what was driven.
constexpr uint32_t kMaxMismatches = 3;

uint8_t mode_code(Mode m) {
  switch (m) {
    case Mode::COOL: return fuji_passive::kModeCool;
    case Mode::HEAT: return fuji_passive::kModeHeat;
    case Mode::FAN: return fuji_passive::kModeFan;
    case Mode::AUTO: return fuji_passive::kModeAuto;
  }
  return fuji_passive::kModeAuto;
}

uint8_t fan_code(Fan f) {
  switch (f) {
    case Fan::FAN_HIGH: return fuji_passive::kFanHigh;
    case Fan::FAN_MEDIUM: return fuji_passive::kFanMedium;
    case Fan::FAN_LOW: return fuji_passive::kFanLow;
    case Fan::FAN_AUTO: return fuji_passive::kFanAuto;
  }
  return fuji_passive::kFanAuto;
}

Observed observed_from_target(const Target &t) {
  Observed o;
  o.valid = true;
  o.on = t.on;
  if (t.on) {
    o.mode_valid = true;
    o.mode = mode_code(t.mode);
    o.fan_valid = true;
    o.fan = fan_code(t.fan);
    o.temp_valid = t.mode != Mode::FAN;
    o.temp = t.temp;
    o.eco_valid = true;
    o.eco = t.eco;
  }
  return o;
}
}  // namespace

// ---- setup -----------------------------------------------------------------------------------

void Bus::connect(HardwareSerial *serial, int rx_pin, int tx_pin) {
  serial_ = serial;
  rx_pin_ = rx_pin;
  tx_pin_ = tx_pin;
  last_rx_byte_ms_ = 0;
  last_remote_message_ms_ = 0;
#ifdef ESP32
  serial_->begin(500, SERIAL_8E1, rx_pin, tx_pin);
#else
  serial_->begin(500, SERIAL_8E1);
#endif
}

void Bus::pump() {
  drain_rx();
  master_step();
}

// ---- reading ---------------------------------------------------------------------------------

void Bus::drain_rx() {
  if (serial_ == nullptr) {
    return;
  }
  uint8_t chunk[8];
  size_t chunk_len = 0;
  auto flush_chunk = [&]() {
    if (debug_ && chunk_len > 0) {
      // Same line format as the earlier firmware so tools/decode_timeline.py reads these logs too.
      char raw[25] = {};
      for (size_t i = 0; i < chunk_len; i++) {
        snprintf(raw + i * 3, sizeof(raw) - i * 3, " %02X", static_cast<unsigned>(chunk[i]));
      }
      ESP_LOGI(TAG, "RX raw (%u/8, before XOR):%s", static_cast<unsigned>(chunk_len), raw);
    }
    chunk_len = 0;
  };
  while (serial_->available() > 0) {
    const int value = serial_->read();
    if (value < 0) {
      break;
    }
    last_rx_byte_ms_ = millis();
    if (static_cast<int32_t>(last_rx_byte_ms_ - ignore_rx_until_ms_) < 0) {
      continue;  // our own transmission, read back
    }
    chunk[chunk_len++] = static_cast<uint8_t>(value);
    on_byte(static_cast<uint8_t>(value) ^ 0xFF);
    if (chunk_len == sizeof(chunk)) {
      flush_chunk();
    }
  }
  flush_chunk();
}

void Bus::publish_observed(const Observed &state) {
  if (!(state == observed())) {
    observed_word_.store(state.pack());
    observed_revision_.fetch_add(1);
  }
}

void Bus::observe_off() {
  Observed o = observed();
  if (o.valid && !o.on) {
    return;
  }
  Observed off;
  off.valid = true;
  publish_observed(off);
}

void Bus::on_byte(uint8_t value) {
  fuji_passive::Message message;
  if (!parser_.feed(value, &message)) {
    return;
  }
  const uint32_t now = millis();
  if (message.direction == fuji_passive::Direction::REMOTE_TO_UNIT) {
    last_remote_message_ms_ = now;
    remote_message_count_++;
  }
  if (message.kind != fuji_passive::Kind::UNKNOWN) {
    last_message_ms_.store(now);
  }

  switch (message.kind) {
    case fuji_passive::Kind::SETTING: {
      if (master_enabled_.load()) {
        return;  // the byte view aliases settings; the master controller publishes the state it commanded itself
      }
      fuji_passive::Setting setting;
      if (!fuji_passive::decode_setting(message.value, &setting)) {
        if (debug_) {
          ESP_LOGD(TAG, "Unmapped controller state byte: 0x%02X", message.value);
        }
        return;
      }
      Observed o;
      o.valid = true;
      o.on = true;
      o.mode_valid = true;
      o.mode = setting.mode;
      o.fan_valid = setting.fan_valid;
      o.fan = setting.fan;
      o.temp_valid = setting.temperature_valid;
      o.temp = setting.temperature;
      // The remote's message and the unit's echo carry the same state; publish once.
      publish_observed(o);
      break;
    }
    case fuji_passive::Kind::POWER_OFF:
      observe_off();
      break;
    case fuji_passive::Kind::STATUS:
      if (message.direction == fuji_passive::Direction::UNIT_TO_REMOTE) {
        unit_running_known_ = true;
        unit_running_ = message.running;
        unit_run_state_.store(message.running ? 1 : 0);
      }
      // A running -> idle change also means off; this covers a missed power-off message. The idle -> running change
      // is ignored because the setting message already set On.
      if (passive_running_known_ && passive_running_ && !message.running) {
        observe_off();
        if (master_enabled_.load()) {
          applied_ = Target();  // the unit stopped by itself: allow the same target to be sent again
        }
      }
      passive_running_known_ = true;
      passive_running_ = message.running;
      break;
    default:
      break;
  }
}

// ---- master controller: main thread API ------------------------------------------------------

const char *Bus::master_state_text(MasterState state) {
  switch (state) {
    case MasterState::OFF: return "Off";
    case MasterState::CHECKING_BUS: return "Checking bus";
    case MasterState::RUNNING: return "Running";
    case MasterState::REFUSED_REMOTE_PRESENT: return "Refused: wired remote is on the bus";
    case MasterState::REFUSED_BUS_LOW: return "Refused: bus held low";
    case MasterState::STOPPED_REMOTE_PRESENT: return "Stopped: wired remote detected";
    case MasterState::STOPPED_TX_FAILED: return "Stopped: bus fault while sending";
  }
  return "Unknown";
}

bool Bus::start_master() {
  if (master_enabled_.exchange(true)) {
    return false;
  }
  master_state_.store(static_cast<uint8_t>(MasterState::CHECKING_BUS));
  command_.store(kCmdStart);
  return true;
}

void Bus::stop_master() {
  if (master_enabled_.load()) {
    command_.store(kCmdStop);
  }
}

bool Bus::submit_target(const Target &target) {
  if (!master_enabled_.load()) {
    return false;
  }
  target_word_.store(target.pack());
  target_dirty_.store(true);
  return true;
}

// ---- master controller: bus task side --------------------------------------------------------

// Turns the latest submitted target into a scripted change once the previous one has been sent.
void Bus::apply_target() {
  if (scheduler_.busy() || !target_dirty_.exchange(false)) {
    return;
  }
  const Target target = Target::unpack(target_word_.load());
  if (target.on) {
    if (target == applied_) {
      return;
    }
    if (!scheduler_.queue_setting(target.mode, target.fan, target.temp, target.eco)) {
      ESP_LOGW(TAG, "Master: refused target mode=%u temp=%u", static_cast<unsigned>(target.mode),
               static_cast<unsigned>(target.temp));
      return;
    }
    ESP_LOGW(TAG, "Master: sending mode=%u fan=%u temp=%u eco=%u", static_cast<unsigned>(target.mode),
             static_cast<unsigned>(target.fan), static_cast<unsigned>(target.temp), static_cast<unsigned>(target.eco));
  } else {
    if (!applied_.on && !(unit_running_known_ && unit_running_)) {
      return;  // already off
    }
    scheduler_.queue_off();
    ESP_LOGW(TAG, "Master: sending power-off");
  }
  applied_ = target;
  publish_observed(observed_from_target(target));
}

void Bus::finish_master(MasterState state) {
  scheduler_.stop();
  stage_ = Stage::IDLE;
  target_dirty_.store(false);
  master_state_.store(static_cast<uint8_t>(state));
  master_enabled_.store(false);
  observe_off();  // the unit shuts down by itself once polling stops
  ESP_LOGW(TAG, "Master controller: %s. The bus is released; the unit stops without polling.",
           master_state_text(state));
}

bool Bus::transmit(const uint8_t *cells, size_t count, const char *what) {
  const unsigned long duration_us = static_cast<unsigned long>(count * kRemoteCellTenthsUs / 10);
  if (dry_run_.load()) {
    ESP_LOGW(TAG, "Master DRY RUN would send %s (%u cells, %lu us)", what, static_cast<unsigned>(count), duration_us);
    return true;
  }
#ifdef ESP32
  if (rx_pin_ < 0 || tx_pin_ < 0) {
    ESP_LOGE(TAG, "Master: TX/RX pins unknown");
    return false;
  }
  const gpio_num_t tx = static_cast<gpio_num_t>(tx_pin_);
  const gpio_num_t rx = static_cast<gpio_num_t>(rx_pin_);

  // Take the TX pin away from the UART and drive it as a plain GPIO, idle high (recessive).
  esp_rom_gpio_connect_out_signal(tx, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(tx, GPIO_MODE_OUTPUT);
  gpio_set_level(tx, 1);

  uint32_t mismatches = 0;
  int first_bad = -1;
  int last_level = 1;
  const int64_t t0 = esp_timer_get_time();
  for (size_t i = 0; i < count; i++) {
    const int64_t cell_start = t0 + static_cast<int64_t>(i * kRemoteCellTenthsUs / 10);
    const int64_t cell_mid = cell_start + (kRemoteCellTenthsUs / 20);
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
      if (++mismatches >= kMaxMismatches) {
        break;
      }
    }
  }
  if (mismatches < kMaxMismatches) {
    const int64_t end = t0 + static_cast<int64_t>(count * kRemoteCellTenthsUs / 10);
    while (esp_timer_get_time() < end) {
    }
  }
  gpio_set_level(tx, 1);

  // Give the pin back to the UART.
  uart_set_pin(static_cast<uart_port_t>(2), tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  if (mismatches >= kMaxMismatches) {
    ESP_LOGE(TAG, "Master: %s aborted, %u read-back mismatches (first at cell %d); line released", what,
             static_cast<unsigned>(mismatches), first_bad);
    return false;
  }
  if (first_bad >= 0) {
    ESP_LOGW(TAG, "Master: %s sent with %u read-back mismatch (cell %d)", what, static_cast<unsigned>(mismatches),
             first_bad);
  }
  return true;
#else
  (void) cells;
  ESP_LOGE(TAG, "Master controller needs an ESP32");
  return false;
#endif
}

void Bus::master_step() {
  uint32_t now = millis();

  const uint32_t command = command_.exchange(0);
  if (command == kCmdStop) {
    if (master_enabled_.load()) {
      finish_master(MasterState::OFF);
    }
    return;
  }
  if (command == kCmdStart) {
    applied_ = Target();
    ignore_rx_until_ms_ = now;
    unit_running_known_ = false;  // learn it from the unit's replies once polling starts
    unit_run_state_.store(-1);
    scheduler_.stop();
    stage_ = Stage::WAIT_QUIET;
    stage_start_ms_ = now;
    ESP_LOGW(TAG, "Master%s: checking that the bus is silent for %u ms. The wired remote must be disconnected.",
             dry_run_.load() ? " (DRY RUN)" : "", static_cast<unsigned>(kQuietMs));
  }

  switch (stage_) {
    case Stage::IDLE:
      return;
    case Stage::WAIT_QUIET: {
      // Only messages from the wired remote count: the unit is seen sending a stray byte by itself about 5 s after
      // power-up with no remote connected, which must not block the start.
      const bool traffic_before =
          last_remote_message_ms_ != 0 && static_cast<uint32_t>(stage_start_ms_ - last_remote_message_ms_) < kQuietMs;
      const bool traffic_since =
          last_remote_message_ms_ != 0 && static_cast<int32_t>(last_remote_message_ms_ - stage_start_ms_) > 0;
      if (traffic_before || traffic_since) {
        ESP_LOGE(TAG, "Master refused: wired remote messages seen within %u ms of starting. Fully disconnect the "
                      "wired remote (all three wires) and start again.",
                 static_cast<unsigned>(kQuietMs));
        finish_master(MasterState::REFUSED_REMOTE_PRESENT);
        return;
      }
      if (now - stage_start_ms_ >= kQuietMs) {
#ifdef ESP32
        // A bus that never idles high cannot carry a message (an unpowered remote left attached clamps the line low).
        if (!dry_run_.load() && rx_pin_ >= 0 && gpio_get_level(static_cast<gpio_num_t>(rx_pin_)) == 0) {
          ESP_LOGE(TAG, "Master refused: the bus (GPIO%d) is held low with no traffic, so it does not idle high. "
                        "Disconnect the wired remote completely (all three wires) and check the LIN wire reads 12 V.",
                   rx_pin_);
          finish_master(MasterState::REFUSED_BUS_LOW);
          return;
        }
#endif
        scheduler_.start(now);
        remote_baseline_ = remote_message_count_;
        stage_ = Stage::LEAD;
        stage_start_ms_ = now;
        ESP_LOGW(TAG, "Master: bus silent, polling started (first poll has the run flag off, then it follows the "
                      "unit)");
      }
      return;
    }
    case Stage::LEAD:
      if (now - stage_start_ms_ >= kLeadMs) {
        stage_ = Stage::RUN;
        master_state_.store(static_cast<uint8_t>(MasterState::RUNNING));
        publish_observed(observed_from_target(applied_));
        ESP_LOGW(TAG, "Master: running, polling continuously. Waiting for a command.");
      }
      break;
    case Stage::RUN:
      apply_target();
      break;
  }
  if (remote_message_count_ - remote_baseline_ >= kRemoteMessagesToStop) {
    ESP_LOGE(TAG, "Master: a wired remote is talking on the bus, giving it back.");
    finish_master(MasterState::STOPPED_REMOTE_PRESENT);
    return;
  }

  if (unit_running_known_) {
    scheduler_.observe_unit_running(unit_running_);
  }
  Action action;
  if (!scheduler_.due(now, &action)) {
    return;
  }
  if (serial_->available() > 0 || (last_rx_byte_ms_ != 0 && now - last_rx_byte_ms_ < kQuietBeforeTxMs)) {
    return;  // the unit is still talking, try again next time round
  }

  uint8_t cells[kSettingCells];
  size_t count = 0;
  const char *what = "poll";
  switch (action.kind) {
    case Action::Kind::POLL:
      build_poll(action.running, action.pending, cells);
      count = kPollCells;
      what = action.pending ? "poll (pending)" : (action.running ? "poll (running)" : "poll (idle)");
      break;
    case Action::Kind::SETTING:
      if (!build_setting(action.mode, action.fan, action.temp, action.eco, action.repeat, cells)) {
        ESP_LOGE(TAG, "Master: invalid setting, stopping");
        finish_master(MasterState::STOPPED_TX_FAILED);
        return;
      }
      count = kSettingCells;
      what = action.repeat ? "setting (round 2)" : "setting (round 1)";
      break;
    case Action::Kind::OFF:
      build_off(cells);
      count = kOffCells;
      what = "power-off";
      break;
    default:
      return;
  }

  now = millis();
  if (!transmit(cells, count, what)) {
    finish_master(MasterState::STOPPED_TX_FAILED);
    return;
  }
  scheduler_.sent(now);
  ignore_rx_until_ms_ = millis() + kOwnEchoMs;
  if (action.kind != Action::Kind::POLL) {
    ESP_LOGW(TAG, "Master: sent %s", what);
  }
}

}  // namespace fuji_aot30
