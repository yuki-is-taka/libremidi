#include "../include_catch.hpp"

#include <libremidi/backends/winmidi/ump_batch.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

// Tests for the Windows MIDI Services input dispatch: what one service
// callback batch turns into, through a real midi2::input_state_machine and a
// real ump_input_configuration. The dispatch is WinRT-free, so these run on
// every platform.

namespace
{
using libremidi::winmidi::dispatch_ump_batch;
using libremidi::winmidi::ump_group_filter;

constexpr ump_group_filter no_filter{};
constexpr ump_group_filter group(uint8_t first, uint8_t count = 1)
{
  return {.enabled = true, .first = first, .count = count};
}

// UMP builders. g = group, c = channel.
constexpr uint32_t mt2_note_on(uint8_t g, uint8_t note = 60, uint8_t c = 0)
{
  return 0x20900000u | (uint32_t(g) << 24) | (uint32_t(c) << 16) | (uint32_t(note) << 8) | 0x40;
}
constexpr uint32_t mt2_cc(uint8_t g, uint8_t cc, uint8_t value)
{
  return 0x20B00000u | (uint32_t(g) << 24) | (uint32_t(cc) << 8) | value;
}
constexpr uint32_t mt1_system(uint8_t g, uint8_t status)
{
  return 0x10000000u | (uint32_t(g) << 24) | (uint32_t(status) << 16);
}

// SysEx7 packets for n data bytes on group g: Complete, or Start, Continue..., End.
std::vector<uint32_t> sysex7(std::size_t n, uint8_t g = 0)
{
  std::vector<uint32_t> words;
  std::size_t pos = 0;
  do
  {
    const std::size_t len = std::min<std::size_t>(6, n - pos);
    uint32_t status = (pos == 0) ? ((len == n) ? 0x0 : 0x1) : ((pos + len == n) ? 0x3 : 0x2);
    uint8_t b[6]{};
    for (std::size_t i = 0; i < len; i++)
      b[i] = static_cast<uint8_t>((pos + i + 1) & 0x7F);
    words.push_back(
        0x30000000u | (uint32_t(g) << 24) | (status << 20) | (uint32_t(len) << 16)
        | (uint32_t(b[0]) << 8) | b[1]);
    words.push_back(
        (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) | (uint32_t(b[4]) << 8) | b[5]);
    pos += len;
  } while (pos < n);
  return words;
}

std::vector<uint32_t> concat(std::initializer_list<std::vector<uint32_t>> parts)
{
  std::vector<uint32_t> v;
  for (auto& p : parts)
    v.insert(v.end(), p.begin(), p.end());
  return v;
}

// A real input configuration and state machine, as winmidi::midi_in_impl holds them,
// with counters on both callbacks.
struct input
{
  libremidi::ump_input_configuration conf;
  std::vector<libremidi::ump> messages;
  std::vector<std::vector<uint32_t>> raw;
  std::vector<int64_t> raw_timestamps;
  libremidi::midi2::input_state_machine processing{conf};

  input()
  {
    conf.on_message = [this](libremidi::ump&& m) { messages.push_back(m); };
    conf.on_raw_data = [this](std::span<const uint32_t> w, libremidi::timestamp ts) {
      raw.emplace_back(w.begin(), w.end());
      raw_timestamps.push_back(ts);
    };
    conf.ignore_sysex = false;
    conf.ignore_timing = false;
    conf.ignore_sensing = false;
    conf.midi1_channel_events_to_midi2 = false;
    conf.timestamps = libremidi::timestamp_mode::Absolute;
  }

  // One service callback: a batch and its timestamp (already in ns).
  void batch(
      const std::vector<uint32_t>& words, const ump_group_filter& groups, uint64_t ns = 1000)
  {
    dispatch_ump_batch(processing, std::span<const uint32_t>(words), groups, [ns] { return ns; });
  }

  uint32_t type(std::size_t i) const { return messages.at(i).data[0] >> 28; }
  uint32_t grp(std::size_t i) const { return (messages.at(i).data[0] >> 24) & 0xF; }
};
}

