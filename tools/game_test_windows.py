"""Launch one native GameTest entry point on a private WinSta0 desktop.

This module does not switch desktops, synthesize input, change global audio, or
control services.  The caller owns the process lifecycle and must wait for (or
kill) the direct child before closing this object.

Win32 contracts used here:
https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-createdesktopw
https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw
https://learn.microsoft.com/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow
https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-openinputdesktop
https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-enumdesktopwindows
"""
from __future__ import annotations

from dataclasses import dataclass
import ctypes
from ctypes import wintypes
import hashlib
import math
import msvcrt
import os
from pathlib import Path
import secrets
import subprocess
from typing import BinaryIO, Mapping, Sequence


CREATE_NO_WINDOW = 0x08000000
CREATE_UNICODE_ENVIRONMENT = 0x00000400
EXTENDED_STARTUPINFO_PRESENT = 0x00080000
STARTF_USESHOWWINDOW = 0x00000001
STARTF_USESTDHANDLES = 0x00000100
SW_SHOWNOACTIVATE = 4

WAIT_OBJECT_0 = 0x00000000
WAIT_TIMEOUT = 0x00000102
INFINITE = 0xFFFFFFFF
STILL_ACTIVE = 259

STD_INPUT_HANDLE = 0xFFFFFFF6
STD_OUTPUT_HANDLE = 0xFFFFFFF5
STD_ERROR_HANDLE = 0xFFFFFFF4
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x00000080
HANDLE_FLAG_INHERIT = 0x00000001
DUPLICATE_SAME_ACCESS = 0x00000002

DESKTOP_READOBJECTS = 0x0001
GENERIC_ALL = 0x10000000
UOI_NAME = 2
PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020002

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("nLength", wintypes.DWORD),
        ("lpSecurityDescriptor", ctypes.c_void_p),
        ("bInheritHandle", wintypes.BOOL),
    ]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_ubyte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class STARTUPINFOEXW(ctypes.Structure):
    _fields_ = [
        ("StartupInfo", STARTUPINFOW),
        ("lpAttributeList", ctypes.c_void_p),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


@dataclass
class _Spawned:
    process_handle: int
    pid: int
    stdout: BinaryIO | None
    stderr: BinaryIO | None


@dataclass
class _DesktopState:
    name: str
    window_count: int
    target_window_count: int


@dataclass
class _ChildStream:
    child_handle: int
    parent_read_handle: int | None = None


def _desktop_name(run_id: str) -> str:
    if not isinstance(run_id, str) or not run_id:
        raise ValueError("run_id must be a non-empty string")
    if "\x00" in run_id:
        raise ValueError("run_id contains NUL")
    digest = hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:12]
    return f"SDGO-GameTest-{os.getpid()}-{digest}-{secrets.token_hex(4)}"


def _argument_list(arguments: Sequence[object]) -> list[str]:
    if isinstance(arguments, (str, bytes, os.PathLike)):
        raise TypeError("arguments must be a non-empty sequence, not a command string")
    values = [os.fspath(value) if isinstance(value, os.PathLike) else str(value)
              for value in arguments]
    if not values or not values[0]:
        raise ValueError("arguments must contain an executable")
    if any("\x00" in value for value in values):
        raise ValueError("arguments contain NUL")
    return values


def _environment_block(environment: Mapping[str, str] | None) -> ctypes.Array | None:
    if environment is None:
        return None
    entries: list[str] = []
    for raw_key, raw_value in environment.items():
        key = str(raw_key)
        value = str(raw_value)
        if not key or "\x00" in key or "\x00" in value or (
            "=" in key and not key.startswith("=")
        ):
            raise ValueError(f"invalid environment entry name: {key!r}")
        entries.append(f"{key}={value}")
    entries.sort(key=str.upper)
    # create_unicode_buffer adds the final NUL; this explicit NUL makes the
    # double-NUL-terminated block required by CreateProcessW.
    return ctypes.create_unicode_buffer("\x00".join(entries) + "\x00")


