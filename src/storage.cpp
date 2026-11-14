#include "storage.h"
#include "win32.h"
#include "worker.h"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <format>
#include <cfgmgr32.h>
#include <winioctl.h>

// some mingw-w64 releases lack the disk attribute declarations in winioctl.h
#ifndef IOCTL_DISK_GET_DISK_ATTRIBUTES
#define IOCTL_DISK_GET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS)
struct GET_DISK_ATTRIBUTES { ULONG Version; ULONG Reserved1; ULONGLONG Attributes; };
#endif
#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
struct SET_DISK_ATTRIBUTES { ULONG Version; BOOLEAN Persist; BOOLEAN Reserved1[3]; ULONGLONG Attributes; ULONGLONG AttributesMask; ULONG Reserved2[4]; };
#endif
#ifndef DISK_ATTRIBUTE_OFFLINE
#define DISK_ATTRIBUTE_OFFLINE 0x0000000000000001ULL
#endif
static_assert(sizeof(GET_DISK_ATTRIBUTES) == 16);
static_assert(sizeof(SET_DISK_ATTRIBUTES) == 40);

using namespace nic_onboard;

namespace {
	// GUID_DEVINTERFACE_DISK
	constexpr auto disk_interface = GUID{ 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

	Handle open_device(const std::wstring& path, DWORD access = 0)
	{
		auto device = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (device == INVALID_HANDLE_VALUE)
			throw last_error(L"Open " + path);

		return Handle{ device };
	}

	void control(HANDLE device, DWORD code, void* input, DWORD inputSize, void* output, DWORD outputSize, const std::wstring_view& operation)
	{
		auto returned = DWORD{ 0 };
		if (!DeviceIoControl(device, code, input, inputSize, output, outputSize, &returned, nullptr))
			throw last_error(operation);
	}

	bool is_iscsi(HANDLE disk)
	{
		auto query = STORAGE_PROPERTY_QUERY{};
		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;

		auto header = STORAGE_DESCRIPTOR_HEADER{};
		::control(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header), L"Query disk descriptor size");
		if (header.Size < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) || header.Size > 1024 * 1024)
			throw Error{ L"The disk returned an invalid storage descriptor." };

		auto buffer = std::vector<std::byte>(header.Size);
		::control(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(), static_cast<DWORD>(buffer.size()), L"Identify disk bus type");

		// a disk we cannot classify could be either: better stop than take the wrong one offline
		const auto& descriptor = *reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
		if (descriptor.BusType == BusTypeUnknown)
			throw Error{ L"A disk has an unknown bus type. Storage isolation was stopped." };

