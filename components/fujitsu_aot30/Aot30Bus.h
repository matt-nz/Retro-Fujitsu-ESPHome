#pragma once

// ---------------------------------------------------------------------------------------------
// Bus side of the AOT30L/ART30RL support (old ducted Fujitsu units with a 3-wire LIN wired remote).
//
// On this bus the wired remote is the master: it polls the unit about every 247 ms and the unit only
// replies (see docs/aot30-master-controller.md). This class does two jobs:
//
//  * Always: read the bus without blocking, split it into messages (Aot30Passive.h) and keep track of bus
//    activity, the unit's run flag and, while a real remote is on the bus, a best-effort view of its state.
//  * On request ("master controller"): take over the remote's role. Check that the bus is silent and idles
//    high, poll continuously, and turn the targets submitted by the main thread into the captured setting and
//    off messages (Aot30Cells.h, Aot30Scheduler.h), bit-banged on the TX pin with a read-back of every cell.
//    The wired remote must be disconnected (all three wires). The controller stops itself if a remote is heard
//    or a transmission does not read back correctly.
//
// pump() runs on one dedicated task. Everything else is thread safe by construction: the main thread only uses
// atomics, and the state it reads is packed into single words.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <Arduino.h>
#include <HardwareSerial.h>

#include "Aot30Cells.h"
#include "Aot30Passive.h"
#include "Aot30Scheduler.h"

namespace fuji_aot30 {

// Best-effort state seen on the bus while a real remote is connected, or the state the master controller
// commanded. Mode and fan use the fuji_passive::kMode* / kFan* codes.
struct Observed {
  bool valid = false;
  bool on = false;
  bool mode_valid = false;
  uint8_t mode = 0;
  bool fan_valid = false;
  uint8_t fan = 0;
  bool temp_valid = false;
  uint8_t temp = 0;
  bool eco_valid = false;
  bool eco = false;

  bool operator==(const Observed &o) const { return pack() == o.pack(); }
  uint32_t pack() const {
    return (valid ? 1u : 0u) | (on ? 2u : 0u) | (mode_valid ? 4u : 0u) | (static_cast<uint32_t>(mode & 7) << 3) |
           (fan_valid ? 64u : 0u) | (static_cast<uint32_t>(fan & 7) << 7) | (temp_valid ? 1024u : 0u) |
           (static_cast<uint32_t>(temp & 31) << 11) | (eco_valid ? 65536u : 0u) | (eco ? 131072u : 0u);
  }
  static Observed unpack(uint32_t w) {
    Observed o;
    o.valid = (w & 1u) != 0;
    o.on = (w & 2u) != 0;
    o.mode_valid = (w & 4u) != 0;
    o.mode = static_cast<uint8_t>((w >> 3) & 7u);
    o.fan_valid = (w & 64u) != 0;
    o.fan = static_cast<uint8_t>((w >> 7) & 7u);
    o.temp_valid = (w & 1024u) != 0;
    o.temp = static_cast<uint8_t>((w >> 11) & 31u);
    o.eco_valid = (w & 65536u) != 0;
    o.eco = (w & 131072u) != 0;
    return o;
  }
};

enum class MasterState : uint8_t {
  OFF,
  CHECKING_BUS,
  RUNNING,
  REFUSED_REMOTE_PRESENT,
  REFUSED_BUS_LOW,
  STOPPED_REMOTE_PRESENT,
  STOPPED_TX_FAILED,
};

class Bus {
 public:
  // Task side ------------------------------------------------------------------------------------
  void connect(HardwareSerial *serial, int rx_pin, int tx_pin);
  void set_debug(bool debug) { debug_ = debug; }
  // One non-blocking pass: read what has arrived and, if the master controller is on, send what is due.
  void pump();

  // Main thread ----------------------------------------------------------------------------------
  // False if the master controller is already running or starting. The bus task checks the bus and may still
  // refuse; master_state() says what happened.
  bool start_master();
  void stop_master();
  bool master_active() const { return master_enabled_.load(); }
  MasterState master_state() const { return static_cast<MasterState>(master_state_.load()); }
  static const char *master_state_text(MasterState state);
  // Ask for a new state; false if the master controller is not running.
  bool submit_target(const Target &target);
  Target last_target() const { return Target::unpack(target_word_.load()); }
  // Run flag in the unit's last reply: -1 unknown, 0 idle, 1 running.
  int unit_run_state() const { return unit_run_state_.load(); }
  Observed observed() const { return Observed::unpack(observed_word_.load()); }
  uint32_t observed_revision() const { return observed_revision_.load(); }
  // millis() of the last recognised message on the bus, 0 if none yet.
  uint32_t last_message_ms() const { return last_message_ms_.load(); }
  // Test hook for the host tests; the firmware never turns dry run on.
  void set_dry_run(bool dry_run) { dry_run_.store(dry_run); }

 private:
  enum class Stage : uint8_t { IDLE, WAIT_QUIET, LEAD, RUN };

  void drain_rx();
  void on_byte(uint8_t value);
  void publish_observed(const Observed &state);
  void observe_off();
  void master_step();
  void apply_target();
  void finish_master(MasterState state);
  bool transmit(const uint8_t *cells, size_t count, const char *what);

  HardwareSerial *serial_ = nullptr;
  int rx_pin_ = -1;
  int tx_pin_ = -1;
  bool debug_ = false;
  std::atomic<bool> dry_run_{false};

  // Reading
  fuji_passive::Parser parser_;
  uint32_t last_rx_byte_ms_ = 0;
  uint32_t last_remote_message_ms_ = 0;  // wired remote (sender nibble 2) only; ignores stray unit bytes
  uint32_t remote_message_count_ = 0;     // our own transmissions are discarded before parsing
  uint32_t ignore_rx_until_ms_ = 0;
  bool passive_running_known_ = false;
  bool passive_running_ = false;
  bool unit_running_known_ = false;
  bool unit_running_ = false;
  std::atomic<uint32_t> last_message_ms_{0};
  std::atomic<int8_t> unit_run_state_{-1};
  std::atomic<uint32_t> observed_word_{0};
  std::atomic<uint32_t> observed_revision_{0};

  // Master controller
  Scheduler scheduler_;
  std::atomic<bool> master_enabled_{false};
  std::atomic<uint32_t> command_{0};
  std::atomic<uint8_t> master_state_{0};
  std::atomic<uint32_t> target_word_{0};
  std::atomic<bool> target_dirty_{false};
  Target applied_;  // bus task: what the unit was last told
  Stage stage_ = Stage::IDLE;
  uint32_t stage_start_ms_ = 0;
  uint32_t remote_baseline_ = 0;
};

}  // namespace fuji_aot30
