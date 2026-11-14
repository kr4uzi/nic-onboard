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

		// a row shows a spinner from Begin until it is completed (check mark) or failed (cross)
		void Begin(Step step);
		void Complete(const std::wstring_view& result);

		// the row which is running gets its cross, and a box takes the place of the footer:
		// it tells what could not be done and shows the reason in full
		void Fail(const std::wstring_view& reason);

		// nothing can be clicked and the window cannot be closed, except while someone waits here:
		// true if the button was clicked, false if the window was closed instead
		task<bool> WaitForButton(const std::wstring_view& label);

		static void ShowError(const std::wstring_view& message);
	};
}
