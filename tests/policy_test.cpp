#include "boot_plan.h"
#include "drivers.h"
#include "error.h"
#include "ibft.h"
#include "network.h"
#include "registry.h"
#include "storage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <print>
#include <stdexcept>

using namespace nic_onboard;

namespace {
	void check(bool condition, const char* expression)
	{
		if (!condition)
			throw std::runtime_error{ expression };
	}
#define CHECK(expression) ::check(static_cast<bool>(expression), #expression)

	// the action has to throw an Error whose message starts like this
	template<typename Action>
	void expect_error(const std::wstring_view& message, Action action)
	{
		auto failed = false;
		try {
			action();
		}
		catch (const Error& e) {
			failed = e.message().starts_with(message);
		}

		CHECK(failed);
	}

	NetworkAdapter adapter(std::wstring instance = L"PCI\\VEN_8086\\SLOT_1", std::wstring mac = L"001122334455")
	{
		return { std::move(instance), L"Test Ethernet", L"TestNic", L"{network-class}\\0003", L"{running-guid}", std::move(mac), true, L"X:\\Driver\\test.inf" };
	}

	RegistrySnapshot snapshot()
	{
		return { 2, { { 0, L"{original-guid}", L"PCI\\ORIGINAL", true } }, {}, {}, L"System32\\drivers\\test.sys", 4 };
	}

	const auto package = DriverPackage{ L"oem9.inf", L"W:\\DriverStore\\test" };

	void windows_selection()
	{
		// not the local one, and not WinPE's own X:
		CHECK(choose_windows({ { L"C:\\", false }, { L"x:\\", true }, { L"W:\\", true } }).root == L"W:\\");
		::expect_error(L"Multiple", [] { choose_windows({ { L"W:\\", true }, { L"V:\\", true } }); });
		::expect_error(L"No matching", [] { choose_windows({ { L"C:\\", false } }); });
	}

	void adapter_selection()
	{
		// without an iBFT the connected physical adapter has to be the only one
		auto first = ::adapter();
		auto second = ::adapter(L"PCI\\VEN_8086\\SLOT_2", L"AABBCCDDEEFF");
		::expect_error(L"Multiple", [&] { choose_adapter({ first, second }, std::nullopt); });
		CHECK(choose_adapter({ ::adapter(L"pci\\ven_8086\\slot_1") }, std::nullopt).instance.ends_with(L"slot_1"));

		auto disconnected = ::adapter();
		disconnected.connected = false;
		CHECK(choose_adapter({ disconnected, second, ::adapter(L"ROOT\\VIRTUAL") }, std::nullopt).instance.ends_with(L"SLOT_2"));
		::expect_error(L"No matching", [&] { choose_adapter({ disconnected }, std::nullopt); });

		auto withoutDriver = ::adapter();
		withoutDriver.inf.clear();
		::expect_error(L"No matching", [&] { choose_adapter({ withoutDriver }, std::nullopt); });

		// the iBFT names the boot adapter among several, whatever the case of its MAC
		CHECK(choose_adapter({ first, second }, L"aabbccddeeff").instance.ends_with(L"SLOT_2"));
		CHECK(choose_adapter({ first, second }, L"001122334455").instance.ends_with(L"SLOT_1"));

		// and it is binding: a boot adapter WinPE cannot use is not replaced by another one
		::expect_error(L"The network adapter this machine booted from (AA:BB:CC:DD:EE:FF)", [&] { choose_adapter({ first }, L"AABBCCDDEEFF"); });
		::expect_error(L"The network adapter this machine booted from", [&] { choose_adapter({ disconnected, second }, L"001122334455"); });
	}

