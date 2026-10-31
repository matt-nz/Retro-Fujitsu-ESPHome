// Desktop test for components/fujitsu_climate/FujiPassiveStream.h.
//
// Build and run from the repository root:
//   g++ -std=c++17 -Wall -Wextra -Icomponents/fujitsu_climate tests/passive_stream_test.cpp -o /tmp/passive_stream_test
//   /tmp/passive_stream_test captures/matt-captures

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "FujiPassiveStream.h"

using namespace fuji_passive;

static int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

// kGap in a byte vector marks a discontinuity (the capture files are concatenated sessions).
constexpr int kGap = -1;

// A gap resets the parser and is reported as a zero-length UNKNOWN message.
static std::vector<Message> parse(const std::vector<int> &bytes) {
  Parser parser;
  std::vector<Message> out;
  Message m;
  for (int b : bytes) {
    if (b == kGap) {
      parser.reset();
      out.push_back(Message());
    } else if (parser.feed(static_cast<uint8_t>(b), &m)) {
      out.push_back(m);
    }
  }
  return out;
}

static void test_synthetic() {
  // Trailing 7F completes the final message.
  auto msgs = parse({0x7F, 0x28, 0x17, 0x08, 0x7F, 0x18, 0x17, 0x48, 0x80, 0x7F});
  CHECK(msgs.size() == 2);
  CHECK(msgs[0].kind == Kind::STATUS && msgs[0].running && msgs[0].direction == Direction::REMOTE_TO_UNIT);
  CHECK(msgs[1].kind == Kind::STATUS && msgs[1].running && msgs[1].direction == Direction::UNIT_TO_REMOTE);

  msgs = parse({0x7F, 0x28, 0x13, 0x0A, 0x7F});
  CHECK(msgs.size() == 1 && msgs[0].kind == Kind::STATUS && !msgs[0].running);

  msgs = parse({0x7F, 0x22, 0x03, 0xF2, 0x02, 0x5F, 0x7E, 0x7F, 0x12, 0x13, 0xF2, 0x02, 0x5F, 0x7E, 0x7F});
  CHECK(msgs.size() == 2);
  CHECK(msgs[0].kind == Kind::SETTING && msgs[0].value == 0xF2 && msgs[0].direction == Direction::REMOTE_TO_UNIT);
  CHECK(msgs[1].kind == Kind::SETTING && msgs[1].value == 0xF2 && msgs[1].direction == Direction::UNIT_TO_REMOTE);

  msgs = parse({0x7F, 0x24, 0x03, 0x7F, 0x14, 0x03, 0x7F});
  CHECK(msgs.size() == 2);
  CHECK(msgs[0].kind == Kind::POWER_OFF && msgs[1].kind == Kind::POWER_OFF);

  // Bytes before the first start byte are ignored.
  msgs = parse({0x48, 0x80, 0x7F, 0x24, 0x03, 0x7F});
  CHECK(msgs.size() == 1 && msgs[0].kind == Kind::POWER_OFF);

  // Truncated, corrupted and wrongly shaped messages are never interpreted.
  msgs = parse({0x7F, 0x12, 0x01, 0x7F, 0x22, 0x03, 0xF2, 0x02, 0x5F, 0x7F, 0x24, 0x03, 0x00, 0x7F, 0x28, 0x17, 0x7F,
                0x28, 0x15, 0x08, 0x7F, 0x38, 0x13, 0x08, 0x7F});
  CHECK(msgs.size() == 6);
  for (const auto &m : msgs) {
    CHECK(m.kind == Kind::UNKNOWN);
  }

  // A runaway message is dropped and the parser resynchronises on the next start byte.
  std::vector<int> runaway = {0x7F};
  for (int i = 0; i < 40; i++) {
    runaway.push_back(0x55);
  }
  runaway.insert(runaway.end(), {0x7F, 0x24, 0x03, 0x7F});
  msgs = parse(runaway);
  CHECK(msgs.size() == 1 && msgs[0].kind == Kind::POWER_OFF);
}

