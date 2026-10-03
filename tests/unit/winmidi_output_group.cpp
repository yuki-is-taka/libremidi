#include "../include_catch.hpp"

#include <libremidi/backends/winmidi/ump_batch.hpp>
#include <libremidi/detail/conversion.hpp>
#include <libremidi/detail/ump_stream.hpp>

#include <cstdint>
#include <vector>

// Tests for the group a Windows MIDI Services output port stamps on what it
// sends. The service has no destination parameter: the group in the message
// selects the cable. The policy is WinRT-free, so these run on every
// platform.

namespace
{
using libremidi::winmidi::ump_output_group;
using action = libremidi::winmidi::ump_output_group::action;

constexpr uint32_t with_group(uint32_t w0, uint8_t g)
{
  return (w0 & 0xF0FFFFFFu) | (uint32_t(g) << 24);
}

// One UMP per group-bearing message type, on group 0.
constexpr uint32_t grouped_types[] = {
    0x10F80000, // 1: system real time
    0x20903C40, // 2: MIDI 1.0 channel voice
    0x30160021, // 3: SysEx7
    0x40903C00, // 4: MIDI 2.0 channel voice
    0x50010000, // 5: SysEx8 / mixed data set
    0xD0100000, // D: flex data
};

// Groupless and reserved types, with bits 27..24 set to 0 and to 5.
constexpr uint32_t groupless_types[] = {
    0x00200123, 0x05200123, // 0: utility (JR timestamp)
    0xF0000101, 0xF5000101, // F: UMP stream
    0x60000000, 0x65000000, 0x70000000, 0x80000000, 0x90000000, 0xA0000000,
    0xB0000000, 0xC0000000, 0xE0000000, 0xE5000000, // reserved
};

// A single-group port on group 1 of an endpoint that receives groups 0-2:
// the Push 3 User port.
constexpr ump_output_group push_user{.enabled = true, .first = 1, .count = 1, .receivable = 0b111};
}

TEST_CASE("winmidi output: group masks", "[winmidi][output_group]")
{
  using libremidi::winmidi::ump_group_mask;
  CHECK(ump_group_mask(0, 0) == 0x0000);
  CHECK(ump_group_mask(0, 1) == 0x0001);
  CHECK(ump_group_mask(1, 1) == 0x0002);
  CHECK(ump_group_mask(2, 3) == 0x001C);
  CHECK(ump_group_mask(15, 1) == 0x8000);
  CHECK(ump_group_mask(0, 16) == 0xFFFF);
  CHECK(ump_group_mask(14, 4) == 0xC000); // clipped to 16 groups
}

TEST_CASE("winmidi output: rewriting the group of a word", "[winmidi][output_group]")
{
  using libremidi::winmidi::ump_with_group;
  CHECK(ump_with_group(0x20903C40, 1) == 0x21903C40);
  CHECK(ump_with_group(0x2F903C40, 0) == 0x20903C40);
  CHECK(ump_with_group(0x40903C00, 15) == 0x4F903C00);
}

TEST_CASE("winmidi output: the verdict per message", "[winmidi][output_group]")
{
  SECTION("single-group port: another group is restamped to the port's")
  {
    for (uint32_t w : grouped_types)
    {
      CHECK(push_user.decide(w) == action::restamp);
      CHECK(push_user.decide(with_group(w, 1)) == action::send);
      CHECK(push_user.decide(with_group(w, 2)) == action::restamp);
    }
  }

  SECTION("groupless and reserved types are left alone")
  {
    for (uint32_t w : groupless_types)
      CHECK(push_user.decide(w) == action::send);
  }

  SECTION("multi-group port: the caller's group stands inside the block")
  {
    // first group 2, 3 groups: 2, 3 and 4.
    const ump_output_group port{.enabled = true, .first = 2, .count = 3, .receivable = 0xFFFF};
    for (uint8_t g = 0; g < 16; g++)
    {
      INFO("group " << int(g));
      CHECK(port.decide(with_group(0x20903C40, g)) == (g >= 2 && g < 5 ? action::send : action::restamp));
    }
  }

  SECTION("no host-to-device block covers the port's group: left unchanged")
  {
    // The resolved block says group 1, but the endpoint only receives group 0
    // (e.g. the block's direction is the other one on an asymmetric device).
    const ump_output_group port{.enabled = true, .first = 1, .count = 1, .receivable = 0b001};
    CHECK(port.decide(0x20903C40) == action::send_unverified);
    CHECK(port.decide(0x21903C40) == action::send);
  }

  SECTION("virtual port: everything goes out as given")
  {
    const ump_output_group port{};
    for (uint32_t w : grouped_types)
      CHECK(port.decide(with_group(w, 7)) == action::send);
  }
}

