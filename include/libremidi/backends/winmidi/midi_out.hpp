#pragma once
#include <libremidi/backends/winmidi/config.hpp>
#include <libremidi/backends/winmidi/helpers.hpp>
#include <libremidi/backends/winmidi/observer.hpp>
#include <libremidi/backends/winmidi/ump_batch.hpp>
#include <libremidi/detail/midi_out.hpp>
#include <libremidi/detail/ump_stream.hpp>

NAMESPACE_LIBREMIDI::winmidi
{

class midi_out_impl final
    : public midi2::out_api
    , public error_handler
    , public winmidi_shared_data
{
public:
  struct
      : libremidi::output_configuration
      , winmidi::output_configuration
  {
  } configuration;

  midi_out_impl(libremidi::output_configuration&& conf, winmidi::output_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    // No WinRT exception may leave the constructor: when the session cannot be
    // created the object stays in its not-ready state (client_open_ is
    // not_connected), and open_port refuses.
    std::string error;
    if (!this->self->ready)
      error = "Windows MIDI Services is not available";
    else
      m_session = make_session(configuration.context, configuration.client_name, error);

    if (!m_session)
    {
      libremidi_handle_error(this->configuration, "winmidi: cannot create a MIDI session: " + error);
      return;
    }

    this->client_open_ = stdx::error{};
  }

  ~midi_out_impl() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::WINDOWS_MIDI_SERVICES;
  }

  stdx::error open_port(const output_port& port, std::string_view) override
  {
    auto device_id = get_if<std::string>(&port.device);
    if (!device_id)
      return std::errc::invalid_argument;

    if (!m_session)
      return std::errc::not_connected;

    try
    {
      auto [ep, gp] = get_port(*device_id, port.port);
      if (!ep || !gp)
        return std::errc::address_not_available;

      const auto first_group = gp.FirstGroup();
      if (!first_group)
        return std::errc::address_not_available;

      // The service has no destination parameter: the group in each message
      // selects the cable. See ump_output_group for the (interim) policy.
      set_group(
          {.enabled = true,
           .first = first_group.Index(),
           .count = gp.GroupCount(),
           .receivable = receivable_groups(ep)});

      m_endpoint = m_session.CreateEndpointConnection(ep.EndpointDeviceId());
      if (!m_endpoint)
      {
        abandon_open();
        return std::errc::device_or_resource_busy;
      }
  #if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
      if (FAILED(m_endpoint.as(libremidi::IID_IMidiEndpointConnectionRaw, m_raw_endpoint.put_void()))
          || !m_raw_endpoint)
      {
        abandon_open();
        return std::errc::io_error;
      }
    #endif

      // [noexcept] in the projection: a failure comes back as false.
      if (!m_endpoint.Open())
      {
        abandon_open();
        return std::errc::io_error;
      }

      return stdx::error{};
    }
    catch (...)
    {
      libremidi_handle_error(
          this->configuration, "winmidi: cannot open the output port: " + current_exception_message());
      abandon_open();
      return std::errc::io_error;
    }
  }

#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
  stdx::error open_virtual_port(std::string_view port_name) override
  {
    // Create endpoint information for the virtual device
    using namespace winrt::Windows::Devices::Midi2;
    using namespace winrt::Windows::Devices::Midi2::Enumeration;
    using namespace winrt::Windows::Devices::Midi2::Transports::Virtual;

    if (!m_session)
      return std::errc::not_connected;

    try
    {
      auto conf = setup_virtualdevice_config(configuration.client_name, port_name, port_name, MidiFunctionBlockDirection::BlockOutput);

      // A virtual port sends every message as given.
      set_group({});

      m_virtual = MidiVirtualDeviceManager::CreateVirtualDevice(conf);
      if (m_virtual == nullptr)
        return std::errc::device_or_resource_busy;

      m_endpoint = m_session.CreateEndpointConnection(m_virtual.DeviceEndpointDeviceId());
      if (!m_endpoint)
      {
        abandon_open();
        return std::errc::device_or_resource_busy;
      }

      (void)m_endpoint.AddMessageProcessingPlugin(m_virtual);

      if (!m_endpoint.Open())
      {
        abandon_open();
        return std::errc::io_error;
      }

      return stdx::error{};
    }
    catch (...)
    {
      libremidi_handle_error(
          this->configuration,
          "winmidi: cannot open the virtual output port: " + current_exception_message());
      abandon_open();
      return std::errc::io_error;
    }
  }
#endif

  stdx::error close_port() override
  {
    if(!m_endpoint)
      return std::errc::not_connected;

    try
    {
      m_session.DisconnectEndpointConnection(m_endpoint.ConnectionId());
      set_group({});
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
      if (m_virtual)
      {
        m_virtual.Cleanup();
        m_virtual = nullptr;
      }
  #endif
    }
    catch (...)
    {
      libremidi_handle_error(
          this->configuration, "winmidi: cannot close the output port: " + current_exception_message());
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  std::errc write_raw(const uint32_t* ump, int64_t bytes)
  {
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
    if (!m_raw_endpoint)
      return std::errc::not_connected;

    HRESULT ret{};
    switch (bytes / 4)
    {
      case 1:
        assert(m_raw_endpoint->ValidateBufferHasOnlyCompleteUmps(1, ump));
        ret = m_raw_endpoint->SendMidiMessagesRaw(0, 1, ump);
        break;
      case 2:
        assert(m_raw_endpoint->ValidateBufferHasOnlyCompleteUmps(2, ump));
        ret = m_raw_endpoint->SendMidiMessagesRaw(0, 2, ump);
        break;
      case 3:
        assert(m_raw_endpoint->ValidateBufferHasOnlyCompleteUmps(3, ump));
        ret = m_raw_endpoint->SendMidiMessagesRaw(0, 3, ump);
        break;
      case 4:
        assert(m_raw_endpoint->ValidateBufferHasOnlyCompleteUmps(4, ump));
        ret = m_raw_endpoint->SendMidiMessagesRaw(0, 4, ump);
        break;
      default:
        return std::errc::bad_message;
    }
    if(ret < 0)
      return std::errc::io_error;
    return std::errc{};
  #else
    return write(ump, bytes);
  #endif
  }

  std::errc write(const uint32_t* ump, int64_t bytes)
  {
    if (!m_endpoint)
      return std::errc::not_connected;

    MidiSendMessageResults ret{};
    try
    {
      switch (bytes / 4)
      {
        case 1:
          ret = m_endpoint.SendSingleMessagePacket(MidiMessage32(0, ump[0]));
          break;
        case 2:
          ret = m_endpoint.SendSingleMessagePacket(MidiMessage64(0, ump[0], ump[1]));
          break;
        case 3:
          ret = m_endpoint.SendSingleMessagePacket(MidiMessage96(0, ump[0], ump[1], ump[2]));
          break;
        case 4:
          ret = m_endpoint.SendSingleMessagePacket(
              MidiMessage128(0, ump[0], ump[1], ump[2], ump[3]));
          break;
        default:
          return std::errc::bad_message;
      }
    }
    catch (...)
    {
      return std::errc::io_error;
    }
    if (ret != MidiSendMessageResults::Succeeded)
      return std::errc::io_error;
    return std::errc{};
  }

  stdx::error send_ump(const uint32_t* message, size_t size) override
  {
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
    if(m_virtual)
    {
      // Virtual port does not support raw API per
      // https://github.com/celtera/libremidi/issues/194#issuecomment-4156127901

      return segment_ump_stream(message, size,
                                [this](const uint32_t* ump, int64_t bytes) -> std::errc {
        return write_with_group(
            m_group, ump, bytes,
            [this](const uint32_t* u, int64_t b) { return write(u, b); },
            [this](ump_output_group::action a) { report_group(a); });
      }, []() { });
    }
    else
#endif
    {
      return segment_ump_stream(message, size,
                                [this](const uint32_t* ump, int64_t bytes) -> std::errc {
        return write_with_group(
            m_group, ump, bytes,
            [this](const uint32_t* u, int64_t b) { return write_raw(u, b); },
            [this](ump_output_group::action a) { report_group(a); });
      }, []() { });
    }
  }

private:
  // The groups the endpoint receives from the host. A block's direction is
  // its own: BlockInput (or Bidirectional) receives. Function blocks when the
  // endpoint declares them, group terminal blocks otherwise; the two are never
  // merged.
  static uint16_t receivable_groups(const MidiEndpointDeviceInformation& ep)
  {
    uint16_t mask = 0;
    const auto fbs = ep.GetDeclaredFunctionBlocks();
    if (fbs && fbs.Size() > 0)
    {
      for (const auto& fb : fbs)
      {
        if (!fb)
          continue;
        const auto first = fb.FirstGroup();
        if (first && fb.Direction() != MidiFunctionBlockDirection::BlockOutput)
          mask = static_cast<uint16_t>(mask | ump_group_mask(first.Index(), fb.GroupCount()));
      }
    }
    else if (const auto gtbs = ep.GetGroupTerminalBlocks())
    {
      for (const auto& gtb : gtbs)
      {
        if (!gtb)
          continue;
        const auto first = gtb.FirstGroup();
        if (first && gtb.Direction() != MidiGroupTerminalBlockDirection::BlockOutput)
          mask = static_cast<uint16_t>(mask | ump_group_mask(first.Index(), gtb.GroupCount()));
      }
    }
    return mask;
  }

  // Applies a port's group to both send paths: UMP (send_ump) and MIDI 1.0
  // bytes (send_message converts them with this->converter, then calls
  // send_ump). Reports are re-armed for each open.
  void set_group(const ump_output_group& group)
  {
    m_group = group;
    this->converter.context.group = group.midi1_group();
    m_reported_restamp = false;
    m_reported_unverified = false;
  }

  // Each kind of group change is reported once per open.
  void report_group(ump_output_group::action a)
  {
    if (a == ump_output_group::action::restamp && !m_reported_restamp)
    {
      m_reported_restamp = true;
      libremidi_handle_warning(
          this->configuration,
          "winmidi: a message's group is outside the output port's block; it is sent on the "
          "block's first group (reported once per open)");
    }
    else if (a == ump_output_group::action::send_unverified && !m_reported_unverified)
    {
      m_reported_unverified = true;
      libremidi_handle_warning(
          this->configuration,
          "winmidi: a message's group is outside the output port's block, and no "
          "host-to-device block of the endpoint covers the block's first group; it is sent "
          "unchanged (reported once per open)");
    }
  }

  ump_output_group m_group{};
  bool m_reported_restamp{};
  bool m_reported_unverified{};

  //! Undoes a failed open: nothing is left registered with the service.
  void abandon_open() noexcept
  {
    try
    {
      if (m_endpoint)
        m_session.DisconnectEndpointConnection(m_endpoint.ConnectionId());
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
      if (m_virtual)
        m_virtual.Cleanup();
#endif
    }
    catch (...)
    {
    }

    m_endpoint = nullptr;
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
    m_raw_endpoint = nullptr;
#endif
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
    m_virtual = nullptr;
#endif
    set_group({});
  }

  MidiSession m_session{nullptr};
  winrt::Windows::Devices::Midi2::MidiEndpointConnection m_endpoint{nullptr};
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
  winrt::impl::com_ref<IMidiEndpointConnectionRaw> m_raw_endpoint{};
#endif
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
  winrt::Windows::Devices::Midi2::Transports::Virtual::MidiVirtualDevice m_virtual{nullptr};
#endif
};

}
