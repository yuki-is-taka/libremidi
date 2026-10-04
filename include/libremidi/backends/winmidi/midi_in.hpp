#pragma once
#include <libremidi/backends/winmidi/config.hpp>
#include <libremidi/backends/winmidi/helpers.hpp>
#include <libremidi/backends/winmidi/observer.hpp>
#include <libremidi/backends/winmidi/ump_batch.hpp>
#include <libremidi/detail/midi_in.hpp>
#include <libremidi/detail/midi_stream_decoder.hpp>

NAMESPACE_LIBREMIDI::winmidi
{
class midi_in_impl final
    : public midi2::in_api
    , public error_handler
    , public winmidi_shared_data
{
public:
  struct
      : libremidi::ump_input_configuration
      , winmidi::input_configuration
  {
  } configuration;

#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
  struct raw_callback_type final : IMidiEndpointConnectionMessagesReceivedCallback
  {
    explicit raw_callback_type(midi_in_impl& self)
        : self{self}
    {

    }

    midi_in_impl& self;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
      if (!ppvObject)
        return E_POINTER;

      if (riid == __uuidof(IUnknown) ||
          riid == libremidi::IID_IMidiEndpointConnectionMessagesReceivedCallback)
      {
        *ppvObject = static_cast<IMidiEndpointConnectionMessagesReceivedCallback*>(this);
        AddRef();
        return S_OK;
      }

      *ppvObject = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE MessagesReceived(
        GUID sessionId,
        GUID connectionId,
        UINT64 timestamp,
        UINT32 wordCount,
        const UINT32* messages) override {
      HRESULT res{};
      self.process_message(sessionId, connectionId, timestamp, wordCount, messages);
      return res;
    }
  } raw_callback{*this};
#endif

  explicit midi_in_impl(
      libremidi::ump_input_configuration&& conf, winmidi::input_configuration&& apiconf)
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

    try
    {
      m_tick_frequency_hz = MidiClock::TimestampFrequency();
    }
    catch (...)
    {
      m_tick_frequency_hz = 0;
    }
    if (m_tick_frequency_hz == 0)
      m_tick_frequency_hz = 10'000'000; // 100 ns ticks
  }

  ~midi_in_impl() override
  {
    close_port();
    this->client_open_ = std::errc::not_connected;
  }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::WINDOWS_MIDI_SERVICES;
  }

  stdx::error open_port(const input_port& port, std::string_view) override
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

      // port.port is the Group Terminal Block Number(), which is not necessarily equal to
      // the UMP group index the block spans. For example a USB MIDI 1.0 device may expose
      // its input GTB as Number()==2 while its messages arrive on group 0. Using
      // `port.port - 1` as the group filter therefore silently drops all input from such
      // devices. Filter on the groups the resolved block actually spans instead:
      // [FirstGroup, FirstGroup + GroupCount).
      m_groups = {.enabled = true, .first = first_group.Index(), .count = gp.GroupCount()};

      // TODO use a MidiGroupEndpointListener for the filtering
      m_endpoint = m_session.CreateEndpointConnection(ep.EndpointDeviceId());
      if (!m_endpoint)
      {
        abandon_open();
        return std::errc::device_or_resource_busy;
      }

  #if !LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
      m_revoke_token = m_endpoint.MessageReceived(
          [this](
              const winrt::Windows::Devices::Midi2::IMidiMessageReceivedEventSource&,
              const winrt::Windows::Devices::Midi2::MidiMessageReceivedEventArgs& args) {
        process_message(args);
      });
  #else
      if (FAILED(m_endpoint.as(libremidi::IID_IMidiEndpointConnectionRaw, m_raw_endpoint.put_void()))
          || !m_raw_endpoint)
      {
        abandon_open();
        return std::errc::io_error;
      }

      // Must be installed before Open().
      if (FAILED(m_raw_endpoint->SetMessagesReceivedCallback(&raw_callback)))
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
          this->configuration, "winmidi: cannot open the input port: " + current_exception_message());
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
      auto conf = setup_virtualdevice_config(configuration.client_name, port_name, port_name, MidiFunctionBlockDirection::BlockInput);

      // A virtual port receives every group.
      m_groups = {};

      m_virtual = MidiVirtualDeviceManager::CreateVirtualDevice(conf);
      if (m_virtual == nullptr)
        return std::errc::device_or_resource_busy;

      // Create a connection to the device-side endpoint
      m_endpoint = m_session.CreateEndpointConnection(m_virtual.DeviceEndpointDeviceId());
      if (!m_endpoint)
      {
        abandon_open();
        return std::errc::device_or_resource_busy;
      }

      // Add the virtual device as a message processing plugin to receive messages
      (void)m_endpoint.AddMessageProcessingPlugin(m_virtual);

      // Register message received event handler
      m_revoke_token = m_endpoint.MessageReceived(
          [this](
              const winrt::Windows::Devices::Midi2::IMidiMessageReceivedEventSource&,
              const winrt::Windows::Devices::Midi2::MidiMessageReceivedEventArgs& args) {
        process_message(args);
      });

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
          "winmidi: cannot open the virtual input port: " + current_exception_message());
      abandon_open();
      return std::errc::io_error;
    }
  }
