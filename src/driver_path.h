#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace nic_onboard {
	struct WindowsInstallation;

	// a service ImagePath relative to Windows, suitable for saving in the offline SYSTEM hive.
	// parses Windows paths on every host, so this policy is also testable on Linux
	std::wstring canonical_driver_image(std::wstring_view image);

	// where that ImagePath points to while the Windows installation is mounted offline
	std::filesystem::path driver_binary(const WindowsInstallation& windows, std::wstring_view image);
}
