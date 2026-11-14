#pragma once

#include "task.h"

#include <concepts>
#include <type_traits>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace nic_onboard {
	// disk, registry and driver store calls block for seconds. they run on this one thread, so the window keeps
	// painting, and the coroutine awaiting them continues on its own thread (the one running the window)
	class Worker
	{
		boost::asio::thread_pool m_Thread{ 1 };

	public:
		template<std::invocable Work>
		task<std::invoke_result_t<Work&>> Run(Work work)
		{
			co_return co_await boost::asio::co_spawn(m_Thread, [&]() -> task<std::invoke_result_t<Work&>> {
				co_return work();
			}, boost::asio::use_awaitable);
		}
	};
}
