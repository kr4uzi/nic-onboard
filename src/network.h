#pragma once

#include "task.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nic_onboard {
	class Worker;

	// a network adapter as WinPE runs it right now
	struct NetworkAdapter
	{
		std::wstring instance;
		std::wstring name;
		std::wstring service;
		std::wstring class_key;
		std::wstring guid;
		std::wstring mac;
		bool connected = false;
		std::filesystem::path inf;
	};

	class Network
	{
		Worker& m_Worker;

	public:
		explicit Network(Worker& worker) : m_Worker{ worker } {}

		task<std::vector<NetworkAdapter>> GetAdapters();
		// the MAC address of the adapter the firmware booted from, as the adapters above spell it ("001122334455").
		// nothing when this machine was not booted from the network
		task<std::optional<std::wstring>> GetBootMac();
	};
}
