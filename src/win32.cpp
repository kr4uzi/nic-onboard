#include "win32.h"

#include <format>

using namespace nic_onboard;

Error nic_onboard::system_error(const std::wstring_view& operation, std::uint32_t code)
{
	wchar_t buffer[2048]{};
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);

	// system messages end with a line break
	auto description = std::wstring_view{ buffer };
	while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n' || description.back() == L' '))
		description.remove_suffix(1);

	return Error{ std::format(L"{}: {} (0x{:08X})", operation, description, code) };
}

Error nic_onboard::last_error(const std::wstring_view& operation)
{
	return system_error(operation, GetLastError());
}

bool nic_onboard::equal_text(const std::wstring_view& left, const std::wstring_view& right)
{
	return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
