#include "driver_path.h"
#include "error.h"
#include "storage.h"

#include <algorithm>
#include <ranges>
#include <vector>

using namespace nic_onboard;

namespace {
	bool same_text(std::wstring_view left, std::wstring_view right)
	{
		constexpr auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
		return std::ranges::equal(left, right, {}, lower, lower);
	}

	[[noreturn]] void unsupported_path()
	{
		throw Error{ L"The driver has an unsupported service ImagePath." };
	}
}

std::wstring nic_onboard::canonical_driver_image(std::wstring_view image)
{
	if (image.size() >= 2 && image.front() == L'"' && image.back() == L'"')
		image = image.substr(1, image.size() - 2);

	// Use Windows separators independently of the host's std::filesystem path rules.
	auto text = std::wstring{ image };
	std::ranges::replace(text, L'/', L'\\');
	image = text;

	auto systemRoot = false;
	for (auto prefix : { std::wstring_view{ L"\\SystemRoot\\" }, std::wstring_view{ L"%SystemRoot%\\" }, std::wstring_view{ L"%windir%\\" } }) {
		if (::same_text(image.substr(0, prefix.size()), prefix)) {
			image.remove_prefix(prefix.size());
			systemRoot = true;
			break;
		}
	}

	// Absolute paths belong to the source Windows installation, not to the next boot's drive letter.
	if (!systemRoot) {
		auto ntPath = image.starts_with(L"\\??\\");
		if (ntPath)
			image.remove_prefix(4);
		if (image.size() >= 3 && image[1] == L':' && image[2] == L'\\'
			&& ((image[0] >= L'A' && image[0] <= L'Z') || (image[0] >= L'a' && image[0] <= L'z'))) {
			image.remove_prefix(3);
			constexpr auto windows = std::wstring_view{ L"Windows\\" };
			if (!::same_text(image.substr(0, windows.size()), windows))
				::unsupported_path();
			image.remove_prefix(windows.size());
		}
		else if (ntPath)
			::unsupported_path();
	}

	if (image.empty() || image.front() == L'\\' || image.back() == L'\\')
		::unsupported_path();

	// Normalize within System32. Never accept a path which climbs out of it, even if it comes back later.
	auto parts = std::vector<std::wstring_view>{};
	while (!image.empty()) {
		auto separator = image.find(L'\\');
		auto part = image.substr(0, separator);
		image = separator == image.npos ? std::wstring_view{} : image.substr(separator + 1);
		if (part.empty() || part == L".")
			continue;
		if (part == L"..") {
			if (parts.size() <= 1)
				::unsupported_path();
			parts.pop_back();
			continue;
		}
		if (part.find_first_of(L"<>:\"|?*%") != part.npos || part.find(L'\0') != part.npos || part.back() == L'.' || part.back() == L' ')
			::unsupported_path();
		parts.push_back(part);
	}

	if (parts.size() < 2 || !::same_text(parts.front(), L"System32")
		|| parts.back().size() <= 4 || !::same_text(parts.back().substr(parts.back().size() - 4), L".sys"))
		::unsupported_path();

	auto result = std::wstring{ L"System32" };
	for (auto part : parts | std::views::drop(1)) {
		result += L'\\';
		result += part;
	}
	return result;
}

std::filesystem::path nic_onboard::driver_binary(const WindowsInstallation& windows, std::wstring_view image)
{
	auto relative = canonical_driver_image(image);
	std::ranges::replace(relative, L'\\', L'/');
	return windows.root / L"Windows" / relative;
}
