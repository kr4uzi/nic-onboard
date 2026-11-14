#pragma once

#include "task.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nic_onboard {
	class Worker;

	struct Disk
	{
		std::uint32_t number = 0;
		std::wstring instance;
		bool iscsi = false;
	};

	struct WindowsInstallation
	{
		std::filesystem::path root;
		bool iscsi_backed = false;
	};

	class Storage
	{
		Worker& m_Worker;

	public:
		explicit Storage(Worker& worker) : m_Worker{ worker } {}

		task<std::vector<Disk>> GetDisks();
		// takes the disks offline for this WinPE session only (nothing is persisted on them)
		task<void> IsolateDisks(const std::vector<Disk>& disks);
		task<std::vector<WindowsInstallation>> FindWindowsInstallations();
	};
}
