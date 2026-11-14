#include "ibft.h"
#include "network.h"
#include "win32.h"
#include "worker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <iphlpapi.h>

#ifdef __MINGW32__
// the mingw-w64 toolchain (which defines __MINGW32__ on every target) lacks this declaration in setupapi.h, its import library has the function
extern "C" WINSETUPAPI BOOL WINAPI SetupGetInfDriverStoreLocationW(PCWSTR FileName, PSP_ALTPLATFORM_INFO AlternatePlatformInfo, PCWSTR LocaleName, PWSTR ReturnBuffer, DWORD ReturnBufferSize, PDWORD RequiredSize);
#endif

using namespace nic_onboard;

namespace {
	// GUID_DEVCLASS_NET
	constexpr auto network_class = GUID{ 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
	constexpr auto class_key_prefix = std::wstring_view{ L"SYSTEM\\CurrentControlSet\\Control\\Class\\" };

	// a string property of a device, empty if the device does not have it
	std::wstring device_property(HDEVINFO devices, SP_DEVINFO_DATA& info, DWORD property)
	{
		auto type = DWORD{ 0 };
		auto size = DWORD{ 0 };
		if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, property, &type, nullptr, 0, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			if (GetLastError() == ERROR_INVALID_DATA)
				return {};

			throw last_error(L"Read adapter property size");
		}

		auto buffer = std::vector<wchar_t>(size / sizeof(wchar_t) + 1);
		if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, property, &type, reinterpret_cast<PBYTE>(buffer.data()), size, nullptr))
			throw last_error(L"Read adapter property");

		if (type != REG_SZ)
			throw Error{ L"The adapter returned an invalid property type." };

		return buffer.data();
	}

	std::wstring registry_string(const std::wstring& key, const wchar_t* name)
	{
		auto size = DWORD{ 0 };
		auto code = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size);
		if (code != ERROR_SUCCESS)
			throw system_error(L"Read adapter registry value", code);

		auto buffer = std::vector<wchar_t>(size / sizeof(wchar_t) + 1);
		code = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
		if (code != ERROR_SUCCESS)
			throw system_error(L"Read adapter registry value", code);

		return buffer.data();
	}

	std::wstring format_mac(const std::uint8_t* address, std::size_t length)
	{
		auto mac = std::wstring{};
		for (auto index = std::size_t{ 0 }; index < length; index++)
			mac += std::format(L"{:02X}", address[index]);

		return mac;
	}

	struct Connection
	{
		std::wstring guid;
		std::wstring mac;
	};

	// the ethernet interfaces which have a link right now
	std::vector<Connection> active_connections()
	{
		auto size = ULONG{ 16 * 1024 };
		auto buffer = std::vector<std::byte>(size);
		auto code = ULONG{ ERROR_BUFFER_OVERFLOW };
		for (auto attempt = 0; attempt < 4 && code == ERROR_BUFFER_OVERFLOW; attempt++) {
			buffer.resize(size);
			code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
		}

		if (code != ERROR_SUCCESS)
			throw system_error(L"Enumerate network connections", code);

		auto result = std::vector<Connection>{};
		for (auto item = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); item; item = item->Next) {
			if (item->OperStatus != IfOperStatusUp || item->PhysicalAddressLength != 6)
				continue;

			result.push_back(Connection{ .guid = widen(item->AdapterName), .mac = ::format_mac(item->PhysicalAddress, item->PhysicalAddressLength) });
		}

		return result;
	}

	// GetSystemFirmwareTable wants the four signature characters packed the way they lie in memory
	constexpr DWORD firmware_table_id(const std::string_view& signature)
	{
		auto id = DWORD{ 0 };
		for (auto index = std::size_t{ 0 }; index < 4; index++)
			id |= DWORD{ static_cast<unsigned char>(signature[index]) } << (8 * index);

		return id;
	}

	std::vector<std::byte> firmware_table(const std::string_view& signature)
	{
		// "ACPI" as the provider, but spelled big endian: that is how the documentation defines the provider values
		constexpr auto acpi = DWORD{ ('A' << 24) | ('C' << 16) | ('P' << 8) | 'I' };
		auto size = GetSystemFirmwareTable(acpi, ::firmware_table_id(signature), nullptr, 0);
		if (!size)
			return {};

		auto buffer = std::vector<std::byte>(size);
		auto written = GetSystemFirmwareTable(acpi, ::firmware_table_id(signature), buffer.data(), size);
		if (!written || written > size)
			throw last_error(L"Read the iSCSI boot firmware table");

		buffer.resize(written);
		return buffer;
	}

	std::optional<std::wstring> boot_mac()
	{
		// the original specification spells the signature "iBFT" (so does iPXE), ACPI standardized it as "IBFT"
		auto table = ::firmware_table("iBFT");
		if (table.empty())
			table = ::firmware_table("IBFT");

		if (table.empty())
			return std::nullopt;

		return boot_mac_from_ibft(table);
	}

	std::vector<NetworkAdapter> adapters()
	{
		auto connections = ::active_connections();

		auto deviceList = SetupDiGetClassDevsW(&::network_class, nullptr, nullptr, DIGCF_PRESENT);
		if (deviceList == INVALID_HANDLE_VALUE)
			throw last_error(L"Enumerate network adapters");

		auto devices = DeviceSet{ deviceList };
		auto result = std::vector<NetworkAdapter>{};
		for (auto index = DWORD{ 0 }; ; index++) {
			auto info = SP_DEVINFO_DATA{};
			info.cbSize = sizeof(info);
			if (!SetupDiEnumDeviceInfo(devices.get(), index, &info)) {
				if (GetLastError() == ERROR_NO_MORE_ITEMS)
					break;

				throw last_error(L"Enumerate network device");
			}

			wchar_t instanceId[512]{};
			if (!SetupDiGetDeviceInstanceIdW(devices.get(), &info, instanceId, static_cast<DWORD>(std::size(instanceId)), nullptr))
				throw last_error(L"Read NIC device instance");

			auto instance = std::wstring{ instanceId };
			if (!instance.starts_with(L"PCI\\") && !instance.starts_with(L"VMBUS\\") && !instance.starts_with(L"USB\\"))
				continue;

			// "{4d36e972-e325-11ce-bfc1-08002be10318}\0003": the adapter's instance below the network class key
			auto classKey = ::device_property(devices.get(), info, SPDRP_DRIVER);
			auto service = ::device_property(devices.get(), info, SPDRP_SERVICE);
			if (classKey.empty() || service.empty())
				continue;

			auto guid = ::registry_string(std::wstring{ ::class_key_prefix } + classKey, L"NetCfgInstanceId");
			auto connection = std::ranges::find_if(connections, [&](const auto& candidate) { return equal_text(candidate.guid, guid); });
			auto connected = connection != connections.end();

			auto name = ::device_property(devices.get(), info, SPDRP_FRIENDLYNAME);
			if (name.empty())
				name = ::device_property(devices.get(), info, SPDRP_DEVICEDESC);

			// oem3.inf -> X:\Windows\System32\DriverStore\FileRepository\e1d68x64.inf_amd64_...\e1d68x64.inf
			auto publishedInf = ::registry_string(std::wstring{ ::class_key_prefix } + classKey, L"InfPath");
			wchar_t inf[MAX_PATH]{};
			if (!SetupGetInfDriverStoreLocationW(publishedInf.c_str(), nullptr, nullptr, inf, static_cast<DWORD>(std::size(inf)), nullptr))
				throw last_error(L"Locate the running NIC's driver package");

			result.push_back(NetworkAdapter{
				.instance = std::move(instance),
				.name = std::move(name),
				.service = std::move(service),
				.class_key = std::move(classKey),
				.guid = std::move(guid),
				.mac = connected ? connection->mac : L"",
				.connected = connected,
				.inf = inf
			});
		}

		return result;
	}
}

task<std::vector<NetworkAdapter>> Network::GetAdapters()
{
	co_return co_await m_Worker.Run([] { return ::adapters(); });
}

task<std::optional<std::wstring>> Network::GetBootMac()
{
	co_return co_await m_Worker.Run([] { return ::boot_mac(); });
}
