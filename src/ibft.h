#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace nic_onboard {
	// the MAC address ("001122334455") of the adapter the firmware booted through, read from the
	// iSCSI Boot Firmware Table it left in memory. nothing if the table does not name one adapter
	std::optional<std::wstring> boot_mac_from_ibft(std::span<const std::byte> table);
}