TEST_CASE(
    "winmidi batch: each message reaches on_message and on_raw_data on its own",
    "[winmidi][ump_batch][mutation]")
{
  // MUTATION GUARD. A Push 3 mode report (9 bytes of SysEx) arrives as one
  // 4-word batch: SysEx7 Start + End. Handing the whole batch to
  // input_state_machine::on_bytes (the behaviour before this fix) yields a
  // single UMP holding both packets, so a consumer reading MT 3 as 2 words
  // loses the End packet, and on_raw_data sees 4 words at once. This test
  // fails if that behaviour is restored.
  input in;
  const auto words = sysex7(9);
  REQUIRE(words.size() == 4);
  in.batch(words, group(0));

  REQUIRE(in.messages.size() == 2);
  CHECK(in.messages[0].data[0] == words[0]);
  CHECK(in.messages[0].data[1] == words[1]);
  CHECK(in.messages[0].data[2] == 0);
  CHECK(in.messages[0].data[3] == 0);
  CHECK(in.messages[1].data[0] == words[2]);
  CHECK(in.messages[1].data[1] == words[3]);

  REQUIRE(in.raw.size() == 2);
  CHECK(in.raw[0] == std::vector<uint32_t>{words[0], words[1]});
  CHECK(in.raw[1] == std::vector<uint32_t>{words[2], words[3]});
}

TEST_CASE("winmidi batch: SysEx7 batches yield every packet in order", "[winmidi][ump_batch]")
{
  // 4, 6, 8 and 10 words: a 9-byte reply, a 17-byte palette reply, a 23-byte
  // Live poll reply, a 29-byte Erae finger stream.
  for (std::size_t bytes : {9u, 17u, 23u, 29u})
  {
    for (auto filter : {no_filter, group(0)})
    {
      DYNAMIC_SECTION(bytes << " bytes, filter " << filter.enabled)
      {
        input in;
        const auto words = sysex7(bytes);
        in.batch(words, filter);

        const std::size_t packets = words.size() / 2;
        REQUIRE(in.messages.size() == packets);
        REQUIRE(in.raw.size() == packets);
        for (std::size_t i = 0; i < packets; i++)
        {
          CHECK(in.messages[i].data[0] == words[2 * i]);
          CHECK(in.messages[i].data[1] == words[2 * i + 1]);
          CHECK(in.raw[i].size() == 2);
          CHECK(in.messages[i].timestamp == 1000);
        }
      }
    }
  }
}

TEST_CASE("winmidi batch: a burst of channel messages yields every message", "[winmidi][ump_batch]")
{
  std::vector<uint32_t> words;
  for (uint8_t i = 0; i < 8; i++)
    words.push_back(mt2_cc(0, 20 + i, i));

  SECTION("MIDI 1 channel voice kept")
  {
    input in;
    in.batch(words, group(0));
    REQUIRE(in.messages.size() == 8);
    for (std::size_t i = 0; i < 8; i++)
      CHECK(in.messages[i].data[0] == words[i]);
    CHECK(in.raw.size() == 8);
  }

  SECTION("MIDI 1 channel voice upgraded to MIDI 2")
  {
    input in;
    in.conf.midi1_channel_events_to_midi2 = true;
    in.batch(words, group(0));
    REQUIRE(in.messages.size() == 8);
    for (std::size_t i = 0; i < 8; i++)
    {
      CHECK(in.type(i) == 0x4);
      CHECK(((in.messages[i].data[0] >> 8) & 0x7F) == 20 + i);
    }
    // on_raw_data is not transformed.
    REQUIRE(in.raw.size() == 8);
    CHECK(in.raw[0] == std::vector<uint32_t>{words[0]});
  }
}

