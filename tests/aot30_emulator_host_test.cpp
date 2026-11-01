// Exercises the remote-emulation code in FujiHeatPump.cpp on a desktop compiler with fake time, GPIO and UART.
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
int gpio_get_level(int) { return g_loopback ? g_tx_level : 1; }
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
    hp.aot30EmulatorPump();
  }
}

static void reset(HardwareSerial &serial, FujiHeatPump &hp) {
  g_log.clear();
  g_edges.clear();
  g_loopback = true;
  g_tx_level = 1;
  g_uart_set_pin_calls = g_matrix_calls = g_gpio_sets = 0;
  serial.rx.clear();
  hp.connect(&serial, true, 16, 17);
}

int main() {
  using namespace fuji_aot30;
  static HardwareSerial serial;
  static FujiHeatPump hp;
  g_now_us = 10 * 1000 * 1000;

  // 1. Dry run: starts after 1.5 s of silence, logs the scripted messages, drives nothing.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  hp.aot30EmulatorSetEnabled(true);
  CHECK(hp.aot30EmulatorActive());
  advance(hp, 1000);
  CHECK(!log_has("polling started"));
  advance(hp, 700);
  CHECK(log_has("polling started"));
  CHECK(hp.aot30EmulatorQueueSetting(Mode::HEAT, Fan::FAN_AUTO, 20, false));
  advance(hp, 2500);
  CHECK(log_has("setting queued (mode 2, fan 5, 20 C, eco 0)"));
  CHECK(log_count("DRY RUN would send setting (round 1)") == 1);
  CHECK(log_count("DRY RUN would send setting (round 2)") == 1);
  CHECK(log_count("DRY RUN would send poll (pending)") == 2);
  CHECK(g_gpio_sets == 0 && g_matrix_calls == 0);
  CHECK(!hp.aot30EmulatorQueueSetting(Mode::COOL, Fan::FAN_AUTO, 17, false));  // below the COOL minimum
  CHECK(!hp.aot30EmulatorQueueSetting(Mode::HEAT, Fan::FAN_AUTO, 31, false));

  // 2. Refuses to start when the bus was busy just before.
  reset(serial, hp);
  hp.aot30EmulatorSetEnabled(false);
  serial.rx.push_back(0x80);
  advance(hp, 1);  // not active: pump not called here, so emulate the bus task's own receive path
  hp.aot30EmulatorSetEnabled(true);
  serial.rx.push_back(0x80);  // traffic after the start request
  advance(hp, 300);
  CHECK(log_has("refused, bus traffic seen"));
  CHECK(!hp.aot30EmulatorActive());

  // 3. Live: pulse train equals the encoded message, pin handed back to the UART after each message.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  hp.aot30EmulatorSetEnabled(true);
  advance(hp, 1700);
  CHECK(log_has("polling started"));
  g_edges.clear();
  g_uart_set_pin_calls = 0;
  // The next poll slot is within 247 ms; capture the first transmission only.
  advance(hp, 300);
  CHECK(g_uart_set_pin_calls >= 1);
  CHECK(!g_edges.empty());
  uint8_t cells[kSettingCells];
  build_poll(false, false, cells);
  // Rebuild the expected line-level edge list from the cells and compare timing within 10 us.
  std::vector<Edge> expected;
  int last = 1;
  for (size_t i = 0; i < kPollCells; i++) {
    int lvl = cells[i] ? 0 : 1;
    if (lvl != last) {
      expected.push_back({static_cast<int64_t>(i * kRemoteCellTenthsUs / 10), lvl});
      last = lvl;
    }
  }
  // g_edges includes the initial high set and the final release; take the ones in between.
  CHECK(g_edges.size() >= expected.size() + 1);
  bool timing_ok = g_edges.size() >= expected.size() + 1;
  if (timing_ok) {
    // first recorded set is the initial idle-high write before cell 0; the cell-0 edge follows it
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
  CHECK(!g_edges.empty() && g_edges.back().level == 1);  // line released high

  // A full change goes out as setting, 0A poll, setting, 0A poll.
  CHECK(hp.aot30EmulatorQueueSetting(Mode::HEAT, Fan::FAN_AUTO, 20, false));
  g_log.clear();
  advance(hp, 2500);
  CHECK(log_count("sent setting (round 1)") == 1 && log_count("sent setting (round 2)") == 1);
  CHECK(hp.aot30EmulatorQueueOff());
  g_log.clear();
  advance(hp, 1500);
  CHECK(log_count("sent power-off") == 1);
  CHECK(hp.aot30EmulatorActive());

  // 4. Read-back mismatch (no loopback, or another device pulling the line) aborts and releases.
  g_loopback = false;
  g_log.clear();
  advance(hp, 600);
  CHECK(log_has("aborted"));
  CHECK(!hp.aot30EmulatorActive());
  CHECK(g_tx_level == 1);

  // 5. Disabling stops everything.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  hp.aot30EmulatorSetEnabled(true);
  advance(hp, 1600);
  hp.aot30EmulatorSetEnabled(false);
  CHECK(!hp.aot30EmulatorActive());
  CHECK(!hp.aot30EmulatorQueueOff());

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
