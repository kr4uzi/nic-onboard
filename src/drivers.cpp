#include "drivers.h"
#include "network.h"
#include "subprocess.h"
#include "registry.h"
#include "storage.h"
#include "win32.h"
#include "worker.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>

using namespace nic_onboard;

namespace {
	std::string read_file(const std::filesystem::path& path)
	{
		auto file = std::ifstream{ path, std::ios::binary };
		if (!file)
			throw Error{ L"Cannot read driver INF: " + path.wstring() };

		return std::string{ std::istreambuf_iterator<char>{ file }, {} };
	}

	DriverPackage find_package(const WindowsInstallation& windows, const NetworkAdapter& adapter)
	{
		// DISM publishes the driver as oemNN.inf, and that number differs between WinPE and the offline Windows.
		// what stays the same is the (signed) INF itself: find the published INF by its content
		auto inf = ::read_file(adapter.inf);
		if (inf.empty())
			throw Error{ L"The running driver's INF is empty." };

		auto package = DriverPackage{};
		for (const auto& entry : std::filesystem::directory_iterator{ windows.root / L"Windows/INF" }) {
			if (!entry.is_regular_file() || !equal_text(entry.path().extension().wstring(), L".inf"))
				continue;

			if (entry.file_size() == inf.size() && ::read_file(entry.path()) == inf) {
				package.published_inf = entry.path().filename().wstring();
				break;
			}
		}

		if (package.published_inf.empty())
			throw Error{ L"The running NIC driver package is missing from the offline Windows after driver injection." };

		// the package itself is in the driver store, usually in a folder named like the one in WinPE
		auto store = windows.root / L"Windows/System32/DriverStore/FileRepository";
		auto contains_inf = [&](const std::filesystem::path& folder) {
			auto candidate = folder / adapter.inf.filename();
			return std::filesystem::is_regular_file(candidate) && std::filesystem::file_size(candidate) == inf.size() && ::read_file(candidate) == inf;
		};

		auto sameFolder = store / adapter.inf.parent_path().filename();
		if (contains_inf(sameFolder)) {
			package.directory = sameFolder;
			return package;
		}

		for (const auto& entry : std::filesystem::directory_iterator{ store }) {
			if (!entry.is_directory() || !contains_inf(entry.path()))
				continue;

			if (!package.directory.empty())
				throw Error{ L"Multiple offline driver-store packages match the running NIC driver." };

			package.directory = entry.path();
		}

		if (package.directory.empty())
			throw Error{ L"The signed NIC driver package is missing from the offline driver store." };

		return package;
	}

	// the part of child below parent, nothing if child is somewhere else
	std::optional<std::filesystem::path> relative_inside(const std::filesystem::path& child, const std::filesystem::path& parent)
	{
		auto normalizedChild = child.lexically_normal();
		auto normalizedParent = parent.lexically_normal();
		auto item = normalizedChild.begin();
		for (const auto& component : normalizedParent) {
			if (item == normalizedChild.end() || !equal_text(item->wstring(), component.wstring()))
				return std::nullopt;

			++item;
		}

		auto result = std::filesystem::path{};
		for (; item != normalizedChild.end(); ++item)
			result /= *item;

		return result;
	}

	bool same_binary(const std::filesystem::path& left, const std::filesystem::path& right)
	{
		if (!std::filesystem::is_regular_file(right) || std::filesystem::file_size(left) != std::filesystem::file_size(right))
			return false;

		auto a = std::ifstream{ left, std::ios::binary };
		auto b = std::ifstream{ right, std::ios::binary };
		if (!a || !b)
			throw Error{ L"Cannot compare the installed network driver binary." };

		auto first = std::array<char, 65536>{};
		auto second = std::array<char, 65536>{};
		do {
			a.read(first.data(), first.size());
			b.read(second.data(), second.size());
			if (a.bad() || b.bad())
				throw Error{ L"Cannot read the installed network driver binary." };

			if (a.gcount() != b.gcount() || !std::equal(first.begin(), first.begin() + a.gcount(), second.begin()))
				return false;
		} while (a.gcount() != 0);

		return true;
	}

	std::wstring install_service_binary(const WindowsInstallation& windows, const NetworkAdapter& adapter, const DriverPackage& package, const RegistrySnapshot& snapshot)
	{
		auto liveImage = canonical_driver_image(snapshot.live_service_image);
		auto installedImage = snapshot.installed_service_image.transform(canonical_driver_image);

		// reuse an identical installed binary, but always save a path independent of WinPE's drive letters
		auto already_installed = [&](const std::filesystem::path& staged) {
			return installedImage && ::same_binary(staged, driver_binary(windows, *installedImage));
		};

		// modern drivers run straight from the driver store (DIRID 13):
		// the ImagePath is the same path inside the package - which has another folder name in the offline Windows
		auto winpe = WindowsInstallation{ .root = system_directory().parent_path().parent_path() };
		auto relative = ::relative_inside(driver_binary(winpe, liveImage), adapter.inf.parent_path());
		if (relative && !relative->empty()) {
			auto staged = package.directory / *relative;
			if (!std::filesystem::is_regular_file(staged))
				throw Error{ L"The driver service binary is missing from its offline package." };

			if (already_installed(staged))
				return *installedImage;

			return canonical_driver_image(staged.lexically_relative(windows.root / L"Windows").wstring());
		}

		// classic drivers are copied to System32\drivers (DIRID 12), which staging a driver offline does not do.
		// copy the file named by the ImagePath out of the package - the name is taken from there, never guessed
		auto destination = driver_binary(windows, liveImage);
		auto name = ::relative_inside(destination, windows.root / L"Windows/System32/drivers");
		if (!name || name->empty() || name->has_parent_path())
			throw Error{ L"The driver binary cannot be mapped safely into offline Windows." };

		auto staged = std::filesystem::path{};
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ package.directory }) {
			if (!entry.is_regular_file() || !equal_text(entry.path().filename().wstring(), destination.filename().wstring()))
				continue;

			if (!staged.empty())
				throw Error{ L"The driver package contains ambiguous service binaries." };

			staged = entry.path();
		}

		if (staged.empty())
			throw Error{ L"The service binary is missing from the verified driver package." };

		if (already_installed(staged))
			return *installedImage;

		if (!::same_binary(staged, destination))
			std::filesystem::copy_file(staged, destination, std::filesystem::copy_options::overwrite_existing);

		return liveImage;
	}
}

task<DriverPackage> Drivers::Install(const WindowsInstallation& windows, const NetworkAdapter& adapter)
{
	co_await run_process(system_directory() / L"dism.exe", {
		L"/Image:" + windows.root.wstring(),
		L"/Add-Driver",
		L"/Driver:" + adapter.inf.wstring(),
		L"/English"
	});

	co_return co_await m_Worker.Run([&] { return ::find_package(windows, adapter); });
}

task<std::wstring> Drivers::InstallServiceBinary(const WindowsInstallation& windows, const NetworkAdapter& adapter, const DriverPackage& package, const RegistrySnapshot& snapshot)
{
	co_return co_await m_Worker.Run([&] { return ::install_service_binary(windows, adapter, package, snapshot); });
}
