#pragma once

// only for the .cpp files which talk to Windows: everything else stays free of <windows.h>
#include "error.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <windows.h>
#include <setupapi.h>

namespace nic_onboard {
	// "Open \\.\PhysicalDrive1: Access is denied. (0x00000005)"
	Error system_error(const std::wstring_view& operation, std::uint32_t code);
	Error last_error(const std::wstring_view& operation);

	bool equal_text(const std::wstring_view& left, const std::wstring_view& right);

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