TEST_CASE("winmidi output: writing one message", "[winmidi][output_group]")
{
  using libremidi::winmidi::write_with_group;

  std::vector<std::vector<uint32_t>> written;
  std::vector<action> notes;
  auto write = [&](const uint32_t* ump, int64_t bytes) {
    written.emplace_back(ump, ump + bytes / 4);
    return std::errc{};
  };
  auto note = [&](action a) { notes.push_back(a); };

  SECTION("restamped: only the group changes, the caller's words are untouched")
  {
    const uint32_t ump[2] = {0x40903C00, 0xC0000000};
    CHECK(write_with_group(push_user, ump, 8, write, note) == std::errc{});
    REQUIRE(written.size() == 1);
    CHECK(written[0] == std::vector<uint32_t>{0x41903C00, 0xC0000000});
    CHECK(ump[0] == 0x40903C00);
    CHECK(notes == std::vector<action>{action::restamp});
  }

  SECTION("in range: sent as is, nothing to report")
  {
    const uint32_t ump[1] = {0x21903C40};
    CHECK(write_with_group(push_user, ump, 4, write, note) == std::errc{});
    CHECK(written == std::vector<std::vector<uint32_t>>{{0x21903C40}});
    CHECK(notes.empty());
  }

  SECTION("not covered by a host-to-device block: sent as is, reported")
  {
    const ump_output_group port{.enabled = true, .first = 1, .count = 1, .receivable = 0b001};
    const uint32_t ump[1] = {0x20903C40};
    CHECK(write_with_group(port, ump, 4, write, note) == std::errc{});
    CHECK(written == std::vector<std::vector<uint32_t>>{{0x20903C40}});
    CHECK(notes == std::vector<action>{action::send_unverified});
  }

  SECTION("a write error is returned")
  {
    const uint32_t ump[1] = {0x20903C40};
    auto failing = [](const uint32_t*, int64_t) { return std::errc::io_error; };
    CHECK(write_with_group(push_user, ump, 4, failing, note) == std::errc::io_error);
  }

  SECTION("a stream, segmented as send_ump does")
  {
    const std::vector<uint32_t> stream{
        0x20903C40,                                     // MT2, group 0
        0x40903C00, 0xC0000000,                         // MT4, group 0
        0x00000000,                                     // NOOP padding
        0xF0000101, 0xFFFFFFFF, 0x00000000, 0x00000000, // MT F
        0x30160021, 0x1D01010A, 0x30310100, 0x00000000, // SysEx7 Start + End, group 0
    };
    auto err = libremidi::segment_ump_stream(
        stream.data(), static_cast<int64_t>(stream.size()),
        [&](const uint32_t* ump, int64_t bytes) {
      return write_with_group(push_user, ump, bytes, write, note);
    }, [] { });
    CHECK(err == stdx::error{});
    CHECK(
        written
        == std::vector<std::vector<uint32_t>>{
            {0x21903C40},
            {0x41903C00, 0xC0000000},
            {0xF0000101, 0xFFFFFFFF, 0x00000000, 0x00000000},
            {0x31160021, 0x1D01010A},
            {0x31310100, 0x00000000}});
  }
}

TEST_CASE("winmidi output: MIDI 1.0 bytes and the converter's group", "[winmidi][output_group]")
{
  auto convert = [](libremidi::midi1_to_midi2& converter, std::vector<uint8_t> bytes) {
    std::vector<uint32_t> out;
    auto err = converter.convert(
        bytes.data(), bytes.size(), 0, [&](const uint32_t* ump, std::size_t n, int64_t) {
      out.assign(ump, ump + n);
      return stdx::error{};
    });
    REQUIRE(err == stdx::error{});
    return out;
  };

  SECTION("the converter writes its context group")
  {
    libremidi::midi1_to_midi2 converter;
    REQUIRE(((convert(converter, {0x91, 0x3C, 0x40})[0] >> 24) & 0xF) == 0);
    converter.context.group = 1;
    REQUIRE(((convert(converter, {0x91, 0x3C, 0x40})[0] >> 24) & 0xF) == 1);
  }

  SECTION("the group a port gives MIDI 1.0 bytes")
  {
    CHECK(push_user.midi1_group() == 1);
    // Not covered by a host-to-device block: left at 0, as before.
    CHECK(ump_output_group{.enabled = true, .first = 1, .count = 1, .receivable = 0b001}.midi1_group() == 0);
    // Virtual port.
    CHECK(ump_output_group{}.midi1_group() == 0);
  }

  SECTION("MIDI 1.0 bytes on the Push User port land on its group without a restamp")
  {
    libremidi::midi1_to_midi2 converter;
    converter.context.group = push_user.midi1_group();
    const auto ump = convert(converter, {0x91, 0x3C, 0x40});
    CHECK(push_user.decide(ump[0]) == action::send);
    CHECK(((ump[0] >> 24) & 0xF) == 1);
  }
}
