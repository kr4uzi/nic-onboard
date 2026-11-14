#include "win32.h"

#include <format>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace nic_onboard;

namespace {
	void check(bool condition, const char* expression)
	{
		if (!condition)
			throw std::runtime_error{ expression };
	}
#define CHECK(expression) ::check(static_cast<bool>(expression), #expression)

	void check_registry(LSTATUS code)
	{
		if (code != ERROR_SUCCESS)
			throw system_error(L"Create test registry data", code);
	}

	struct KeyCloser
	{
		void operator()(HKEY key) const { RegCloseKey(key); }
	};
	using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

	// A disposable key under the current user stands in for the loaded SYSTEM hive.
	// These tests need neither administrator privileges nor a real Windows installation.
	struct Hive
	{
		std::wstring path = std::format(L"Software\\NicOnboardTest_{}_{}", GetCurrentProcessId(), GetTickCount64());
		Key root;

		Hive()
		{
			auto key = HKEY{};
			::check_registry(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr));
			root.reset(key);
		}

		~Hive()
		{
			RegDeleteTreeW(root.get(), nullptr);
			root.reset();
			RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
		}

		Key Create(const wchar_t* path)
		{
			auto key = HKEY{};
			::check_registry(RegCreateKeyExW(root.get(), path, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr));
			return Key{ key };
		}

		void SetNumber(const wchar_t* name, DWORD value)
		{
			auto select = Create(L"Select");
			::check_registry(RegSetValueExW(select.get(), name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)));
		}
	};

	void rejects(auto&& operation)
	{
		try {
			operation();
		}
		catch (const Error&) {
			return;
		}
		throw std::runtime_error{ "Expected a registry error" };
	}

	void selects_the_next_boot()
	{
		auto hive = Hive{};
		hive.Create(L"ControlSet001");
		hive.Create(L"ControlSet002");
		hive.SetNumber(L"Current", 1);
		hive.SetNumber(L"Default", 2);
		CHECK(read_boot_control_set(hive.root.get()) == 2);

		hive.SetNumber(L"Current", 2);
		hive.SetNumber(L"Default", 1);
		CHECK(read_boot_control_set(hive.root.get()) == 1);

		for (auto invalid : { DWORD{ 0 }, DWORD{ 1000 }, DWORD{ 3 } }) {
			hive.SetNumber(L"Default", invalid);
			::rejects([&] { read_boot_control_set(hive.root.get()); });
		}

		// A missing Default must not fall back to Current, even when that set exists.
		auto select = hive.Create(L"Select");
		::check_registry(RegDeleteValueW(select.get(), L"Default"));
		::rejects([&] { read_boot_control_set(hive.root.get()); });
	}

	void reads_strings_without_expanding_them()
	{
		auto hive = Hive{};
		auto service = hive.Create(L"Service");
		auto image = std::wstring{ L"%SystemRoot%\\System32\\drivers\\nic.sys" };
		for (auto type : { REG_SZ, REG_EXPAND_SZ }) {
			::check_registry(RegSetValueExW(service.get(), L"ImagePath", 0, type, reinterpret_cast<const BYTE*>(image.c_str()), static_cast<DWORD>((image.size() + 1) * sizeof(wchar_t))));
			CHECK(read_registry_string(hive.root.get(), L"Service", L"ImagePath") == image);
			CHECK(read_registry_string(service.get(), nullptr, L"ImagePath") == image);
		}
		CHECK(!read_registry_string(service.get(), nullptr, L"Missing"));
		hive.SetNumber(L"Default", 1);
		::rejects([&] { read_registry_string(hive.root.get(), L"Select", L"Default"); });
	}
}

int main()
{
	try {
		::selects_the_next_boot();
		::reads_strings_without_expanding_them();
		std::cout << "boot control set and registry string tests passed\n";
	}
	catch (const std::exception& e) {
		std::cerr << "[failed] " << e.what() << '\n';
		return 1;
	}
}