TEST_CASE("winmidi batch: the group verdict is per message", "[winmidi][ump_batch]")
{
  SECTION("mixed groups: only the port's group is delivered")
  {
    input in;
    const std::vector<uint32_t> words{
        mt2_note_on(0, 60), mt2_note_on(1, 61), mt2_note_on(2, 62), mt2_note_on(1, 63)};
    in.batch(words, group(1));
    REQUIRE(in.messages.size() == 2);
    CHECK(in.messages[0].data[0] == words[1]);
    CHECK(in.messages[1].data[0] == words[3]);
    // Filtered-out messages do not reach on_raw_data either.
    CHECK(in.raw.size() == 2);
  }

  SECTION("a block spanning several groups accepts all of them")
  {
    // first group 2, 3 groups: 2, 3 and 4.
    input in;
    std::vector<uint32_t> words;
    for (uint8_t g = 0; g < 16; g++)
      words.push_back(mt2_note_on(g, 60 + g));
    in.batch(words, group(2, 3));
    REQUIRE(in.messages.size() == 3);
    CHECK(in.grp(0) == 2);
    CHECK(in.grp(1) == 3);
    CHECK(in.grp(2) == 4);
  }

  SECTION("a SysEx on another group is dropped as a whole, the rest passes")
  {
    input in;
    const auto words = concat({sysex7(9, 3), sysex7(9, 0), {mt2_note_on(3)}});
    in.batch(words, group(0));
    REQUIRE(in.messages.size() == 2);
    CHECK(in.grp(0) == 0);
    CHECK(in.grp(1) == 0);
    CHECK(in.type(0) == 0x3);
  }
}

TEST_CASE("winmidi batch: groupless and reserved message types", "[winmidi][ump_batch]")
{
  // MT 0xF (UMP stream, here Endpoint Discovery), MT 0x0 (utility, here a JR
  // timestamp), and reserved types 0x6 (32 bits) and 0xE (128 bits). Their
  // bits 27..24 are not a group.
  const auto words = concat(
      {{0xF0000101, 0xFFFFFFFF, 0x00000000, 0x00000000},
       {0x00200123},
       {0x60000000},
       {0xE0000000, 1, 2, 3},
       {mt2_note_on(0)}});

  SECTION("dropped on a group-filtered port")
  {
    input in;
    in.batch(words, group(0));
    REQUIRE(in.messages.size() == 1);
    CHECK(in.messages[0].data[0] == mt2_note_on(0));
    CHECK(in.raw.size() == 1);
  }

  SECTION("delivered on a port without a filter (virtual)")
  {
    input in;
    in.batch(words, no_filter);
    REQUIRE(in.messages.size() == 5);
    CHECK(in.type(0) == 0xF);
    CHECK(in.type(1) == 0x0);
    CHECK(in.type(2) == 0x6);
    CHECK(in.type(3) == 0xE);
    CHECK(in.type(4) == 0x2);
  }
}

TEST_CASE("winmidi batch: framing", "[winmidi][ump_batch]")
{
  SECTION("a message cut short by the end of the batch is dropped")
  {
    // An MT2 message, then the first word of a 64-bit MIDI 2.0 Note On.
    input in;
    in.batch({mt2_note_on(0), 0x40903C00}, group(0));
    REQUIRE(in.messages.size() == 1);
    CHECK(in.messages[0].data[0] == mt2_note_on(0));
    CHECK(in.messages[0].data[1] == 0);
    REQUIRE(in.raw.size() == 1);
    CHECK(in.raw[0].size() == 1);
  }

  SECTION("NOOP padding between messages is skipped")
  {
    for (auto filter : {no_filter, group(0)})
    {
      input in;
      in.batch({0, mt2_note_on(0, 60), 0, 0, mt2_note_on(0, 61), 0}, filter);
      REQUIRE(in.messages.size() == 2);
      CHECK(in.messages[0].data[0] == mt2_note_on(0, 60));
      CHECK(in.messages[1].data[0] == mt2_note_on(0, 61));
    }
  }

  SECTION("an empty batch delivers nothing")
  {
    input in;
    in.batch({}, group(0));
    in.batch({}, no_filter);
    CHECK(in.messages.empty());
    CHECK(in.raw.empty());
  }

  SECTION("a message size of zero stops the walk instead of looping")
  {
    // The size table gives every message type a size; this guards the walk
    // against a table that ever yields 0. The size function gives up after
    // a few calls so that a missing guard fails here instead of hanging.
    input in;
    int calls = 0;
    auto zero_size = [&calls](uint32_t) -> std::size_t { return ++calls > 8 ? 1 : 0; };
    const std::vector<uint32_t> words{mt2_note_on(0), mt2_note_on(0)};
    dispatch_ump_batch(
        in.processing, std::span<const uint32_t>(words), group(0), [] { return 0ull; },
        zero_size);
    CHECK(calls == 1);
    CHECK(in.messages.empty());
    CHECK(in.raw.empty());
  }
}

