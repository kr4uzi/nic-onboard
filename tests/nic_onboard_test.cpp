#include "fake.h"
#include "error.h"
#include "nic_onboard.h"
#include "window.h"
#include "worker.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <print>
#include <stdexcept>
#include <thread>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

using namespace nic_onboard;

namespace {
	using RowState = FakeMachine::RowState;

	void check(bool condition, const char* expression)
	{
		if (!condition)
			throw std::runtime_error{ expression };
	}
#define CHECK(expression) ::check(static_cast<bool>(expression), #expression)

	// runs the application on the machine described by g_Machine, the way main() does
	int launch(std::chrono::seconds iscsiTimeout = std::chrono::seconds{ 60 })
	{
		auto context = boost::asio::io_context{};
		auto window = Window{ context };
		auto application = NicOnboard{ context, window, iscsiTimeout };
		auto exitCode = boost::asio::co_spawn(context, application.Launch(), boost::asio::use_future);
		context.run();
		return exitCode.get();
	}

	bool called(const std::string& name)
	{
		return std::ranges::contains(g_Machine.calls, name);
	}

	void prepares_and_restarts()
	{
		g_Machine = {};
		g_Machine.clicks = { true };
		CHECK(launch() == 0);

		auto expected = std::vector<std::string>{
			"init", "get-disks", "isolate-disks", "find-windows", "get-boot-mac", "get-adapters", "install-driver",
			"inspect-registry", "install-service-binary", "apply-plan", "verify-plan", "reboot"
		};
		CHECK(g_Machine.calls == expected);
		CHECK(std::ranges::all_of(g_Machine.rows, [](const auto& row) { return row.state == RowState::completed; }));
		CHECK(g_Machine.rows[2].message == L"W:\\" && g_Machine.rows[3].message == L"Test Ethernet" && g_Machine.rows[4].message == L"oem9.inf");

		// only the local disk was taken offline
		CHECK(g_Machine.isolated.size() == 1 && g_Machine.isolated.front().number == 0);

		CHECK(g_Machine.applied && g_Machine.applied->network_guid == L"{original-guid}");
		CHECK(g_Machine.applied->image_path == L"System32\\drivers\\test.sys");
		CHECK(g_Machine.buttons == std::vector<std::wstring>{ L"Restart" });
		CHECK(g_Machine.reboots == 1);
	}

	void closing_the_window_does_not_restart()
	{
		g_Machine = {};
		g_Machine.clicks = { false };
		CHECK(launch() == 0);
		CHECK(g_Machine.applied && g_Machine.reboots == 0 && !called("reboot"));
	}

	void stops_at_the_first_failure()
	{
		// the call that fails, and the row which shows it
		auto steps = std::vector<std::pair<std::string, std::size_t>>{
			{ "init", 0 }, { "get-disks", 1 }, { "isolate-disks", 1 }, { "find-windows", 2 }, { "get-boot-mac", 3 }, { "get-adapters", 3 }, { "install-driver", 4 },
			{ "inspect-registry", 5 }, { "install-service-binary", 5 }, { "apply-plan", 5 }, { "verify-plan", 6 }
		};

		for (const auto& [name, row] : steps) {
			g_Machine = {};
			g_Machine.fail_at = name;
			g_Machine.clicks = { true };
			CHECK(launch() == 1);

			// nothing ran after the failing call
			CHECK(g_Machine.calls.back() == name);
			CHECK(g_Machine.reboots == 0);
			CHECK(g_Machine.buttons == std::vector<std::wstring>{ L"Close" });
			CHECK(g_Machine.titles.back() == L"Preparation stopped");

			CHECK(g_Machine.error == L"Test failure \u00e9");
			for (auto index = std::size_t{ 0 }; index < g_Machine.rows.size(); index++) {
				auto expected = index < row ? RowState::completed : index == row ? RowState::failed : RowState::waiting;
				CHECK(g_Machine.rows[index].state == expected);
			}
		}

		// an exception which is not ours is shown just the same
		g_Machine = {};
		g_Machine.crash_at = "install-driver";
		g_Machine.clicks = { true };
		CHECK(launch() == 1);
		CHECK(g_Machine.rows[4].state == RowState::failed && g_Machine.error == L"Native operation failed");
		CHECK(!g_Machine.applied);
	}

	void waits_for_the_iscsi_disk()
	{
		g_Machine = {};
		g_Machine.polls_without_iscsi = 1;
		g_Machine.clicks = { false };
		CHECK(launch() == 0);
		CHECK(std::ranges::count(g_Machine.calls, std::string{ "get-disks" }) == 2);

		// it never shows up: no disk is touched
		g_Machine = {};
		g_Machine.polls_without_iscsi = 1000;
		g_Machine.clicks = { true };
		CHECK(launch(std::chrono::seconds{ 0 }) == 1);
		CHECK(g_Machine.rows[1].state == RowState::failed && g_Machine.error.starts_with(L"No iSCSI disk appeared."));
		CHECK(!called("isolate-disks"));
	}

