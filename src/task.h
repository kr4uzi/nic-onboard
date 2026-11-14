#pragma once

#include <boost/asio/awaitable.hpp>

namespace nic_onboard {
	template<typename T>
	using task = boost::asio::awaitable<T>;
}
