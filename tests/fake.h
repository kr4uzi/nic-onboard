#pragma once

#include "boot_plan.h"
#include "drivers.h"
#include "network.h"
#include "registry.h"
#include "storage.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

// the tests do not mock anything: they link the real nic_onboard.cpp against fake.cpp instead of the Windows code.
// fake.cpp implements the very same classes (Window, WinPE, Storage, Network, Drivers, Registry) on top of this
// description of a machine, and writes down what the application did with it
namespace nic_onboard {
	struct FakeMachine
	{
		enum class RowState { waiting, running, completed, failed };
		struct Row
		{
			RowState state = RowState::waiting;
			std::wstring message;
		};

		// what the machine looks like
		std::vector<Disk> disks{ { 0, L"local", false }, { 1, L"iscsi", true } };
		// the iSCSI disk is missing in the first few answers of GetDisks
		int polls_without_iscsi = 0;
		std::vector<WindowsInstallation> installations{ { L"W:\\", true } };
		std::vector<NetworkAdapter> adapters{ { L"PCI\\VEN_8086\\SLOT_1", L"Test Ethernet", L"TestNic", L"{network-class}\\0003", L"{running-guid}", L"001122334455", true, L"X:\\Driver\\test.inf" } };
		// what the iBFT says, nothing if the firmware left none
		std::optional<std::wstring> boot_mac{ L"001122334455" };
		RegistrySnapshot registry{ 2, { { 0, L"{original-guid}", L"PCI\\ORIGINAL", true } }, {}, {}, L"System32\\drivers\\test.sys", 4 };
		VerificationReport verification{ true, true, true, true, true, true };

		// what goes wrong: the call (as it is named in `calls`) which throws an Error, or something unexpected
		std::optional<std::string> fail_at;
		std::optional<std::string> crash_at;
		// the call during which the window goes away
		std::optional<std::string> window_breaks_at;
		int failing_reboots = 0;

		// what the user does: one entry per WaitForButton, true clicks the button and false closes the window
		std::deque<bool> clicks;

		// what the application did
		std::vector<std::string> calls;
		std::vector<Disk> isolated;
		std::optional<BootPlan> applied;
		int reboots = 0;

		// what the window showed
		bool window_open = true;
		std::array<Row, 7> rows;
		std::vector<std::wstring> titles;
		std::wstring footer;
		std::wstring error;
		std::vector<std::wstring> buttons;
	};

	extern FakeMachine g_Machine;
}
