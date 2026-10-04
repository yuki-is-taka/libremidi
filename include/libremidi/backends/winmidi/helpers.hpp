#pragma once
// clang-format off
#if !defined(NOMINMAX)
  #define NOMINMAX 1
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
  #define WIN32_LEAN_AND_MEAN 1
#endif
#include <libremidi/detail/midi_api.hpp>
#include <libremidi/detail/memory.hpp>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <guiddef.h>
#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi2.h>
#include <winrt/Windows.Devices.Midi2.Enumeration.h>
#if __has_include(<winrt/Windows.Devices.Midi2.Transports.Virtual.h>)
#define LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE 1
#include <winrt/Windows.Devices.Midi2.Transports.Virtual.h>
#endif
#include <libremidi/cmidi2.hpp>

#if __has_include(<WindowsMidiServicesAppSdkComExtensions.h>)
#include <WindowsMidiServicesAppSdkComExtensions.h>
  #define LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS 1

#define LIBREMIDI_DEFINE_GUID_CONSTEXPR(type,name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) \
        LIBREMIDI_STATIC constexpr const type name = {l,w1,w2,{b1,b2,b3,b4,b5,b6,b7,b8}}

NAMESPACE_LIBREMIDI {
  LIBREMIDI_DEFINE_GUID_CONSTEXPR(IID, IID_IMidiEndpointConnectionMessagesReceivedCallback, 0x8087b303, 0x0519, 0x31d1, 0x31, 0xd1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10);
  LIBREMIDI_DEFINE_GUID_CONSTEXPR(IID, IID_IMidiEndpointConnectionRaw,                      0x8087b303, 0x0519, 0x31d1, 0x31, 0xd1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20);
}
#endif

// clang-format on

namespace midi2 = winrt::Windows::Devices::Midi2;
namespace foundation = winrt::Windows::Foundation;

NAMESPACE_LIBREMIDI::winmidi
{
using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Devices::Midi2;
using namespace winrt::Windows::Devices::Midi2::Enumeration;

inline bool ichar_equals(char a, char b)
{
  return std::tolower(static_cast<unsigned char>(a))
         == std::tolower(static_cast<unsigned char>(b));
}

inline bool iequals(std::string_view lhs, std::string_view rhs)
{
  return std::ranges::equal(lhs, rhs, ichar_equals);
}

//! A description of the exception being handled, for error reports.
//! Must be called from inside a catch block.
inline std::string current_exception_message() noexcept
{
  try
  {
    try
    {
      throw;
    }
    catch (const winrt::hresult_error& e)
    {
      char code[16]{};
      std::snprintf(
          code, sizeof(code), "0x%08X",
          static_cast<unsigned>(static_cast<std::int32_t>(e.code())));
      return std::string{code} + " " + winrt::to_string(e.message());
    }
    catch (const std::exception& e)
    {
      return e.what();
    }
    catch (...)
    {
      return "unknown exception";
    }
  }
  catch (...)
  {
    return {};
  }
}

//! Creates the MIDI session, or returns a null session when that is not possible.
//! This never lets a WinRT or standard exception escape: the reason is put in `error`.
//! Most of the API is [noexcept] and answers failures with null, but looking up the
//! activation factory throws when the runtime is missing.
inline MidiSession make_session(
    MidiSession* context, const std::string& client_name, std::string& error)
{
  try
  {
    if (context)
      return *context;

    auto session = MidiSession::Create(winrt::to_hstring(client_name));
    if (!session)
      error = "MidiSession::Create returned null";
    return session;
  }
  catch (...)
  {
    error = current_exception_message();
    return MidiSession{nullptr};
  }
}

//! Every pointer-like thing the service hands out may be null: the projection
//! turns a failed call into null instead of an exception. Callers must check.
inline std::pair<MidiEndpointDeviceInformation, MidiGroupTerminalBlock>
get_port(const std::string& device_name, libremidi::port_handle group_terminal_block)
{
  auto eps = MidiEndpointDeviceInformation::FindAll();
  if (!eps)
    return {nullptr, nullptr};

  for (const auto& ep : eps)
  {
    if (!ep)
      continue;

    auto str = to_string(ep.EndpointDeviceId());
    if (str.empty())
      continue;

    if (iequals(str, device_name))
    {
      auto gtbs = ep.GetGroupTerminalBlocks();
      if (!gtbs)
        continue;

      for (const auto& gp : gtbs)
      {
        if (gp && gp.Number() == group_terminal_block)
        {
          return std::make_pair(ep, gp);
        }
      }
    }
  }
  return {nullptr, nullptr};
}

//! Whether Windows MIDI Services can be used:
//! - the Windows.Devices.Midi2 activation factory can be reached (in-box, or the
//!   app-local Windows.Devices.Midi2.dll + .pri), and
//! - the selected API mode is the full Windows MIDI Services mode (in the legacy
//!   modes the classes activate, but the service is not in use), and
//! - the service is running or can be demand-started.
//! Nothing escapes from here, whatever fails.
struct winmidi_shared_data_instance
{
  bool ready{false};

  winmidi_shared_data_instance() noexcept
  {
    try
    {
      if (!winrt::try_get_activation_factory<MidiApi, IMidiApiStatics>())
        return;

      if (MidiApi::GetCurrentlySelectedApiMode() != MidiApiMode::FullWindowsMidiServicesMode)
        return;

      ready = MidiApi::EnsureServiceAvailable();
    }
    catch (...)
    {
      ready = false;
    }
  }
};

struct winmidi_shared_data
{
  std::shared_ptr<winmidi_shared_data_instance> self = libremidi::instance<winmidi_shared_data_instance>();

  winmidi_shared_data()
  {
  }

  ~winmidi_shared_data()
  {
  }
};


#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
inline winrt::Windows::Devices::Midi2::Transports::Virtual::MidiVirtualDeviceCreationConfig setup_virtualdevice_config(
    std::string_view manufacturer_name,
    std::string_view product_id,
    std::string_view port_name,
    MidiFunctionBlockDirection direction)
{
  using namespace winrt::Windows::Devices::Midi2;
  using namespace winrt::Windows::Devices::Midi2::Enumeration;
  using namespace winrt::Windows::Devices::Midi2::Transports::Virtual;

  MidiDeclaredEndpointInfo endpointInfo;
  endpointInfo.HasStaticFunctionBlocks(true);
  endpointInfo.Name(to_hstring(port_name));
  endpointInfo.ProductInstanceId(to_hstring(product_id));
  endpointInfo.SupportsMidi10Protocol(true);
  endpointInfo.SupportsMidi20Protocol(true);
  endpointInfo.SupportsReceivingJitterReductionTimestamps(false);
  endpointInfo.SupportsSendingJitterReductionTimestamps(false);
  endpointInfo.SpecificationVersionMajor(1);
  endpointInfo.SpecificationVersionMinor(1);

  // Create the virtual device configuration
  if(manufacturer_name.empty())
    manufacturer_name = "libremidi";

  MidiVirtualDeviceCreationConfig creationConfig(
      to_hstring(port_name), // name
      to_hstring(std::string("Virtual input port: ") + std::string(port_name)), // description
      to_hstring(manufacturer_name), // manufacturer
      endpointInfo);

  // Add a default function block for the virtual device
  MidiFunctionBlock block0;
  block0.Number(0);
  block0.IsActive(true);
  block0.Name(to_hstring(port_name));
  block0.FirstGroup(MidiGroup(static_cast<uint8_t>(0)));
  block0.GroupCount(16);  // All 16 groups
  block0.Direction(direction);
  creationConfig.FunctionBlocks().Append(block0);

  return creationConfig;

}
#endif
}
