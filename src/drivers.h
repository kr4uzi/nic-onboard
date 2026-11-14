#pragma once

#include "driver_path.h"
#include "task.h"

#include <filesystem>
#include <string>

namespace nic_onboard {
	class Worker;
	struct NetworkAdapter;
	struct RegistrySnapshot;
	struct WindowsInstallation;

	// the adapter's driver package as it is staged in the offline Windows
	struct DriverPackage
	{
		std::wstring published_inf;
		std::filesystem::path directory;
	};

	class Drivers
	{
		Worker& m_Worker;

	public:
		explicit Drivers(Worker& worker) : m_Worker{ worker } {}

		// adds the driver WinPE runs the adapter with to the offline Windows (DISM)
		task<DriverPackage> Install(const WindowsInstallation& windows, const NetworkAdapter& adapter);
		// makes sure the driver's .sys exists in the offline Windows, returns the service ImagePath pointing to it
		task<std::wstring> InstallServiceBinary(const WindowsInstallation& windows, const NetworkAdapter& adapter, const DriverPackage& package, const RegistrySnapshot& snapshot);
	};
}