class _Win32Api:
    def __init__(self) -> None:
        if os.name != "nt":
            raise OSError("WindowsBackgroundProcess requires Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self._configure()

    def _configure(self) -> None:
        k = self.kernel
        u = self.user
        k.GetCurrentProcess.restype = wintypes.HANDLE
        k.GetStdHandle.argtypes = [wintypes.DWORD]
        k.GetStdHandle.restype = wintypes.HANDLE
        k.DuplicateHandle.argtypes = [
            wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
            ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL,
            wintypes.DWORD,
        ]
        k.DuplicateHandle.restype = wintypes.BOOL
        k.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
            ctypes.POINTER(SECURITY_ATTRIBUTES), wintypes.DWORD,
            wintypes.DWORD, wintypes.HANDLE,
        ]
        k.CreateFileW.restype = wintypes.HANDLE
        k.CreatePipe.argtypes = [
            ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
            ctypes.POINTER(SECURITY_ATTRIBUTES), wintypes.DWORD,
        ]
        k.CreatePipe.restype = wintypes.BOOL
        k.SetHandleInformation.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
        ]
        k.SetHandleInformation.restype = wintypes.BOOL
        k.InitializeProcThreadAttributeList.argtypes = [
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        k.InitializeProcThreadAttributeList.restype = wintypes.BOOL
        k.UpdateProcThreadAttribute.argtypes = [
            ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p,
            ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
        ]
        k.UpdateProcThreadAttribute.restype = wintypes.BOOL
        k.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
        k.CreateProcessW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPWSTR,
            ctypes.POINTER(SECURITY_ATTRIBUTES),
            ctypes.POINTER(SECURITY_ATTRIBUTES), wintypes.BOOL,
            wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
            ctypes.POINTER(STARTUPINFOW),
            ctypes.POINTER(PROCESS_INFORMATION),
        ]
        k.CreateProcessW.restype = wintypes.BOOL
        k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.WaitForSingleObject.restype = wintypes.DWORD
        k.GetExitCodeProcess.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
        ]
        k.GetExitCodeProcess.restype = wintypes.BOOL
        k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        k.TerminateProcess.restype = wintypes.BOOL
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.CloseHandle.restype = wintypes.BOOL

        u.GetProcessWindowStation.restype = wintypes.HANDLE
        u.GetUserObjectInformationW.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        u.GetUserObjectInformationW.restype = wintypes.BOOL
        u.CreateDesktopW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD,
            wintypes.DWORD, ctypes.POINTER(SECURITY_ATTRIBUTES),
        ]
        u.CreateDesktopW.restype = wintypes.HANDLE
        u.CloseDesktop.argtypes = [wintypes.HANDLE]
        u.CloseDesktop.restype = wintypes.BOOL
        u.OpenInputDesktop.argtypes = [
            wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
        ]
        u.OpenInputDesktop.restype = wintypes.HANDLE
        u.GetWindowThreadProcessId.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.DWORD),
        ]
        u.GetWindowThreadProcessId.restype = wintypes.DWORD
        u.GetForegroundWindow.restype = wintypes.HWND

    @staticmethod
    def _error(message: str) -> OSError:
        code = ctypes.get_last_error()
        return OSError(code, f"{message}: {ctypes.FormatError(code)}")

    @staticmethod
    def _valid_handle(handle: object) -> bool:
        value = int(handle or 0)
        return value not in (0, INVALID_HANDLE_VALUE)

    def close_handle(self, handle: int | None) -> None:
        if self._valid_handle(handle):
            self.kernel.CloseHandle(wintypes.HANDLE(handle))

    def close_desktop(self, desktop: int | None) -> None:
        if self._valid_handle(desktop):
            self.user.CloseDesktop(wintypes.HANDLE(desktop))

    def _user_object_name(self, handle: int) -> str:
        needed = wintypes.DWORD()
        buffer = ctypes.create_unicode_buffer(256)
        if self.user.GetUserObjectInformationW(
            wintypes.HANDLE(handle), UOI_NAME, buffer, ctypes.sizeof(buffer),
            ctypes.byref(needed),
        ):
            return buffer.value
        if ctypes.get_last_error() != 122 or needed.value == 0:
            raise self._error("GetUserObjectInformationW(UOI_NAME) failed")
        large = ctypes.create_unicode_buffer(
            max(1, math.ceil(needed.value / ctypes.sizeof(ctypes.c_wchar)))
        )
        if not self.user.GetUserObjectInformationW(
            wintypes.HANDLE(handle), UOI_NAME, large, ctypes.sizeof(large),
            ctypes.byref(needed),
        ):
            raise self._error("GetUserObjectInformationW(UOI_NAME) failed")
        return large.value

    def assert_window_station(self) -> str:
        handle = self.user.GetProcessWindowStation()
        if not self._valid_handle(handle):
            raise self._error("GetProcessWindowStation failed")
        name = self._user_object_name(int(handle))
        if name.casefold() != "winsta0":
            raise RuntimeError(
                f"GameTest background desktop requires WinSta0, found {name!r}"
            )
        return name

    def input_desktop_name(self) -> str:
        desktop = self.user.OpenInputDesktop(0, False, DESKTOP_READOBJECTS)
        if not self._valid_handle(desktop):
            raise self._error("OpenInputDesktop failed")
        try:
            return self._user_object_name(int(desktop))
        finally:
            self.close_desktop(int(desktop))

    def create_desktop(self, name: str) -> int:
        desktop = self.user.CreateDesktopW(
            name, None, None, 0, GENERIC_ALL, None,
        )
        if not self._valid_handle(desktop):
            raise self._error("CreateDesktopW failed")
        return int(desktop)

    def _duplicate_handle(self, source: int) -> int:
        current = self.kernel.GetCurrentProcess()
        copied = wintypes.HANDLE()
        if not self.kernel.DuplicateHandle(
            current, wintypes.HANDLE(source), current, ctypes.byref(copied),
            0, True, DUPLICATE_SAME_ACCESS,
        ):
            raise self._error("DuplicateHandle failed")
        return int(copied.value)

    def _nul_handle(self, access: int) -> int:
        attributes = SECURITY_ATTRIBUTES(
            ctypes.sizeof(SECURITY_ATTRIBUTES), None, True,
        )
        handle = self.kernel.CreateFileW(
            "NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
            ctypes.byref(attributes), OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None,
        )
        if not self._valid_handle(handle):
            raise self._error("opening NUL failed")
        return int(handle)

    def _inherited_standard_handle(self, standard: int, access: int) -> int:
        handle = self.kernel.GetStdHandle(standard)
        if not self._valid_handle(handle):
            return self._nul_handle(access)
        return self._duplicate_handle(int(handle))

    def _pipe(self) -> _ChildStream:
        attributes = SECURITY_ATTRIBUTES(
            ctypes.sizeof(SECURITY_ATTRIBUTES), None, True,
        )
        read_handle = wintypes.HANDLE()
        write_handle = wintypes.HANDLE()
        if not self.kernel.CreatePipe(
            ctypes.byref(read_handle), ctypes.byref(write_handle),
            ctypes.byref(attributes), 0,
        ):
            raise self._error("CreatePipe failed")
        read_value = int(read_handle.value)
        write_value = int(write_handle.value)
        if not self.kernel.SetHandleInformation(
            read_handle, HANDLE_FLAG_INHERIT, 0,
        ):
            self.close_handle(read_value)
            self.close_handle(write_value)
            raise self._error("SetHandleInformation failed")
        return _ChildStream(write_value, read_value)

    def _output_stream(self, spec: object, standard: int) -> _ChildStream:
        if spec is subprocess.PIPE:
            return self._pipe()
        if spec is subprocess.DEVNULL:
            return _ChildStream(self._nul_handle(GENERIC_WRITE))
        if spec is None:
            return _ChildStream(
                self._inherited_standard_handle(standard, GENERIC_WRITE)
            )
        if spec is subprocess.STDOUT:
            raise ValueError("STDOUT is valid only for stderr")
        descriptor = spec if isinstance(spec, int) else spec.fileno()
        source = msvcrt.get_osfhandle(descriptor)
        if source == -1:
            raise OSError("invalid output file descriptor")
        return _ChildStream(self._duplicate_handle(source))

    def _stdio(
        self, stdout: object, stderr: object,
    ) -> tuple[_ChildStream, _ChildStream, _ChildStream]:
        stdin_stream = _ChildStream(self._nul_handle(GENERIC_READ))
        try:
            stdout_stream = self._output_stream(stdout, STD_OUTPUT_HANDLE)
        except BaseException:
            self.close_handle(stdin_stream.child_handle)
            raise
        try:
            if stderr is subprocess.STDOUT:
                stderr_stream = _ChildStream(
                    self._duplicate_handle(stdout_stream.child_handle)
                )
            else:
                stderr_stream = self._output_stream(stderr, STD_ERROR_HANDLE)
        except BaseException:
            self.close_handle(stdin_stream.child_handle)
            self.close_handle(stdout_stream.child_handle)
            self.close_handle(stdout_stream.parent_read_handle)
            raise
        return stdin_stream, stdout_stream, stderr_stream

    def _parent_pipe(self, handle: int | None) -> BinaryIO | None:
        if handle is None:
            return None
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            self.close_handle(handle)
            raise
        try:
            return os.fdopen(descriptor, "rb", buffering=0)
        except BaseException:
            os.close(descriptor)
            raise

    def spawn(
        self,
        arguments: Sequence[str],
        cwd: str | None,
        environment: Mapping[str, str] | None,
        desktop_path: str,
        stdout: object,
        stderr: object,
    ) -> _Spawned:
        stdin_stream, stdout_stream, stderr_stream = self._stdio(stdout, stderr)
        streams = (stdin_stream, stdout_stream, stderr_stream)
        attribute_buffer: ctypes.Array | None = None
        attribute_list: int | None = None
        process = PROCESS_INFORMATION()
        process_created = False
        parent_stdout: BinaryIO | None = None
        parent_stderr: BinaryIO | None = None
        try:
            unique_handles = list(dict.fromkeys(
                stream.child_handle for stream in streams
            ))
            handle_array = (wintypes.HANDLE * len(unique_handles))(
                *unique_handles
            )
            attribute_size = ctypes.c_size_t()
            ctypes.set_last_error(0)
            self.kernel.InitializeProcThreadAttributeList(
                None, 1, 0, ctypes.byref(attribute_size)
            )
            if attribute_size.value == 0:
                raise self._error(
                    "InitializeProcThreadAttributeList size query failed"
                )
            attribute_buffer = ctypes.create_string_buffer(attribute_size.value)
            attribute_list = ctypes.addressof(attribute_buffer)
            if not self.kernel.InitializeProcThreadAttributeList(
                ctypes.c_void_p(attribute_list), 1, 0,
                ctypes.byref(attribute_size),
            ):
                raise self._error("InitializeProcThreadAttributeList failed")
            if not self.kernel.UpdateProcThreadAttribute(
                ctypes.c_void_p(attribute_list), 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                ctypes.cast(handle_array, ctypes.c_void_p),
                ctypes.sizeof(handle_array), None, None,
            ):
                raise self._error(
                    "UpdateProcThreadAttribute(handle list) failed"
                )

            startup = STARTUPINFOEXW()
            startup.StartupInfo.cb = ctypes.sizeof(startup)
            startup.StartupInfo.lpDesktop = desktop_path
            startup.StartupInfo.dwFlags = (
                STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES
            )
            # Render normally on the private, non-input desktop. Some legacy
            # Direct3D clients require a visible native window to initialize.
            # This desktop is never switched to the user's input desktop.
            startup.StartupInfo.wShowWindow = SW_SHOWNOACTIVATE
            startup.StartupInfo.hStdInput = wintypes.HANDLE(
                stdin_stream.child_handle
            )
            startup.StartupInfo.hStdOutput = wintypes.HANDLE(
                stdout_stream.child_handle
            )
            startup.StartupInfo.hStdError = wintypes.HANDLE(
                stderr_stream.child_handle
            )
            startup.lpAttributeList = ctypes.c_void_p(attribute_list)

            command_line = ctypes.create_unicode_buffer(
                subprocess.list2cmdline(list(arguments))
            )
            environment_buffer = _environment_block(environment)
            environment_pointer = (
                ctypes.cast(environment_buffer, ctypes.c_void_p)
                if environment_buffer is not None else None
            )
            flags = (
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                EXTENDED_STARTUPINFO_PRESENT
            )
            if not self.kernel.CreateProcessW(
                None, command_line, None, None, True, flags,
                environment_pointer, cwd, ctypes.byref(startup.StartupInfo),
                ctypes.byref(process),
            ):
                raise self._error("CreateProcessW failed")
            process_created = True
            self.close_handle(int(process.hThread))
            process.hThread = None

            parent_stdout = self._parent_pipe(
                stdout_stream.parent_read_handle
            )
            stdout_stream.parent_read_handle = None
            if stderr is not subprocess.STDOUT:
                parent_stderr = self._parent_pipe(
                    stderr_stream.parent_read_handle
                )
                stderr_stream.parent_read_handle = None
            return _Spawned(
                int(process.hProcess), int(process.dwProcessId),
                parent_stdout, parent_stderr,
            )
        except BaseException:
            if parent_stdout is not None:
                parent_stdout.close()
            if parent_stderr is not None:
                parent_stderr.close()
            if process_created:
                self.kernel.TerminateProcess(process.hProcess, 1)
                self.kernel.WaitForSingleObject(process.hProcess, INFINITE)
            self.close_handle(int(process.hThread or 0))
            self.close_handle(int(process.hProcess or 0))
            raise
        finally:
            if attribute_list is not None:
                self.kernel.DeleteProcThreadAttributeList(
                    ctypes.c_void_p(attribute_list)
                )
            for stream in streams:
                self.close_handle(stream.child_handle)
                self.close_handle(stream.parent_read_handle)

    def poll(self, process_handle: int) -> int | None:
        result = self.kernel.WaitForSingleObject(
            wintypes.HANDLE(process_handle), 0,
        )
        if result == WAIT_TIMEOUT:
            return None
        if result != WAIT_OBJECT_0:
            raise self._error("WaitForSingleObject failed")
        code = wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(
            wintypes.HANDLE(process_handle), ctypes.byref(code),
        ):
            raise self._error("GetExitCodeProcess failed")
        if code.value == STILL_ACTIVE:
            return None
        return int(code.value)

    def wait(self, process_handle: int, timeout: float | None) -> int:
        if timeout is None:
            milliseconds = INFINITE
        else:
            if timeout < 0:
                raise ValueError("timeout must be non-negative")
            milliseconds = min(INFINITE - 1, math.ceil(timeout * 1000))
        result = self.kernel.WaitForSingleObject(
            wintypes.HANDLE(process_handle), milliseconds,
        )
        if result == WAIT_TIMEOUT:
            raise TimeoutError
        if result != WAIT_OBJECT_0:
            raise self._error("WaitForSingleObject failed")
        code = self.poll(process_handle)
        if code is None:
            raise RuntimeError("signaled process still reports STILL_ACTIVE")
        return code

    def terminate(self, process_handle: int) -> None:
        if self.poll(process_handle) is not None:
            return
        if not self.kernel.TerminateProcess(
            wintypes.HANDLE(process_handle), 1,
        ):
            raise self._error("TerminateProcess failed")

    def _desktop_windows(
        self, desktop: int, target_pid: int,
    ) -> tuple[int, int]:
        total = 0
        target = 0
        callback_type = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HWND, wintypes.LPARAM
        )
        self.user.EnumDesktopWindows.argtypes = [
            wintypes.HANDLE, callback_type, wintypes.LPARAM,
        ]
        self.user.EnumDesktopWindows.restype = wintypes.BOOL

        @callback_type
        def visit(window: int, _: int) -> bool:
            nonlocal total, target
            total += 1
            owner = wintypes.DWORD()
            self.user.GetWindowThreadProcessId(
                wintypes.HWND(window), ctypes.byref(owner)
            )
            if owner.value == target_pid:
                target += 1
            return True

        ctypes.set_last_error(0)
        if not self.user.EnumDesktopWindows(
            wintypes.HANDLE(desktop), visit, 0,
        ):
            code = ctypes.get_last_error()
            if code:
                raise self._error("EnumDesktopWindows failed")
        return total, target

    def desktop_state(self, desktop: int, target_pid: int) -> _DesktopState:
        total, target = self._desktop_windows(desktop, target_pid)
        return _DesktopState(
            self._user_object_name(desktop), total, target
        )

    def input_desktop_state(self, target_pid: int) -> _DesktopState:
        desktop = self.user.OpenInputDesktop(0, False, DESKTOP_READOBJECTS)
        if not self._valid_handle(desktop):
            raise self._error("OpenInputDesktop failed")
        try:
            return self.desktop_state(int(desktop), target_pid)
        finally:
            self.close_desktop(int(desktop))

    def foreground_pid(self) -> int:
        window = self.user.GetForegroundWindow()
        if not window:
            return 0
        owner = wintypes.DWORD()
        self.user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        return int(owner.value)


