#pragma once

#include "task.h"

namespace nic_onboard {
	class WinPE
	{
	public:
		// wpeinit: brings up plug and play and the network
		task<void> Init();
		task<void> Reboot();
	};
}
