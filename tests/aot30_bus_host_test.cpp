// Exercises the master-controller code in Aot30Bus.cpp on a desktop compiler with fake time, GPIO and UART.
//   g++ -std=c++17 -w -DESP32 -Itests/stubs -Icomponents/fujitsu_aot30 tests/aot30_bus_host_test.cpp \
//       components/fujitsu_aot30/Aot30Bus.cpp -o /tmp/aot30_bus_host_test && /tmp/aot30_bus_host_test
// Nothing here touches hardware. The fake GPIO loops the driven level back to the RX pin, like the transceiver.

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>

#include "Aot30Bus.h"

using namespace fuji_aot30;

// ---- fakes ---------------------------------------------------------------------------------------
static int64_t g_now_us = 0;
static std::vector<std::string> g_log;
static int g_tx_level = 1;
static bool g_loopback = true;
static bool g_rx_forced_low = false;
struct Edge {
  int64_t t;
  int level;
};
static std::vector<Edge> g_edges;
static int g_uart_set_pin_calls = 0;
static int g_matrix_calls = 0;
static int g_gpio_sets = 0;

unsigned long millis() { return static_cast<unsigned long>(g_now_us / 1000); }
void delay(unsigned long ms) { g_now_us += static_cast<int64_t>(ms) * 1000; }
int64_t esp_timer_get_time() { return g_now_us += 3; }  // each read advances 3 us
void fake_log(char level, const char *fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_log.push_back(std::string(1, level) + " " + buf);
}
int gpio_set_direction(int, gpio_mode_t) { return 0; }
int gpio_set_level(int, uint32_t level) {
  g_gpio_sets++;
  g_tx_level = static_cast<int>(level);
  g_edges.push_back({g_now_us, g_tx_level});
  return 0;
}
int gpio_get_level(int) { return g_rx_forced_low ? 0 : (g_loopback ? g_tx_level : 1); }
int uart_set_pin(int, int, int, int, int) {
  g_uart_set_pin_calls++;
  return 0;
}
void esp_rom_gpio_connect_out_signal(uint32_t, uint32_t, bool, bool) { g_matrix_calls++; }

// ---- helpers -------------------------------------------------------------------------------------
static int failures = 0;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);    \
      failures++;                                         \
    }                                                     \
  } while (0)

static bool log_has(const char *needle) {
  for (const auto &l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}
static size_t log_count(const char *needle) {
  size_t n = 0;
  for (const auto &l : g_log)
    if (l.find(needle) != std::string::npos) n++;
  return n;
}

static void advance(Bus &hp, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    g_now_us += 1000;
    hp.pump();
  }
}

static void reset(HardwareSerial &serial, Bus &hp) {
  g_log.clear();
  g_edges.clear();
  g_loopback = true;
  g_rx_forced_low = false;
  g_tx_level = 1;
  g_uart_set_pin_calls = g_matrix_calls = g_gpio_sets = 0;
  serial.rx.clear();
  hp.connect(&serial, 16, 17);
}

