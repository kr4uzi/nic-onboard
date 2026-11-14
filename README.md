# WinPE network boot preparation

A small Windows PE program with a window. It runs on a machine that was booted from the network into WinPE with
the writable iSCSI snapshot of the golden image attached, and changes that snapshot so it can boot *this* machine:
it adds the driver of the network adapter WinPE is using and registers the adapter in the offline SYSTEM hive.
Then it offers a Restart button.

## Reading the code

**Start at [`NicOnboard::Launch`](src/nic_onboard.cpp).** The whole application is that one coroutine and the ones it awaits,
read top to bottom:

```cpp
task<int> NicOnboard::Launch()
{
	if (!co_await Prepare()) {
		m_Window.SetTitle(L"Preparation stopped");
		co_await m_Window.WaitForButton(L"Close");
		co_return 1;
	}

	m_Window.SetTitle(L"Your network boot is ready");
	m_Window.SetFooter(L"Restart this computer to start Windows.");
	while (co_await m_Window.WaitForButton(L"Restart")) {
		...
		co_await m_WinPE.Reboot();
```

```cpp
		co_await InitWinPE();
		co_await ProtectLocalDisks();

		auto windows = co_await FindWindows();
		auto adapter = co_await FindNetworkAdapter();
		auto package = co_await InstallDriver(windows, adapter);
		auto plan = co_await ConfigureBoot(windows, adapter, package);
		co_await VerifyBoot(windows, plan);
```

Each of those is one row of the window and one short member coroutine. It stops at the first exception: `Prepare`
catches it once and hands it to `Window::Fail`, which marks the row that was running as failed and shows
"Could not <that step>" with the full reason in a box at the bottom of the window. There is no retry and no rollback.

| File | What is in it |
| --- | --- |
| `main.cpp` | `io_context`, `Window`, `NicOnboard`, one `co_spawn`, the message loop. |
| `nic_onboard.*` | `NicOnboard`: the application (see above). Knows nothing about Windows. |
| `boot_plan.*` | The decisions, as plain functions without I/O: which Windows, which adapter, `BootPlan::Create`. They throw if there is no candidate or more than one. |
| `ibft.*` | `boot_mac_from_ibft`: the boot adapter's MAC out of the firmware table iPXE leaves behind. Pure parsing, no I/O. |
| `window.*` | `Window`: the Win32 window. Rows with spinner/check mark/cross, the error box, and `WaitForButton`. |
| `winpe.*` | `WinPE`: `wpeinit`, `wpeutil reboot`. |
| `storage.*` | `Storage`: disks, taking local disks offline, finding Windows installations. |
| `network.*` | `Network`: the network adapters WinPE runs. |
| `drivers.*` | `Drivers`: DISM, finding the staged package, putting the service binary in place. |
| `registry.*` | `Registry`: reading, changing and verifying the offline SYSTEM hive. |
| `subprocess.*` | `run_process`: start a tool, `co_await` its exit. |
| `worker.h` | `Worker`: one thread for blocking calls, see below. |
| `task.h`, `error.*`, `win32.*` | `task<T>`, the `Error` exception and `widen` (Windows code page conversion, a plain copy elsewhere), Win32 helpers (`<windows.h>` is only included by `.cpp` files, through `win32.h`). |

The structs live next to the class that produces them (`Disk` in `storage.h`, `NetworkAdapter` in `network.h`, ...).
There are no interfaces: `NicOnboard` holds a `Storage`, a `Registry` and so on as plain members.

## How the asynchronous part works

Everything runs on one thread, the one that owns the window. Three things make that possible:

- **The message loop runs asio.** A window needs `GetMessage`, so there is no `context.run()`. Instead a 15 ms window
  timer calls `context.poll()` (`Window::Impl::Tick`), which runs whatever became ready: this is where the coroutines
  continue. It keeps working while Windows runs a loop of its own (dragging the window, a menu).
- **A click is awaited like a socket read.** The window procedure `try_send`s into an asio channel and
  `Window::WaitForButton` receives from it. The button and the close box are only enabled while a coroutine waits
  there, so there are no "busy" flags. Destroying the window closes the channel, which ends the application.
- **Blocking calls go to the `Worker`.** Disk, registry and driver store calls take seconds.
  `co_await m_Worker.Run([&] { ... })` runs them on one separate thread and continues on the window's thread.
  Those calls are wrapped inside the classes, so the application just writes `co_await m_Storage.GetDisks()`.
  Tools (DISM, wpeinit, wpeutil) are not waited for on a thread: `run_process` awaits the process handle.

The window cannot be closed while the preparation runs. If it goes away on its own (an exception inside the window
procedure), the operation that is running finishes, the next step does not begin, and `main` shows the reason.

## Build

Requirements: CMake 3.30+, a compiler and standard library with C++26 mode (GCC 16 is what this was built with), and
Boost 1.86+ headers. Boost is header-only here; the executable needs no Boost or compiler DLLs, PowerShell, WMI or .NET.
WinPE still needs its own NIC/storage/iSCSI drivers and DISM.

