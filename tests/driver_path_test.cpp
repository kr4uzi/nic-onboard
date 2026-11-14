#include "driver_path.h"
#include "error.h"
#include "storage.h"

#include <iostream>
#include <stdexcept>

using namespace nic_onboard;

namespace {
	void check(bool condition, const char* expression)
	{
		if (!condition)
			throw std::runtime_error{ expression };
	}
#define CHECK(expression) ::check(static_cast<bool>(expression), #expression)

	void source_paths_become_boot_paths()
	{
		const auto expected = std::wstring{ L"System32\\drivers\\nic.sys" };
		for (auto input : {
			L"System32\\drivers\\nic.sys",
			L"\\SystemRoot\\System32\\drivers\\nic.sys",
			L"%SystemRoot%\\System32\\drivers\\nic.sys",
			L"%windir%\\System32\\drivers\\nic.sys",
			L"X:\\Windows\\System32\\drivers\\nic.sys",
			L"\\??\\X:\\Windows\\System32\\drivers\\nic.sys",
			L"\"\\??\\C:\\Windows\\System32\\drivers\\nic.sys\"",
			L"x:/windows/system32/drivers/nic.sys",
			L"System32\\.\\drivers\\old\\..\\nic.sys"
		}) {
			auto saved = canonical_driver_image(input);
			CHECK(saved == expected);
			CHECK(canonical_driver_image(saved) == saved);
		}
	}

	void lookup_and_saved_value_name_the_same_offline_file()
	{
		// The source uses X:, the offline disk is mounted elsewhere, and its future boot drive is unknown.
		auto windows = WindowsInstallation{ .root = std::filesystem::path{ "offline-image" } };
		auto source = std::wstring{ L"\\??\\X:\\Windows\\System32\\drivers\\nic.sys" };
		auto saved = canonical_driver_image(source);
		auto expected = windows.root / "Windows" / "System32" / "drivers" / "nic.sys";
		CHECK(driver_binary(windows, source) == expected);
		CHECK(driver_binary(windows, saved) == expected);
		CHECK(saved.find(L'X') == saved.npos && saved.find(L':') == saved.npos);

		// An already-installed service also loses its absolute drive or expansion token when re-saved.
		CHECK(canonical_driver_image(L"C:\\Windows\\System32\\drivers\\nic.sys") == saved);
		CHECK(canonical_driver_image(L"%SystemRoot%\\System32\\drivers\\nic.sys") == saved);
	}

	void driver_store_and_unicode_names_survive()
	{
		auto expected = std::wstring{ L"System32\\DriverStore\\FileRepository\\nic.inf_amd64_123\\nic.sys" };
		CHECK(canonical_driver_image(L"\\SystemRoot\\System32\\DriverStore\\FileRepository\\nic.inf_amd64_123\\nic.sys") == expected);
		CHECK(canonical_driver_image(L"System32\\drivers\\r\u00e9seau.sys") == L"System32\\drivers\\r\u00e9seau.sys");
		CHECK(canonical_driver_image(L"SYSTEM32\\Drivers\\NIC.SYS") == L"System32\\Drivers\\NIC.SYS");
	}

	void refuses_ambiguous_or_escaping_paths()
	{
		for (auto input : {
			L"", L"\"\"", L"System32", L"System32\\drivers\\nic.dll",
			L"..\\System32\\drivers\\nic.sys",
			L"System32\\..\\elsewhere\\nic.sys",
			L"System32\\..\\System32\\drivers\\nic.sys",
			L"System32\\drivers\\..\\..\\System32\\drivers\\nic.sys",
			L"X:Windows\\System32\\drivers\\nic.sys",
			L"X:\\Elsewhere\\System32\\drivers\\nic.sys",
			L"\\Windows\\System32\\drivers\\nic.sys",
			L"\\\\server\\share\\Windows\\System32\\drivers\\nic.sys",
			L"\\??\\UNC\\server\\share\\Windows\\System32\\drivers\\nic.sys",
			L"\\??\\System32\\drivers\\nic.sys",
			L"\\SystemRoot\\X:\\Windows\\System32\\drivers\\nic.sys",
			L"%unknown%\\System32\\drivers\\nic.sys",
			L"System32\\drivers\\nic.sys:other.sys",
			L"System32\\drivers.\\nic.sys",
			L"System32\\drivers \\nic.sys",
			L"System32\\drivers\\nic.sys\\",
			L"System32\\drivers\\nic.sys\"",
			L"\"System32\\drivers\\nic.sys\" --argument"
		}) {
			auto rejected = false;
			try {
				canonical_driver_image(input);
			}
			catch (const Error&) {
				rejected = true;
			}
			CHECK(rejected);
		}
	}
}

int main()
{
	try {
		::source_paths_become_boot_paths();
		::lookup_and_saved_value_name_the_same_offline_file();
		::driver_store_and_unicode_names_survive();
		::refuses_ambiguous_or_escaping_paths();
		std::cout << "driver service path tests passed\n";
	}
	catch (const std::exception& e) {
		std::cerr << "[failed] " << e.what() << '\n';
		return 1;
	}
	return 0;
}
