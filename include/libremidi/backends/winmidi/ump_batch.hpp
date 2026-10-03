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
};

//! The size of a UMP in 32-bit words, read from its first word.
struct ump_size_in_words
{
  std::size_t operator()(uint32_t w0) const noexcept
  {
    return cmidi2_ump_get_num_bytes(w0) / 4;
  }
};

//! Hands one batch of words, as the service delivered it, to the decoder.
//! to_ns returns the batch's service timestamp in nanoseconds.
template <typename ToNs, typename Sizer = ump_size_in_words>
inline void dispatch_ump_batch(
    midi2::input_state_machine& processing, std::span<const uint32_t> batch,
    const ump_group_filter& groups, ToNs to_ns, Sizer = {})
{
  if (batch.empty())
    return;

  if (groups.enabled)
  {
    int group = cmidi2_ump_get_group(batch.data());
    if (group != groups.first)
      return;
  }

  processing.on_bytes(batch, processing.timestamp<input_timestamp_info>(to_ns, 0));
}
}
