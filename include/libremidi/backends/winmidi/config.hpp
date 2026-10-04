#pragma once
#include <libremidi/config.hpp>

#include <string>

#if !defined(LIBREMIDI_MODULE_BUILD) || !defined(_WIN32)
namespace winrt::Windows::Devices::Midi2
{
struct MidiSession;
struct MidiEndpointConnection;
}
namespace winrt::Windows::Devices::Midi2::Enumeration
{
struct MidiEndpointDeviceInformation;
}
#endif

// TODO allow to share midi session and endpoints
NAMESPACE_LIBREMIDI::winmidi
{

struct input_configuration
{
  std::string client_name = "libremidi input";
  winrt::Windows::Devices::Midi2::MidiSession* context{};
};

struct output_configuration
{
  std::string client_name = "libremidi output";
  winrt::Windows::Devices::Midi2::MidiSession* context{};
};

struct observer_configuration
{
  std::string client_name = "libremidi observer";
};

}
