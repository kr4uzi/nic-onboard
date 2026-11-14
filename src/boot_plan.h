#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nic_onboard {
	struct DriverPackage;
	struct NetworkAdapter;
	struct RegistrySnapshot;
	struct WindowsInstallation;

	// the registry values that make the offline Windows start from the network with this machine's adapter
	struct BootPlan
	{
		std::uint32_t control_set = 0;
		std::uint32_t class_slot = 0;
		std::wstring network_guid;
		std::wstring device_instance;
		std::wstring service;
		std::wstring published_inf;
		std::wstring image_path;
		std::uint32_t service_start = 0;
		std::uint32_t boot_flags = 0;

		static BootPlan Create(const RegistrySnapshot& snapshot, const NetworkAdapter& adapter, const DriverPackage& package);
	};

	// these throw an Error when there is no candidate - or more than one: we never guess.
	// the MAC from the iBFT (if the firmware left one) names the boot adapter even among several connected ones
	WindowsInstallation choose_windows(const std::vector<WindowsInstallation>& installations);
	NetworkAdapter choose_adapter(const std::vector<NetworkAdapter>& adapters, const std::optional<std::wstring>& bootMac);
}
