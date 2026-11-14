#pragma once

#include "task.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nic_onboard {
	class Worker;
	struct BootPlan;
	struct NetworkAdapter;
	struct WindowsInstallation;

	// one instance below the network adapter class key (Control\Class\{4d36e972-...}\0000, 0001, ...)
	struct NetworkClass
	{
		std::uint32_t slot = 0;
		std::wstring guid;
		std::wstring hardware_id;
		bool tcpip_bound = false;
	};

	// everything the boot plan is decided from: the offline SYSTEM hive, plus the adapter's service as WinPE runs it
	struct RegistrySnapshot
	{
		std::uint32_t boot_control_set = 0;
		std::vector<NetworkClass> classes;
		std::optional<std::uint32_t> existing_device_slot;
		std::optional<std::wstring> installed_service_image;
		std::wstring live_service_image;
		std::uint32_t live_boot_flags = 0;
	};

	struct VerificationReport
	{
		bool control_set = false;
		bool device = false;
		bool adapter_class = false;
		bool service = false;
		bool tcpip_binding = false;
		bool binary = false;

		bool passed() const { return control_set && device && adapter_class && service && tcpip_binding && binary; }
	};

	// the SYSTEM hive of the offline Windows
	class Registry
	{
		Worker& m_Worker;

	public:
		explicit Registry(Worker& worker) : m_Worker{ worker } {}

		task<RegistrySnapshot> Inspect(const WindowsInstallation& windows, const NetworkAdapter& adapter);
		task<void> Apply(const WindowsInstallation& windows, const NetworkAdapter& adapter, const BootPlan& plan);
		task<VerificationReport> Verify(const WindowsInstallation& windows, const BootPlan& plan);
	};
}
