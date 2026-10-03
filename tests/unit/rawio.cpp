#include "../include_catch.hpp"

#include <libremidi/configurations.hpp>
#include <libremidi/libremidi.hpp>

TEST_CASE("rawio midi1 roundtrip", "[rawio]")
{
  // The callback that the library will give us to feed bytes into
  libremidi::rawio_input_configuration::receive_callback on_receive;

  // Received messages
  std::vector<libremidi::message> received;

  libremidi::midi_in midiin{
      libremidi::input_configuration{
          .on_message = [&](const libremidi::message& m) { received.push_back(m); }},
      libremidi::rawio_input_configuration{
          .set_receive_callback = [&](auto cb) { on_receive = std::move(cb); },
          .stop_receive = [&] { on_receive = nullptr; }}};

  REQUIRE(midiin.get_current_api() == libremidi::API::RAW_IO);

  // Bytes written by midi_out
  std::vector<uint8_t> written;

  libremidi::midi_out midiout{
      libremidi::output_configuration{},
      libremidi::rawio_output_configuration{
          .write_bytes = [&](std::span<const uint8_t> bytes) -> stdx::error {
    written.assign(bytes.begin(), bytes.end());
    return {};
  }}};

  REQUIRE(midiout.get_current_api() == libremidi::API::RAW_IO);

  midiin.open_virtual_port("test");
  midiout.open_virtual_port("test");

  SECTION("note on roundtrip")
  {
    // Send a note-on through midi_out
    midiout.send_message(0x90, 60, 100);

    // Verify the output wrote correct bytes
    REQUIRE(written.size() == 3);
    REQUIRE(written[0] == 0x90);
    REQUIRE(written[1] == 60);
    REQUIRE(written[2] == 100);

    // Feed those bytes back into midi_in (simulating a loopback transport)
    REQUIRE(on_receive);
    on_receive(written, 0);

    // Verify the message was received and parsed
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].bytes.size() == 3);
    REQUIRE(received[0].bytes[0] == 0x90);
    REQUIRE(received[0].bytes[1] == 60);
    REQUIRE(received[0].bytes[2] == 100);
  }

  SECTION("multiple messages")
  {
    // Note on
    midiout.send_message(0x90, 60, 100);
    REQUIRE(on_receive);
    on_receive(written, 0);

    // Note off
    midiout.send_message(0x80, 60, 0);
    on_receive(written, 0);

    // CC
    midiout.send_message(0xB0, 7, 100);
    on_receive(written, 0);

    REQUIRE(received.size() == 3);
    REQUIRE(received[0].bytes[0] == 0x90);
    REQUIRE(received[1].bytes[0] == 0x80);
    REQUIRE(received[2].bytes[0] == 0xB0);
    REQUIRE(received[2].bytes[1] == 7);
    REQUIRE(received[2].bytes[2] == 100);
  }

  SECTION("close port calls stop_receive")
  {
    REQUIRE(on_receive);
    midiin.close_port();
    REQUIRE_FALSE(on_receive);
  }
}

TEST_CASE("rawio midi2 ump roundtrip", "[rawio]")
{
  // The callback that the library will give us to feed UMP words into
  libremidi::rawio_ump_input_configuration::receive_callback on_receive;

  // Received UMP messages
  std::vector<libremidi::ump> received;

  libremidi::midi_in midiin{
      libremidi::ump_input_configuration{
          .on_message = [&](const libremidi::ump& m) { received.push_back(m); }},
      libremidi::rawio_ump_input_configuration{
          .set_receive_callback = [&](auto cb) { on_receive = std::move(cb); },
          .stop_receive = [&] { on_receive = nullptr; }}};

  REQUIRE(midiin.get_current_api() == libremidi::API::RAW_IO_UMP);

  // Words written by midi_out
  std::vector<uint32_t> written;

  libremidi::midi_out midiout{
      libremidi::output_configuration{},
      libremidi::rawio_ump_output_configuration{
          .write_ump = [&](std::span<const uint32_t> words) -> stdx::error {
    written.assign(words.begin(), words.end());
    return {};
  }}};

  REQUIRE(midiout.get_current_api() == libremidi::API::RAW_IO_UMP);

  midiin.open_virtual_port("test");
  midiout.open_virtual_port("test");

  SECTION("ump roundtrip")
  {
    // Send a MIDI 2.0 note-on UMP (type 4, group 0, channel 0, note 60, velocity 0xC000)
    uint32_t ump[2] = {0x40900000 | 60, 0xC0000000};
    midiout.send_ump(ump, 2);

    // Verify output
    REQUIRE(written.size() == 2);

    // Feed back into input
    REQUIRE(on_receive);
    on_receive(written, 0);

    // Verify reception
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].data[0] == ump[0]);
    REQUIRE(received[0].data[1] == ump[1]);
  }
}