static void test_decode_table() {
  Setting s;
  CHECK(decode_setting(0xF2, &s) && s.mode == kModeHeat && s.fan_valid && s.fan == kFanLow && s.temperature == 23);
  CHECK(decode_setting(0xF6, &s) && s.mode == kModeHeat && s.fan == kFanAuto);
  CHECK(decode_setting(0x09, &s) && s.mode == kModeFan && s.fan == kFanLow && !s.temperature_valid);
  CHECK(decode_setting(0xBC, &s) && s.mode == kModeCool && s.fan == kFanLow && s.temperature == 23);
  CHECK(decode_setting(0x79, &s) && s.mode == kModeAuto && s.fan == kFanLow && s.temperature == 23);
  CHECK(decode_setting(0xC4, &s) && s.mode == kModeCool && !s.fan_valid && s.temperature == 24);
  for (uint8_t temperature = 18; temperature <= 30; temperature++) {
    const uint8_t value = static_cast<uint8_t>(0x95 + (temperature - 18) * 0x08);
    CHECK(decode_setting(value, &s) && s.mode == kModeCool && s.fan_valid && s.fan == kFanAuto &&
          s.temperature_valid && s.temperature == temperature);
  }
  CHECK(!decode_setting(0x94, &s) && !decode_setting(0xFD, &s));
  CHECK(decode_setting(0x52, &s) && s.mode == kModeHeat && !s.fan_valid && s.temperature == 18);
  CHECK(decode_setting(0x72, &s) && s.temperature == 19);
  CHECK(decode_setting(0x92, &s) && s.temperature == 20);
  CHECK(decode_setting(0xB2, &s) && s.temperature == 21);
  CHECK(decode_setting(0xD2, &s) && s.temperature == 22);
  CHECK(!decode_setting(0x12, &s));  // 24 C HEAT has not been captured
  CHECK(!decode_setting(0x00, &s) && !decode_setting(0x7F, &s) && !decode_setting(0xFF, &s));
}

// Rebuilds the inverted byte stream from the "RX raw (n/8, before XOR): .." log lines.
static double log_time_seconds(const std::string &line) {
  int h = 0, m = 0;
  double sec = 0;
  size_t open = line.find('[');
  if (open == std::string::npos || std::sscanf(line.c_str() + open, "[%d:%d:%lf]", &h, &m, &sec) != 3) {
    return -1;
  }
  return h * 3600.0 + m * 60.0 + sec;
}

static std::vector<int> load_stream(const std::string &path, bool *ok) {
  std::ifstream in(path);
  *ok = in.good();
  std::vector<int> bytes;
  std::string line;
  double previous = -1;
  const std::string marker = "before XOR):";
  while (std::getline(in, line)) {
    size_t pos = line.find("RX raw (");
    if (pos == std::string::npos) {
      continue;
    }
    pos = line.find(marker, pos);
    if (pos == std::string::npos) {
      continue;
    }
    pos += marker.size();
    const double now = log_time_seconds(line);
    if (previous >= 0 && now - previous > 1.0) {
      bytes.push_back(kGap);
    }
    previous = now;
    for (;;) {
      while (pos < line.size() && line[pos] == ' ') pos++;
      if (pos + 2 > line.size()) break;
      char hex[3] = {line[pos], line[pos + 1], 0};
      bytes.push_back(static_cast<int>(std::strtol(hex, nullptr, 16)) ^ 0xFF);
      pos += 2;
    }
  }
  return bytes;
}

// Remote-to-unit messages only, so each button press is counted once.
static std::vector<uint8_t> setting_values(const std::vector<Message> &msgs) {
  std::vector<uint8_t> v;
  for (const auto &m : msgs) {
    if (m.kind == Kind::SETTING && m.direction == Direction::REMOTE_TO_UNIT && (v.empty() || v.back() != m.value)) {
      v.push_back(m.value);
    }
  }
  return v;
}

static size_t count_kind(const std::vector<Message> &msgs, Kind k, Direction d) {
  size_t n = 0;
  for (const auto &m : msgs) {
    if (m.kind == k && m.direction == d) n++;
  }
  return n;
}

