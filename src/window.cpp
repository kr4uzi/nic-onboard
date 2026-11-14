#include "window.h"
#include "win32.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <commctrl.h>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

using namespace nic_onboard;

namespace {
	constexpr auto window_class = L"NicOnboard";
	constexpr auto window_title = L"Network boot preparation";

	constexpr auto button_command = 101;

	constexpr auto blue = RGB(0, 100, 190);
	constexpr auto red = RGB(185, 35, 35);
	constexpr auto grey = RGB(105, 112, 122);
	constexpr auto black = RGB(35, 42, 52);

	// one per Step
	constexpr auto step_titles = std::array{
		L"Initialize Windows PE",
		L"Protect local storage",
		L"Find your Windows disk",
		L"Identify the network adapter",
		L"Prepare the network driver",
		L"Configure network boot",
		L"Verify the configuration"
	};
	static_assert(step_titles.size() == std::to_underlying(Step::verify_boot) + 1);

	enum class RowState { waiting, running, completed, failed };

	void set_text(HWND control, const std::wstring_view& text)
	{
		SetWindowTextW(control, std::wstring{ text }.c_str());
	}

	void set_font(HWND control, HFONT font)
	{
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void set_cell(HWND list, std::size_t row, int column, const std::wstring_view& text)
	{
		auto value = std::wstring{ text };
		auto item = LVITEMW{};
		item.iSubItem = column;
		item.pszText = value.data();
		SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
	}
}

struct Window::Impl
{
	boost::asio::io_context& m_Context;
	// WaitForButton receives what the window procedure sends: true for the button, false for the close box.
	// it has no buffer, so a click nobody waits for is dropped
	boost::asio::experimental::channel<void(boost::system::error_code, bool)> m_Clicks;
	bool m_Waiting = false;

	HWND m_Window = nullptr;
	HWND m_Title = nullptr;
	HWND m_Steps = nullptr;
	HWND m_Footer = nullptr;
	HWND m_Button = nullptr;
	std::optional<std::size_t> m_Running;

	// native controls keep long errors scrollable and selectable, including at high DPI
	HWND m_ErrorHeading = nullptr;
	HWND m_Error = nullptr;
	bool m_HasError = false;

	HFONT m_BodyFont = nullptr;
	HFONT m_TitleFont = nullptr;
	HFONT m_RowFont = nullptr;
	int m_Dpi = 96;

	unsigned m_Ticks = 0;

	// whatever went wrong inside the window procedure: exceptions cannot pass through Windows
	std::exception_ptr m_Exception;

	Impl(boost::asio::io_context& context)
		: m_Context{ context }, m_Clicks{ context }
	{
		SetProcessDPIAware();
		auto controls = INITCOMMONCONTROLSEX{ sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES };
		if (!InitCommonControlsEx(&controls))
			throw Error{ L"Initialize preparation controls." };

		auto definition = WNDCLASSEXW{};
		definition.cbSize = sizeof(definition);
		definition.hInstance = GetModuleHandleW(nullptr);
		definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		definition.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		definition.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
		definition.lpfnWndProc = WindowProcedure;
		definition.lpszClassName = ::window_class;
		if (!RegisterClassExW(&definition))
			throw last_error(L"Register preparation window");

		auto screen = GetDC(nullptr);
		if (!screen)
			throw last_error(L"Read display DPI");

		m_Dpi = GetDeviceCaps(screen, LOGPIXELSX);
		ReleaseDC(nullptr, screen);

		// centered, and not larger than the screen
		auto workArea = RECT{};
		if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
			throw last_error(L"Read screen dimensions");

		auto width = std::min(Scale(840), static_cast<int>(workArea.right - workArea.left - 20));
		auto height = std::min(Scale(560), static_cast<int>(workArea.bottom - workArea.top - 20));
		auto left = workArea.left + (workArea.right - workArea.left - width) / 2;
		auto top = workArea.top + (workArea.bottom - workArea.top - height) / 2;

		// the controls are created while this call handles WM_CREATE
		auto style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
		if (!CreateWindowExW(WS_EX_CONTROLPARENT, ::window_class, ::window_title, style, left, top, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this)) {
			if (m_Exception)
				std::rethrow_exception(m_Exception);

			throw last_error(L"Create preparation window");
		}

		ShowWindow(m_Window, SW_SHOW);
		UpdateWindow(m_Window);
	}

	~Impl()
	{
		if (m_Window)
			DestroyWindow(m_Window);

		DeleteFonts();
	}

	// Begin and WaitForButton end up here once the window is gone, which stops the application
	[[noreturn]] void ThrowClosed() const
	{
		if (m_Exception)
			std::rethrow_exception(m_Exception);

		throw Error{ L"The preparation window was closed." };
	}

