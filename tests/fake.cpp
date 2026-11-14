#include "fake.h"
#include "error.h"
#include "window.h"
#include "winpe.h"

#include <stdexcept>
#include <utility>

using namespace nic_onboard;

FakeMachine nic_onboard::g_Machine;

namespace {
	using RowState = FakeMachine::RowState;

	std::optional<std::size_t> g_Running;

	// every fake operation starts with this
	void call(const std::string& name)
	{
		g_Machine.calls.push_back(name);
		if (g_Machine.window_breaks_at == name)
			g_Machine.window_open = false;

		if (g_Machine.crash_at == name)
			throw std::runtime_error{ "Native operation failed" };

		if (g_Machine.fail_at == name)
			throw Error{ L"Test failure \u00e9" };
	}
}

struct Window::Impl
{

};

Window::Window(boost::asio::io_context&)
{
	g_Running.reset();
}

Window::~Window()
{

}

void Window::Run()
{

}

void Window::Close()
{
	g_Machine.window_open = false;
}

void Window::SetTitle(const std::wstring_view& title)
{
	g_Machine.titles.emplace_back(title);
}

void Window::SetFooter(const std::wstring_view& text)
{
	g_Machine.footer = text;
	g_Machine.error.clear();
}

void Window::Begin(Step step)
{
	if (!g_Machine.window_open)
		throw Error{ L"The preparation window was closed." };

	g_Running = std::to_underlying(step);
	g_Machine.rows[*g_Running] = { RowState::running, L"Working" };
}

void Window::Complete(const std::wstring_view& result)
{
	g_Machine.rows[g_Running.value()] = { RowState::completed, std::wstring{ result } };
	g_Running.reset();
}

void Window::Fail(const std::wstring_view& reason)
{
	g_Machine.error = reason;
	if (g_Running)
		g_Machine.rows[*g_Running] = { RowState::failed, L"Failed" };

	g_Running.reset();
}

task<bool> Window::WaitForButton(const std::wstring_view& label)
{
	if (!g_Machine.window_open || g_Machine.clicks.empty())
		throw Error{ L"The preparation window was closed." };

	g_Machine.buttons.emplace_back(label);
	auto clicked = g_Machine.clicks.front();
	g_Machine.clicks.pop_front();
	co_return clicked;
}

void Window::ShowError(const std::wstring_view&)
{

}

task<void> WinPE::Init()
{
	::call("init");
	co_return;
}

task<void> WinPE::Reboot()
{
	::call("reboot");
	if (g_Machine.failing_reboots-- > 0)
		throw Error{ L"wpeutil.exe failed" };

	g_Machine.reboots++;
	co_return;
}

task<std::vector<Disk>> Storage::GetDisks()
{
	::call("get-disks");
	auto disks = g_Machine.disks;
	if (g_Machine.polls_without_iscsi-- > 0)
		std::erase_if(disks, [](const auto& disk) { return disk.iscsi; });

	co_return disks;
}

task<void> Storage::IsolateDisks(const std::vector<Disk>& disks)
{
	::call("isolate-disks");
	g_Machine.isolated = disks;
	co_return;
}

task<std::vector<WindowsInstallation>> Storage::FindWindowsInstallations()
{
	::call("find-windows");
	co_return g_Machine.installations;
}

task<std::vector<NetworkAdapter>> Network::GetAdapters()
{
	::call("get-adapters");
	co_return g_Machine.adapters;
}

task<std::optional<std::wstring>> Network::GetBootMac()
{
	::call("get-boot-mac");
	co_return g_Machine.boot_mac;
}

task<DriverPackage> Drivers::Install(const WindowsInstallation&, const NetworkAdapter&)
{
	::call("install-driver");
	co_return DriverPackage{ L"oem9.inf", L"W:\\DriverStore\\test" };
}

task<std::wstring> Drivers::InstallServiceBinary(const WindowsInstallation&, const NetworkAdapter&, const DriverPackage&, const RegistrySnapshot& snapshot)
{
	::call("install-service-binary");
	co_return snapshot.live_service_image;
}

task<RegistrySnapshot> Registry::Inspect(const WindowsInstallation&, const NetworkAdapter&)
{
	::call("inspect-registry");
	co_return g_Machine.registry;
}

task<void> Registry::Apply(const WindowsInstallation&, const NetworkAdapter&, const BootPlan& plan)
{
	::call("apply-plan");
	g_Machine.applied = plan;
	co_return;
}

task<VerificationReport> Registry::Verify(const WindowsInstallation&, const BootPlan&)
{
	::call("verify-plan");
	co_return g_Machine.verification;
}