TEST_CASE("winmidi batch: ignore flags apply per message", "[winmidi][ump_batch]")
{
  const auto words = concat(
      {sysex7(9),                                // 2 SysEx7 packets
       {mt1_system(0, 0xF8)},                    // timing clock
       {mt1_system(0, 0xFE)},                    // active sensing
       {mt2_note_on(0)}});

  SECTION("nothing ignored")
  {
    input in;
    in.batch(words, group(0));
    CHECK(in.messages.size() == 5);
  }

  SECTION("everything but the note ignored")
  {
    input in;
    in.conf.ignore_sysex = true;
    in.conf.ignore_timing = true;
    in.conf.ignore_sensing = true;
    in.batch(words, group(0));
    REQUIRE(in.messages.size() == 1);
    CHECK(in.messages[0].data[0] == mt2_note_on(0));
    // on_raw_data is not filtered by the ignore flags, only by the group.
    CHECK(in.raw.size() == 5);
  }
}

TEST_CASE("winmidi batch: timestamp modes", "[winmidi][ump_batch]")
{
  const std::vector<uint32_t> words{mt2_note_on(0, 60), mt2_note_on(0, 61), mt2_note_on(0, 62)};

  auto timestamps = [](const input& in) {
    std::vector<int64_t> v;
    for (auto& m : in.messages)
      v.push_back(m.timestamp);
    return v;
  };

  SECTION("NoTimestamp")
  {
    input in;
    in.conf.timestamps = libremidi::timestamp_mode::NoTimestamp;
    in.batch(words, group(0), 5000);
    CHECK(timestamps(in) == std::vector<int64_t>{0, 0, 0});
  }

  SECTION("Absolute: the service timestamp, once per batch")
  {
    input in;
    in.conf.timestamps = libremidi::timestamp_mode::Absolute;
    in.batch(words, group(0), 5000);
    CHECK(timestamps(in) == std::vector<int64_t>{5000, 5000, 5000});
    CHECK(in.raw_timestamps == std::vector<int64_t>{5000, 5000, 5000});
  }

  SECTION("Relative: zero deltas inside a batch")
  {
    input in;
    in.conf.timestamps = libremidi::timestamp_mode::Relative;
    in.batch(words, group(0), 5000);
    in.batch(words, group(0), 8000);
    CHECK(timestamps(in) == std::vector<int64_t>{0, 0, 0, 3000, 0, 0});
  }

  SECTION("SystemMonotonic: the system clock at dispatch time")
  {
    namespace clk = std::chrono;
    input in;
    in.conf.timestamps = libremidi::timestamp_mode::SystemMonotonic;
    const int64_t before
        = clk::duration_cast<clk::nanoseconds>(clk::steady_clock::now().time_since_epoch()).count();
    in.batch(words, group(0), 5000);
    const int64_t after
        = clk::duration_cast<clk::nanoseconds>(clk::steady_clock::now().time_since_epoch()).count();
    REQUIRE(in.messages.size() == 3);
    for (auto ts : timestamps(in))
    {
      CHECK(ts >= before);
      CHECK(ts <= after);
    }
  }

  SECTION("AudioFrame: no samples on this backend")
  {
    input in;
    in.conf.timestamps = libremidi::timestamp_mode::AudioFrame;
    in.batch(words, group(0), 5000);
    CHECK(timestamps(in) == std::vector<int64_t>{0, 0, 0});
  }

  SECTION("Custom: called once per delivered message with the service timestamp")
  {
    input in;
    std::vector<int64_t> seen;
    in.conf.timestamps = libremidi::timestamp_mode::Custom;
    in.conf.get_timestamp = [&](int64_t t) {
      seen.push_back(t);
      return t + 1;
    };
    in.batch(words, group(0), 5000);
    CHECK(seen == std::vector<int64_t>{5000, 5000, 5000});
    CHECK(timestamps(in) == std::vector<int64_t>{5001, 5001, 5001});
  }
}
