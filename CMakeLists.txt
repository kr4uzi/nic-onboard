cmake_minimum_required(VERSION 3.30)

project(nic_onboard VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# an installed Boost package, or (for cross builds) a directory which contains the boost/ headers. nothing is downloaded
set(NIC_ONBOARD_BOOST_ROOT "" CACHE PATH "Local Boost source/header tree")
add_library(nic_onboard_boost INTERFACE)
if(NIC_ONBOARD_BOOST_ROOT)
	if(NOT EXISTS "${NIC_ONBOARD_BOOST_ROOT}/boost/asio/awaitable.hpp")
		message(FATAL_ERROR "NIC_ONBOARD_BOOST_ROOT must contain boost/asio/awaitable.hpp")
	endif()
	target_include_directories(nic_onboard_boost SYSTEM INTERFACE "${NIC_ONBOARD_BOOST_ROOT}")
else()
	find_package(Boost 1.86 CONFIG REQUIRED)
	target_link_libraries(nic_onboard_boost INTERFACE Boost::headers)
endif()
find_package(Threads REQUIRED)
target_link_libraries(nic_onboard_boost INTERFACE Threads::Threads)

if(WIN32)
	add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
	# asio initializes Winsock (WSAStartup) even though we never open a socket
	target_link_libraries(nic_onboard_boost INTERFACE ws2_32)
endif()
if(MSVC)
	add_compile_options(/W4 /permissive- /EHsc)
else()
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()
if(MINGW)
	# no DLLs next to the executable: WinPE has none of them.
	# in C++26 the copy constructors of std::runtime_error and std::logic_error are constexpr, so they are emitted into
	# our objects as well - and the static libstdc++ of mingw-w64 (GCC 16) still has its own, which the linker rejects
	add_link_options(-static -static-libgcc -static-libstdc++ -Wl,--allow-multiple-definition)
endif()

# the application itself. it does not know Windows: the classes it uses are implemented by the sources below
set(NIC_ONBOARD_SOURCES src/nic_onboard.cpp src/boot_plan.cpp src/ibft.cpp src/error.cpp)
set(NIC_ONBOARD_WINDOWS_SOURCES
	src/main.cpp
	src/window.cpp
	src/winpe.cpp
	src/storage.cpp
	src/network.cpp
	src/drivers.cpp
	src/registry.cpp
	src/subprocess.cpp
	src/win32.cpp
	src/resources.rc
)

if(WIN32)
	enable_language(RC)
	add_executable(nic-onboard WIN32 ${NIC_ONBOARD_SOURCES} ${NIC_ONBOARD_WINDOWS_SOURCES})
	target_include_directories(nic-onboard PRIVATE src)
	target_link_libraries(nic-onboard PRIVATE nic_onboard_boost setupapi cfgmgr32 advapi32 iphlpapi user32 gdi32)
	if(MSVC)
		# a GUI application (no console window) which still starts at int main()
		target_link_options(nic-onboard PRIVATE /ENTRY:mainCRTStartup)
	endif()
endif()

# the tests run anywhere: tests/fake.cpp takes the place of the Windows sources
include(CTest)
if(BUILD_TESTING)
	add_executable(nic-onboard-policy-tests tests/policy_test.cpp src/boot_plan.cpp src/ibft.cpp src/error.cpp)
	target_include_directories(nic-onboard-policy-tests PRIVATE src)
	target_link_libraries(nic-onboard-policy-tests PRIVATE nic_onboard_boost)
	add_test(NAME policy COMMAND nic-onboard-policy-tests)

	add_executable(nic-onboard-tests tests/nic_onboard_test.cpp tests/fake.cpp ${NIC_ONBOARD_SOURCES})
	target_include_directories(nic-onboard-tests PRIVATE src)
	target_link_libraries(nic-onboard-tests PRIVATE nic_onboard_boost)
	add_test(NAME nic-onboard COMMAND nic-onboard-tests)
	set_tests_properties(nic-onboard PROPERTIES TIMEOUT 30)

	if(MINGW)
		# std::println
		target_link_libraries(nic-onboard-policy-tests PRIVATE stdc++exp)
		target_link_libraries(nic-onboard-tests PRIVATE stdc++exp)
	endif()
endif()
