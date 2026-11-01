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
    hp.aot30EmulatorPump();
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
  static HardwareSerial serial;
  static FujiHeatPump hp;
  g_now_us = 10 * 1000 * 1000;

  // 1. Dry run: the test starts after 1.5 s of silence, polls, sends HEAT 20, polls on, finishes by itself.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  CHECK(!hp.aot30EmulatorActive());
  CHECK(hp.aot30RunTest());
  CHECK(!hp.aot30RunTest());  // already running
  CHECK(hp.aot30EmulatorActive());
  advance(hp, 1000);
  CHECK(!log_has("polling started"));
  advance(hp, 700);
  CHECK(log_has("polling started"));
  advance(hp, 2000);
  CHECK(log_has("sending HEAT 20, fan auto, ECO off"));
  advance(hp, 2500);
  CHECK(log_count("DRY RUN would send setting (round 1)") == 1);
  CHECK(log_count("DRY RUN would send setting (round 2)") == 1);
  CHECK(log_count("DRY RUN would send poll (pending)") == 2);
  CHECK(hp.aot30EmulatorActive());
  advance(hp, 12000);
  CHECK(log_has("test finished"));
  CHECK(!hp.aot30EmulatorActive());
  CHECK(g_gpio_sets == 0 && g_matrix_calls == 0);

  // 2. Refuses to start when the bus was busy just before or during the silence check.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  CHECK(hp.aot30RunTest());
  // A real remote poll `7F 28 13 08` followed by the unit's next start byte, right after the button press.
  for (uint8_t b : {0x80, 0xD7, 0xEC, 0xF7, 0x80}) serial.rx.push_back(b);
  advance(hp, 300);
  CHECK(log_has("wired remote messages seen"));
  CHECK(!hp.aot30EmulatorActive());
  advance(hp, 2000);  // the bus has now been quiet for longer than the check
  CHECK(hp.aot30RunTest());  // can be started again
  advance(hp, 2000);
  CHECK(log_has("polling started"));
  advance(hp, 20000);
  CHECK(!hp.aot30EmulatorActive());

  // 2b. Stray bytes from the unit itself (no remote) do not block the test.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  serial.rx.push_back(0xFF);
  serial.rx.push_back(0xFF);
  CHECK(hp.aot30RunTest());
  advance(hp, 100);
  serial.rx.push_back(0xFF);
  advance(hp, 1700);
  CHECK(log_has("polling started"));
  CHECK(!log_has("refused"));
  advance(hp, 20000);

  // 3. Live: pulse train equals the encoded message, pin handed back to the UART after each message.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  CHECK(hp.aot30RunTest());
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
  g_log.clear();
  advance(hp, 17000);  // the rest of the test: lead polls, the two-round change, tail polls
  CHECK(log_count("sent setting (round 1)") == 1);
  CHECK(log_count("sent setting (round 2)") == 1);
  CHECK(log_has("test finished"));
  CHECK(!hp.aot30EmulatorActive());
  CHECK(!g_edges.empty() && g_edges.back().level == 1);  // line released high
  CHECK(g_tx_level == 1);

  // 4. Read-back mismatch (no loopback, or another device pulling the line) aborts and releases.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  CHECK(hp.aot30RunTest());
  advance(hp, 1600);
  g_loopback = false;
  g_log.clear();
  advance(hp, 600);
  CHECK(log_has("aborted"));
  CHECK(!hp.aot30EmulatorActive());
  CHECK(g_tx_level == 1);

  // 4b. A bus held low (no idle-high pull-up, as in test.sr) is refused before anything is sent.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  g_rx_forced_low = true;
  CHECK(hp.aot30RunTest());
  advance(hp, 1700);
  CHECK(log_has("held low with no traffic"));
  CHECK(!hp.aot30EmulatorActive());
  CHECK(g_gpio_sets == 0);

  // 5. A finished or aborted test can be started again.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  CHECK(hp.aot30RunTest());
  advance(hp, 1600);
  CHECK(log_has("polling started"));
  advance(hp, 20000);
  CHECK(!hp.aot30EmulatorActive());

  // 6. Continuous emulator: polls forever, turns a submitted target into scripted messages, publishes that state.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  CHECK(!hp.aot30SubmitTarget(Target()));  // not running yet
  CHECK(hp.aot30StartEmulator());
  CHECK(!hp.aot30StartEmulator());
  CHECK(hp.aot30ContinuousActive());
  advance(hp, 3700);
  CHECK(log_has("running, polling continuously"));
  CHECK(hp.getObservedControllerState().valid && !hp.getObservedControllerState().on);
  // Our own poll comes back on RX; it must neither count as a wired remote nor reach the state decoder.
  size_t uart_calls = g_uart_set_pin_calls;
  for (int i = 0; i < 20000; i++) {
    advance(hp, 1);
    if (static_cast<size_t>(g_uart_set_pin_calls) != uart_calls) {
      uart_calls = g_uart_set_pin_calls;
      for (uint8_t b : {0x80, 0xD7, 0xEC, 0xF7, 0x80}) serial.rx.push_back(b);
    }
    if (i == 2000) {
      Target t;
      t.on = true;
      t.mode = Mode::HEAT;
      t.temp = 20;
      CHECK(hp.aot30SubmitTarget(t));
    }
    if (i == 12000) {
      Target t = hp.aot30LastTarget();
      t.on = false;
      CHECK(hp.aot30SubmitTarget(t));
    }
    if (i == 2100) {
      FujiObservedControllerState st = hp.getObservedControllerState();
      CHECK(st.on && st.mode == static_cast<byte>(FujiMode::HEAT) && st.temperature == 20 && st.temperature_valid);
    }
  }
  CHECK(hp.aot30ContinuousActive());
  CHECK(!log_has("giving it back"));
  CHECK(log_count("sent setting (round 1)") == 1);
  CHECK(log_count("sent setting (round 2)") == 1);
  CHECK(log_count("sent power-off") == 1);
  CHECK(!hp.getObservedControllerState().on);
  // No target resubmitted: nothing more is sent, polling goes on (about 4 polls a second).
  g_log.clear();
  advance(hp, 5000);
  CHECK(log_count("sent ") == 0);
  CHECK(hp.aot30ContinuousActive());
  // Submitting the state it is already in sends nothing.
  Target off = hp.aot30LastTarget();
  CHECK(hp.aot30SubmitTarget(off));
  advance(hp, 500);
  CHECK(log_count("sent ") == 0);
  // Stop releases the bus.
  hp.aot30StopEmulator();
  advance(hp, 50);
  CHECK(!hp.aot30ContinuousActive() && !hp.aot30EmulatorActive());
  CHECK(log_has("emulator stopped"));
  CHECK(g_tx_level == 1);

  // 6b. A wired remote showing up while emulating makes the emulator give the bus back.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  CHECK(hp.aot30StartEmulator());
  advance(hp, 4000);
  CHECK(hp.aot30ContinuousActive());
  for (int n = 0; n < 3; n++) {
    for (uint8_t b : {0x80, 0xD7, 0xEC, 0xF7, 0x80}) serial.rx.push_back(b);
    advance(hp, 250);
  }
  CHECK(log_has("giving it back"));
  CHECK(!hp.aot30EmulatorActive());

  // 6c. A remote already on the bus refuses the continuous start as well.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(true);
  CHECK(hp.aot30StartEmulator());
  for (uint8_t b : {0x80, 0xD7, 0xEC, 0xF7, 0x80}) serial.rx.push_back(b);
  advance(hp, 300);
  CHECK(log_has("wired remote messages seen"));
  CHECK(!hp.aot30EmulatorActive());

  // 7. Setting cell-time experiment: shorter cells, and a report on whether the unit echoed.
  reset(serial, hp);
  hp.aot30EmulatorSetDryRun(false);
  hp.aot30SetSettingCellPercent(50);
  CHECK(hp.aot30RunTest());
  advance(hp, 1600);
  g_log.clear();
  g_edges.clear();
  advance(hp, 4500);
  CHECK(log_has("setting at 50% cell time was NOT echoed"));
  int half_width_pulses = 0;  // 990 us = half of 1980.8 us
  for (size_t i = 1; i + 1 < g_edges.size(); i++) {
    const int64_t w = g_edges[i + 1].t - g_edges[i].t;
    if (w > 900 && w < 1100) half_width_pulses++;
  }
  CHECK(half_width_pulses >= 10);
  advance(hp, 15000);
  hp.aot30SetSettingCellPercent(100);

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
