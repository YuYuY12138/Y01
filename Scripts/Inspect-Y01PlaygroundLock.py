"""只查询 Windows Restart Manager 的资产占用者，不释放句柄或终止进程。"""
import ctypes as c
from ctypes import wintypes as w
import sys

class UniqueProcess(c.Structure):
    _fields_ = [("pid", w.DWORD), ("start", w.FILETIME)]

class ProcessInfo(c.Structure):
    _fields_ = [("process", UniqueProcess), ("app", w.WCHAR * 256),
                ("service", w.WCHAR * 64), ("kind", c.c_int),
                ("status", w.ULONG), ("session", w.DWORD), ("restartable", w.BOOL)]

rm = c.WinDLL("rstrtmgr")
handle = w.DWORD()
key = c.create_unicode_buffer(33)
status = rm.RmStartSession(c.byref(handle), 0, key)
if status:
    raise OSError(status, "RmStartSession")
try:
    files = (w.LPCWSTR * 1)(sys.argv[1])
    status = rm.RmRegisterResources(handle, 1, files, 0, None, 0, None)
    if status:
        raise OSError(status, "RmRegisterResources")
    needed, count, reasons = w.UINT(), w.UINT(), w.DWORD()
    status = rm.RmGetList(handle, c.byref(needed), c.byref(count), None, c.byref(reasons))
    if status not in (0, 234):
        raise OSError(status, "RmGetList")
    entries = (ProcessInfo * needed.value)()
    count.value = needed.value
    status = rm.RmGetList(handle, c.byref(needed), c.byref(count), entries, c.byref(reasons))
    if status:
        raise OSError(status, "RmGetList")
    print("Asset:", sys.argv[1], "Holders:", count.value)
    for entry in entries[:count.value]:
        print("PID=", entry.process.pid, "Application=", entry.app, "Service=", entry.service)
finally:
    rm.RmEndSession(handle)
