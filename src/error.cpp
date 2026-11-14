#include "error.h"

#ifdef _WIN32
#include "win32.h"
#endif

using namespace nic_onboard;

std::wstring nic_onboard::widen(const std::string_view& text)
{
#ifdef _WIN32
	if (text.empty())
		return {};

	// exception texts are UTF-8 if they are ours, but filesystem and runtime errors may use the local code page
	auto codePage = UINT{ CP_UTF8 };
	auto length = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (!length) {
		codePage = CP_ACP;
		length = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	}

	auto result = std::wstring(length, L'\0');
	MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
	return result;
#else
	// the tests run here, and their texts are ASCII
	return std::wstring{ text.begin(), text.end() };
#endif
}
