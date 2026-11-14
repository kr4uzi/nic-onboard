#include "nic_onboard.h"
#include "boot_plan.h"
#include "error.h"
#include "window.h"

#include <algorithm>
#include <ranges>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

using namespace nic_onboard;

NicOnboard::NicOnboard(boost::asio::io_context& context, Window& window, std::chrono::seconds iscsiTimeout)
	: m_Context{ context }, m_Window{ window }, m_IscsiTimeout{ iscsiTimeout },
	m_Storage{ m_Worker }, m_Network{ m_Worker }, m_Drivers{ m_Worker }, m_Registry{ m_Worker }
{

}

task<int> NicOnboard::Launch()
{
	if (!co_await Prepare()) {
		m_Window.SetTitle(L"Preparation stopped");
		co_await m_Window.WaitForButton(L"Close");
		co_return 1;
	}

	m_Window.SetTitle(L"Your network boot is ready");
	m_Window.SetFooter(L"Restart this computer to start Windows.");
	while (co_await m_Window.WaitForButton(L"Restart")) {
		try {
			m_Window.SetFooter(L"Restarting\u2026");
			co_await m_WinPE.Reboot();
			break;
		}
		catch (const std::exception& e) {
			m_Window.Fail(describe(e));
		}
	}

	co_return 0;
}

task<bool> NicOnboard::Prepare()
{
	try {
		co_await InitWinPE();
		co_await ProtectLocalDisks();

		auto windows = co_await FindWindows();
		auto adapter = co_await FindNetworkAdapter();
		auto package = co_await InstallDriver(windows, adapter);
		auto plan = co_await ConfigureBoot(windows, adapter, package);
		co_await VerifyBoot(windows, plan);

		co_return true;
	}
	catch (const std::exception& e) {
		// there is no retry and no rollback: a snapshot that failed half way is discarded and provisioned again
		m_Window.Fail(describe(e));
		co_return false;
	}
}

task<void> NicOnboard::InitWinPE()
{
	m_Window.Begin(Step::init_winpe);

	co_await m_WinPE.Init();

	m_Window.Complete(L"Devices and networking initialized");
}

task<void> NicOnboard::ProtectLocalDisks()
{
	m_Window.Begin(Step::protect_disks);

	// everything after this step writes to "the Windows disk": make sure that can only be the network one
	auto disks = co_await WaitForIscsiDisk();
	auto local = disks
		| std::views::filter([](const auto& disk) { return !disk.iscsi; })
		| std::ranges::to<std::vector>();

	co_await m_Storage.IsolateDisks(local);

	m_Window.Complete(L"Local disks isolated for this session");
}

task<std::vector<Disk>> NicOnboard::WaitForIscsiDisk()
{
	// the disk shows up some time after wpeinit brought up the network
	auto deadline = std::chrono::steady_clock::now() + m_IscsiTimeout;
	auto timer = boost::asio::steady_timer{ m_Context };
	while (true) {
		auto disks = co_await m_Storage.GetDisks();
		if (std::ranges::contains(disks, true, &Disk::iscsi))
			co_return disks;

		if (std::chrono::steady_clock::now() >= deadline)
			throw Error{ L"No iSCSI disk appeared. Check the WinPE NIC driver and network connection." };

		timer.expires_after(std::chrono::milliseconds{ 500 });
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

task<WindowsInstallation> NicOnboard::FindWindows()
{
	m_Window.Begin(Step::find_windows);

	auto windows = choose_windows(co_await m_Storage.FindWindowsInstallations());

	m_Window.Complete(windows.root.wstring());
	co_return windows;
}

task<NetworkAdapter> NicOnboard::FindNetworkAdapter()
{
	m_Window.Begin(Step::find_adapter);

	// iPXE leaves the MAC of the adapter it booted through in the iBFT: Windows will have to boot through that one too
	auto bootMac = co_await m_Network.GetBootMac();
	auto adapter = choose_adapter(co_await m_Network.GetAdapters(), bootMac);

	m_Window.Complete(adapter.name);
	co_return adapter;
}

task<DriverPackage> NicOnboard::InstallDriver(const WindowsInstallation& windows, const NetworkAdapter& adapter)
{
	m_Window.Begin(Step::install_driver);

	auto package = co_await m_Drivers.Install(windows, adapter);

	m_Window.Complete(package.published_inf);
	co_return package;
}

task<BootPlan> NicOnboard::ConfigureBoot(const WindowsInstallation& windows, const NetworkAdapter& adapter, const DriverPackage& package)
{
	m_Window.Begin(Step::configure_boot);

	auto snapshot = co_await m_Registry.Inspect(windows, adapter);
	auto plan = BootPlan::Create(snapshot, adapter, package);
	plan.image_path = co_await m_Drivers.InstallServiceBinary(windows, adapter, package, snapshot);
	co_await m_Registry.Apply(windows, adapter, plan);

	m_Window.Complete(L"Device instance and bindings configured");
	co_return plan;
}

task<void> NicOnboard::VerifyBoot(const WindowsInstallation& windows, const BootPlan& plan)
{
	m_Window.Begin(Step::verify_boot);

	// read everything back from the saved hive: the restart button only appears if it is really there
	auto report = co_await m_Registry.Verify(windows, plan);
	if (!report.passed())
		throw Error{ L"The saved network-boot configuration failed verification." };

	m_Window.Complete(L"Saved configuration verified");
}
