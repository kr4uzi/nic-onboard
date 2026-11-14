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
| `window.*` | `Window`: native Win32 controls for progress rows, scrollable error text, and `WaitForButton`. |
| `winpe.*` | `WinPE`: `wpeinit`, `wpeutil reboot`. |
| `storage.*` | `Storage`: disks, taking local disks offline, finding Windows installations. |
| `network.*` | `Network`: the network adapters WinPE runs. |
| `drivers.*` | `Drivers`: DISM, finding the staged package, putting the service binary in place. |
| `driver_path.*` | Windows service path parsing and canonical boot paths, independent of the host OS and drive letters. |
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

Requirements: CMake 3.30+, a compiler and standard library supporting the C++23 features used here (GCC 16 is tested), and
Boost 1.86+ headers. Boost is header-only here; the executable needs no Boost or compiler DLLs, PowerShell, WMI or .NET.
WinPE still needs its own NIC/storage/iSCSI drivers and DISM.

C++23 provides the coroutines, ranges, formatting and printing this program uses. Requesting a specific standard
keeps compiler upgrades from silently enabling a newer language mode. In particular, it avoids the GCC 16/MinGW
C++26 duplicate exception-symbol problem without suppressing duplicate-symbol linker errors. CMake chooses the
compiler's corresponding standard switch; older MSVC/CMake combinations may still use `/std:c++latest` to supply C++23.

`CMakeLists.txt` is the build definition for all platforms; `CMakePresets.json` supplies the usual configurations.
Visual Studio and VS Code with CMake Tools can open the repository folder directly.

Cross build with mingw-w64. Expose only the `boost` directory to the cross compiler, not all of `/usr/include`:

```sh
mkdir -p build/boost-headers
ln -sfn /usr/include/boost build/boost-headers/boost
cmake --preset mingw
cmake --build --preset mingw
```

The result is `build/mingw/nic-onboard.exe`. Set `NIC_ONBOARD_BOOST_ROOT` with `-D` if the headers are elsewhere.
For x86, use `mingw-x86` in both commands; the result is `build/mingw-x86/nic-onboard.exe`.
On SteamOS, run these commands inside the existing development container with `distrobox enter mingw-build`.

On Windows, install Visual Studio 2022 or its Build Tools with the C++ workload, a recent CMake, and vcpkg.
Set `VCPKG_ROOT` to the vcpkg installation (including the one bundled with Visual Studio):

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

The root vcpkg manifest provides `boost-asio`; the preset selects the static x64 triplet and runtime. The result is
`build/windows/Release/nic-onboard.exe`. CMake also generates a solution under `build/windows` if you prefer one.
For x86, use `windows-x86` in all three commands. It selects the Win32 platform and static x86 triplet, with the result
at `build/windows-x86/Release/nic-onboard.exe`.
MSVC embeds `application.manifest` directly; MinGW embeds it through `resources.rc`.
**MSVC has not been tested here.** The Linux tests and MinGW cross-build are tested.

The target is `nic-onboard.exe` (x64 or x86, GUI subsystem, regular `int main()`). The declaration of
`SetupGetInfDriverStoreLocationW` in `network.cpp` fills a gap in the MinGW headers; its import library has the function.

On Arch Linux the packages are `gcc cmake ninja boost mingw-w64-gcc`.

### GitHub Actions

The [Build Windows workflow](.github/workflows/build-windows.yml) builds and tests x64 and x86 Release executables with
Visual Studio 2022 on pushes to `main` and pull requests. You can also start it from **Actions → Build Windows → Run workflow**.
It uses the same `windows` and `windows-x86` CMake presets and the runner's preinstalled vcpkg.

After a successful run, open its summary and download **nic-onboard-windows-x64** or **nic-onboard-windows-x86** under **Artifacts**.
Extract `nic-onboard.exe` from the downloaded ZIP. Artifacts are kept for 30 days.

## Use

WinPE must already run the NIC with its driver (built into `boot.wim` or loaded with `drvload`). Start the program
against the intended writable iSCSI snapshot:

```bat
nic-onboard.exe
```