#endif

  // split in two so ticks * 1e9 can't overflow
  std::uint64_t ticks_to_ns(std::uint64_t ticks) const noexcept
  {
    constexpr std::uint64_t ns_per_s = 1'000'000'000ull;
    const std::uint64_t whole_seconds = ticks / m_tick_frequency_hz;
    const std::uint64_t remainder_ticks = ticks % m_tick_frequency_hz;
    return whole_seconds * ns_per_s + (remainder_ticks * ns_per_s) / m_tick_frequency_hz;
  }

  void process_message(const winrt::Windows::Devices::Midi2::MidiMessageReceivedEventArgs& msg)
  {
    const auto ump = msg.GetMessagePacket();
    if (!ump)
      return;
    const auto b = ump.GetAllWords();
    if (!b)
      return;

    uint32_t ump_space[64];
    array_view<uint32_t> ref{ump_space};
    const auto count = b.GetMany(0, ref);

    const auto ns = ticks_to_ns(ump.Timestamp());
    dispatch_ump_batch(m_processing, {ump_space, ump_space + count}, m_groups, [ns] { return ns; });
  }

#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
  void process_message(
      const GUID& /* sessionId */,
      const GUID& /* connectionId */,
      UINT64 timestamp,
      UINT32 wordCount,
      const UINT32* ump)
  {
    if (!ump || wordCount == 0)
      return;

    // One service timestamp per batch, converted once.
    const auto ns = ticks_to_ns(timestamp);
    dispatch_ump_batch(m_processing, {ump, ump + wordCount}, m_groups, [ns] { return ns; });
  }
#endif

  stdx::error close_port() override
  {
    if(!m_endpoint)
      return std::errc::not_connected;

    try
    {
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
      if(m_raw_endpoint) {
        m_raw_endpoint->RemoveMessagesReceivedCallback();
        m_raw_endpoint = nullptr;
      }
  #if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
      // Otherwise: only virtual ports go through revoke_token.
      else if(m_virtual)
        m_endpoint.MessageReceived(m_revoke_token);
  #endif
#else
      // When no raw API: everything goes through revoke_token.
      m_endpoint.MessageReceived(m_revoke_token);
#endif

      m_session.DisconnectEndpointConnection(m_endpoint.ConnectionId());
      m_groups = {};

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
          this->configuration, "winmidi: cannot close the input port: " + current_exception_message());
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  virtual timestamp absolute_timestamp() const noexcept override { return {}; }

private:
  //! Undoes a failed open: nothing is left registered with the service.
  void abandon_open() noexcept
  {
    try
    {
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
      if (m_raw_endpoint)
        m_raw_endpoint->RemoveMessagesReceivedCallback();
#endif
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
    m_groups = {};
  }

  MidiSession m_session{nullptr};
  winrt::event_token m_revoke_token{};
  winrt::Windows::Devices::Midi2::MidiEndpointConnection m_endpoint{nullptr};
#if LIBREMIDI_WINMIDI_HAS_COM_EXTENSIONS
  winrt::impl::com_ref<IMidiEndpointConnectionRaw> m_raw_endpoint{};
#endif
#if LIBREMIDI_WINMIDI_HAS_VIRTUAL_DEVICE
  winrt::Windows::Devices::Midi2::Transports::Virtual::MidiVirtualDevice m_virtual{nullptr};
#endif
  midi2::input_state_machine m_processing{this->configuration};
  ump_group_filter m_groups{};
  std::uint64_t m_tick_frequency_hz{};
};
}
