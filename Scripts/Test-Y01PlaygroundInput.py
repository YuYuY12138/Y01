"""显式桌面验收：只向本脚本创建的游戏窗口发送键盘；不操作用户编辑器。"""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parent.parent
exe = Path(r"G:\UE_Engine\g01_engine\Engine\Binaries\Win64\UnrealEditor.exe")
label = "Playground-Input-" + time.strftime("%Y%m%d-%H%M%S")
log = root / "Saved" / (label + ".log")
user = c.WinDLL("user32", use_last_error=True)
user.GetForegroundWindow.restype = w.HWND
user.SetForegroundWindow.argtypes = [w.HWND]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.IsWindowVisible.argtypes = [w.HWND]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
p = subprocess.Popen([str(exe), str(root / "Y01.uproject"),
    "/Game/Y01Playground/Maps/L_Y01Playground", "-game", "-windowed", "-ResX=1280", "-ResY=800",
    "-ForceRes", "-nosound", "-nop4", "-NoSplash", "-NoScreenMessages", "-ExecCmds=t.MaxFPS 30", "-abslog=" + str(log)])
window = None

def focused():
    if user.GetForegroundWindow() != window:
        foreground_thread = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
        current_thread = c.WinDLL("kernel32").GetCurrentThreadId()
        attached = user.AttachThreadInput(current_thread, foreground_thread, True)
        try:
            user.SetForegroundWindow(window)
        finally:
            if attached:
                user.AttachThreadInput(current_thread, foreground_thread, False)
        time.sleep(0.3)
    if user.GetForegroundWindow() != window:
        raise RuntimeError(f"游戏未获得焦点；停止验收。Target={window} Foreground={user.GetForegroundWindow()}")

def key(vk, seconds=0.07):
    focused()
    user.keybd_event(vk, 0, 0, 0)
    time.sleep(seconds)
    user.keybd_event(vk, 0, 2, 0)

def command(text):
    key(0xC0)
    time.sleep(0.2)
    for char in text:
        user.PostMessageW(window, 0x102, ord(char), 0)
    time.sleep(0.15)
    key(0x0D)
    time.sleep(0.2)
    key(0xC0)
    time.sleep(0.2)

def capture(stage):
    path = root / "Saved" / "Screenshots" / (label + "-" + stage + ".png")
    command("shot showui filename=" + path.as_posix() + " -nosuffix")
    time.sleep(0.8)
    print("SCREENSHOT", path, "exists=", path.exists(), flush=True)

try:
    deadline = time.monotonic() + 70
    while time.monotonic() < deadline:
        windows = []
        @callback_type
        def collect(hwnd, _):
            pid = w.DWORD()
            user.GetWindowThreadProcessId(hwnd, c.byref(pid))
            if pid.value == p.pid and user.IsWindowVisible(hwnd):
                title, rect = c.create_unicode_buffer(256), w.RECT()
                user.GetWindowTextW(hwnd, title, 256)
                user.GetClientRect(hwnd, c.byref(rect))
                if rect.right >= 1200 and rect.bottom >= 700:
                    print("GAME_WINDOW", hwnd, title.value, rect.right, rect.bottom, flush=True)
                    windows.append(hwnd)
            return True
        user.EnumWindows(collect, 0)
        if windows and log.exists() and "Load map complete" in log.read_text(encoding="utf-8", errors="replace"):
            window = windows[0]
            break
        if p.poll() is not None:
            raise RuntimeError("游戏提前退出")
        time.sleep(0.3)
    if not window:
        raise TimeoutError("未找到本进程的就绪游戏窗口")
    print("OWNED_PID", p.pid, "LOG", log, flush=True)
    focused()
    time.sleep(7)
    capture("Waiting")
    command("getalllocation Y01PlaygroundCharacter")
    key(ord("W"), 0.4)
    time.sleep(0.2)
    command("getalllocation Y01PlaygroundCharacter")
    capture("Moved")
    key(ord("S"), 0.4)
    key(0x0D)
    time.sleep(6.5)
    key(ord("1"))
    key(ord("2"))
    command("getall Y01PlaygroundController Observer")
    capture("Attacked")
    key(0x74)
    time.sleep(0.2)
    capture("F5")
    command("getall Y01PlaygroundController Observer")
    print("INPUT_PROBE_FINISHED; inspect screenshots and console state, not a gameplay PASS assertion", flush=True)
finally:
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=10)