Use matching architectures for the executable, WinPE, and target Windows. The program copies WinPE's NIC driver
package into the target, so x86 support is for preparing x86 Windows from x86 WinPE.
[WinPE cannot run applications built for another architecture](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro#limitations),
including x86 applications on x64 WinPE.

There are no command-line options. The program finds the iSCSI Windows installation, the connected physical NIC and
the original adapter of the image on its own, stops if any of them is ambiguous, and waits up to 60 seconds for the
iSCSI disk. With several cabled NICs the boot adapter is taken from the iBFT, the ACPI table iPXE leaves behind with
the MAC it booted through (`Network::GetBootMac`); without an iBFT there has to be exactly one. Multiple selected
iBFT NICs, multiple unselected candidates, or multiple connected adapters matching the boot MAC stop preparation.
After verification, click Restart. The exit code is 0 after a successful preparation, 1 otherwise.

Things to know:

- Local disks are taken offline and disabled for the WinPE session only; identities are checked again right before.
- The control set is the one named by `Select\Default` of the offline hive, which selects the next normal boot.
  `Select\Current` describes the previous boot; `ControlSet001` is not assumed.
  A machine that was onboarded before gets its class instance slot again.
- The published `oemNN.inf` and the driver store package in the offline Windows are found by comparing the INF
  contents with the one WinPE uses. The service binary name comes from the service's ImagePath, it is never guessed.
  The saved path is relative to Windows (`System32\...`), so WinPE's drive letters and environment do not leak into it.
- Before the registry plan is first applied, the SYSTEM hive is saved as
  `Windows\System32\Config\SYSTEM.nic-onboard-before` on the target. This is after DISM driver injection;
  the backup is kept across later preparations.
- Nothing is logged and the output of tools is discarded. Errors can be scrolled, selected and copied in the window.
- The program cannot tell a golden image from its writable snapshot: the boot service must only expose the copy.
  A failed preparation can leave a snapshot half changed: discard it and provision a new one.
- Verification proves that the planned values were saved, not that every NIC/Windows combination boots.

## Tests

The tests do not use mocks or interfaces. They link the real `nic_onboard.cpp` against [`tests/fake.cpp`](tests/fake.cpp),
which implements the same classes (`Window`, `WinPE`, `Storage`, `Network`, `Drivers`, `Registry`) on top of a
described machine, so they run on Linux:

```sh
cmake --preset host
cmake --build --preset host
ctest --preset host
```

- `policy`: choosing Windows and the adapter (with and without the iBFT MAC), parsing an iPXE-style iBFT, ambiguity,
  GUID preservation, slot reuse, invalid control set.
- `driver-path`: canonicalizing service paths from WinPE and existing installations, mapping them into offline
  Windows, and rejecting ambiguous or escaping paths. Windows path rules are tested even on Linux.
- `nic-onboard`: the order of operations, stopping at each possible failure with the right row marked, waiting for the
  iSCSI disk and its timeout, failed verification, Restart/Close/retry, a window that goes away, and that the `Worker`
  runs elsewhere and resumes on the caller's thread.
- `registry` (Windows builds): reads `Select\Default` when `Current` differs, rejects a missing default, invalid numbers
  or absent control sets, and reads strings without expanding WinPE environment variables. It uses a temporary key under the current
  user, without administrator privileges or changes to the live SYSTEM hive.

The Windows tests can also be cross-built and run under Wine:

```sh
cmake --preset mingw -DBUILD_TESTING=ON -DCMAKE_CROSSCOMPILING_EMULATOR=wine
cmake --build --preset mingw
ctest --test-dir build/mingw --output-on-failure
```

For x86 tests, use `mingw-x86` as the preset and `build/mingw-x86` as the test directory.

These tests do not exercise real disk isolation, adapter enumeration, DISM, offline hive loading/writing, or an
actual network boot. They also do not automate the UI or process-launching code.

The three Linux test suites and four Windows suites pass; the Windows tests were run for x64 and x86 under Wine 11.20.
A separate temporary UI harness checked the native controls at 100%, 150% and 200% DPI on a 1024x768 display:
rows stay separate, errors scroll, Unicode error text can be copied, and the button resumes the coroutine.
Column widths are fixed by the application: Details fills the remaining space, with hover tips for clipped text.
Earlier manual Wine checks covered ignored close requests while preparing, process waiting and the iSCSI timeout.
Wine does not establish that real adapter enumeration, driver servicing or offline SYSTEM changes will boot Windows.
Before changing the PXE startup, try disposable WinPE/iSCSI snapshots:
an unknown and a previously onboarded NIC, several NICs, a missing driver, a failing DISM, local disks coming back
after the restart, the window at 1024x768 and with high DPI, and an actual network boot of the prepared Windows.
