#pragma once

#include "task.h"

#include <memory>
#include <string_view>
#include <boost/asio/io_context.hpp>

namespace nic_onboard {
	// the rows of the window, top to bottom
	enum class Step
	{
		init_winpe,
		protect_disks,
		find_windows,
		find_adapter,
		install_driver,
		configure_boot,
		verify_boot
	};

	class Window
	{
		struct Impl;
		std::unique_ptr<Impl> m_Impl;

	public:
		explicit Window(boost::asio::io_context& context);
		~Window();

		// the message loop. it also runs the asio context, so the coroutines run on the thread that owns the window
		void Run();
		void Close();

		void SetTitle(const std::wstring_view& title);
		// one line below the rows
		void SetFooter(const std::wstring_view& text);

		// a row shows Working from Begin until it is Done or Failed
		void Begin(Step step);
		void Complete(const std::wstring_view& result);

		// the running row is marked Failed, and scrollable, selectable error text replaces the footer
		void Fail(const std::wstring_view& reason);

		// the action button and close box are enabled only while someone waits here:
		// true if the button was clicked, false if the window was closed instead
		task<bool> WaitForButton(const std::wstring_view& label);

		static void ShowError(const std::wstring_view& message);
	};
}
