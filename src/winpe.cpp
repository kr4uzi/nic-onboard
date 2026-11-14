#include "winpe.h"
#include "subprocess.h"
#include "win32.h"

using namespace nic_onboard;

task<void> WinPE::Init()
{
	// this key only exists in WinPE. everything we do would be harmful to a regular, running Windows
	auto key = HKEY{};
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ, &key) != ERROR_SUCCESS)
		throw Error{ L"Preparation must run in Windows PE." };

	RegCloseKey(key);

	co_await run_process(system_directory() / L"wpeinit.exe", {});
}

task<void> WinPE::Reboot()
{
	co_await run_process(system_directory() / L"wpeutil.exe", { L"reboot" });
}