	void EnableButton(bool enable)
	{
		m_Waiting = enable;
		EnableWindow(m_Button, enable);
		EnableMenuItem(GetSystemMenu(m_Window, FALSE), SC_CLOSE, MF_BYCOMMAND | (enable ? MF_ENABLED : MF_GRAYED));

		// so the enter key presses it
		if (enable)
			SetFocus(m_Button);
	}

	void SetRow(std::size_t index, RowState state, const std::wstring_view& message)
	{
		constexpr auto labels = std::array{ L"Waiting", L"Working...", L"Done", L"Failed" };
		::set_cell(m_Steps, index, 0, labels[std::to_underlying(state)]);
		::set_cell(m_Steps, index, 2, message);
		LayoutColumns();
		if (state == RowState::running || state == RowState::failed) {
			ListView_SetItemState(m_Steps, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_SetItemState(m_Steps, static_cast<int>(index), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_EnsureVisible(m_Steps, static_cast<int>(index), FALSE);
		}
	}

	// the status area shows one or the other: the footer, or an error
	void ShowStatus(const std::wstring_view& footer, const std::wstring_view& errorHeading, const std::wstring_view& error)
	{
		m_HasError = !error.empty();
		::set_text(m_ErrorHeading, errorHeading.empty() ? L"Operation failed" : errorHeading);
		// EDIT uses CRLF for hard line breaks; exception messages can also contain bare LF.
		auto text = std::wstring{};
		for (auto character : error) {
			if (character == L'\n' && (text.empty() || text.back() != L'\r'))
				text += L'\r';
			text += character;
		}
		::set_text(m_Error, text);
		SendMessageW(m_Error, EM_SETSEL, 0, 0);
		SendMessageW(m_Error, EM_SCROLLCARET, 0, 0);
		::set_text(m_Footer, footer);
		ShowWindow(m_Footer, m_HasError ? SW_HIDE : SW_SHOW);
		ShowWindow(m_ErrorHeading, m_HasError ? SW_SHOW : SW_HIDE);
		ShowWindow(m_Error, m_HasError ? SW_SHOW : SW_HIDE);
		Layout();
	}

private:
	int Scale(int value) const
	{
		return MulDiv(value, m_Dpi, 96);
	}

	HWND CreateControl(const wchar_t* kind, const wchar_t* text, DWORD style, int id = 0)
	{
		auto control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, m_Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
		if (!control)
			throw last_error(L"Create interface control");

		return control;
	}

	void CreateControls()
	{
		m_Title = CreateControl(L"STATIC", L"Preparing your network boot", SS_NOPREFIX);
		m_Steps = CreateControl(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER);
		ListView_SetExtendedListViewStyle(m_Steps, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		auto header = ListView_GetHeader(m_Steps);
		SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_NOSIZING);
		constexpr auto columns = std::array{ L"Status", L"Preparation step", L"Details" };
		for (auto index = std::size_t{ 0 }; index < columns.size(); index++) {
			auto title = std::wstring{ columns[index] };
			auto column = LVCOLUMNW{};
			column.mask = LVCF_TEXT;
			column.pszText = title.data();
			if (ListView_InsertColumn(m_Steps, static_cast<int>(index), &column) == -1)
				throw Error{ L"Create preparation columns." };
		}
		for (auto index = std::size_t{ 0 }; index < ::step_titles.size(); index++) {
			auto status = std::wstring{ L"Waiting" };
			auto row = LVITEMW{};
			row.mask = LVIF_TEXT;
			row.iItem = static_cast<int>(index);
			row.pszText = status.data();
			if (ListView_InsertItem(m_Steps, &row) == -1)
				throw Error{ L"Create preparation step." };
			::set_cell(m_Steps, index, 1, ::step_titles[index]);
		}

		m_Footer = CreateControl(L"STATIC", L"Please keep this computer connected to the network.", SS_NOPREFIX);
		m_ErrorHeading = CreateControl(L"STATIC", L"", SS_NOPREFIX);
		m_Error = CreateControl(L"EDIT", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY);
		ShowWindow(m_ErrorHeading, SW_HIDE);
		ShowWindow(m_Error, SW_HIDE);
		m_Button = CreateControl(L"BUTTON", L"Preparing\u2026", WS_TABSTOP | BS_DEFPUSHBUTTON, ::button_command);
		EnableButton(false);

		CreateFonts();
		Layout();

		// see Tick
		if (!SetTimer(m_Window, 1, 15, nullptr))
			throw last_error(L"Start interface timer");
	}

	void DeleteFonts()
	{
		for (auto font : { &m_BodyFont, &m_TitleFont, &m_RowFont }) {
			if (*font)
				DeleteObject(*font);

			*font = nullptr;
		}
	}

	void CreateFonts()
	{
		DeleteFonts();

		auto create = [&](int size, int weight) {
			auto font = CreateFontW(-Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
			if (!font)
				throw last_error(L"Create interface font");

			return font;
		};

		m_BodyFont = create(15, FW_NORMAL);
		m_TitleFont = create(25, FW_SEMIBOLD);
		m_RowFont = create(16, FW_SEMIBOLD);

		::set_font(m_Title, m_TitleFont);
		::set_font(m_Steps, m_BodyFont);
		::set_font(m_Footer, m_BodyFont);
		::set_font(m_ErrorHeading, m_RowFont);
		::set_font(m_Error, m_BodyFont);
		::set_font(m_Button, m_BodyFont);
	}

	// The list view owns row sizing and scrolling. An error gets up to half the content area;
	// its edit control scrolls independently, so neither the rows nor the error text must be squeezed.
	void Layout()
	{
		auto bounds = RECT{};
		GetClientRect(m_Window, &bounds);
		auto width = static_cast<int>(bounds.right);
		auto height = static_cast<int>(bounds.bottom);
		auto margin = Scale(24);
		auto gap = Scale(12);
		auto contentWidth = std::max(0, width - margin * 2);
		auto rowsTop = Scale(76);
		auto statusBottom = height - Scale(68);
		auto contentHeight = std::max(0, statusBottom - rowsTop);
		auto statusHeight = m_HasError ? std::min(Scale(170), contentHeight / 2) : Scale(24);
		auto statusTop = statusBottom - statusHeight;

		MoveWindow(m_Title, margin, Scale(20), contentWidth, Scale(36), TRUE);
		MoveWindow(m_Steps, margin, rowsTop, contentWidth, std::max(0, statusTop - gap - rowsTop), TRUE);
		MoveWindow(m_Footer, margin, statusTop, contentWidth, statusHeight, TRUE);
		MoveWindow(m_ErrorHeading, margin, statusTop, contentWidth, Scale(24), TRUE);
		MoveWindow(m_Error, margin, statusTop + Scale(30), contentWidth, std::max(0, statusHeight - Scale(30)), TRUE);
		MoveWindow(m_Button, width - margin - Scale(145), height - Scale(48), Scale(145), Scale(31), TRUE);
		LayoutColumns();
		auto selected = ListView_GetNextItem(m_Steps, -1, LVNI_SELECTED);
		if (selected != -1)
			ListView_EnsureVisible(m_Steps, selected, FALSE);
		InvalidateRect(m_Window, nullptr, TRUE);
	}

	void LayoutColumns()
	{
		auto statusWidth = ListView_GetStringWidth(m_Steps, L"Working...") + Scale(12);
		ListView_SetColumnWidth(m_Steps, 0, statusWidth);
		ListView_SetColumnWidth(m_Steps, 1, LVSCW_AUTOSIZE);
		// For the last column, this fills the remaining width. Clipped details have native hover tips.
		ListView_SetColumnWidth(m_Steps, 2, LVSCW_AUTOSIZE_USEHEADER);
	}

	// every 15ms. the window owns the thread's loop (GetMessage), so asio does not get to run its own:
	// instead, each tick runs whatever became ready since the last one. this is where the coroutines continue
	void Tick()
	{
		// a context which had nothing to wait for marks itself as stopped, and from
		// then on poll() would return without running anything that was started later
		m_Context.restart();
		m_Context.poll();

		// one of them may just have closed the window
		if (!m_Window)
			return;

		if (++m_Ticks % 20 == 0 && m_Running) {
			constexpr auto progress = std::array{ L"Working", L"Working.", L"Working..", L"Working..." };
			::set_cell(m_Steps, *m_Running, 0, progress[(m_Ticks / 20) % progress.size()]);
		}
	}

	HBRUSH ColorText(HDC dc, HWND control)
	{
		auto color = control == m_Title ? ::blue
			: control == m_ErrorHeading ? ::red
			: control == m_Error ? ::black : ::grey;

		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, color);
		return GetSysColorBrush(COLOR_WINDOW);
	}

	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message) {
		case WM_CREATE:
			CreateControls();
			return 0;
		case WM_TIMER:
			Tick();
			return 0;
		case WM_SIZE:
			if (m_Steps)
				Layout();
			return 0;
		case WM_CTLCOLORSTATIC:
			return reinterpret_cast<LRESULT>(ColorText(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
		case WM_COMMAND:
			if (LOWORD(wParam) == ::button_command)
				m_Clicks.try_send(boost::system::error_code{}, true);
			return 0;
		case WM_CLOSE:
			// closing the window is a request, just like the button: the application decides what it means
			if (m_Waiting)
				m_Clicks.try_send(boost::system::error_code{}, false);
			return 0;
		case WM_DPICHANGED: {
			m_Dpi = HIWORD(wParam);
			CreateFonts();
			auto suggested = reinterpret_cast<const RECT*>(lParam);
			auto monitor = MONITORINFO{};
			monitor.cbSize = sizeof(monitor);
			if (!GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor))
				throw last_error(L"Read display work area");
			const auto& work = monitor.rcWork;
			auto width = std::min(suggested->right - suggested->left, work.right - work.left);
			auto height = std::min(suggested->bottom - suggested->top, work.bottom - work.top);
			auto left = std::clamp(suggested->left, work.left, work.right - width);
			auto top = std::clamp(suggested->top, work.top, work.bottom - height);
			SetWindowPos(m_Window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
			Layout();
			return 0;
		}
		case WM_DESTROY:
			KillTimer(m_Window, 1);
			m_Window = nullptr;
			// whoever waits for a click learns that there will be none
			m_Clicks.close();
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProcW(m_Window, message, wParam, lParam);
	}

	static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
	{
		// CreateWindowEx passes `this` along with the first message, from then on the window carries it
		auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		if (message == WM_NCCREATE) {
			self = static_cast<Impl*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
			self->m_Window = window;
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}

		if (!self || !self->m_Window)
			return DefWindowProcW(window, message, wParam, lParam);

		try {
			return self->HandleMessage(message, wParam, lParam);
		}
		catch (...) {
			self->m_Exception = std::current_exception();

			// a window that cannot be created is destroyed by CreateWindowEx itself
			if (message == WM_CREATE)
				return -1;

			DestroyWindow(window);
			return 0;
		}
	}

};

Window::Window(boost::asio::io_context& context)
	: m_Impl{ std::make_unique<Impl>(context) }
{

}

Window::~Window()
{

}

void Window::Run()
{
	// until the WM_QUIT which destroying the window posts
	auto message = MSG{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		// the enter key presses the button
		if (m_Impl->m_Window && IsDialogMessageW(m_Impl->m_Window, &message))
			continue;

		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

void Window::Close()
{
	if (m_Impl->m_Window)
		DestroyWindow(m_Impl->m_Window);
}

void Window::SetTitle(const std::wstring_view& title)
{
	::set_text(m_Impl->m_Title, title);
}

void Window::SetFooter(const std::wstring_view& text)
{
	m_Impl->ShowStatus(text, {}, {});
}

void Window::Begin(Step step)
{
	if (!m_Impl->m_Window)
		m_Impl->ThrowClosed();

	m_Impl->m_Running = std::to_underlying(step);
	m_Impl->SetRow(*m_Impl->m_Running, RowState::running, L"Working\u2026");
}

void Window::Complete(const std::wstring_view& result)
{
	if (!m_Impl->m_Window || !m_Impl->m_Running)
		return;

	m_Impl->SetRow(*m_Impl->m_Running, RowState::completed, result);
	m_Impl->m_Running.reset();
}

void Window::Fail(const std::wstring_view& reason)
{
	if (!m_Impl->m_Window)
		return;

	// "Find your Windows disk" -> "Could not find your Windows disk"
	auto heading = std::wstring{};
	if (m_Impl->m_Running) {
		heading = ::step_titles[*m_Impl->m_Running];
		heading.front() = static_cast<wchar_t>(std::towlower(heading.front()));
		heading.insert(0, L"Could not ");

		m_Impl->SetRow(*m_Impl->m_Running, RowState::failed, L"Failed");
		m_Impl->m_Running.reset();
	}

	m_Impl->ShowStatus({}, heading, reason);
}

task<bool> Window::WaitForButton(const std::wstring_view& label)
{
	if (!m_Impl->m_Window)
		m_Impl->ThrowClosed();

	::set_text(m_Impl->m_Button, label);
	m_Impl->EnableButton(true);

	auto [error, clicked] = co_await m_Impl->m_Clicks.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
	if (error)
		m_Impl->ThrowClosed();

	m_Impl->EnableButton(false);
	co_return clicked;
}

void Window::ShowError(const std::wstring_view& message)
{
	// a window that was destroyed before Run may have left its WM_QUIT behind, which would dismiss the message box at once
	auto quit = MSG{};
	PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);

	MessageBoxW(nullptr, std::wstring{ message }.c_str(), ::window_title, MB_OK | MB_ICONERROR);
}