		return descriptor.BusType == BusTypeiScsi;
	}

	std::vector<Disk> disks()
	{
		auto deviceList = SetupDiGetClassDevsW(&::disk_interface, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
		if (deviceList == INVALID_HANDLE_VALUE)
			throw last_error(L"Enumerate disk interfaces");

		auto devices = DeviceSet{ deviceList };

		auto result = std::vector<Disk>{};
		for (auto index = DWORD{ 0 }; ; index++) {
			auto interfaceData = SP_DEVICE_INTERFACE_DATA{};
			interfaceData.cbSize = sizeof(interfaceData);
			if (!SetupDiEnumDeviceInterfaces(devices.get(), nullptr, &::disk_interface, index, &interfaceData)) {
				if (GetLastError() == ERROR_NO_MORE_ITEMS)
					break;

				throw last_error(L"Enumerate disk interface");
			}

			auto size = DWORD{ 0 };
			SetupDiGetDeviceInterfaceDetailW(devices.get(), &interfaceData, nullptr, 0, &size, nullptr);
			if (!size)
				throw last_error(L"Size disk interface details");

			auto buffer = std::vector<std::byte>(size);
			auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
			detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			auto info = SP_DEVINFO_DATA{};
			info.cbSize = sizeof(info);
			if (!SetupDiGetDeviceInterfaceDetailW(devices.get(), &interfaceData, detail, size, nullptr, &info))
				throw last_error(L"Read disk interface details");

			auto disk = ::open_device(detail->DevicePath);
			auto number = STORAGE_DEVICE_NUMBER{};
			::control(disk.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number), L"Read disk number");

			wchar_t instance[512]{};
			if (!SetupDiGetDeviceInstanceIdW(devices.get(), &info, instance, static_cast<DWORD>(std::size(instance)), nullptr))
				throw last_error(L"Read disk device instance");

			result.push_back(Disk{ .number = number.DeviceNumber, .instance = instance, .iscsi = ::is_iscsi(disk.get()) });
		}

		return result;
	}

	void isolate(const std::vector<Disk>& selected)
	{
		// the disks were listed a moment ago and plug and play may have moved on since:
		// check every one of them again before the first one is changed
		auto current = ::disks();
		for (const auto& disk : selected) {
			auto found = std::ranges::find_if(current, [&](const auto& candidate) {
				return candidate.number == disk.number && equal_text(candidate.instance, disk.instance);
			});

			if (disk.iscsi || found == current.end() || found->iscsi)
				throw Error{ L"Disk identity changed before isolation; no further disks were changed." };
		}

		for (const auto& disk : selected) {
			// offline: no volume of the disk gets mounted. not persisted, the disk is back after a restart
			auto device = ::open_device(std::format(L"\\\\.\\PhysicalDrive{}", disk.number), GENERIC_READ | GENERIC_WRITE);
			auto state = GET_DISK_ATTRIBUTES{};
			state.Version = sizeof(state);
			::control(device.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &state, sizeof(state), L"Read local disk attributes");
			if (!(state.Attributes & DISK_ATTRIBUTE_OFFLINE)) {
				auto attributes = SET_DISK_ATTRIBUTES{};
				attributes.Version = sizeof(attributes);
				attributes.Persist = FALSE;
				attributes.Attributes = DISK_ATTRIBUTE_OFFLINE;
				attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
				::control(device.get(), IOCTL_DISK_SET_DISK_ATTRIBUTES, &attributes, sizeof(attributes), nullptr, 0, L"Offline local disk");
			}

			device.reset();

			// and the device itself gets disabled for this session
			auto instance = disk.instance;
			auto node = DEVINST{};
			auto status = CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL);
			if (status != CR_SUCCESS)
				throw system_error(L"Locate local disk device", CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE));

			status = CM_Disable_DevNode(node, CM_DISABLE_UI_NOT_OK);
			if (status != CR_SUCCESS)
				throw system_error(L"Temporarily disable local disk", CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE));
		}
	}

	bool volume_is_iscsi(const std::filesystem::path& root)
	{
		auto drive = root.wstring();
		if (drive.size() != 3 || drive[1] != L':')
			return false;

		// a volume can span several disks: ask for all of them
		auto volume = ::open_device(L"\\\\.\\" + drive.substr(0, 2));
		auto buffer = std::vector<std::byte>(4096);
		auto returned = DWORD{ 0 };
		while (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
			if (GetLastError() != ERROR_MORE_DATA || buffer.size() >= 1024 * 1024)
				throw last_error(L"Find the physical disk behind " + drive);

			buffer.resize(buffer.size() * 2);
		}

		if (returned < offsetof(VOLUME_DISK_EXTENTS, Extents))
			throw Error{ L"The volume returned invalid disk extents." };

		const auto& extents = *reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
		if (!extents.NumberOfDiskExtents)
			return false;

		if (extents.NumberOfDiskExtents > (returned - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT))
			throw Error{ L"The volume returned truncated disk extents." };

		for (auto index = DWORD{ 0 }; index < extents.NumberOfDiskExtents; index++) {
			auto disk = ::open_device(std::format(L"\\\\.\\PhysicalDrive{}", extents.Extents[index].DiskNumber));
			if (!::is_iscsi(disk.get()))
				return false;
		}

		return true;
	}

	bool contains_windows(const std::filesystem::path& root)
	{
		// local volumes may keep their drive letter for a moment after the disk went offline:
		// a volume we cannot read simply has no Windows on it (and must not hide the iSCSI one by throwing)
		auto error = std::error_code{};
		if (!std::filesystem::is_regular_file(root / L"Windows/System32/Config/SYSTEM", error) || error)
			return false;

		return std::filesystem::is_regular_file(root / L"Windows/System32/ntoskrnl.exe", error) && !error;
	}

	std::vector<WindowsInstallation> find_installations()
	{
		// "C:\<null>D:\<null><null>"
		wchar_t drives[1024]{};
		auto length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
		if (!length || length >= std::size(drives))
			throw last_error(L"Enumerate volumes");

		auto result = std::vector<WindowsInstallation>{};
		for (const wchar_t* drive = drives; *drive; drive += std::wcslen(drive) + 1) {
			// X: is WinPE itself
			if (equal_text(drive, L"X:\\") || GetDriveTypeW(drive) != DRIVE_FIXED)
				continue;

			auto root = std::filesystem::path{ drive };
			if (::contains_windows(root))
				result.push_back(WindowsInstallation{ .root = root, .iscsi_backed = ::volume_is_iscsi(root) });
		}

		return result;
	}
}

task<std::vector<Disk>> Storage::GetDisks()
{
	co_return co_await m_Worker.Run([] { return ::disks(); });
}

task<void> Storage::IsolateDisks(const std::vector<Disk>& disks)
{
	co_await m_Worker.Run([&] { ::isolate(disks); });
}

task<std::vector<WindowsInstallation>> Storage::FindWindowsInstallations()
{
	co_return co_await m_Worker.Run([] { return ::find_installations(); });
}
