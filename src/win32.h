#pragma once

// only for the .cpp files which talk to Windows: everything else stays free of <windows.h>
#include "error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <windows.h>
#include <setupapi.h>

namespace nic_onboard {
	// "Open \\.\PhysicalDrive1: Access is denied. (0x00000005)"
	Error system_error(const std::wstring_view& operation, std::uint32_t code);
	Error last_error(const std::wstring_view& operation);

	bool equal_text(const std::wstring_view& left, const std::wstring_view& right);

	// Registry strings stay unexpanded: paths in an offline hive refer to that Windows, not to WinPE.
	std::optional<std::wstring> read_registry_string(HKEY key, const wchar_t* subkey, const wchar_t* name);
	// Select\Default is used at the next normal boot; Select\Current describes the previous boot.
	std::uint32_t read_boot_control_set(HKEY systemHive);

	struct HandleCloser
	{
		void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
	};
	using Handle = std::unique_ptr<void, HandleCloser>;

	struct DeviceSetCloser
	{
		void operator()(HDEVINFO set) const noexcept { SetupDiDestroyDeviceInfoList(set); }
	};
	using DeviceSet = std::unique_ptr<void, DeviceSetCloser>;
}