// A MIDI 1 user on a UMP backend (e.g. Windows MIDI Services): SysEx7 arrives
// as packets and must come out as one complete F0 ... F7 message.
TEST_CASE("rawio ump: SysEx7 packets reach a MIDI 1 user as one message", "[rawio][sysex]")
{
  libremidi::rawio_ump_input_configuration::receive_callback on_receive;
  std::vector<libremidi::message> received;

  libremidi::midi_in midiin{
      libremidi::input_configuration{
          .on_message = [&](const libremidi::message& m) { received.push_back(m); },
          .ignore_sysex = false},
      libremidi::rawio_ump_input_configuration{
          .set_receive_callback = [&](auto cb) { on_receive = std::move(cb); },
          .stop_receive = [&] { on_receive = nullptr; }}};
  midiin.open_virtual_port("test");
  REQUIRE(on_receive);

  auto feed = [&](std::vector<uint32_t> words, int64_t ts = 0) { on_receive(words, ts); };
  auto bytes = [&](std::size_t i) {
    return std::vector<uint8_t>(received.at(i).bytes.begin(), received.at(i).bytes.end());
  };

  // A Push 3 mode report: F0 00 21 1D 01 01 0A 01 F7, 7 data bytes = Start + End.
  const std::vector<uint32_t> start{0x30160021, 0x1D01010A};
  const std::vector<uint32_t> end{0x30310100, 0x00000000};
  const std::vector<uint8_t> mode_report{0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x0A, 0x01, 0xF7};

  SECTION("Start and End in one batch")
  {
    feed({start[0], start[1], end[0], end[1]});
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == mode_report);
  }

  SECTION("Start and End in separate callbacks; the Start timestamp is kept")
  {
    feed(start, 100);
    CHECK(received.empty());
    feed(end, 200);
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == mode_report);
    CHECK(received[0].timestamp == 100);
  }

  SECTION("Start, Continue, End")
  {
    // 29 data bytes (an Erae finger stream): 5 packets.
    std::vector<uint32_t> words;
    std::vector<uint8_t> expected{0xF0};
    for (int p = 0, pos = 0; p < 5; p++)
    {
      const int len = (p < 4) ? 6 : 5;
      const uint32_t status = (p == 0) ? 0x1 : (p < 4 ? 0x2 : 0x3);
      uint8_t b[6]{};
      for (int i = 0; i < len; i++)
        expected.push_back(b[i] = static_cast<uint8_t>(pos + i + 1));
      pos += len;
      words.push_back(
          0x30000000u | (status << 20) | (uint32_t(len) << 16) | (uint32_t(b[0]) << 8) | b[1]);
      words.push_back(
          (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) | (uint32_t(b[4]) << 8) | b[5]);
    }
    expected.push_back(0xF7);
    feed(words);
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == expected);
  }

  SECTION("a complete one-packet SysEx")
  {
    feed({0x30037E7F, 0x06000000}); // F0 7E 7F 06 F7
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == std::vector<uint8_t>{0xF0, 0x7E, 0x7F, 0x06, 0xF7});
  }

  SECTION("a channel message between the packets passes on its own")
  {
    feed(start);
    feed({0x20903C40}); // MIDI 1 Note On
    feed(end);
    REQUIRE(received.size() == 2);
    CHECK(bytes(0) == std::vector<uint8_t>{0x90, 0x3C, 0x40});
    CHECK(bytes(1) == mode_report);
  }

  SECTION("Continue or End without a Start is dropped")
  {
    feed(end);
    feed({0x30260A01, 0x02030405});
    CHECK(received.empty());
  }

  SECTION("a new Start abandons the unfinished message")
  {
    feed({0x30160102, 0x03040506}); // Start of a message that never ends
    feed(start);
    feed(end);
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == mode_report);
  }

  SECTION("a message past the size bound is dropped, the next one passes")
  {
    // 1 MiB of data bytes is the bound; send a little more.
    std::vector<uint32_t> words{0x30160102, 0x03040506};
    for (std::size_t n = 6; n <= 1024 * 1024; n += 6)
    {
      words.push_back(0x30260102);
      words.push_back(0x03040506);
    }
    words.push_back(0x30310100);
    words.push_back(0x00000000);
    feed(words);
    CHECK(received.empty());

    feed(start);
    feed(end);
    REQUIRE(received.size() == 1);
    CHECK(bytes(0) == mode_report);
  }

  SECTION("each group reassembles on its own")
  {
    feed({start[0] | 0x01000000, start[1]}); // group 1 Start
    feed(start);                             // group 0 Start
    feed({end[0] | 0x01000000, end[1]});     // group 1 End
    feed(end);                               // group 0 End
    REQUIRE(received.size() == 2);
    CHECK(bytes(0) == mode_report);
    CHECK(bytes(1) == mode_report);
  }
}
