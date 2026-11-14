#pragma once

#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nic_onboard {
	// an error whose message is written for the person in front of the machine. it is wide because it names
	// paths and devices, and it ends up in the window
	class Error : public std::runtime_error
	{
		std::wstring m_Message;

	public:
		explicit Error(std::wstring message)
			: std::runtime_error{ ascii(message) }, m_Message{ std::move(message) }
		{

		}

		const std::wstring& message() const noexcept { return m_Message; }

	private:
		static std::string ascii(const std::wstring_view& text)
		{
			return text
				| std::views::transform([](wchar_t character) { return character < 0x80 ? static_cast<char>(character) : '?'; })
				| std::ranges::to<std::string>();
		}
	};

	std::wstring widen(const std::string_view& text);

	// the text to show for any exception: our own message, or whatever the library that threw has to say
	inline std::wstring describe(const std::exception& e)
	{
		auto error = dynamic_cast<const Error*>(&e);
		return error ? error->message() : widen(e.what());
	}
}
