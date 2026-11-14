#include "registry.h"
#include "boot_plan.h"
#include "drivers.h"
#include "network.h"
#include "storage.h"
#include "win32.h"
#include "worker.h"

#include <algorithm>
#include <format>
#include <type_traits>

using namespace nic_onboard;

namespace {
	// the device setup class of network adapters. every adapter Windows knows has an instance below it: ...\0000, ...\0001
	constexpr auto network_class = std::wstring_view{ L"{4d36e972-e325-11ce-bfc1-08002be10318}" };
	// the registry WinPE itself runs with: the adapter works there, so its keys are the template for the offline Windows
	constexpr auto live_system = std::wstring_view{ L"SYSTEM\\CurrentControlSet" };

	void check(LSTATUS code, const std::wstring_view& operation)
	{
		if (code != ERROR_SUCCESS)
			throw system_error(operation, static_cast<std::uint32_t>(code));
	}

	// a key below HKEY_LOCAL_MACHINE
	class Key
	{
		struct Closer
		{
			void operator()(HKEY key) const noexcept { RegCloseKey(key); }
		};
		std::unique_ptr<std::remove_pointer_t<HKEY>, Closer> m_Key;

		explicit Key(HKEY key) : m_Key{ key } {}

	public:
		HKEY NativeHandle() const { return m_Key.get(); }

