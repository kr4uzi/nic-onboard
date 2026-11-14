#pragma once

#include "task.h"

#include <filesystem>
#include <string>
#include <vector>

namespace nic_onboard {
	// X:\Windows\System32 in WinPE
	std::filesystem::path system_directory();

	// runs a hidden console tool and waits for it without blocking. throws if it fails, the output is discarded
	task<void> run_process(std::filesystem::path executable, std::vector<std::wstring> arguments);
}
