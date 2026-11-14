#include "window.h"
#include "win32.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <exception>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

using namespace nic_onboard;

namespace {
	constexpr auto window_class = L"NicOnboard";
	constexpr auto window_title = L"Network boot preparation";

	constexpr auto button_command = 101;

	constexpr auto blue = RGB(0, 100, 190);
	constexpr auto green = RGB(20, 125, 65);
	constexpr auto red = RGB(185, 35, 35);
	constexpr auto light_red = RGB(253, 236, 236);
	constexpr auto grey = RGB(105, 112, 122);
	constexpr auto light_grey = RGB(210, 214, 220);
	constexpr auto white = RGB(255, 255, 255);
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

	struct Row
	{
		HWND title = nullptr;
		HWND message = nullptr;
		RowState state = RowState::waiting;
	};

	void set_text(HWND control, const std::wstring_view& text)
	{
		SetWindowTextW(control, std::wstring{ text }.c_str());
	}

	void set_font(HWND control, HFONT font)
	{
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
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
	HWND m_Subtitle = nullptr;
	HWND m_Footer = nullptr;
	HWND m_Button = nullptr;
	std::array<Row, step_titles.size()> m_Rows;
	std::optional<std::size_t> m_Running;

	// the area between the rows and the button. it shows the footer, or - after Fail - the error (see PaintError)
	RECT m_Status{};
	std::wstring m_ErrorHeading;
	std::wstring m_Error;

	HFONT m_BodyFont = nullptr;
	HFONT m_TitleFont = nullptr;
	HFONT m_RowFont = nullptr;
	HFONT m_SmallFont = nullptr;
	int m_Dpi = 96;
	int m_RowHeight = 64;

	unsigned m_Ticks = 0;
	int m_SpinnerFrame = 0;

	// whatever went wrong inside the window procedure: exceptions cannot pass through Windows
	std::exception_ptr m_Exception;

	Impl(boost::asio::io_context& context)
		: m_Context{ context }, m_Clicks{ context }
	{
		SetProcessDPIAware();

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
		auto height = std::min(Scale(680), static_cast<int>(workArea.bottom - workArea.top - 20));
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
		m_Rows[index].state = state;
		::set_text(m_Rows[index].message, message);
		InvalidateIcons();
	}

	// the status area shows one or the other: the footer, or an error
	void ShowStatus(const std::wstring_view& footer, const std::wstring_view& errorHeading, const std::wstring_view& error)
	{
		m_ErrorHeading = errorHeading;
		m_Error = error;
		::set_text(m_Footer, footer);
		ShowWindow(m_Footer, m_Error.empty() ? SW_SHOW : SW_HIDE);
		Layout();
	}

private:
	int Scale(int value) const
	{
		return MulDiv(value, m_Dpi, 96);
	}

	HWND CreateControl(const wchar_t* kind, const wchar_t* text, DWORD style, int id = 0, HWND parent = nullptr)
	{
		auto control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, parent ? parent : m_Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
		if (!control)
			throw last_error(L"Create interface control");

		return control;
	}

	void CreateControls()
	{
		m_Title = CreateControl(L"STATIC", L"Preparing your network boot", SS_NOPREFIX);
		m_Subtitle = CreateControl(L"STATIC", L"Setting up this computer to start Windows from your network disk.", SS_NOPREFIX);
		for (auto index = std::size_t{ 0 }; index < m_Rows.size(); index++) {
			m_Rows[index].title = CreateControl(L"STATIC", ::step_titles[index], SS_NOPREFIX | SS_ENDELLIPSIS);
			m_Rows[index].message = CreateControl(L"STATIC", L"Waiting", SS_NOPREFIX | SS_ENDELLIPSIS);
		}

		m_Footer = CreateControl(L"STATIC", L"Please keep this computer connected to the network.", SS_NOPREFIX | SS_ENDELLIPSIS);
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
		for (auto font : { &m_BodyFont, &m_TitleFont, &m_RowFont, &m_SmallFont }) {
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
		m_SmallFont = create(13, FW_NORMAL);

		::set_font(m_Title, m_TitleFont);
		::set_font(m_Subtitle, m_BodyFont);
		::set_font(m_Footer, m_SmallFont);
		::set_font(m_Button, m_BodyFont);
		for (const auto& row : m_Rows) {
			::set_font(row.title, m_RowFont);
			::set_font(row.message, m_SmallFont);
		}
	}

	// title and subtitle on top, the button at the bottom and the status area above it. the rows share what is left:
	// each row has its icon (see Paint) on the left and two lines of text next to it.
	// the status area grows with the error it has to show, as far as the rows can give way
	void Layout()
	{
		auto bounds = RECT{};
		GetClientRect(m_Window, &bounds);
		auto width = static_cast<int>(bounds.right);
		auto height = static_cast<int>(bounds.bottom);
		auto margin = Scale(28);
		auto textLeft = Scale(78);
		auto rowsTop = Scale(113);
		auto statusBottom = height - Scale(60);
		auto statusLimit = statusBottom - Scale(6) - rowsTop - Scale(46) * static_cast<int>(m_Rows.size());
		auto statusHeight = std::max(Scale(86), std::min(MeasureError(width - margin * 2), statusLimit));

		m_Status = RECT{ margin, statusBottom - statusHeight, width - margin, statusBottom };
		m_RowHeight = (m_Status.top - Scale(6) - rowsTop) / static_cast<int>(m_Rows.size());

		MoveWindow(m_Title, margin, Scale(22), width - margin * 2, Scale(36), TRUE);
		MoveWindow(m_Subtitle, margin, Scale(66), width - margin * 2, Scale(38), TRUE);
		for (auto index = std::size_t{ 0 }; index < m_Rows.size(); index++) {
			auto top = rowsTop + static_cast<int>(index) * m_RowHeight;
			MoveWindow(m_Rows[index].title, textLeft, top, width - textLeft - margin, Scale(23), TRUE);
			MoveWindow(m_Rows[index].message, textLeft, top + Scale(24), width - textLeft - margin, Scale(19), TRUE);
		}

		MoveWindow(m_Footer, margin, m_Status.bottom - Scale(24), width - margin * 2, Scale(21), TRUE);
		MoveWindow(m_Button, width - margin - Scale(145), height - Scale(44), Scale(145), Scale(31), TRUE);
		InvalidateRect(m_Window, nullptr, TRUE);
	}

	void InvalidateIcons()
	{
		auto icons = RECT{ Scale(25), Scale(108), Scale(65), Scale(113) + m_RowHeight * static_cast<int>(m_Rows.size()) };
		InvalidateRect(m_Window, &icons, TRUE);
	}

	// the icons: an empty circle while waiting, a spinner while running, then a check mark or a cross
	void Paint()
	{
		auto paint = PAINTSTRUCT{};
		auto dc = BeginPaint(m_Window, &paint);
		auto previousPen = SelectObject(dc, GetStockObject(DC_PEN));
		auto previousBrush = SelectObject(dc, GetStockObject(DC_BRUSH));

		for (auto index = std::size_t{ 0 }; index < m_Rows.size(); index++) {
			auto x = Scale(45);
			auto y = Scale(113 + 19) + static_cast<int>(index) * m_RowHeight;
			auto radius = Scale(12);
			auto state = m_Rows[index].state;

			if (state == RowState::running) {
				// ten dots on a circle, the brightest one moves on with every frame
				for (auto dot = 0; dot < 10; dot++) {
					auto angle = 2 * std::numbers::pi * dot / 10;
					auto dotX = x + static_cast<int>(std::cos(angle) * radius);
					auto dotY = y + static_cast<int>(std::sin(angle) * radius);
					auto fade = ((dot - m_SpinnerFrame) % 10 + 10) % 10;
					auto color = RGB(20 + fade * 20, 105 + fade * 13, 190 + fade * 6);
					SetDCPenColor(dc, color);
					SetDCBrushColor(dc, color);
					Ellipse(dc, dotX - Scale(2), dotY - Scale(2), dotX + Scale(3), dotY + Scale(3));
				}

				continue;
			}

			auto color = state == RowState::completed ? ::green : state == RowState::failed ? ::red : ::light_grey;
			SetDCPenColor(dc, color);
			SetDCBrushColor(dc, state == RowState::waiting ? ::white : color);
			Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
			if (state == RowState::waiting)
				continue;

			auto pen = CreatePen(PS_SOLID, std::max(2, Scale(2)), ::white);
			auto previous = SelectObject(dc, pen);
			if (state == RowState::completed) {
				const POINT check[]{ { x - Scale(6), y }, { x - Scale(2), y + Scale(4) }, { x + Scale(6), y - Scale(5) } };
				Polyline(dc, check, 3);
			}
			else {
				MoveToEx(dc, x - Scale(4), y - Scale(4), nullptr);
				LineTo(dc, x + Scale(4), y + Scale(4));
				MoveToEx(dc, x + Scale(4), y - Scale(4), nullptr);
				LineTo(dc, x - Scale(4), y + Scale(4));
			}

			SelectObject(dc, previous);
			DeleteObject(pen);
		}

		PaintError(dc);

		SelectObject(dc, previousPen);
		SelectObject(dc, previousBrush);
		EndPaint(m_Window, &paint);
	}

	// the height PaintError needs for a box of this width
	int MeasureError(int width) const
	{
		if (m_Error.empty())
			return 0;

		auto dc = GetDC(m_Window);
		auto previousFont = SelectObject(dc, m_BodyFont);
		auto text = RECT{ 0, 0, width - Scale(32), 0 };
		DrawTextW(dc, m_Error.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
		SelectObject(dc, previousFont);
		ReleaseDC(m_Window, dc);

		return static_cast<int>(text.bottom) + Scale(18) + (m_ErrorHeading.empty() ? 0 : Scale(25));
	}

	// a light red box with a red bar on its left: what could not be done, and below it the whole reason
	void PaintError(HDC dc)
	{
		if (m_Error.empty())
			return;

		auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
		SetDCBrushColor(dc, ::light_red);
		FillRect(dc, &m_Status, brush);

		auto bar = m_Status;
		bar.right = bar.left + Scale(4);
		SetDCBrushColor(dc, ::red);
		FillRect(dc, &bar, brush);

		auto text = RECT{ m_Status.left + Scale(18), m_Status.top + Scale(10), m_Status.right - Scale(14), m_Status.bottom - Scale(8) };
		auto previousFont = SelectObject(dc, m_RowFont);
		SetBkMode(dc, TRANSPARENT);
		if (!m_ErrorHeading.empty()) {
			SetTextColor(dc, ::red);
			DrawTextW(dc, m_ErrorHeading.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			text.top += Scale(25);
		}

		// the reason wraps like in an edit control: also inside a long path, and without a last line cut in half
		SelectObject(dc, m_BodyFont);
		SetTextColor(dc, ::black);
		DrawTextW(dc, m_Error.c_str(), -1, &text, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
		SelectObject(dc, previousFont);
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

		if (++m_Ticks % 5 == 0) {
			m_SpinnerFrame = (m_SpinnerFrame + 1) % 10;
			InvalidateIcons();
		}
	}

	HBRUSH ColorText(HDC dc, HWND control)
	{
		auto color = control == m_Title ? ::blue : ::grey;
		for (const auto& row : m_Rows) {
			if (control == row.title)
				color = ::black;
			else if (control == row.message && row.state == RowState::failed)
				color = ::red;
		}

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
		case WM_PAINT:
			Paint();
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
			SetWindowPos(m_Window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
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
