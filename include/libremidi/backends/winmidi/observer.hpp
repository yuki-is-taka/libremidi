#pragma once
#include <optional>
#include <libremidi/backends/winmidi/config.hpp>
#include <libremidi/backends/winmidi/helpers.hpp>
#include <libremidi/detail/observer.hpp>

NAMESPACE_LIBREMIDI::winmidi
{
struct port_info
{
  winrt::hstring id;
  winrt::hstring name;
};

class observer_impl final
    : public observer_api
    , public error_handler
    , public winmidi_shared_data
{
public:
  struct
      : libremidi::observer_configuration
      , winmidi::observer_configuration
  {
  } configuration;

  MidiEndpointDeviceWatcher watcher = nullptr;
  MidiEndpointDeviceWatcher::Added_revoker m_addHandler;
  MidiEndpointDeviceWatcher::Updated_revoker m_updHandler;
  MidiEndpointDeviceWatcher::Removed_revoker m_delHandler;
  std::map<winrt::hstring, std::vector<input_port>> m_known_input_devices;
  std::map<winrt::hstring, std::vector<output_port>> m_known_output_devices;
  std::mutex m_devices_mtx;
  std::atomic_bool m_in_constructor{true};

  explicit observer_impl(
      libremidi::observer_configuration&& conf, winmidi::observer_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    struct on_delete { observer_impl& self; ~on_delete() { self.m_in_constructor = false; } } on_delete{*this};

    if (!configuration.has_callbacks())
      return;

    // Without Windows MIDI Services there is nothing to watch: stay without a watcher.
    if (!this->self->ready)
    {
      libremidi_handle_error(configuration, "winmidi: Windows MIDI Services is not available");
      return;
    }

    // Note: winmidi also notifies for existing devices, so the "notify in constructor"
    // is handled in the callbacks
    try
    {
      watcher = MidiEndpointDeviceWatcher::Create();
      if (!watcher)
      {
        libremidi_handle_error(configuration, "winmidi: cannot create the endpoint device watcher");
        return;
      }

      auto addHandler = foundation::TypedEventHandler<
          MidiEndpointDeviceWatcher, MidiEndpointDeviceInformationAddedEventArgs>(
          this, &observer_impl::on_device_added);
      auto updHandler = foundation::TypedEventHandler<
          MidiEndpointDeviceWatcher, MidiEndpointDeviceInformationUpdatedEventArgs>(
          this, &observer_impl::on_device_updated);
      auto delHandler = foundation::TypedEventHandler<
          MidiEndpointDeviceWatcher, MidiEndpointDeviceInformationRemovedEventArgs>(
          this, &observer_impl::on_device_removed);

      m_addHandler = watcher.Added(winrt::auto_revoke, addHandler);
      m_updHandler = watcher.Updated(winrt::auto_revoke, updHandler);
      m_delHandler = watcher.Removed(winrt::auto_revoke, delHandler);

      watcher.Start();
    }
    catch (...)
    {
      // Leave the observer without a watcher: no notifications, polling still works.
      libremidi_handle_error(
          configuration,
          "winmidi: cannot start the endpoint device watcher: " + current_exception_message());
      m_addHandler.revoke();
      m_updHandler.revoke();
      m_delHandler.revoke();
      watcher = nullptr;
    }
  }

  ~observer_impl()
  {
    if (!configuration.has_callbacks())
      return;

    m_addHandler.revoke();
    m_updHandler.revoke();
    m_delHandler.revoke();
  }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::WINDOWS_MIDI_SERVICES;
  }

  static transport_type code_to_type(std::string_view str) noexcept
  {
    using enum transport_type;

    if (str.starts_with("KS"))
      return hardware;
    if (str == "BLE")
      return transport_type(hardware | bluetooth);
    if (str == "VPB" || str == "APP")
      return software;
    if (str == "LOOP")
      return transport_type(software | loopback);
    if (str.starts_with("NET"))
      return network;
    return unknown;
  }

  // gp can be either MidiGroupTerminalBlock or MidiFunctionBlock
  template <bool Input>
  auto to_port_info(const MidiEndpointDeviceInformation& p, const auto& gp)
      const noexcept -> std::conditional_t<Input, input_port, output_port>
  {
    // The projection answers a failed call with null.
    const auto tinfo = p.GetTransportSuppliedInfo();

    return {
        {.api = libremidi::API::WINDOWS_MIDI_SERVICES,
         .client = 0,
         .container = std::bit_cast<libremidi::uuid>(p.ContainerId()),
         .device = to_string(p.EndpointDeviceId()),
         .port = gp.Number(),
         .manufacturer = tinfo ? to_string(tinfo.ManufacturerName()) : std::string{},
         .product = tinfo ? to_string(tinfo.Name()) : std::string{},
         .serial = tinfo ? to_string(tinfo.SerialNumber()) : std::string{},
         .device_name = to_string(p.Name()),
         .port_name = to_string(gp.Name()),
         .display_name = to_string(gp.Name()) + " " + std::to_string(gp.Number()),
         .type = tinfo ? code_to_type(to_string(tinfo.TransportCode())) : transport_type::unknown}};
  }

  //! The transport comes from the endpoint's transport code, so it is only
  //! known once the port has been built.
  template <bool Input>
  auto wanted_port(const MidiEndpointDeviceInformation& p, const auto& gp) const noexcept
      -> std::optional<std::conditional_t<Input, input_port, output_port>>
  {
    auto info = to_port_info<Input>(p, gp);

    // An unrecognised transport code still names a real endpoint, so it is
    // filtered as hardware: `unknown` is only selected by track_any.
    auto transport = info.type;
    if (transport == transport_type::unknown)
      transport = transport_type::hardware;

    if (!this->configuration.accepts(transport))
      return std::nullopt;
    return info;
  }

  std::vector<libremidi::input_port> get_input_ports() const noexcept override
  {
    std::vector<libremidi::input_port> ret;

    try
    {
      const auto eps = MidiEndpointDeviceInformation::FindAll();
      if (!eps)
        return ret;

      for (const auto& ep : eps)
      {
        if (!ep || ep.Name().starts_with(L"Diagnostics"))
        {
          continue;
        }

        if (const auto fbs = ep.GetDeclaredFunctionBlocks())
        {
          for (const auto& gp : fbs)
          {
            if (gp && gp.Direction() != MidiFunctionBlockDirection::BlockOutput)
              if (auto p = wanted_port<true>(ep, gp))
                ret.emplace_back(std::move(*p));
          }
        }

        if (const auto gtbs = ep.GetGroupTerminalBlocks())
        {
          for (const auto& gp : gtbs)
          {
            if (gp && gp.Direction() != MidiGroupTerminalBlockDirection::BlockOutput)
              if (auto p = wanted_port<true>(ep, gp))
                ret.emplace_back(std::move(*p));
          }
        }
      }
    }
    catch (...)
    {
      // Return what was collected.
    }

    return ret;
  }

  std::vector<libremidi::output_port> get_output_ports() const noexcept override
  {
    std::vector<libremidi::output_port> ret;

    try
    {
      const auto eps = MidiEndpointDeviceInformation::FindAll();
      if (!eps)
        return ret;

      for (const auto& ep : eps)
      {
        if (!ep || ep.Name().starts_with(L"Diagnostics"))
        {
          continue;
        }

        if (const auto fbs = ep.GetDeclaredFunctionBlocks())
        {
          for (const auto& gp : fbs)
          {
            if (gp && gp.Direction() != MidiFunctionBlockDirection::BlockInput)
              if (auto p = wanted_port<false>(ep, gp))
                ret.emplace_back(std::move(*p));
          }
        }

        if (const auto gtbs = ep.GetGroupTerminalBlocks())
        {
          for (const auto& gp : gtbs)
          {
            if (gp && gp.Direction() != MidiGroupTerminalBlockDirection::BlockInput)
              if (auto p = wanted_port<false>(ep, gp))
                ret.emplace_back(std::move(*p));
          }
        }
      }
    }
    catch (...)
    {
      // Return what was collected.
    }

    return ret;
  }

  // Note: these callbacks are called from some random thread!
  void on_device_added(
      const MidiEndpointDeviceWatcher&, const MidiEndpointDeviceInformationAddedEventArgs& result)
  {
    try
    {
      add_device(result.AddedDevice());
    }
    catch (...)
    {
      report_notification_failure();
    }
  }

  void on_device_updated(
      const MidiEndpointDeviceWatcher&,
      const MidiEndpointDeviceInformationUpdatedEventArgs& result)
  {
    try
    {
      // The event hands over the device itself.
      const auto dev = result.UpdatedDevice();
      if (!dev)
        return;

      // OPTIMIZEME
      remove_device(dev.EndpointDeviceId());
      add_device(dev);
    }
    catch (...)
    {
      report_notification_failure();
    }
  }

  void on_device_removed(
      const MidiEndpointDeviceWatcher&,
      const MidiEndpointDeviceInformationRemovedEventArgs& result)
  {
    try
    {
      if (const auto dev = result.RemovedDevice())
        remove_device(dev.EndpointDeviceId());
    }
    catch (...)
    {
      report_notification_failure();
    }
  }

  void report_notification_failure() const noexcept
  {
    try
    {
      libremidi_handle_error(
          configuration,
          "winmidi: failed to handle a device notification: " + current_exception_message());
    }
    catch (...)
    {
    }
  }

  void add_block(const MidiEndpointDeviceInformation& ep, const auto& gp)
  {
    const auto add_input = [&] {
      if (configuration.input_added)
      {
        auto ip = to_port_info<true>(ep, gp);
        {
          std::lock_guard _{m_devices_mtx};
          m_known_input_devices[ep.EndpointDeviceId()].push_back(ip);
        }

        if(!m_in_constructor || configuration.notify_in_constructor)
          configuration.input_added(std::move(ip));
      }
    };
    auto add_output = [&] {
      if (configuration.output_added)
      {
        auto op = to_port_info<false>(ep, gp);
        {
          std::lock_guard _{m_devices_mtx};
          m_known_output_devices[ep.EndpointDeviceId()].push_back(op);
        }

        if(!m_in_constructor || configuration.notify_in_constructor)
          configuration.output_added(std::move(op));
      }
    };

    using direction_type = decltype(gp.Direction());
    switch (gp.Direction())
    {
      case direction_type::Bidirectional:
        add_input();
        add_output();
        break;
      case direction_type::BlockInput:
        add_input();
        break;
      case direction_type::BlockOutput:
          add_output();
        break;
      default:
        // Undefined direction, does it make sense to handle it somewhere?
        break;
    }
  }

  void add_device(const MidiEndpointDeviceInformation& ep)
  {
    if (!ep)
      return;

    if (const auto fbs = ep.GetDeclaredFunctionBlocks())
    {
      for (const auto& fb : fbs)
      {
        if (fb)
          add_block(ep, fb);
      }
    }
    if (const auto gtbs = ep.GetGroupTerminalBlocks())
    {
      for (const auto& gp : gtbs)
      {
        if (gp)
          add_block(ep, gp);
      }
    }
  }

  void remove_device(winrt::hstring eid)
  {
    std::vector<input_port> to_remove_in;
    std::vector<output_port> to_remove_out;

    {
      std::lock_guard _{m_devices_mtx};
      if (auto it = m_known_input_devices.find(eid); it != m_known_input_devices.end())
      {
        for (auto& ip : it->second)
        {
          to_remove_in.push_back(ip);
        }
        m_known_input_devices.erase(it);
      }
      if (auto it = m_known_output_devices.find(eid); it != m_known_output_devices.end())
      {
        for (auto& op : it->second)
        {
          to_remove_out.push_back(op);
        }
        m_known_output_devices.erase(it);
      }
    }

    if (configuration.input_removed)
      for (auto& port : to_remove_in)
        configuration.input_removed(port);
    if (configuration.output_removed)
      for (auto& port : to_remove_out)
        configuration.output_removed(port);
  }
};

}
