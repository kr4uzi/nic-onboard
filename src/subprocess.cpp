#include "subprocess.h"
#include "win32.h"

#include <cstddef>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/windows/object_handle.hpp>

using namespace nic_onboard;

namespace {
	// quotes one argument the way the C runtime of the child splits its command line again:
	// backslashes are literal, unless they are followed by a quote
	std::wstring quote(const std::wstring_view& argument)
	{
		auto result = std::wstring{ L"\"" };
		auto slashes = std::size_t{ 0 };
		for (auto character : argument) {
			if (character == L'\\') {
				slashes++;
				continue;
			}

			result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
			result += character;
			slashes = 0;
		}

		result.append(slashes * 2, L'\\');
		return result + L'"';
	}

	Handle start_process(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments)
	{
		// stdin, stdout and stderr of the child all go to NUL
		auto security = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		auto nulDevice = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (nulDevice == INVALID_HANDLE_VALUE)
			throw last_error(L"Open child process streams");

		auto nul = Handle{ nulDevice };

		// the child inherits that one handle, and none of ours
		auto size = SIZE_T{ 0 };
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		auto storage = std::vector<std::byte>(size);
		auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
		if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
			throw last_error(L"Initialize child process attributes");

		auto deleteAttributes = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
		auto attributeOwner = std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(deleteAttributes)>{ attributes, deleteAttributes };

		HANDLE inherited[]{ nul.get() };
		if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
			throw last_error(L"Set child process handles");

		auto startup = STARTUPINFOEXW{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startup.StartupInfo.wShowWindow = SW_HIDE;
		startup.StartupInfo.hStdInput = nul.get();
		startup.StartupInfo.hStdOutput = nul.get();
		startup.StartupInfo.hStdError = nul.get();
		startup.lpAttributeList = attributes;

		auto commandLine = ::quote(executable.wstring());
		for (const auto& argument : arguments)
			commandLine += L" " + ::quote(argument);

		auto process = PROCESS_INFORMATION{};
		if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process))
			throw last_error(L"Start " + executable.filename().wstring());

		CloseHandle(process.hThread);
		return Handle{ process.hProcess };
	}
}

std::filesystem::path nic_onboard::system_directory()
{
	wchar_t buffer[MAX_PATH]{};
	auto length = GetSystemDirectoryW(buffer, static_cast<UINT>(std::size(buffer)));
	if (!length || length >= std::size(buffer))
		throw last_error(L"Locate System32");

	return buffer;
}

task<void> nic_onboard::run_process(std::filesystem::path executable, std::vector<std::wstring> arguments)
{
	// a process handle gets signaled when the process exits: that is something asio can wait for
	auto process = boost::asio::windows::object_handle{ co_await boost::asio::this_coro::executor, ::start_process(executable, arguments).release() };
	co_await process.async_wait(boost::asio::use_awaitable);

	auto exitCode = DWORD{ 0 };
	if (!GetExitCodeProcess(process.native_handle(), &exitCode))
		throw last_error(L"Read process result");

	// DISM reports "done, but restart to finish" this way - we are about to restart anyway
	if (exitCode != ERROR_SUCCESS && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED)
		throw system_error(executable.filename().wstring() + L" failed", exitCode);
}