	void boot_plan()
	{
		// a new adapter: the next free slot, the GUID of the original adapter, loaded at boot
		auto observed = ::snapshot();
		auto plan = BootPlan::Create(observed, ::adapter(), ::package);
		CHECK(plan.control_set == 2 && plan.class_slot == 1);
		CHECK(plan.network_guid == L"{original-guid}" && plan.published_inf == L"oem9.inf");
		CHECK(plan.device_instance == L"PCI\\VEN_8086\\SLOT_1" && plan.service == L"TestNic");
		CHECK(plan.boot_flags == 5 && plan.service_start == 0);

		// onboarded before: the slot is used again
		observed.classes.push_back({ 7, L"{ORIGINAL-GUID}", L"PCI\\ONBOARDED", true });
		observed.existing_device_slot = 7;
		plan = BootPlan::Create(observed, ::adapter(), ::package);
		CHECK(plan.class_slot == 7);

		// two different bound adapters, and nothing tells which one is the original
		observed.existing_device_slot.reset();
		observed.classes.push_back({ 3, L"{other-guid}", L"PCI\\OTHER", true });
		::expect_error(L"The original boot adapter", [&] { BootPlan::Create(observed, ::adapter(), ::package); });

		// unless this machine's adapter already is one of them
		observed.existing_device_slot = 3;
		plan = BootPlan::Create(observed, ::adapter(), ::package);
		CHECK(plan.network_guid == L"{other-guid}" && plan.class_slot == 3);

		for (auto& entry : observed.classes)
			entry.tcpip_bound = false;
		::expect_error(L"The original boot adapter", [&] { BootPlan::Create(observed, ::adapter(), ::package); });

		observed.current_control_set = 0;
		::expect_error(L"The offline control set", [&] { BootPlan::Create(observed, ::adapter(), ::package); });
	}

	// an iBFT as iPXE writes it: ACPI header, control block with the fixed four offsets, then the NIC blocks
	struct Ibft
	{
		std::vector<std::byte> bytes = std::vector<std::byte>(48 + 18);

		Ibft()
		{
			// control block: id 1, length 18 (header, extensions, initiator, nic0, tgt0, nic1, tgt1)
			Put(48, std::uint8_t{ 1 });
			Put(50, std::uint16_t{ 18 });
		}

		template<typename T>
		void Put(std::size_t offset, const T& value)
		{
			if (offset + sizeof(value) > bytes.size())
				bytes.resize(offset + sizeof(value));

			std::memcpy(bytes.data() + offset, &value, sizeof(value));
		}

		// appends a NIC block with these flags and MAC, linked from control slot 0 (nic0) or 1 (nic1)
		void AddNic(std::size_t slot, std::uint8_t flags, const std::array<std::uint8_t, 6>& mac)
		{
			auto offset = static_cast<std::uint16_t>(bytes.size());
			bytes.resize(offset + 102);
			Put(offset, std::uint8_t{ 3 });
			Put(offset + 2, std::uint16_t{ 102 });
			Put(offset + 5, flags);
			Put(offset + 90, mac);
			Put(56 + 4 * slot + 2, offset);
		}
	};

	void ibft_parsing()
	{
		constexpr auto first = std::array<std::uint8_t, 6>{ 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
		constexpr auto second = std::array<std::uint8_t, 6>{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

		// two NICs, the second one booted
		auto table = Ibft{};
		table.AddNic(0, 1, first);
		table.AddNic(1, 1 | 2, second);
		CHECK(boot_mac_from_ibft(table.bytes) == L"AABBCCDDEEFF");

		// a single valid NIC without the boot flag counts, an invalid block does not
		table = Ibft{};
		table.AddNic(0, 0, first);
		table.AddNic(1, 1, second);
		CHECK(boot_mac_from_ibft(table.bytes) == L"AABBCCDDEEFF");

		// two valid NICs and no boot flag: nothing to go by
		table = Ibft{};
		table.AddNic(0, 1, first);
		table.AddNic(1, 1, second);
		CHECK(!boot_mac_from_ibft(table.bytes));

		// no NIC at all, and a table too short to hold a control block
		CHECK(!boot_mac_from_ibft(Ibft{}.bytes));
		::expect_error(L"The iSCSI boot firmware table", [] { boot_mac_from_ibft(std::vector<std::byte>(40)); });
	}

	void verification()
	{
		const auto valid = VerificationReport{ true, true, true, true, true, true };
		CHECK(valid.passed());

		for (auto field : { &VerificationReport::control_set, &VerificationReport::device, &VerificationReport::adapter_class, &VerificationReport::service, &VerificationReport::tcpip_binding, &VerificationReport::binary }) {
			auto invalid = valid;
			invalid.*field = false;
			CHECK(!invalid.passed());
		}
	}
}

int main()
{
	try {
		::windows_selection();
		::adapter_selection();
		::boot_plan();
		::ibft_parsing();
		::verification();
		std::println("windows, adapter and boot plan tests passed");
	}
	catch (const std::exception& e) {
		std::println(std::cerr, "[failed] {}", e.what());
		return 1;
	}

	return 0;
}
