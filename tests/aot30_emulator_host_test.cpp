// Exercises the master-controller code in FujiHeatPump.cpp on a desktop compiler with fake time, GPIO and UART.
//   g++ -std=c++17 -w -DESP32 -Itests/stubs -Icomponents/fujitsu_climate tests/aot30_emulator_host_test.cpp \
//       components/fujitsu_climate/FujiHeatPump.cpp -o /tmp/aot30_emulator_host_test && /tmp/aot30_emulator_host_test
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

#include "FujiHeatPump.h"

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

static void advance(FujiHeatPump &hp, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    g_now_us += 1000;
    hp.aot30MasterPump();
  }
}

static void reset(HardwareSerial &serial, FujiHeatPump &hp) {
  g_log.clear();
  g_edges.clear();
  g_loopback = true;
  g_rx_forced_low = false;
  g_tx_level = 1;
  g_uart_set_pin_calls = g_matrix_calls = g_gpio_sets = 0;
  serial.rx.clear();
  hp.connect(&serial, true, 16, 17);
}

int main() {
  using namespace fuji_aot30;
  using MS = FujiHeatPump::MasterState;
  static HardwareSerial serial;
  static FujiHeatPump hp;
  g_now_us = 10 * 1000 * 1000;
  const uint8_t kRemotePoll[] = {0x80, 0xD7, 0xEC, 0xF7, 0x80};  // 7F 28 13 08 plus the next start byte

  // 1. Dry run: starts after 1.5 s of silence, polls, then runs and waits for a command.
  reset(serial, hp);
  hp.aot30SetDryRun(true);
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::OFF);
  CHECK(!hp.aot30SubmitTarget(Target()));  // not running yet
  CHECK(hp.aot30StartMaster());
  CHECK(!hp.aot30StartMaster());  // already running
  CHECK(hp.aot30MasterActive() && hp.aot30MasterState() == MS::CHECKING_BUS);
  advance(hp, 1000);
  CHECK(!log_has("polling started"));
  advance(hp, 700);
  CHECK(log_has("polling started"));
  CHECK(hp.aot30MasterState() == MS::CHECKING_BUS);
  advance(hp, 2000);
  CHECK(log_has("running, polling continuously"));
  CHECK(hp.aot30MasterState() == MS::RUNNING);
  CHECK(hp.getObservedControllerState().valid && !hp.getObservedControllerState().on);
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
  CHECK(hp.aot30SubmitTarget(heat));
  advance(hp, 100);
  FujiObservedControllerState st = hp.getObservedControllerState();
  CHECK(st.on && st.mode == static_cast<byte>(FujiMode::HEAT) && st.temperature == 20 && st.temperature_valid);
  CHECK(st.eco_valid && st.eco && st.fan_valid && st.fan_mode == static_cast<byte>(FujiFanMode::FAN_AUTO));
  advance(hp, 3000);
  CHECK(log_count("DRY RUN would send setting (round 1)") == 1);
  CHECK(log_count("DRY RUN would send setting (round 2)") == 1);
  CHECK(log_count("DRY RUN would send poll (pending)") == 2);
  g_log.clear();
  advance(hp, 5000);
  CHECK(log_count("would send setting") == 0 && log_count("would send poll") >= 15);
  CHECK(hp.aot30SubmitTarget(heat));  // same state again: nothing is sent
  advance(hp, 1000);
  CHECK(log_count("would send setting") == 0);
  Target off = heat;
  off.on = false;
  CHECK(hp.aot30SubmitTarget(off));
  advance(hp, 1500);
  CHECK(log_count("DRY RUN would send power-off") == 1);
  CHECK(!hp.getObservedControllerState().on);
  CHECK(hp.aot30SubmitTarget(off));  // already off: nothing sent
  advance(hp, 1500);
  CHECK(log_count("would send power-off") == 1);
  hp.aot30StopMaster();
  advance(hp, 50);
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::OFF);
  CHECK(log_has("Off. The bus is released"));
  CHECK(!hp.aot30SubmitTarget(heat));

  // 2. Refuses to start when a wired remote was heard just before or during the silence check.
  reset(serial, hp);
  hp.aot30SetDryRun(true);
  CHECK(hp.aot30StartMaster());
  for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
  advance(hp, 300);
  CHECK(log_has("wired remote messages seen"));
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::REFUSED_REMOTE_PRESENT);
  advance(hp, 2000);  // the bus has now been quiet for longer than the check
  CHECK(hp.aot30StartMaster());
  advance(hp, 2000);
  CHECK(log_has("polling started"));
  hp.aot30StopMaster();
  advance(hp, 10);
  CHECK(!hp.aot30MasterActive());

  // 2b. Stray bytes from the unit itself (no remote) do not block the start.
  reset(serial, hp);
  hp.aot30SetDryRun(true);
  serial.rx.push_back(0xFF);
  serial.rx.push_back(0xFF);
  CHECK(hp.aot30StartMaster());
  advance(hp, 100);
  serial.rx.push_back(0xFF);
  advance(hp, 1700);
  CHECK(log_has("polling started"));
  CHECK(!log_has("refused"));
  hp.aot30StopMaster();
  advance(hp, 10);

  // 3. Live: pulse train equals the encoded message, pin handed back to the UART after each message.
  reset(serial, hp);
  hp.aot30SetDryRun(false);
  CHECK(hp.aot30StartMaster());
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
    if (i == 1000) CHECK(hp.aot30SubmitTarget(heat));
    if (i == 9000) CHECK(hp.aot30SubmitTarget(off));
  }
  CHECK(hp.aot30MasterActive() && hp.aot30MasterState() == MS::RUNNING);
  CHECK(!log_has("giving it back"));
  CHECK(log_count("sent setting (round 1)") == 1);
  CHECK(log_count("sent setting (round 2)") == 1);
  CHECK(log_count("sent power-off") == 1);
  CHECK(!hp.getObservedControllerState().on);
  hp.aot30StopMaster();
  advance(hp, 50);
  CHECK(!hp.aot30MasterActive());
  CHECK(!g_edges.empty() && g_edges.back().level == 1 && g_tx_level == 1);  // line released high

  // 4. Read-back mismatch (no loopback, or another device pulling the line) stops it and releases the line.
  reset(serial, hp);
  hp.aot30SetDryRun(false);
  CHECK(hp.aot30StartMaster());
  advance(hp, 1600);
  g_loopback = false;
  g_log.clear();
  advance(hp, 600);
  CHECK(log_has("aborted"));
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::STOPPED_TX_FAILED);
  CHECK(g_tx_level == 1);

  // 4b. A bus held low (no idle-high, as in test.sr) is refused before anything is sent.
  reset(serial, hp);
  hp.aot30SetDryRun(false);
  g_rx_forced_low = true;
  CHECK(hp.aot30StartMaster());
  advance(hp, 1700);
  CHECK(log_has("held low with no traffic"));
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::REFUSED_BUS_LOW);
  CHECK(g_gpio_sets == 0);

  // 5. A wired remote showing up while running makes the master controller give the bus back.
  reset(serial, hp);
  hp.aot30SetDryRun(true);
  CHECK(hp.aot30StartMaster());
  advance(hp, 4000);
  CHECK(hp.aot30MasterActive());
  for (int n = 0; n < 3; n++) {
    for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
    advance(hp, 250);
  }
  CHECK(log_has("giving it back"));
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::STOPPED_REMOTE_PRESENT);

  // 6. A stop before the bus task handled the start leaves it off, and the unit run flag follows the unit's replies.
  reset(serial, hp);
  hp.aot30SetDryRun(true);
  CHECK(hp.aot30UnitRunState() == -1);
  CHECK(hp.aot30StartMaster());
  hp.aot30StopMaster();
  advance(hp, 20);
  CHECK(!hp.aot30MasterActive() && hp.aot30MasterState() == MS::OFF);
  CHECK(std::string(FujiHeatPump::aot30MasterStateText(MS::REFUSED_BUS_LOW)).find("held low") != std::string::npos);
  CHECK(hp.aot30StartMaster());
  advance(hp, 1600);
  for (uint8_t b : {0x80, 0xE7, 0xEC, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit reply 7F 18 13 48 80, idle
  advance(hp, 100);
  CHECK(hp.aot30UnitRunState() == 0);
  for (uint8_t b : {0x80, 0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // same with the run flag 17
  advance(hp, 100);
  CHECK(hp.aot30UnitRunState() == 1);
  hp.aot30StopMaster();
  advance(hp, 10);

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