class WindowsBackgroundProcess:
    """A small Popen-like owner for one process on an isolated desktop."""

    @staticmethod
    def _new_api() -> _Win32Api:
        return _Win32Api()

    def __init__(
        self,
        arguments: Sequence[object],
        cwd: str | os.PathLike[str] | None,
        env: Mapping[str, str] | None,
        stdout: object,
        stderr: object,
        run_id: str,
    ) -> None:
        self.args = _argument_list(arguments)
        self.run_id = run_id
        self.desktop_name = _desktop_name(run_id)
        self.desktop_path = f"WinSta0\\{self.desktop_name}"
        self.returncode: int | None = None
        self.pid = 0
        self.stdout: BinaryIO | None = None
        self.stderr: BinaryIO | None = None
        self._closed = False
        self._process_handle: int | None = None
        self._desktop_handle: int | None = None
        self._api = self._new_api()

        self._api.assert_window_station()
        self._input_desktop_at_launch = self._api.input_desktop_name()
        desktop = self._api.create_desktop(self.desktop_name)
        self._desktop_handle = desktop
        try:
            current_input = self._api.input_desktop_name()
            if current_input.casefold() == self.desktop_name.casefold():
                raise RuntimeError(
                    "CreateDesktopW unexpectedly made the background desktop input"
                )
            working_directory = (
                str(Path(cwd).resolve()) if cwd is not None else None
            )
            spawned = self._api.spawn(
                self.args, working_directory, env, self.desktop_path,
                stdout, stderr,
            )
        except BaseException:
            self._api.close_desktop(self._desktop_handle)
            self._desktop_handle = None
            raise
        self._process_handle = spawned.process_handle
        self.pid = spawned.pid
        self.stdout = spawned.stdout
        self.stderr = spawned.stderr

    def poll(self) -> int | None:
        if self.returncode is not None:
            return self.returncode
        if self._process_handle is None:
            return self.returncode
        result = self._api.poll(self._process_handle)
        if result is not None:
            self.returncode = result
        return result

    def wait(self, timeout: float | None = None) -> int:
        if self.returncode is not None:
            return self.returncode
        if self._process_handle is None:
            raise RuntimeError("process handle is closed")
        try:
            self.returncode = self._api.wait(self._process_handle, timeout)
        except TimeoutError as error:
            raise subprocess.TimeoutExpired(self.args, timeout) from error
        return self.returncode

    def kill(self) -> None:
        if self.returncode is not None:
            return
        if self._process_handle is None:
            raise RuntimeError("process handle is closed")
        self._api.terminate(self._process_handle)

    def verify_background(self, target_pid: int | None = None) -> dict:
        """Return JSON-safe evidence that target_pid has a window only off-screen."""
        selected_pid = self.pid if target_pid is None else int(target_pid)
        receipt = {
            "verified": False,
            "run_id": self.run_id,
            "entry_pid": self.pid,
            "target_pid": selected_pid,
            "desktop_name": self.desktop_name,
            "desktop_path": self.desktop_path,
            "background_window_count": 0,
            "target_background_window_count": 0,
            "input_desktop_name": None,
            "input_desktop_unchanged": False,
            "input_target_window_count": 0,
            "foreground_pid": 0,
            "foreground_safe": False,
            "input_desktop_safe": False,
            "error": None,
        }
        if selected_pid <= 0:
            receipt["error"] = "target_pid must be positive"
            return receipt
        if self._desktop_handle is None:
            receipt["error"] = "background desktop is closed"
            return receipt
        try:
            background = self._api.desktop_state(
                self._desktop_handle, selected_pid
            )
            current_input = self._api.input_desktop_state(selected_pid)
            foreground_pid = self._api.foreground_pid()
            receipt.update(
                background_window_count=background.window_count,
                target_background_window_count=(
                    background.target_window_count
                ),
                input_desktop_name=current_input.name,
                input_desktop_unchanged=(
                    current_input.name.casefold() ==
                    self._input_desktop_at_launch.casefold()
                ),
                input_target_window_count=(
                    current_input.target_window_count
                ),
                foreground_pid=foreground_pid,
                foreground_safe=foreground_pid != selected_pid,
                input_desktop_safe=(
                    current_input.name.casefold() !=
                    self.desktop_name.casefold() and
                    current_input.target_window_count == 0
                ),
            )
            receipt["verified"] = bool(
                receipt["target_background_window_count"] > 0 and
                receipt["foreground_safe"] and
                receipt["input_desktop_safe"]
            )
        except OSError as error:
            receipt["error"] = f"{type(error).__name__}: {error}"
        return receipt

    def close(self) -> None:
        if self._closed:
            return
        if self.poll() is None:
            raise RuntimeError(
                "cannot close background desktop while owned child is running"
            )
        streams = []
        if self.stdout is not None:
            streams.append(self.stdout)
        if self.stderr is not None and self.stderr is not self.stdout:
            streams.append(self.stderr)
        for stream in streams:
            stream.close()
        self.stdout = None
        self.stderr = None
        self._api.close_handle(self._process_handle)
        self._process_handle = None
        self._api.close_desktop(self._desktop_handle)
        self._desktop_handle = None
        self._closed = True

    def __del__(self) -> None:
        # Never close the desktop underneath a running child.  At interpreter
        # shutdown Windows releases the controller's remaining handles.
        try:
            if not self._closed and self.poll() is not None:
                self.close()
        except BaseException:
            pass