		static Key Open(const std::wstring& path, bool write = false)
		{
			auto key = HKEY{};
			::check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, write ? KEY_ALL_ACCESS : KEY_READ, &key), L"Open registry key " + path);
			return Key{ key };
		}

		static Key Create(const std::wstring& path)
		{
			auto key = HKEY{};
			::check(RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr), L"Create registry key " + path);
			return Key{ key };
		}

		static bool Exists(const std::wstring& path)
		{
			auto key = HKEY{};
			auto code = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
			if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
				return false;

			::check(code, L"Check registry key " + path);
			RegCloseKey(key);
			return true;
		}

		std::optional<std::wstring> GetString(const wchar_t* name) const
		{
			return read_registry_string(m_Key.get(), nullptr, name);
		}

		std::optional<DWORD> GetNumber(const wchar_t* name) const
		{
			auto value = DWORD{ 0 };
			auto size = DWORD{ sizeof(value) };
			auto code = RegGetValueW(m_Key.get(), nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
			if (code == ERROR_FILE_NOT_FOUND)
				return std::nullopt;

			::check(code, L"Read registry DWORD");
			return value;
		}

		void SetString(const wchar_t* name, const std::wstring& value) const
		{
			::check(RegSetValueExW(m_Key.get(), name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))), L"Write registry string");
		}

		void SetNumber(const wchar_t* name, DWORD value) const
		{
			::check(RegSetValueExW(m_Key.get(), name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)), L"Write registry DWORD");
		}

		std::vector<std::wstring> GetSubkeys() const
		{
			auto result = std::vector<std::wstring>{};
			for (auto index = DWORD{ 0 }; ; index++) {
				wchar_t name[256]{};
				auto length = static_cast<DWORD>(std::size(name));
				auto code = RegEnumKeyExW(m_Key.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
				if (code == ERROR_NO_MORE_ITEMS)
					break;

				::check(code, L"Enumerate registry subkeys");
				result.emplace_back(name, length);
			}

			return result;
		}

		// copies all values and subkeys of source into this key
		void CopyFrom(const Key& source) const
		{
			::check(RegCopyTreeW(source.m_Key.get(), nullptr, m_Key.get()), L"Copy registry tree");
		}

		void Flush() const
		{
			::check(RegFlushKey(m_Key.get()), L"Flush offline SYSTEM hive");
		}

		void SaveTo(const std::filesystem::path& file) const
		{
			::check(RegSaveKeyExW(m_Key.get(), file.c_str(), nullptr, REG_LATEST_FORMAT), L"Back up offline SYSTEM hive");
		}
	};

	std::wstring control_set(std::uint32_t number)
	{
		return std::format(L"ControlSet{:03}", number);
	}

	void enable_privilege(const wchar_t* name)
	{
		auto processToken = HANDLE{};
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &processToken))
			throw last_error(L"Open process privileges");

		auto token = Handle{ processToken };
		auto privileges = TOKEN_PRIVILEGES{};
		privileges.PrivilegeCount = 1;
		privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid))
			throw last_error(L"Look up registry privilege");

		// succeeds even if the privilege was not assigned: only the last error tells
		SetLastError(ERROR_SUCCESS);
		if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) || GetLastError() != ERROR_SUCCESS)
			throw last_error(L"Enable registry privilege");
	}

	// the SYSTEM hive of the offline Windows, loaded below HKEY_LOCAL_MACHINE for as long as this object lives
	class Hive
	{
		std::wstring m_Mount;
		bool m_Loaded = false;

	public:
		explicit Hive(const WindowsInstallation& windows)
			: m_Mount{ std::format(L"NicOnboard_{}_{}", GetCurrentProcessId(), GetTickCount64()) }
		{
			// loading a hive requires both
			::enable_privilege(SE_BACKUP_NAME);
			::enable_privilege(SE_RESTORE_NAME);
			::check(RegLoadKeyW(HKEY_LOCAL_MACHINE, m_Mount.c_str(), (windows.root / L"Windows/System32/Config/SYSTEM").c_str()), L"Load offline SYSTEM hive");
			m_Loaded = true;
		}

		~Hive()
		{
			if (m_Loaded)
				RegUnLoadKeyW(HKEY_LOCAL_MACHINE, m_Mount.c_str());
		}

		Hive(const Hive&) = delete;
		Hive& operator=(const Hive&) = delete;

		std::wstring Path(const std::wstring_view& relative = {}) const
		{
			return relative.empty() ? m_Mount : std::format(L"{}\\{}", m_Mount, relative);
		}

		// an offline hive has no CurrentControlSet link. Default selects the next normal boot.
		std::uint32_t GetBootControlSet() const
		{
			return read_boot_control_set(Key::Open(Path()).NativeHandle());
		}

		// writes the hive back to its file. unlike the destructor, this throws if that did not work
		void Unload()
		{
			// the temporary Key is closed again before the hive is unloaded: a hive with open keys stays loaded
			Key::Open(Path(), true).Flush();
			::check(RegUnLoadKeyW(HKEY_LOCAL_MACHINE, m_Mount.c_str()), L"Unload offline SYSTEM hive");
			m_Loaded = false;
		}
	};

	bool is_class_slot(const std::wstring_view& name)
	{
		return name.size() == 4 && std::ranges::all_of(name, [](wchar_t character) { return character >= L'0' && character <= L'9'; });
	}

	// "{4d36e972-e325-11ce-bfc1-08002be10318}\0003": how a device refers to its class instance
	std::wstring class_instance(std::uint32_t slot)
	{
		return std::format(L"{}\\{:04}", ::network_class, slot);
	}

	RegistrySnapshot inspect(const WindowsInstallation& windows, const NetworkAdapter& adapter)
	{
		auto hive = Hive{ windows };
		auto snapshot = RegistrySnapshot{};
		snapshot.boot_control_set = hive.GetBootControlSet();
		auto controlSet = hive.Path(::control_set(snapshot.boot_control_set));

		// the adapters the offline Windows knows, and which of them TCP/IP is bound to
		auto classes = std::format(L"{}\\Control\\Class\\{}", controlSet, ::network_class);
		for (const auto& slot : Key::Open(classes).GetSubkeys()) {
			if (!::is_class_slot(slot))
				continue;

			auto entry = Key::Open(classes + L"\\" + slot);
			auto guid = entry.GetString(L"NetCfgInstanceId").value_or(L"");
			snapshot.classes.push_back(NetworkClass{
				.slot = static_cast<std::uint32_t>(std::stoul(slot)),
				.guid = guid,
				.hardware_id = entry.GetString(L"MatchingDeviceId").value_or(L""),
				.tcpip_bound = !guid.empty() && Key::Exists(controlSet + L"\\Services\\Tcpip\\Parameters\\Interfaces\\" + guid)
			});
		}

		// this machine's adapter may have been onboarded before: then its device already points to a class instance
		auto device = controlSet + L"\\Enum\\" + adapter.instance;
		if (Key::Exists(device)) {
			auto driver = Key::Open(device).GetString(L"Driver").value_or(L"");
			auto prefix = std::wstring{ ::network_class } + L"\\";
			if (driver.size() == prefix.size() + 4 && equal_text(driver.substr(0, prefix.size()), prefix) && ::is_class_slot(driver.substr(prefix.size())))
				snapshot.existing_device_slot = static_cast<std::uint32_t>(std::stoul(driver.substr(prefix.size())));
		}

		auto installedService = controlSet + L"\\Services\\" + adapter.service;
		if (Key::Exists(installedService))
			snapshot.installed_service_image = Key::Open(installedService).GetString(L"ImagePath");

		auto liveService = Key::Open(std::format(L"{}\\Services\\{}", ::live_system, adapter.service));
		snapshot.live_service_image = liveService.GetString(L"ImagePath").value_or(L"");
		snapshot.live_boot_flags = liveService.GetNumber(L"BootFlags").value_or(0);

		hive.Unload();
		return snapshot;
	}

	void apply(const WindowsInstallation& windows, const NetworkAdapter& adapter, const BootPlan& plan)
	{
		auto hive = Hive{ windows };
		if (hive.GetBootControlSet() != plan.control_set)
			throw Error{ L"The selected offline control set changed before applying the boot plan." };

		// preserve the hive before our registry changes (DISM has already staged the driver). kept across later preparations
		auto backup = windows.root / L"Windows/System32/Config/SYSTEM.nic-onboard-before";
		if (!std::filesystem::exists(backup))
			Key::Open(hive.Path()).SaveTo(backup);

		auto controlSet = hive.Path(::control_set(plan.control_set));
		auto classInstance = ::class_instance(plan.class_slot);

		// this scope closes all keys again before the hive is unloaded
		{
			// the driver's service, loaded by the boot loader and started before the network stack needs it
			auto servicePath = controlSet + L"\\Services\\" + plan.service;
			auto service = Key::Create(servicePath);
			service.CopyFrom(Key::Open(std::format(L"{}\\Services\\{}", ::live_system, adapter.service)));
			service.SetString(L"ImagePath", plan.image_path);
			service.SetNumber(L"Start", plan.service_start);
			service.SetNumber(L"BootFlags", plan.boot_flags);
			if (Key::Exists(servicePath + L"\\StartOverride"))
				Key::Open(servicePath + L"\\StartOverride", true).SetNumber(L"0", plan.service_start);

			// the adapter's class instance, under the GUID which the TCP/IP configuration of the image is bound to
			auto adapterClass = Key::Create(controlSet + L"\\Control\\Class\\" + classInstance);
			adapterClass.CopyFrom(Key::Open(std::format(L"{}\\Control\\Class\\{}", ::live_system, adapter.class_key)));
			adapterClass.SetString(L"NetCfgInstanceId", plan.network_guid);
			adapterClass.SetString(L"InfPath", plan.published_inf);

			// the device itself: Windows finds it already installed, with this class instance and service
			auto device = Key::Create(controlSet + L"\\Enum\\" + plan.device_instance);
			device.CopyFrom(Key::Open(std::format(L"{}\\Enum\\{}", ::live_system, adapter.instance)));
			device.SetString(L"Driver", classInstance);
			device.SetString(L"Service", plan.service);
		}

		hive.Unload();
	}

	VerificationReport verify(const WindowsInstallation& windows, const BootPlan& plan)
	{
		auto hive = Hive{ windows };
		auto controlSet = hive.Path(::control_set(plan.control_set));
		auto classInstance = ::class_instance(plan.class_slot);

		auto report = VerificationReport{};
		report.control_set = hive.GetBootControlSet() == plan.control_set;

		// this scope closes all keys again before the hive is unloaded
		{
			auto devicePath = controlSet + L"\\Enum\\" + plan.device_instance;
			if (Key::Exists(devicePath)) {
				auto device = Key::Open(devicePath);
				report.device = equal_text(device.GetString(L"Driver").value_or(L""), classInstance)
					&& equal_text(device.GetString(L"Service").value_or(L""), plan.service);
			}

			auto classPath = controlSet + L"\\Control\\Class\\" + classInstance;
			if (Key::Exists(classPath)) {
				auto adapterClass = Key::Open(classPath);
				report.adapter_class = equal_text(adapterClass.GetString(L"NetCfgInstanceId").value_or(L""), plan.network_guid)
					&& equal_text(adapterClass.GetString(L"InfPath").value_or(L""), plan.published_inf);
			}

			auto servicePath = controlSet + L"\\Services\\" + plan.service;
			if (Key::Exists(servicePath)) {
				auto service = Key::Open(servicePath);
				report.service = service.GetNumber(L"Start") == plan.service_start
					&& service.GetNumber(L"BootFlags") == plan.boot_flags
					&& equal_text(service.GetString(L"ImagePath").value_or(L""), plan.image_path);
			}

			report.tcpip_binding = Key::Exists(controlSet + L"\\Services\\Tcpip\\Parameters\\Interfaces\\" + plan.network_guid);
			report.binary = std::filesystem::is_regular_file(driver_binary(windows, plan.image_path));
		}

		hive.Unload();
		return report;
	}
}

task<RegistrySnapshot> Registry::Inspect(const WindowsInstallation& windows, const NetworkAdapter& adapter)
{
	co_return co_await m_Worker.Run([&] { return ::inspect(windows, adapter); });
}

task<void> Registry::Apply(const WindowsInstallation& windows, const NetworkAdapter& adapter, const BootPlan& plan)
{
	co_await m_Worker.Run([&] { ::apply(windows, adapter, plan); });
}

task<VerificationReport> Registry::Verify(const WindowsInstallation& windows, const BootPlan& plan)
{
	co_return co_await m_Worker.Run([&] { return ::verify(windows, plan); });
}