static void test_captures(const std::string &dir) {
  const char *files[] = {"auto-heat.log", "cold.log", "fan_changes.log",     "heat_20_22_18.log", "hot-fan-cold-auto.log",
                         "on_off.log",    "raw.log",  "running_standby.log", "startup.log", "tx-enabled-new-build.log",
                         "cold-test.log"};
  size_t total_unknown = 0, total_messages = 0;

  for (const char *name : files) {
    bool ok = false;
    auto stream = load_stream(dir + "/" + name, &ok);
    CHECK(ok);
    auto msgs = parse(stream);
    std::printf("%-24s %5zu bytes %4zu msgs\n", name, stream.size(), msgs.size());
    CHECK(!msgs.empty());

    // Every setting byte seen on the bus has a mapping; nothing is ignored as unmapped.
    for (const auto &m : msgs) {
      if (m.kind == Kind::SETTING) {
        Setting s;
        if (!decode_setting(m.value, &s)) {
          std::printf("  unmapped setting value 0x%02X in %s\n", m.value, name);
        }
        CHECK(decode_setting(m.value, &s));
      }
      if (m.kind == Kind::UNKNOWN && m.len > 0) {
        total_unknown++;
      }
      total_messages += m.len > 0 ? 1 : 0;
    }

    // The unit's running flag only rises after a setting message and only falls after a
    // power-off message.
    bool have_state = false, running = false, setting_since_edge = false, off_since_edge = false;
    for (const auto &m : msgs) {
      if (m.len == 0) {
        have_state = false;  // discontinuity: the unit may have changed state unobserved
        setting_since_edge = off_since_edge = false;
        continue;
      }
      if (m.kind == Kind::SETTING) setting_since_edge = true;
      if (m.kind == Kind::POWER_OFF) off_since_edge = true;
      if (m.kind != Kind::STATUS) continue;
      if (have_state && m.running != running) {
        if (m.running) {
          CHECK(setting_since_edge);
        } else {
          CHECK(off_since_edge);
        }
        setting_since_edge = false;
        off_since_edge = false;
      }
      have_state = true;
      running = m.running;
    }
  }

  // Bytes are occasionally lost or truncated at log boundaries; corrupt messages must stay rare.
  std::printf("unknown/corrupt messages across all captures: %zu of %zu\n", total_unknown, total_messages);
  CHECK(total_unknown * 100 < total_messages);

  bool ok = false;
  auto fan = parse(load_stream(dir + "/fan_changes.log", &ok));
  CHECK((setting_values(fan) == std::vector<uint8_t>{0xF6, 0xE6, 0xEA, 0xF2}));

  auto startup = parse(load_stream(dir + "/startup.log", &ok));
  CHECK((setting_values(startup) == std::vector<uint8_t>{0xF2}));
  CHECK(count_kind(startup, Kind::POWER_OFF, Direction::REMOTE_TO_UNIT) == 1);
  CHECK(count_kind(startup, Kind::POWER_OFF, Direction::UNIT_TO_REMOTE) == 1);

  auto onoff = parse(load_stream(dir + "/on_off.log", &ok));
  CHECK((setting_values(onoff) == std::vector<uint8_t>{0x09}));
  CHECK(count_kind(onoff, Kind::POWER_OFF, Direction::REMOTE_TO_UNIT) == 1);

  // Operator-labeled 20 -> 22 -> 18 C HEAT test (after the 09 power-on prelude).
  auto heat = parse(load_stream(dir + "/heat_20_22_18.log", &ok));
  CHECK((setting_values(heat) == std::vector<uint8_t>{0x09, 0x92, 0xB2, 0xD2, 0xB2, 0x92, 0x72, 0x52}));

  // The standby capture only carries idle status messages.
  auto standby = parse(load_stream(dir + "/running_standby.log", &ok));
  CHECK(count_kind(standby, Kind::SETTING, Direction::REMOTE_TO_UNIT) == 0);
  for (const auto &m : standby) {
    CHECK(m.kind != Kind::STATUS || !m.running);
  }

  auto tx_enabled = parse(load_stream(dir + "/tx-enabled-new-build.log", &ok));
  CHECK(ok);
  CHECK((setting_values(tx_enabled) == std::vector<uint8_t>{0xF2, 0xD2, 0xB2, 0x92, 0x09, 0x0B, 0x03, 0x05}));
  CHECK(count_kind(tx_enabled, Kind::POWER_OFF, Direction::REMOTE_TO_UNIT) == 1);
  CHECK(count_kind(tx_enabled, Kind::POWER_OFF, Direction::UNIT_TO_REMOTE) == 1);

  auto cold_test = parse(load_stream(dir + "/cold-test.log", &ok));
  CHECK(ok);
  CHECK((setting_values(cold_test) ==
         std::vector<uint8_t>{0x95, 0x9D, 0xA5, 0xAD, 0xB5, 0xBD, 0xC5, 0xCD, 0xD5, 0xDD, 0xE5, 0xED, 0xF5}));
  CHECK(count_kind(cold_test, Kind::POWER_OFF, Direction::REMOTE_TO_UNIT) == 1);
  CHECK(count_kind(cold_test, Kind::POWER_OFF, Direction::UNIT_TO_REMOTE) == 1);
}

int main(int argc, char **argv) {
  const std::string dir = argc > 1 ? argv[1] : "captures/matt-captures";
  test_synthetic();
  test_decode_table();
  test_captures(dir);
  if (failures == 0) {
    std::printf("ALL PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", failures);
  return 1;
}
