#include "error.h"
#include "nic_onboard.h"
#include "window.h"

#include <exception>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

int main()
{
	try {
		auto context = boost::asio::io_context{};
		auto window = nic_onboard::Window{ context };
		auto application = nic_onboard::NicOnboard{ context, window };

		auto exitCode = 1;
		auto failure = std::exception_ptr{};
		boost::asio::co_spawn(context, application.Launch(), [&](std::exception_ptr ex, int code) {
			failure = ex;
			exitCode = code;
			window.Close();
		});

		// there is no context.run(): the window's message loop runs the context
		window.Run();

		// usually the application has ended by now, and closed the window. if the window went away on its own instead,
		// the application gets to finish what it is doing right now (a registry change, DISM) before everything is torn down
		context.restart();
		context.run();

		if (failure)
			std::rethrow_exception(failure);

		return exitCode;
	}
	catch (const std::exception& e) {
		nic_onboard::Window::ShowError(nic_onboard::describe(e));
		return 1;
	}
}