	void refuses_ambiguity_and_failed_verification()
	{
		g_Machine = {};
		g_Machine.installations.push_back({ L"V:\\", true });
		g_Machine.clicks = { true };
		CHECK(launch() == 1);
		CHECK(g_Machine.rows[2].state == RowState::failed && !called("get-adapters"));

		// two cabled adapters: the iBFT tells which one booted. without it, two is one too many
		g_Machine = {};
		g_Machine.adapters.push_back({ L"PCI\\VEN_8086\\SLOT_2", L"Second Ethernet", L"TestNic", L"{network-class}\\0004", L"{second-guid}", L"AABBCCDDEEFF", true, L"X:\\Driver\\test.inf" });
		g_Machine.boot_mac = L"aabbccddeeff";
		g_Machine.clicks = { false };
		CHECK(launch() == 0);
		CHECK(g_Machine.rows[3].message == L"Second Ethernet" && g_Machine.applied->device_instance == L"PCI\\VEN_8086\\SLOT_2");

		g_Machine = {};
		g_Machine.adapters.push_back({ L"PCI\\VEN_8086\\SLOT_2", L"Second Ethernet", L"TestNic", L"{network-class}\\0004", L"{second-guid}", L"AABBCCDDEEFF", true, L"X:\\Driver\\test.inf" });
		g_Machine.boot_mac.reset();
		g_Machine.clicks = { true };
		CHECK(launch() == 1);
		CHECK(g_Machine.rows[3].state == RowState::failed && g_Machine.error.starts_with(L"Multiple connected"));

		// the boot adapter is known but WinPE cannot use it: no other adapter is taken instead
		g_Machine = {};
		g_Machine.boot_mac = L"AABBCCDDEEFF";
		g_Machine.clicks = { true };
		CHECK(launch() == 1);
		CHECK(g_Machine.rows[3].state == RowState::failed && g_Machine.error.contains(L"(AA:BB:CC:DD:EE:FF)"));
		CHECK(!called("install-driver"));

		g_Machine = {};
		g_Machine.verification.binary = false;
		g_Machine.clicks = { true };
		CHECK(launch() == 1);
		CHECK(g_Machine.rows[6].state == RowState::failed && g_Machine.buttons == std::vector<std::wstring>{ L"Close" });
		CHECK(!called("reboot"));
	}

	void restart_can_be_tried_again()
	{
		g_Machine = {};
		g_Machine.failing_reboots = 1;
		g_Machine.clicks = { true, true };
		CHECK(launch() == 0);
		// the first attempt showed its error, the second one replaced it with the footer again
		CHECK(g_Machine.error.empty() && g_Machine.footer == L"Restarting\u2026");
		CHECK(g_Machine.buttons == (std::vector<std::wstring>{ L"Restart", L"Restart" }));
		CHECK(g_Machine.reboots == 1);
	}

	void stops_when_the_window_breaks()
	{
		g_Machine = {};
		g_Machine.window_breaks_at = "install-driver";
		g_Machine.clicks = { true };

		auto stopped = false;
		try {
			launch();
		}
		catch (const Error&) {
			stopped = true;
		}

		// the operation which was running finished, the next step did not begin
		CHECK(stopped && g_Machine.calls.back() == "install-driver" && !g_Machine.applied);
	}

	task<void> use_worker(Worker& worker, std::thread::id caller, bool& done)
	{
		auto elsewhere = co_await worker.Run([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
			return std::this_thread::get_id() != caller;
		});
		CHECK(elsewhere && std::this_thread::get_id() == caller);

		auto thrown = false;
		try {
			co_await worker.Run([] { throw Error{ L"from the worker" }; });
		}
		catch (const Error& e) {
			thrown = e.message() == L"from the worker";
		}
		CHECK(thrown && std::this_thread::get_id() == caller);

		done = true;
	}

	void worker_returns_to_the_caller()
	{
		auto context = boost::asio::io_context{};
		auto worker = Worker{};
		auto done = false;
		auto result = boost::asio::co_spawn(context, ::use_worker(worker, std::this_thread::get_id(), done), boost::asio::use_future);

		// run() must not return while the worker is busy: main() relies on that to let the application finish
		context.run();
		result.get();
		CHECK(done);
	}
}

int main()
{
	try {
		::prepares_and_restarts();
		::closing_the_window_does_not_restart();
		::stops_at_the_first_failure();
		::waits_for_the_iscsi_disk();
		::refuses_ambiguity_and_failed_verification();
		::restart_can_be_tried_again();
		::stops_when_the_window_breaks();
		::worker_returns_to_the_caller();
		std::println("application flow, failure handling and worker tests passed");
	}
	catch (const std::exception& e) {
		std::println(std::cerr, "[failed] {}", e.what());
		return 1;
	}

	return 0;
}
