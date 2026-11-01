import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards.runtime_control import _atomic_write


class AtomicRuntimeTests(unittest.TestCase):
    def test_transient_replace_denial_reuses_one_durable_temp(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root) / "wallet-status.ini"
            destination.write_bytes(b"old")
            calls = []
            original = os.replace
            def denied(source, target):
                calls.append(source)
                if len(calls) < 3: raise PermissionError("brief native reader")
                return original(source, target)
            with patch("owner.rewards.runtime_control.os.replace", side_effect=denied):
                _atomic_write(destination, b"new")
            self.assertEqual(destination.read_bytes(), b"new")
            self.assertEqual(len(set(calls)), 1)
            self.assertEqual([p.name for p in Path(root).iterdir()], ["wallet-status.ini"])

    def test_permanent_denial_preserves_previous_file_and_cleans_temp(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root) / "wallet-status.ini"
            destination.write_bytes(b"old")
            with patch("owner.rewards.runtime_control.os.replace", side_effect=PermissionError("permanent")), \
                 patch("owner.rewards.runtime_control.time.monotonic", side_effect=[0, 1]):
                with self.assertRaises(PermissionError): _atomic_write(destination, b"new")
            self.assertEqual(destination.read_bytes(), b"old")
            self.assertEqual([p.name for p in Path(root).iterdir()], ["wallet-status.ini"])

    @unittest.skipUnless(sys.platform == "win32", "real Windows sharing semantics")
    def test_real_windows_reader_can_release_before_replace_deadline(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root) / "wallet-status.ini"
            destination.write_bytes(b"old")
            handle = kernel.CreateFileW(str(destination), 0x80000000, 1 | 2, None, 3, 0x80, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            failures = []
            def write():
                try: _atomic_write(destination, b"new")
                except Exception as error: failures.append(error)
            thread = threading.Thread(target=write)
            thread.start()
            try: time.sleep(0.05)
            finally: kernel.CloseHandle(handle)
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(destination.read_bytes(), b"new")


if __name__ == "__main__": unittest.main()