Cross build with mingw-w64. Hand the cross compiler only the `boost` directory, not all of `/usr/include`:

```sh
mkdir -p /tmp/nic-onboard-boost-headers
ln -sfn /usr/include/boost /tmp/nic-onboard-boost-headers/boost
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake \
  -DNIC_ONBOARD_BOOST_ROOT=/tmp/nic-onboard-boost-headers
cmake --build build-win
```

On Windows there is a Visual Studio solution: `nic-onboard.sln` with `src/nic-onboard.vcxproj` (v143 toolset,
`/std:c++latest`, static runtime) and a vcpkg manifest next to it, which provides `boost-asio` (static). It needs
Visual Studio 2022 or just its
Build Tools with the C++ workload, and vcpkg (the one that comes with Visual Studio, or your own after
`vcpkg integrate install`). Open the solution, or:

```powershell
msbuild nic-onboard.sln -p:Configuration=Release -p:Platform=x64
```

The result is `x64\Release\nic-onboard.exe`. The solution does not use `resources.rc`: MSBuild embeds
`application.manifest` itself. **This solution was written on Linux and has not been built yet** - neither has the
code been compiled with MSVC, so expect a first round of fixes.

The target is `nic-onboard.exe` (x64, GUI subsystem, regular `int main()`). Two things in the mingw build are
workarounds: `-Wl,--allow-multiple-definition` (C++26 makes the copy constructors of `std::runtime_error` and
`std::logic_error` constexpr, and the static libstdc++ still has its own) and the declaration of
`SetupGetInfDriverStoreLocationW` in `network.cpp` (missing in the mingw headers, present in the import library).

On Arch Linux the packages are `gcc cmake ninja boost mingw-w64-gcc`.

## Use

WinPE must already run the NIC with its driver (built into `boot.wim` or loaded with `drvload`). Start the program
against the intended writable iSCSI snapshot:

```bat
nic-onboard.exe
```

There are no command-line options. The program finds the iSCSI Windows installation, the connected physical NIC and
the original adapter of the image on its own, stops if any of them is ambiguous, and waits up to 60 seconds for the
iSCSI disk. With several cabled NICs the boot adapter is taken from the iBFT, the ACPI table iPXE leaves behind with
the MAC it booted through (`Network::GetBootMac`); without an iBFT there has to be exactly one. After verification, click Restart. The exit code is 0 after a successful preparation, 1 otherwise.

Things to know:

- Local disks are taken offline and disabled for the WinPE session only; identities are checked again right before.
- The control set is the one named by `Select\Current` of the offline hive, `ControlSet001` is not assumed.
  A machine that was onboarded before gets its class instance slot again.
- The published `oemNN.inf` and the driver store package in the offline Windows are found by comparing the INF
  contents with the one WinPE uses. The service binary name comes from the service's ImagePath, it is never guessed.
- The first time, the untouched SYSTEM hive is saved as `Windows\System32\Config\SYSTEM.nic-onboard-before` on the
  target. It is kept across later preparations.
- Nothing is logged and the output of tools is discarded. Errors are shown in the window.
- The program cannot tell a golden image from its writable snapshot: the boot service must only expose the copy.
  A failed preparation can leave a snapshot half changed: discard it and provision a new one.
- Verification proves that the planned values were saved, not that every NIC/Windows combination boots.

## Tests

The tests do not use mocks or interfaces. They link the real `nic_onboard.cpp` against [`tests/fake.cpp`](tests/fake.cpp),
which implements the same classes (`Window`, `WinPE`, `Storage`, `Network`, `Drivers`, `Registry`) on top of a
described machine, so they run on Linux:

```sh
cmake -S . -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

- `policy`: choosing Windows and the adapter (with and without the iBFT MAC), parsing an iPXE-style iBFT, ambiguity,
  GUID preservation, slot reuse, invalid control set.
- `nic-onboard`: the order of operations, stopping at each possible failure with the right row marked, waiting for the
  iSCSI disk and its timeout, failed verification, Restart/Close/retry, a window that goes away, and that the `Worker`
  runs elsewhere and resumes on the caller's thread.

What is **not** covered by these tests is everything that talks to Windows: `window.cpp`, `storage.cpp`,
`network.cpp`, `drivers.cpp`, `registry.cpp`, `subprocess.cpp`, `winpe.cpp`.

The mingw-w64 executable was run by hand under Wine 11.20 (no WinPE, no disks): the window and its rows, the spinner,
the error box (also with a reason of several lines), the Close button and closing the window, the ignored close request while preparing, waiting for a started
process, the worker thread and the iSCSI timeout all behave as intended there. The code that enumerates adapters,
services drivers and changes the registry has never run. Before changing the PXE startup, try it on disposable
WinPE/iSCSI snapshots:
an unknown and a previously onboarded NIC, several NICs, a missing driver, a failing DISM, local disks coming back
after the restart, the window at 1024x768 and with high DPI, and an actual network boot of the prepared Windows.
