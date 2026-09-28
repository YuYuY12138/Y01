"""显式验收普通启动和 Ctrl+C；只控制本次启动器的独立控制台及其子进程。"""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import queue
import re
import subprocess
import threading
import time
from PIL import Image

root = Path(__file__).resolve().parent.parent
label = "Playground-Lifecycle-" + time.strftime("%Y%m%d-%H%M%S")
powershell = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
kernel = c.WinDLL("kernel32", use_last_error=True)
user = c.WinDLL("user32", use_last_error=True)
gdi = c.WinDLL("gdi32", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.AttachConsole.argtypes = [w.DWORD]
kernel.GenerateConsoleCtrlEvent.argtypes = [w.DWORD, w.DWORD]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.IsWindowVisible.argtypes = [w.HWND]
user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.GetDC.argtypes = [w.HWND]
user.GetDC.restype = w.HDC
user.ReleaseDC.argtypes = [w.HWND, w.HDC]
user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
gdi.CreateCompatibleDC.argtypes = [w.HDC]
gdi.CreateCompatibleDC.restype = w.HDC
gdi.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
gdi.CreateCompatibleBitmap.restype = w.HBITMAP
gdi.SelectObject.argtypes = [w.HDC, w.HANDLE]
gdi.SelectObject.restype = w.HANDLE
gdi.DeleteObject.argtypes = [w.HANDLE]
gdi.DeleteDC.argtypes = [w.HDC]
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

class BitmapHeader(c.Structure):
    _fields_ = [("size", w.DWORD), ("width", w.LONG), ("height", w.LONG),
                ("planes", w.WORD), ("bits", w.WORD), ("compression", w.DWORD),
                ("image_size", w.DWORD), ("x", w.LONG), ("y", w.LONG),
                ("used", w.DWORD), ("important", w.DWORD)]

gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]


def running(handle):
    return kernel.WaitForSingleObject(handle, 0) == 258


def capture(pid, stage):
    windows = []
    @callback_type
    def collect(hwnd, _):
        owner, rect = w.DWORD(), w.RECT()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            user.GetClientRect(hwnd, c.byref(rect))
            if rect.right == 1280 and rect.bottom == 800:
                windows.append(hwnd)
        return True
    user.EnumWindows(collect, 0)
    if len(windows) != 1:
        raise RuntimeError(f"预期唯一1280x800游戏窗口，实际{windows}")
    hwnd = windows[0]
    dc = user.GetDC(hwnd)
    memory = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, 1280, 800)
    old = gdi.SelectObject(memory, bitmap)
    try:
        if not user.PrintWindow(hwnd, memory, 3):
            raise c.WinError(c.get_last_error())
        gdi.SelectObject(memory, old)
        header = BitmapHeader(c.sizeof(BitmapHeader), 1280, -800, 1, 32, 0, 0, 0, 0, 0, 0)
        pixels = c.create_string_buffer(1280 * 800 * 4)
        if gdi.GetDIBits(memory, bitmap, 0, 800, pixels, c.byref(header), 0) != 800:
            raise RuntimeError("GetDIBits未返回完整画面")
        image = Image.frombytes("RGB", (1280, 800), pixels.raw, "raw", "BGRX")
        path = root / "Saved" / "Screenshots" / (label + "-" + stage + ".png")
        image.save(path)
        print("WINDOW_CAPTURE", path, "需读取画面确认Waiting；非FScreenshotRequest", flush=True)
    finally:
        gdi.SelectObject(memory, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(hwnd, dc)


initial = subprocess.check_output([powershell, "-NoProfile", "-Command",
    "Get-Process UnrealEditor* -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id"], text=True)
protected = {int(pid): kernel.OpenProcess(0x100000, False, int(pid)) for pid in initial.split()}
owned = {}
process = subprocess.Popen([powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", str(root / "Scripts" / "Run-Y01Playground.ps1"), "-Port", "18068", "-Label", label],
    creationflags=subprocess.CREATE_NEW_CONSOLE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    text=True, errors="replace")
lines = queue.Queue()


def read_output():
    for line in process.stdout:
        print(line.rstrip(), flush=True)
        lines.put(line)


threading.Thread(target=read_output, daemon=True).start()
try:
    deadline = time.monotonic() + 70
    client_pid = None
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=0.3)
            match = re.search(r"Playground (Server|Client1) PID=(\d+)", line)
            if match:
                pid = int(match[2])
                if pid in protected:
                    raise RuntimeError("拒绝操作预先存在的进程")
                owned[pid] = kernel.OpenProcess(0x100001, False, pid)
                if not owned[pid]:
                    raise c.WinError(c.get_last_error())
                if match[1] == "Client1":
                    client_pid = pid
        except queue.Empty:
            pass
        log = root / "Saved" / (label + "-Client1.log")
        if client_pid and log.exists() and "Load map complete" in log.read_text(encoding="utf-8", errors="replace"):
            break
        if process.poll() is not None:
            raise RuntimeError("普通启动器提前退出")
    else:
        raise TimeoutError("普通客户端未就绪")
    time.sleep(3)
    capture(client_pid, "Initial")
    time.sleep(20)
    assert process.poll() is None and len(owned) == 2 and all(running(h) for h in owned.values())
    capture(client_pid, "Idle20s")
    text = log.read_text(encoding="utf-8", errors="replace")
    assert "-Y01PlaygroundVerify" not in text and "INPUT_PASS" not in text and "VERIFY_CLIENT_PASS" not in text
    print("IDLE_PROCESS_PASS no Verify; no auto-exit; 截图状态需单独确认", flush=True)
    kernel.FreeConsole()
    if not kernel.AttachConsole(process.pid):
        raise c.WinError(c.get_last_error())
    try:
        kernel.SetConsoleCtrlHandler(None, True)
        if not kernel.GenerateConsoleCtrlEvent(0, 0):
            raise c.WinError(c.get_last_error())
        print("CTRL_C_SENT only isolated owned console", flush=True)
        process.wait(timeout=15)
    finally:
        kernel.FreeConsole()
    time.sleep(1)
    assert not any(running(h) for h in owned.values()), "Ctrl+C后仍有本次游戏进程残留"
    assert all(running(h) for h in protected.values()), "原有编辑器不再存活"
    print("LIFECYCLE_PASS owned PIDs cleared; original editor PIDs preserved:", list(protected), flush=True)
finally:
    for handle in owned.values():
        if running(handle):
            kernel.TerminateProcess(handle, 1)
        kernel.CloseHandle(handle)
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=10)
    for handle in protected.values():
        kernel.CloseHandle(handle)
