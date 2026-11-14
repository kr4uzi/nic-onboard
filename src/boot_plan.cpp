#include "boot_plan.h"
#include "drivers.h"
#include "error.h"
#include "network.h"
#include "registry.h"
#include "storage.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string_view>

using namespace nic_onboard;

namespace {
	// device and registry identifiers are ASCII and compare case-insensitively, whatever the locale is
	bool same_identifier(const std::wstring_view& left, const std::wstring_view& right)
	{
		constexpr auto upper = [](wchar_t character) {
			return character >= L'a' && character <= L'z' ? static_cast<wchar_t>(character - (L'a' - L'A')) : character;
		};

		return std::ranges::equal(left, right, {}, upper, upper);
	}

	// a real device like PCI\VEN_8086&DEV_15F3, as opposed to the software adapters of the ROOT and SWD enumerators
	bool is_physical(const std::wstring_view& deviceId)
	{
		constexpr auto buses = std::array<std::wstring_view, 3>{ L"PCI\\", L"VMBUS\\", L"USB\\" };
		return std::ranges::any_of(buses, [&](const auto& bus) {
			return ::same_identifier(deviceId.substr(0, bus.size()), bus);
		});
	}

	// "001122334455" -> "00:11:22:33:44:55"
	std::wstring pretty_mac(const std::wstring_view& mac)
	{
		auto result = std::wstring{};
		for (auto index = std::size_t{ 0 }; index < mac.size(); index += 2)
			result += std::format(L"{}{}", result.empty() ? L"" : L":", mac.substr(index, 2));

		return result;
	}
}

WindowsInstallation nic_onboard::choose_windows(const std::vector<WindowsInstallation>& installations)
{
	// X: is the ram disk WinPE itself runs from
	auto candidates = installations
		| std::views::filter([](const auto& windows) { return windows.iscsi_backed && !::same_identifier(windows.root.wstring(), L"X:\\"); })
		| std::ranges::to<std::vector>();

	if (candidates.empty())
		throw Error{ L"No matching Windows installation was found on an iSCSI disk." };

	if (candidates.size() > 1)
		throw Error{ L"Multiple Windows installations were found on iSCSI disks." };

	return candidates.front();
}

NetworkAdapter nic_onboard::choose_adapter(const std::vector<NetworkAdapter>& adapters, const std::optional<std::wstring>& bootMac)
{
	// the adapter we boot from is the one WinPE reaches the iSCSI target with: a real device, with a link and a driver
	auto candidates = adapters
		| std::views::filter([](const auto& adapter) {
			return adapter.connected && ::is_physical(adapter.instance)
				&& !adapter.service.empty() && !adapter.inf.empty() && !adapter.class_key.empty();
		})
		| std::ranges::to<std::vector>();

	// the firmware wrote the MAC of the adapter it booted through into the iBFT: that one, or nothing
	if (bootMac) {
		std::erase_if(candidates, [&](const auto& adapter) { return !::same_identifier(adapter.mac, *bootMac); });
		if (candidates.empty())
			throw Error{ std::format(L"The network adapter this machine booted from ({}) has no link or no working driver in WinPE.", ::pretty_mac(*bootMac)) };

		if (candidates.size() > 1)
			throw Error{ std::format(L"Multiple connected network adapters match the firmware boot MAC ({}).", ::pretty_mac(*bootMac)) };

		return candidates.front();
	}

	if (candidates.empty())
		throw Error{ L"No matching connected NIC with a working driver was found." };

	if (candidates.size() > 1)
		throw Error{ L"Multiple connected physical network adapters were found, and the firmware left no iBFT naming the boot adapter." };

	return candidates.front();
}

BootPlan BootPlan::Create(const RegistrySnapshot& snapshot, const NetworkAdapter& adapter, const DriverPackage& package)
{
	if (snapshot.boot_control_set == 0 || snapshot.boot_control_set > 999 || package.published_inf.empty())
		throw Error{ L"The offline control set or staged driver identity is invalid." };

	// the adapter the image was installed with owns the TCP/IP bindings Windows boots with, so this machine's
	// adapter takes over its GUID. adapters onboarded earlier did the same: several entries may share that GUID
	auto originals = std::vector<const NetworkClass*>{};
	for (const auto& entry : snapshot.classes) {
		if (!entry.tcpip_bound || entry.guid.empty() || !::is_physical(entry.hardware_id))
			continue;

		if (std::ranges::none_of(originals, [&](auto original) { return ::same_identifier(original->guid, entry.guid); }))
			originals.push_back(&entry);
	}

	// this machine's adapter was onboarded before: stay with the GUID and the slot it got back then
	auto existing = std::ranges::find_if(snapshot.classes, [&](const auto& entry) { return snapshot.existing_device_slot == entry.slot; });
	auto onboarded = existing != snapshot.classes.end();

	auto original = static_cast<const NetworkClass*>(nullptr);
	if (onboarded && existing->tcpip_bound) {
		auto match = std::ranges::find_if(originals, [&](auto candidate) { return ::same_identifier(candidate->guid, existing->guid); });
		if (match != originals.end())
			original = *match;
	}

	if (!original && originals.size() == 1)
		original = originals.front();

	if (!original)
		throw Error{ L"The original boot adapter with TCP/IP bindings is missing or ambiguous." };

	auto slot = onboarded ? existing->slot : std::uint32_t{ 0 };
	if (!onboarded) {
		while (std::ranges::contains(snapshot.classes, slot, &NetworkClass::slot))
			slot++;

		// the instances are named 0000 to 9999
		if (slot > 9999)
			throw Error{ L"No network class instance slot is available." };
	}

	return BootPlan{
		.control_set = snapshot.boot_control_set,
		.class_slot = slot,
		.network_guid = original->guid,
		.device_instance = adapter.instance,
		.service = adapter.service,
		.published_inf = package.published_inf,
		.image_path = {}, // known once the driver binary is in place
		.service_start = 0, // SERVICE_BOOT_START
		.boot_flags = snapshot.live_boot_flags | 1 // CM_SERVICE_NETWORK_BOOT_LOAD
	};
}
