#pragma once
// The WinRT-free part of the Windows MIDI Services input: what happens to a
// batch of UMP words the service hands to an opened port. It includes no
// WinRT header, so it compiles and is unit-tested on every platform.

#include <libremidi/cmidi2.hpp>
#include <libremidi/detail/midi_stream_decoder.hpp>

#include <cstdint>
#include <span>

NAMESPACE_LIBREMIDI::winmidi
{
//! The service stamps each callback with one timestamp, converted to ns.
static constexpr timestamp_backend_info input_timestamp_info{
    .has_absolute_timestamps = true,
    .absolute_is_monotonic = false,
    .has_samples = false,
};

//! The UMP groups an opened input port covers: [first, first + count).
//! Disabled on virtual ports.
struct ump_group_filter
{
  bool enabled{};
  uint8_t first{};
  uint8_t count{};

  //! The verdict for one message, from its first word.
  constexpr bool accepts(uint32_t w0) const noexcept
  {
    if (!enabled)
      return true;
    return ((w0 >> 24) & 0xF) == first;
  }
};

//! The size of a UMP in 32-bit words, read from its first word.
struct ump_size_in_words
{
  std::size_t operator()(uint32_t w0) const noexcept
  {
    return cmidi2_ump_get_num_bytes(w0) / 4;
  }
};

//! Calls fn(message) for each whole UMP of a batch, in order.
//! - NOOP words (0x00000000) between messages are padding and skipped, as in
//!   input_state_machine::on_bytes_multi and segment_ump_stream.
//! - A message cut short by the end of the batch is dropped, and the walk
//!   stops there.
//! - A message size of zero stops the walk (it would never advance).
template <typename F, typename Sizer = ump_size_in_words>
constexpr void for_each_ump(std::span<const uint32_t> batch, F&& fn, Sizer size = {})
{
  std::size_t i = 0;
  while (i < batch.size())
  {
    if (batch[i] == 0)
    {
      ++i;
      continue;
    }

    const std::size_t n = size(batch[i]);
    if (n == 0 || n > batch.size() - i)
      break;

    fn(batch.subspan(i, n));
    i += n;
  }
}

//! Hands one batch of words, as the service delivered it, to the decoder.
//!
//! The service delivers one or more whole messages per callback ("a group of
//! messages that arrived together stays together in a single callback",
//! Microsoft, "Porting a MIDI library"). Each message is filtered on its own
//! and handed to the decoder on its own, so the decoder sees exactly one
//! message per on_bytes call and on_raw_data fires once per delivered
//! message.
//!
//! to_ns returns the batch's service timestamp in nanoseconds. The timestamp
//! mode is applied per delivered message, so Relative mode yields zero deltas
//! inside a batch.
template <typename ToNs, typename Sizer = ump_size_in_words>
inline void dispatch_ump_batch(
    midi2::input_state_machine& processing, std::span<const uint32_t> batch,
    const ump_group_filter& groups, ToNs to_ns, Sizer size = {})
{
  for_each_ump(
      batch,
      [&](std::span<const uint32_t> message) {
    if (groups.accepts(message[0]))
      processing.on_bytes(message, processing.timestamp<input_timestamp_info>(to_ns, 0));
  },
      size);
}
}
