#pragma once

#include "drivers.h"
#include "network.h"
#include "registry.h"
#include "storage.h"
#include "task.h"
#include "winpe.h"
#include "worker.h"

#include <chrono>
#include <vector>
#include <boost/asio/io_context.hpp>

namespace nic_onboard {
	class Window;
	struct BootPlan;

	class NicOnboard
	{
		boost::asio::io_context& m_Context;
		Window& m_Window;
		std::chrono::seconds m_IscsiTimeout;

		Worker m_Worker;
		WinPE m_WinPE;
		Storage m_Storage;
		Network m_Network;
		Drivers m_Drivers;
		Registry m_Registry;

	public:
		NicOnboard(boost::asio::io_context& context, Window& window, std::chrono::seconds iscsiTimeout = std::chrono::seconds{ 60 });

		// the whole application, returns the exit code
		task<int> Launch();

	private:
		task<bool> Prepare();

		task<void> InitWinPE();
		task<void> ProtectLocalDisks();
		task<WindowsInstallation> FindWindows();
		task<NetworkAdapter> FindNetworkAdapter();
		task<DriverPackage> InstallDriver(const WindowsInstallation& windows, const NetworkAdapter& adapter);
		task<BootPlan> ConfigureBoot(const WindowsInstallation& windows, const NetworkAdapter& adapter, const DriverPackage& package);
		task<void> VerifyBoot(const WindowsInstallation& windows, const BootPlan& plan);

		task<std::vector<Disk>> WaitForIscsiDisk();
	};
}
