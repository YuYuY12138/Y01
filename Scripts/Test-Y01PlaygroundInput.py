"""显式原生桌面验收：只操作自建进程；前台身份丢失即停止；不修改玩法。"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import traceback
import zlib


root = Path(__file__).resolve().parent.parent
exe = Path(r'G:\UE_Engine\g01_engine\Engine\Binaries\Win64\UnrealEditor.exe')
p = None
window = None
activated = False
aborted = False
focus_checks = 0
native_events = 0
shot_index = 0
trace = None


def emit(*values):
    line = ' '.join(map(str, values))
    if not values or values[0] not in ('FOREGROUND', 'NATIVE_KEY'):
        print(line, flush=True)
    if trace:
        trace.write(line + '\n')


def configure_native():
    global user, kernel, callback_type
    user = c.WinDLL('user32', use_last_error=True)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.SetProcessDPIAware()
    signatures = {
        'GetForegroundWindow': ([], w.HWND),
        'GetWindowThreadProcessId': ([w.HWND, c.POINTER(w.DWORD)], w.DWORD),
        'GetClientRect': ([w.HWND, c.POINTER(w.RECT)], w.BOOL),
        'ClientToScreen': ([w.HWND, c.POINTER(w.POINT)], w.BOOL),
        'IsWindow': ([w.HWND], w.BOOL),
        'IsWindowVisible': ([w.HWND], w.BOOL),
        'SetForegroundWindow': ([w.HWND], w.BOOL),
        'BringWindowToTop': ([w.HWND], w.BOOL),
        'SetActiveWindow': ([w.HWND], w.HWND),
        'SetFocus': ([w.HWND], w.HWND),
        'AttachThreadInput': ([w.DWORD, w.DWORD, w.BOOL], w.BOOL),
        'EnumWindows': ([callback_type, w.LPARAM], w.BOOL),
        'PostMessageW': ([w.HWND, w.UINT, w.WPARAM, w.LPARAM], w.BOOL),
        'GetKeyboardLayout': ([w.DWORD], w.HANDLE),
        'VkKeyScanExW': ([w.WCHAR, w.HANDLE], c.c_short),
        'GetAsyncKeyState': ([c.c_int], c.c_short),
        'keybd_event': ([w.BYTE, w.BYTE, w.DWORD, c.c_size_t], None),
        'mouse_event': ([w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.c_size_t], None),
        'SetCursorPos': ([c.c_int, c.c_int], w.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(user, name)
        function.argtypes, function.restype = arguments, result
    kernel.GetCurrentThreadId.restype = w.DWORD


def identity(hwnd):
    pid = w.DWORD()
    tid = user.GetWindowThreadProcessId(hwnd, c.byref(pid))
    return {'hwnd': int(hwnd or 0), 'pid': pid.value, 'tid': tid}


def focused(reason='check'):
    """这里只校验，绝不在失焦后抢回焦点继续测试。"""
    global focus_checks, aborted
    target = identity(window)
    actual = identity(user.GetForegroundWindow())
    focus_checks += 1
    valid = (not aborted and p.poll() is None and user.IsWindow(window)
             and target['pid'] == p.pid and actual['hwnd'] == window and actual['pid'] == p.pid)
    emit('FOREGROUND', focus_checks, reason, 'target=', target, 'actual=', actual, 'MATCH=', bool(valid))
    if not valid:
        aborted = True
        raise RuntimeError('前台HWND/PID不再属于本次自建进程；停止所有输入，不重新激活')


def activate():
    global activated
    target, foreground = identity(window), identity(user.GetForegroundWindow())
    current = kernel.GetCurrentThreadId()
    if p.poll() is not None or target['pid'] != p.pid or not user.IsWindow(window):
        raise RuntimeError('激活前目标进程身份不符')
    emit('ACTIVATE', 'target=', target, 'foreground=', foreground, 'current_tid=', current)
    attached = []
    try:
        for tid in dict.fromkeys((target['tid'], foreground['tid'])):
            if tid and tid != current:
                ok = user.AttachThreadInput(current, tid, True)
                emit('ATTACH', current, tid, bool(ok), 'error=', c.get_last_error())
                if not ok:
                    raise c.WinError(c.get_last_error())
                attached.append(tid)
        for name in ('BringWindowToTop', 'SetForegroundWindow', 'SetActiveWindow', 'SetFocus'):
            emit('ACTIVATE_API', name, getattr(user, name)(window))
        deadline = time.monotonic() + 2
        while user.GetForegroundWindow() != window and time.monotonic() < deadline:
            time.sleep(0.01)
    finally:
        for tid in reversed(attached):
            ok = user.AttachThreadInput(current, tid, False)
            emit('DETACH', current, tid, bool(ok))
            if not ok:
                raise c.WinError(c.get_last_error())
    focused('activation-complete')
    activated = True
    # 不释放或切换用户的修饰键；环境不安全则拒绝发送首键。
    if any(user.GetAsyncKeyState(vk) & 0x8000 for vk in (0x10, 0x11, 0x12, 0x5B, 0x5C)):
        raise RuntimeError('检测到用户按住修饰键，拒绝输入')


def key_event(vk, flags):
    global native_events
    focused(f'key-{vk}-{flags}-before')
    user.keybd_event(vk, 0, flags, 0)
    native_events += 1
    focused(f'key-{vk}-{flags}-after')
    emit('NATIVE_KEY', native_events, 'vk=', vk, 'flags=', flags, 'monotonic=', time.monotonic())


def hold(seconds):
    # 这里只定义物理按住时长；玩法通过必须由随后状态回读决定。
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        focused('held-key')
        time.sleep(min(0.01, max(0, deadline - time.monotonic())))


def key(vk, seconds=0.07):
    key_event(vk, 0)
    hold(seconds)
    key_event(vk, 2)


def type_text(value):
    layout = user.GetKeyboardLayout(identity(window)['tid'])
    for char in value:
        code = user.VkKeyScanExW(char, layout)
        if code == -1 or (code >> 8) & ~1:
            raise RuntimeError(f'当前键盘布局不能安全原生输入字符 {char!r}；不使用全局Alt/Ctrl或剪贴板')
        shifted = bool((code >> 8) & 1)
        if shifted:
            key_event(0x10, 0)
        key_event(code & 255, 0)
        key_event(code & 255, 2)
        if shifted:
            key_event(0x10, 2)


def command(value):
    emit('CONSOLE_BEGIN', value)
    key(0xC0)
    # Console.cpp：打开时Capture=true；下一次Pressed清除，不是WM_CHAR或波浪键Released。
    # 全部字符走原生keydown/up，让Windows生成正确字符；首字符的Pressed解除拦截。
    type_text(value)
    # Enter Released提交并关闭Typing；禁止无条件再按波浪键。
    key(0x0D)
    emit('CONSOLE_SUBMITTED', value)


def text():
    return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''


def wait(predicate, description, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if p.poll() is not None:
            raise RuntimeError(f'自建游戏提前退出：{p.returncode}')
        if activated:
            focused('wait:' + description)
        result = predicate()
        if result:
            emit('CHECK', description)
            return result
        time.sleep(0.02)
    raise TimeoutError(description)


def locate():
    found = []
    @callback_type
    def collect(hwnd, unused):
        rect = w.RECT()
        if identity(hwnd)['pid'] == p.pid and user.IsWindowVisible(hwnd):
            if user.GetClientRect(hwnd, c.byref(rect)) and rect.right == 1280 and rect.bottom == 800:
                found.append(hwnd)
        return True
    user.EnumWindows(collect, 0)
    if len(found) > 1:
        raise RuntimeError('同一进程出现多个候选游戏窗口，拒绝猜测')
    return found[0] if found else None


def telemetry():
    rows = re.findall(r'Y01Training OBSERVE ([^\r\n]+)', text())
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', rows[-1])} if rows else {}


def require(condition, description):
    if not condition:
        raise AssertionError(description)
    emit('ASSERT_PASS', description)


def parse_snapshot(value):
    phase = re.search(r'\.Snapshot = \([^\r\n]*?Phase=(\w+)', value)
    observer = re.search(r'\.Observer = ([^\r\n]+)', value)
    if not phase or not observer:
        raise RuntimeError('没有本次完整Snapshot/Observer回读，拒绝沿用旧日志')
    row = observer[1]
    slots = re.findall(r'\(Row="(BasicAttack|HeavyAttack)"([^)]*)\)', row)
    deadlines = []
    for name in ('BasicAttack', 'HeavyAttack'):
        fields = [fields for slot, fields in slots if slot == name]
        if len(fields) != 1:
            raise RuntimeError('技能槽回读缺失或重复：' + name)
        deadline = re.search(r'Deadline=([\d.]+)', fields[0])
        deadlines.append(float(deadline[1]) if deadline else 0.0)
    health = {}
    for line in value.splitlines():
        if '.Health = ' not in line or 'Y01AttributeSet ' not in line:
            continue
        monster = re.search(r'PersistentLevel\.(Y01Monster_\d+)\.', line)
        hp = re.search(r'CurrentValue=(-?[\d.]+)', line)
        if monster and hp:
            health[monster[1]] = float(hp[1])
    target = re.search(r'Target="[^"\r\n]*\.(Y01Monster_\d+)', row)
    notice = re.search(r'Notice="([^"]*)"', row)
    position = re.search(r'Y01PlaygroundCharacter_[0-9]+ \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)', value)
    if not position:
        raise RuntimeError('缺少本次原生getalllocation坐标回读')
    return {'x': float(position[1]), 'y': float(position[2]), 'z': float(position[3]),
            'phase': phase[1], 'ready': 'bReady=True' in row,
            'inside': 'bInside=True' in row, 'health': health, 'deadlines': deadlines,
            'target': target[1] if target else None, 'notice': notice[1] if notice else ''}


def queries():
    # 仅使用引擎只读getall；不调用内部Verify、RPC、Exec脚本或服务器控制接口。
    # UPlayer::ConsoleCommand原生支持竖线分隔；Snapshot放最后作为整组回读完成标志。
    return ('getall Y01PlaygroundController Observer | getall Y01AttributeSet Health | '
            'getalllocation Y01PlaygroundCharacter | getall Y01GameState Snapshot')


def fresh(offset):
    value = text()[offset:]
    if 'Command not recognized:' in value:
        raise RuntimeError('控制台命令没有正确提交：' + value[-1500:])
    return value


def observe():
    offset = len(text())
    command(queries())
    wait(lambda: '.Snapshot = ' in fresh(offset), '本次只读状态回读')
    result = parse_snapshot(fresh(offset))
    emit('OBSERVATION', json.dumps(result, ensure_ascii=False))
    return result


def valid_png(path, since):
    if not path.is_file():
        return False
    stat = path.stat()
    if stat.st_size == 0 or stat.st_mtime_ns < since or stat.st_ctime_ns < since:
        return False
    data = path.read_bytes()
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        return False
    cursor, dimensions, image = 8, None, bytearray()
    while cursor + 12 <= len(data):
        size = struct.unpack_from('!I', data, cursor)[0]
        kind = data[cursor + 4:cursor + 8]
        end = cursor + 12 + size
        if end > len(data):
            return False
        body = data[cursor + 8:cursor + 8 + size]
        crc = struct.unpack_from('!I', data, cursor + 8 + size)[0]
        if zlib.crc32(kind + body) != crc:
            return False
        if kind == b'IHDR':
            dimensions = struct.unpack('!2I5B', body) if len(body) == 13 else None
        elif kind == b'IDAT':
            image.extend(body)
        elif kind == b'IEND':
            if not dimensions or dimensions[:2] != (1280, 800) or end != len(data) or size:
                return False
            try:
                decoded = zlib.decompress(image)
            except zlib.error:
                return False
            channels = {2: 3, 6: 4}.get(dimensions[3])
            return bool(channels and dimensions[2] == 8 and len(decoded) == (1280 * channels + 1) * 800)
        cursor = end
    return False


def capture(stage):
    global shot_index
    shot_index += 1
    path = output / f'{shot_index:02d}-{stage}.png'
    if path.exists():
        raise RuntimeError('拒绝覆盖旧截图：' + str(path))
    since = time.time_ns()
    offset = len(text())
    command(queries() + ' | shot showui filename="' + path.as_posix() + '" -nosuffix')
    wait(lambda: valid_png(path, since) and '.Snapshot = ' in fresh(offset), '新截图完整落盘：' + stage)
    result = parse_snapshot(fresh(offset))
    emit('SCREENSHOT', path, 'size=', path.stat().st_size, 'created_ns=', path.stat().st_ctime_ns,
         'mtime_ns=', path.stat().st_mtime_ns, 'requested_ns=', since,
         'sha256=', hashlib.sha256(path.read_bytes()).hexdigest(), 'STATE=', json.dumps(result, ensure_ascii=False))
    return result


def check_skill(before, after, slot):
    # 原生技能会重新选择最近怪物；按实际对象身份比对所有血量，不能假设锁定旧目标。
    damaged = [name for name, hp in before['health'].items()
               if name in after['health'] and after['health'][name] < hp]
    require(len(damaged) == 1, '本次单体技能恰好伤害一个有前后血量证据的实际怪物')
    target = damaged[0]
    damage = before['health'][target] - after['health'][target]
    require(damage > 0, f'技能{slot + 1}实际伤害：{target} {before["health"][target]} -> {after["health"][target]}')
    require(after['deadlines'][slot] > before['deadlines'][slot], f'技能{slot + 1}权威CD截止时间增加')
    require(after['deadlines'][1 - slot] == before['deadlines'][1 - slot], '未按的另一技能CD不变')
    emit('SKILL_EVIDENCE', slot + 1, 'target=', target, 'damage=', damage,
         'selected_before=', before['target'], 'selected_after=', after['target'],
         'deadline_before=', before['deadlines'][slot], 'deadline_after=', after['deadlines'][slot])


def moves(prefix):
    before = observe()
    key(ord('W'), 0.4)
    moved = capture(prefix + '-W')
    require(moved['x'] > before['x'] + 20, prefix + 'W实际正向位移')
    key(ord('S'), 0.4)
    returned = capture(prefix + '-S')
    # 碰撞会使正反向距离不对称；只验证真实方向位移，不新增强制回原点规则。
    require(returned['x'] < moved['x'] - 20, prefix + 'S实际反向位移')
    emit('MOVE_EVIDENCE', prefix, 'before=', before['x'], 'W=', moved['x'], 'S=', returned['x'])


def until_state(predicate, description, seconds=15):
    return wait(lambda: (lambda state: state if predicate(state) else None)(observe()), description, seconds)


def skills(prefix):
    until_state(lambda s: s['target'] in s['health'] and s['health'].get(s['target'], 0) > 0,
                prefix + '实际目标进入攻击范围')
    before = capture(prefix + '-Target')
    for slot in (0, 1):
        key(ord('1') + slot)
        after = capture(prefix + f'-Skill{slot + 1}')
        check_skill(before, after, slot)
        before = after


def click_training(inside):
    before = text().count('Y01Training UI_CLICK')
    focused('mouse-position-before')
    point = w.POINT(150, 446)
    if not user.ClientToScreen(window, c.byref(point)) or not user.SetCursorPos(point.x, point.y):
        raise c.WinError(c.get_last_error())
    focused('mouse-position-after')
    for flags in (0x0002, 0x0004):
        focused(f'mouse-{flags}-before')
        user.mouse_event(flags, 0, 0, 0, 0)
        focused(f'mouse-{flags}-after')
        if flags == 0x0002:
            hold(0.07)
    wait(lambda: text().count('Y01Training UI_CLICK') == before + 1 and telemetry().get('Inside') == inside,
         f'Canvas原生命中一次且Inside={inside}', 5)
    emit('CLICK_EVIDENCE', 'client_xy=150,446', 'inside=', inside, 'telemetry=', telemetry())


def scenario(mode):
    waiting = capture('Waiting')
    require(waiting['ready'] and waiting['phase'] == 'Waiting' and not waiting['inside'], '配置就绪且主线Waiting')
    require(not waiting['notice'], 'F5提示初始为空')
    moves('Main')
    key(0x0D)
    until_state(lambda s: s['phase'] == 'Preparing', '原生Enter使Waiting进入Preparing')
    capture('Preparing')
    until_state(lambda s: s['phase'] == 'Battle', '实际倒计时完成进入Battle')
    capture('Battle')
    skills('Main')
    if mode == 'training':
        click_training(1)
        entered = capture('Training-Entered')
        require(entered['inside'], '训练按钮切换进入个人房间')
        moves('Training')
        skills('Training')
        click_training(0)
        returned = capture('Training-Returned')
        require(not returned['inside'], '训练返回按钮切回主线')
        moves('Returned')
    key(0x74)
    notice = capture('F5')
    require('Ctrl+C' in notice['notice'] and 'reload disk data' in notice['notice'], '原生F5产生真实重开notice')
    require('Command not recognized:' not in text(), '无控制台误收W/S或损坏命令')
    require('Y01Playground INPUT_PASS' not in text() and 'VERIFY_CLIENT_PASS' not in text(), '未启用内部玩法Verify')


def cleanup():
    if p is None:
        return
    forced = False
    if p.poll() is None:
        # 不按进程名清理；窗口关闭也必须再次验证拥有者PID。
        if window and user.IsWindow(window) and identity(window)['pid'] == p.pid:
            user.PostMessageW(window, 0x0010, 0, 0)
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            forced = True
            p.terminate()
            p.wait(timeout=10)
    emit('OWNED_EXIT', 'pid=', p.pid, 'code=', p.returncode, 'forced=', forced)


def resolve_input_map(name):
    if name != 'legacy':
        raise ValueError('旧单体即时断言仅支持 --map legacy；普通主入口已改为范围/飞行物，本脚本不代表新技能验收')
    return '/Game/Developers/Y01PlaygroundTests/R33U2Legacy/Maps/L_Y01Playground'


def main():
    global p, window, output, log, trace
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=('main', 'training'), default='main', help='training先完整主线，再点击训练并回测')
    parser.add_argument('--label', default='R33-Input-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    parser.add_argument('--map', default='main', help='旧即时验收必须显式选legacy；新技能请用后续专用验收')
    args = parser.parse_args()
    try:
        selected_map = resolve_input_map(args.map)
    except ValueError as error:
        parser.error(str(error))
    if not (root / 'Content' / (selected_map[6:] + '.umap')).is_file():
        parser.error('legacy持久地图缺失；请先运行SkillLegacyCopy，本脚本不生成资产')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
        parser.error('label只能包含ASCII字母、数字、下划线和连字符')
    output = root / 'Saved' / args.label
    output.mkdir(exist_ok=False)
    log = output / 'Client.log'
    trace = (output / 'runner.log').open('x', encoding='utf-8', buffering=1)
    success = False
    try:
        configure_native()
        argv = [str(exe), str(root / 'Y01.uproject'), selected_map,
                '-game', '-windowed', '-ResX=1280', '-ResY=800', '-ForceRes', '-nosound', '-nop4',
                '-NoSplash', '-NoScreenMessages', '-ExecCmds=t.MaxFPS 30', '-Y01TrainingVerify',
                '-forcelogflush', '-abslog=' + str(log)]
        # TrainingVerify仅开启0.2秒只读遥测；没有PlaygroundVerify自动按键。
        p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        emit('OWNED_START', 'pid=', p.pid, 'COMMAND=', subprocess.list2cmdline(argv))
        emit('SCRIPT_SHA256', hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        emit('LEGACY_INPUT_ONLY', 'map=', selected_map, 'scenario=', args.scenario, '新技能可见/原生验收未覆盖')
        window = wait(lambda: locate() if telemetry().get('Inside') == 0 and 'Load map complete' in text() else None,
                      '自建游戏窗口及只读遥测就绪', 90)
        activate()
        scenario(args.scenario)
        success = True
    except BaseException:
        emit('RUN_FAILED', traceback.format_exc())
    finally:
        try:
            cleanup()
        except BaseException:
            success = False
            emit('CLEANUP_FAILED', traceback.format_exc())
        emit('NATIVE_SCENARIO_PASS' if success else 'NATIVE_SCENARIO_FAIL', 'scenario=', args.scenario,
             'screenshots=', shot_index, 'foreground_checks=', focus_checks, 'native_key_events=', native_events)
        emit('RUNNER_EXIT=', 0 if success else 1, 'EVIDENCE=', output)
        trace.close()
        trace = None
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