int main() {
  using MS = MasterState;
  static HardwareSerial serial;
  static Bus hp;
  g_now_us = 10 * 1000 * 1000;
  const uint8_t kRemotePoll[] = {0x80, 0xD7, 0xEC, 0xF7, 0x80};  // 7F 28 13 08 plus the next start byte

  // 1. Dry run: starts after 1.5 s of silence, polls, then runs and waits for a command.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(!hp.submit_target(Target()));  // not running yet
  CHECK(hp.start_master());
  CHECK(!hp.start_master());  // already running
  CHECK(hp.master_active() && hp.master_state() == MS::CHECKING_BUS);
  advance(hp, 1000);
  CHECK(!log_has("polling started"));
  advance(hp, 700);
  CHECK(log_has("polling started"));
  CHECK(hp.master_state() == MS::CHECKING_BUS);
  advance(hp, 2000);
  CHECK(log_has("running, polling continuously"));
  CHECK(hp.master_state() == MS::RUNNING);
  CHECK(hp.observed().valid && !hp.observed().on);
  CHECK(g_gpio_sets == 0 && g_matrix_calls == 0);  // dry run touches no pins
  CHECK(log_count("DRY RUN would send poll") >= 8);

  // 1b. A submitted target becomes the two-round setting (first round 397 ms after the last poll), then the off message.
  // Polls continue and nothing is resent without a new target.
  g_log.clear();
  Target heat;
  heat.on = true;
  heat.mode = Mode::HEAT;
  heat.temp = 20;
  heat.eco = true;
  CHECK(hp.submit_target(heat));
  advance(hp, 100);
  Observed st = hp.observed();
  CHECK(st.on && st.mode == fuji_passive::kModeHeat && st.temp == 20 && st.temp_valid);
  CHECK(st.eco_valid && st.eco && st.fan_valid && st.fan == fuji_passive::kFanAuto);
  advance(hp, 3000);
  CHECK(log_count("DRY RUN would send setting (round 1)") == 1);
  CHECK(log_count("DRY RUN would send setting (round 2)") == 1);
  CHECK(log_count("DRY RUN would send poll (pending)") == 2);
  g_log.clear();
  advance(hp, 5000);
  CHECK(log_count("would send setting") == 0 && log_count("would send poll") >= 15);
  CHECK(hp.submit_target(heat));  // same state again: nothing is sent
  advance(hp, 1000);
  CHECK(log_count("would send setting") == 0);
  Target off = heat;
  off.on = false;
  CHECK(hp.submit_target(off));
  advance(hp, 1500);
  CHECK(log_count("DRY RUN would send power-off") == 1);
  CHECK(!hp.observed().on);
  CHECK(hp.submit_target(off));  // already off: nothing sent
  advance(hp, 1500);
  CHECK(log_count("would send power-off") == 1);
  hp.stop_master();
  advance(hp, 50);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(log_has("Off. The bus is released"));
  CHECK(!hp.submit_target(heat));

  // 2. Refuses to start when a wired remote was heard just before or during the silence check.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
  advance(hp, 300);
  CHECK(log_has("wired remote messages seen"));
  CHECK(!hp.master_active() && hp.master_state() == MS::REFUSED_REMOTE_PRESENT);
  advance(hp, 2000);  // the bus has now been quiet for longer than the check
  CHECK(hp.start_master());
  advance(hp, 2000);
  CHECK(log_has("polling started"));
  hp.stop_master();
  advance(hp, 10);
  CHECK(!hp.master_active());

  // 2b. Stray bytes from the unit itself (no remote) do not block the start.
  reset(serial, hp);
  hp.set_dry_run(true);
  serial.rx.push_back(0xFF);
  serial.rx.push_back(0xFF);
  CHECK(hp.start_master());
  advance(hp, 100);
  serial.rx.push_back(0xFF);
  advance(hp, 1700);
  CHECK(log_has("polling started"));
  CHECK(!log_has("refused"));
  hp.stop_master();
  advance(hp, 10);

  // 3. Live: pulse train equals the encoded message, pin handed back to the UART after each message.
  reset(serial, hp);
  hp.set_dry_run(false);
  CHECK(hp.start_master());
  advance(hp, 1600);
  CHECK(log_has("polling started"));
  g_edges.clear();
  g_uart_set_pin_calls = 0;
  advance(hp, 300);  // the first poll goes out within one poll period
  CHECK(g_uart_set_pin_calls >= 1);
  CHECK(!g_edges.empty());
  uint8_t cells[kSettingCells];
  build_poll(false, false, cells);
  std::vector<Edge> expected;
  int last = 1;
  for (size_t i = 0; i < kPollCells; i++) {
    int lvl = cells[i] ? 0 : 1;
    if (lvl != last) {
      expected.push_back({static_cast<int64_t>(i * kRemoteCellTenthsUs / 10), lvl});
      last = lvl;
    }
  }
  // g_edges starts with the initial idle-high write, then one entry per line change, then the release.
  bool timing_ok = g_edges.size() >= expected.size() + 1;
  if (timing_ok) {
    int64_t base = g_edges[1].t;
    for (size_t i = 0; i < expected.size() && timing_ok; i++) {
      const Edge &e = g_edges[1 + i];
      if (e.level != expected[i].level || std::llabs((e.t - base) - (expected[i].t - expected[0].t)) > 10) {
        timing_ok = false;
        std::printf("edge %zu: level %d at %lld, expected level %d at %lld\n", i, e.level,
                    static_cast<long long>(e.t - base), expected[i].level,
                    static_cast<long long>(expected[i].t - expected[0].t));
      }
    }
  }
  CHECK(timing_ok);

  // 3b. Live, continuous: our own transmissions come back on RX; they must neither count as a wired remote nor
  // reach the state decoder, over a whole change and power-off.
  size_t uart_calls = g_uart_set_pin_calls;
  g_log.clear();
  advance(hp, 2000);
  for (int i = 0; i < 16000; i++) {
    advance(hp, 1);
    if (static_cast<size_t>(g_uart_set_pin_calls) != uart_calls) {
      uart_calls = g_uart_set_pin_calls;
      for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
    }
    if (i == 1000) CHECK(hp.submit_target(heat));
    if (i == 9000) CHECK(hp.submit_target(off));
  }
  CHECK(hp.master_active() && hp.master_state() == MS::RUNNING);
  CHECK(!log_has("giving it back"));
  CHECK(log_count("sent setting (round 1)") == 1);
  CHECK(log_count("sent setting (round 2)") == 1);
  CHECK(log_count("sent power-off") == 1);
  CHECK(!hp.observed().on);
  hp.stop_master();
  advance(hp, 50);
  CHECK(!hp.master_active());
  CHECK(!g_edges.empty() && g_edges.back().level == 1 && g_tx_level == 1);  // line released high

  // 4. Read-back mismatch (no loopback, or another device pulling the line) stops it and releases the line.
  reset(serial, hp);
  hp.set_dry_run(false);
  CHECK(hp.start_master());
  advance(hp, 1600);
  g_loopback = false;
  g_log.clear();
  advance(hp, 600);
  CHECK(log_has("aborted"));
  CHECK(!hp.master_active() && hp.master_state() == MS::STOPPED_TX_FAILED);
  CHECK(g_tx_level == 1);

  // 4b. A bus held low (no idle-high, as in test.sr) is refused before anything is sent.
  reset(serial, hp);
  hp.set_dry_run(false);
  g_rx_forced_low = true;
  CHECK(hp.start_master());
  advance(hp, 1700);
  CHECK(log_has("held low with no traffic"));
  CHECK(!hp.master_active() && hp.master_state() == MS::REFUSED_BUS_LOW);
  CHECK(g_gpio_sets == 0);

  // 5. A wired remote showing up while running makes the master controller give the bus back.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  advance(hp, 4000);
  CHECK(hp.master_active());
  for (int n = 0; n < 3; n++) {
    for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
    advance(hp, 250);
  }
  CHECK(log_has("giving it back"));
  CHECK(!hp.master_active() && hp.master_state() == MS::STOPPED_REMOTE_PRESENT);

  // 6. A stop before the bus task handled the start leaves it off, and the unit run flag follows the unit's replies.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.unit_run_state() == -1);
  CHECK(hp.start_master());
  hp.stop_master();
  advance(hp, 20);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(std::string(Bus::master_state_text(MS::REFUSED_BUS_LOW)).find("held low") != std::string::npos);
  CHECK(hp.start_master());
  advance(hp, 1600);
  for (uint8_t b : {0x80, 0xE7, 0xEC, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit reply 7F 18 13 48 80, idle
  advance(hp, 100);
  CHECK(hp.unit_run_state() == 0);
  for (uint8_t b : {0x80, 0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // same with the run flag 17
  advance(hp, 100);
  CHECK(hp.unit_run_state() == 1);
  hp.stop_master();
  advance(hp, 10);

  // 6b. The unit's echo of a setting is checked: echoed -> acknowledged, no echo -> not acknowledged.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  advance(hp, 4000);
  CHECK(hp.last_ack() == 0);
  Target t1;
  t1.on = true;
  t1.mode = Mode::COOL;
  t1.temp = 24;
  CHECK(hp.submit_target(t1));
  for (int i = 0; i < 3000 && !log_has("would send setting (round 2)"); i++) advance(hp, 1);
  CHECK(log_has("would send setting (round 2)"));
  advance(hp, 100);
  for (uint8_t b : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // 7F 12 13 56 02 5F 7E
  advance(hp, 600);
  CHECK(hp.last_ack() == 1 && log_has("acknowledged the setting"));
  Target t2 = t1;
  t2.temp = 25;
  g_log.clear();
  CHECK(hp.submit_target(t2));
  advance(hp, 2700);  // sent, and no echo arrives
  CHECK(hp.last_ack() == 2 && log_has("did NOT acknowledge the setting"));
  hp.stop_master();
  advance(hp, 10);

  // 7. Monitor mode (master controller off, real remote connected): messages are parsed into a best-effort state.
  static Bus hp2;
  reset(serial, hp2);
  CHECK(hp2.last_message_ms() == 0 && !hp2.observed().valid);
  for (uint8_t b : {0x80, 0xDD, 0xFC, 0x43, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // 7F 22 03 BC 02 5F 7E: COOL low 23
  advance(hp2, 50);
  CHECK(hp2.last_message_ms() != 0);
  Observed mon = hp2.observed();
  CHECK(mon.valid && mon.on && mon.mode == fuji_passive::kModeCool && mon.fan == fuji_passive::kFanLow &&
        mon.temp_valid && mon.temp == 23);
  uint32_t rev = hp2.observed_revision();
  for (uint8_t b : {0xDB, 0xFC, 0x80}) serial.rx.push_back(b);  // 7F 24 03: power off
  advance(hp2, 50);
  CHECK(hp2.observed().valid && !hp2.observed().on && hp2.observed_revision() != rev);
  CHECK(Observed::unpack(mon.pack()) == mon);

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
