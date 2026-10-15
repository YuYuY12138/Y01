"""R33专用真实桌面验收；鼠标命中Canvas，仅读观测日志，只清理自己启动的进程。"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import math
from pathlib import Path
import re
import socket
import struct
import subprocess
import time
import zlib

def resolve_training_map(selection):
    maps = {
        'u2baseline': '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground',
        'compare': '/Game/Developers/Y01PlaygroundTests/R33TrainingCompare/Maps/L_Y01Playground',
    }
    if selection not in maps:
        raise ValueError('零Gold/XP训练验收禁止main；请使用 --map u2baseline（3秒2只）或 --compare（原1秒1只对照，须审计成长关闭）。')
    return maps[selection]


def validate_training_audits(selection, growth_text, training_text):
    """读取本次命令输出；只认目标Profile的实际Character引用，不根据目录名推断成长。"""
    map_root = resolve_training_map(selection).rsplit('/Maps/', 1)[0]
    profile = map_root + '/DA_PlaygroundProfile.DA_PlaygroundProfile'
    hint = (f'训练配置不兼容：Profile={profile}；保留Gold/XP/Kills零断言，拒绝启动。'
            '请采用 --map u2baseline；旧--compare需先核实并精准隔离该Profile的Character引用，'
            '仅使用已核实关闭的legacy/compare快照，保留其他表/字段，不关闭main成长、不自动覆资产。')

    def require(ok, detail):
        if not ok:
            raise ValueError(hint + ' ' + detail)

    def fields(line):
        return dict(re.findall(r'(\w+)=([^\s]+)', line))

    def number(row, key):
        try:
            value = float(row[key])
        except (KeyError, ValueError):
            raise ValueError(hint + f' 缺少或非法字段 {key}={row.get(key)}') from None
        require(math.isfinite(value), f'非法字段 {key}={value}')
        return value

    def table_path(row):
        return bool(re.fullmatch(r'/Game/[A-Za-z0-9_/]+\.[A-Za-z0-9_]+', row.get('Table', '')))

    growth = [fields(line) for line in growth_text.splitlines() if 'LogY01Playground: Display: GROWTH_AUDIT ' in line]
    require(bool(growth) and all(row.get('Profile') == profile and row.get('Stage') == 'BEFORE' for row in growth),
            '缺少本次目标Profile成长审计，或来源不符')
    targets = [row for row in growth if row.get('Row') == row.get('Character') and row.get('Character') not in (None, 'None')]
    require(len(targets) == 1, '必须唯一命中Profile.Character指定行')
    character = targets[0]
    require(table_path(character) and all(row.get('Table') == character['Table'] and
            row.get('Character') == character['Character'] for row in growth), 'Character表引用不完整或不一致')
    require(character.get('Enabled') == '0', f'Character={character["Character"]} Table={character["Table"]} Enabled={character.get("Enabled")}')
    require(number(character, 'HP') > 0, '角色生命必须为正')
    for key in ('Seconds', 'Kills', 'Gold', 'Attack', 'MoveSpeed'):
        require(number(character, key) >= 0, '非法角色配置：' + key)

    headers = [fields(line) for line in training_text.splitlines() if 'LogY01Playground: Display: TRAINING_BEFORE ' in line]
    require(len(headers) == 1, '缺少唯一训练审计头')
    header = headers[0]
    require(header.get('Profile') == profile and header.get('Enabled') == '1' and table_path(header),
            '训练Profile/开关/表引用不符')
    stages = [fields(line) for line in training_text.splitlines() if line.startswith('Row=')]
    spawns = [(match[1], fields(match[2])) for match in re.finditer(r'^Spawns\[(\d+)\] ([^\r\n]+)', training_text, re.M)]
    rewards = [fields(line) for line in training_text.splitlines() if 'LogY01Playground: Display: TRAINING_REWARD ' in line]
    require(len(stages) == len(spawns) == len(rewards) == 1, '旧验收需要唯一阶段、刷怪项及奖励审计；拒绝额外阶段/缺项')
    stage, (index, spawn), reward = stages[0], spawns[0], rewards[0]
    interval, batch = (1, 1) if selection == 'compare' else (3, 2)
    for key, expected in (('Stage', 1), ('StartSeconds', 0), ('SpawnInterval', interval), ('MaxAlive', 6)):
        require(number(stage, key) == expected, f'训练{key}应为{expected}，实际{stage.get(key)}')
    require(index == '0' and spawn.get('Monster') not in (None, 'None'), '刷怪项或怪物引用不符')
    require(number(spawn, 'Count') == batch and number(spawn, 'DelaySeconds') == 0, '训练每批数量/延迟不符')
    require(stage.get('Row') not in (None, 'None') and reward.get('Row') == stage['Row'] and
            stage.get('Reward') not in (None, 'None') and reward.get('Reward') == stage['Reward'], '训练奖励引用不符')
    require(number(reward, 'Items') == 0, '训练不支持物品奖励')
    # 本场景不击杀；配置的击杀奖励可非零，不能把它和最终Gold/XP零观测混淆。
    for key in ('Gold', 'XP'):
        value = number(reward, key)
        require(value >= 0 and value.is_integer(), '训练奖励数值非法：' + key)
    return dict(profile=profile, character=character['Character'], character_table=character['Table'],
                growth_enabled=False, training_table=header['Table'], interval=interval, batch=batch, cap=6)


def audit_training_configuration(selection, project_root, engine_dir):
    """仅运行既有ValidateOnly/GrowthAudit/TrainingAudit；绝不生成或修复资产。"""
    map_root = resolve_training_map(selection).rsplit('/Maps/', 1)[0]
    outputs = []
    for mode in ('-WithMap', '-GrowthAudit', '-TrainingAudit'):
        command = [str(engine_dir / 'UnrealEditor-Cmd.exe'), str(project_root / 'Y01.uproject'),
                   '-run=Y01CreatePlayground', '-Root=' + map_root, '-ValidateOnly', mode,
                   '-nullrhi', '-unattended', '-nop4', '-nosound', '-NoSplash', '-NoLog', '-stdout', '-UTF8Output']
        try:
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValueError(f'训练配置审计失败，拒绝启动：Root={map_root} {mode} {error}') from error
        if result.returncode != 0:
            raise ValueError(f'训练配置审计失败，拒绝启动：Root={map_root} {mode} Exit={result.returncode}\n'
                             + result.stdout + result.stderr)
        outputs.append(result.stdout)
    return validate_training_audits(selection, outputs[1], outputs[2])


parser = argparse.ArgumentParser(description=__doc__)
entry = parser.add_mutually_exclusive_group()
entry.add_argument('--map', default='u2baseline', help='u2baseline=无成长3秒2只；compare=原训练1秒1只对照；禁止main')
entry.add_argument('--compare', action='store_true', help='兼容旧入口：原R33TrainingCompare，绝不替换为U2技能对照')
parser.add_argument('--posted-input', action='store_true', help='仅向拥有的窗口投递鼠标消息，不抢占其他应用焦点')
parser.add_argument('--label', default='R33-U1-Visible-' + time.strftime('%Y%m%d-%H%M%S'))
parser.add_argument('--port', type=int, default=18133)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
engine = Path(r'G:\UE_Engine\g01_engine\Engine\Binaries\Win64')
if args.compare:
    args.map = 'compare'
args.compare = args.map == 'compare'
try:
    map_root = resolve_training_map(args.map).rsplit('/Maps/', 1)[0]
    preflight = audit_training_configuration(args.map, root, engine)
except ValueError as error:
    parser.error(str(error))
output = root / 'Saved' / args.label
output.mkdir(parents=True, exist_ok=False)
trace = (output / 'runner.log').open('w', encoding='utf-8', buffering=1)
user = c.WinDLL('user32', use_last_error=True)
gdi = c.WinDLL('gdi32', use_last_error=True)
kernel = c.WinDLL('kernel32', use_last_error=True)
user.SetProcessDPIAware()
user.GetForegroundWindow.restype = w.HWND
user.SetForegroundWindow.argtypes = [w.HWND]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
user.IsWindowVisible.argtypes = [w.HWND]
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
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
gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

class BitmapHeader(c.Structure):
    _fields_ = [('size', w.DWORD), ('width', w.LONG), ('height', w.LONG),
                ('planes', w.WORD), ('bits', w.WORD), ('compression', w.DWORD),
                ('image_size', w.DWORD), ('x', w.LONG), ('y', w.LONG),
                ('used', w.DWORD), ('important', w.DWORD)]

owned = {}
windows = {}
logs = {}

def emit(*values):
    text = ' '.join(map(str, values))
    print(text, flush=True)
    trace.write(text + '\n')

def text(role):
    return logs[role].read_text(encoding='utf-8', errors='replace') if logs[role].exists() else ''

def launch(role, binary, extra):
    logs[role] = output / (role + '.log')
    command = [str(engine / binary), str(root / 'Y01.uproject'), *extra,
               '-nop4', '-nosound', '-NoSplash', '-NoScreenMessages', '-forcelogflush',
               '-abslog=' + str(logs[role])]
    owned[role] = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    emit('START', role, 'PID=', owned[role].pid, 'COMMAND=', subprocess.list2cmdline(command))

def wait(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for role, process in owned.items():
            if process.poll() is not None:
                raise RuntimeError(f'{role}提前退出：{process.returncode}')
        if predicate():
            emit('CHECK', description)
            return
        time.sleep(0.04)
    raise TimeoutError(description)

def locate(role):
    found = []
    @callback_type
    def collect(hwnd, _):
        pid, rect = w.DWORD(), w.RECT()
        user.GetWindowThreadProcessId(hwnd, c.byref(pid))
        if pid.value == owned[role].pid and user.IsWindowVisible(hwnd):
            user.GetClientRect(hwnd, c.byref(rect))
            if rect.right == 1280 and rect.bottom == 800:
                found.append(hwnd)
        return True
    user.EnumWindows(collect, 0)
    if len(found) == 1:
        windows[role] = found[0]
        return True
    return False

def focus(role):
    hwnd = windows[role]
    foreground = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
    current = kernel.GetCurrentThreadId()
    attached = user.AttachThreadInput(current, foreground, True)
    try:
        user.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user.AttachThreadInput(current, foreground, False)
    # SetForegroundWindow跨线程切换可能尚未完成；只等待本窗口确认，绝不盲发输入。
    deadline = time.monotonic() + 1
    while user.GetForegroundWindow() != hwnd and time.monotonic() < deadline:
        time.sleep(0.02)
    if user.GetForegroundWindow() != hwnd:
        raise RuntimeError(f'拒绝向非本次窗口发送输入 Target={hwnd} Foreground={user.GetForegroundWindow()}')

def state(role):
    lines = re.findall(r'Y01Training OBSERVE ([^\r\n]+)', text(role))
    return dict((k, float(v)) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', lines[-1])) if lines else {}

def click(role, inside):
    before = text(role).count('Y01Training UI_CLICK')
    if args.posted_input:
        # 同一Slate/Canvas命中入口，绝不调用RPC或服务器接口绕过命中。
        position = 150 | (446 << 16)
        user.PostMessageW(windows[role], 0x0200, 0, position)
        time.sleep(0.12)
        user.PostMessageW(windows[role], 0x0201, 1, position)
        time.sleep(0.06)
        user.PostMessageW(windows[role], 0x0202, 0, position)
    else:
        focus(role)
        point = w.POINT(150, 446)
        user.ClientToScreen(windows[role], c.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.12)
        user.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        user.mouse_event(0x0004, 0, 0, 0, 0)
    wait(lambda: text(role).count('Y01Training UI_CLICK') == before + 1 and state(role).get('Inside') == inside,
         f'{role}真实鼠标命中一次，RPC后Inside={inside}', 5)
    emit('CLICK', role, 'client_xy=150,446', 'POSTED_INPUT=', args.posted_input, 'OBS=', state(role))

def png(path, pixels):
    # 标准库编码真实GetDIBits像素，不依赖额外图像库，也不合成HUD。
    rgba = bytearray(len(pixels))
    rgba[0::4], rgba[1::4], rgba[2::4] = pixels[2::4], pixels[1::4], pixels[0::4]
    rgba[3::4] = b'\xff' * (1280 * 800)
    scan = b''.join(b'\0' + rgba[y * 5120:(y + 1) * 5120] for y in range(800))
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 1280, 800, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b''))

def capture(role, stage):
    hwnd = windows[role]
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
            raise RuntimeError('截图像素不完整')
        path = output / f'{stage}-{role}.png'
        png(path, pixels.raw)
        emit('SCREENSHOT', path, 'MONOTONIC=', time.monotonic(), 'OBS=', state(role))
    finally:
        gdi.SelectObject(memory, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(hwnd, dc)

def pair(stage):
    time.sleep(0.12)
    for role in ('A', 'B'):
        capture(role, stage)

emit('TRAINING_PREFLIGHT_PASS', 'map=', args.map, 'AUDIT=', preflight)
success = False
try:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', args.port))
    launch('Server', 'UnrealEditor-Cmd.exe', [map_root + '/Maps/L_Y01Playground', '-server', '-nullrhi', f'-port={args.port}'])
    wait(lambda: 'listening on port' in text('Server'), 'DS监听', 60)
    for i, role in enumerate(('A', 'B')):
        launch(role, 'UnrealEditor.exe', [f'127.0.0.1:{args.port}', '-game', '-windowed', '-ResX=1280', '-ResY=800',
              '-ForceRes', f'-WinX={40 + i * 1300}', '-WinY=40', '-ExecCmds=t.MaxFPS 30', '-Y01TrainingVerify'])
    wait(lambda: all(locate(r) and state(r).get('Inside') == 0 for r in ('A', 'B')), '两可见客户端主线Waiting就绪', 90)
    time.sleep(2)
    pair('01-Waiting')
    batch = 1 if args.compare else 2
    click('A', 1)
    assert state('A')['Alive'] == batch and state('B')['Inside'] == 0
    pair('02-AEnter-BMain')
    click('B', 1)
    assert state('B')['Alive'] == batch and state('A')['Room'] != state('B')['Room']
    pair('03-SeparateRooms')
    wait(lambda: all(state(r).get('Alive', 0) >= batch * 2 for r in ('A', 'B')), '两房独立第二批', 8)
    pair('04-Periodic')
    click('A', 0)
    assert state('A')['Alive'] == 0 and state('B')['Inside'] == 1
    pair('05-AReturn-BContinues')
    wait(lambda: state('B').get('Alive', 0) >= batch * 3, 'A离开后B继续第三批', 5)
    assert state('A')['Alive'] == 0
    pair('06-BContinues')
    click('A', 1)
    assert state('A')['Alive'] == batch and state('A')['Stage'] == 1
    pair('07-AReenter')
    for role in ('A', 'B'):
        assert state(role)['Z'] > 0 and state(role)['HP'] > 0
        assert state(role)['Gold'] == 0 and state(role)['XP'] == 0 and state(role)['Kills'] == 0
        assert '-nullrhi' not in text(role) and 'D3D12' in text(role)
    expected_interval = 1 if args.compare else 3
    assert f'Interval={expected_interval} Cap=6' in text('Server')
    assert text('A').count('Y01Training UI_CLICK') == 3 and text('B').count('Y01Training UI_CLICK') == 1
    assert text('Server').count('Y01Training UI_RPC') == 4
    assert text('Server').count('Accepted=1') == 4
    # 时间戳来自服务器实际批次，不用脚本sleep冒充数据驱动周期。
    batches = re.findall(r'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d+)\].*Y01Training BATCH Player=(\S+) .*Alive=(\d+) Interval=([\d.]+)', text('Server'))
    from datetime import datetime
    players = {}
    for stamp, player, alive, interval in batches:
        players.setdefault(player, []).append((datetime.strptime(stamp, '%Y.%m.%d-%H.%M.%S:%f'), int(alive)))
    assert len(players) == 2
    for player, entries in players.items():
        assert entries[0][1] == batch and entries[1][1] == batch * 2
        elapsed = (entries[1][0] - entries[0][0]).total_seconds()
        assert abs(elapsed - expected_interval) < 0.5, (player, elapsed)
        emit('BATCH_INTERVAL', player, elapsed, 'FIRST=', entries[0][1], 'SECOND=', entries[1][1])
    success = True
    emit('VISIBLE_SCENARIO_PASS', 'compare=', args.compare, '真实命中4次；14张连续窗口截图；无服务器状态注入')
finally:
    # 仅向本脚本拥有的游戏窗口关闭；DS外部结束单独记录，不伪称自然退出0。
    for role in ('A', 'B'):
        if role in windows and owned[role].poll() is None:
            user.PostMessageW(windows[role], 0x0010, 0, 0)
    for role, process in reversed(list(owned.items())):
        forced = False
        try:
            process.wait(timeout=8 if role != 'Server' else 1)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=10)
        emit('EXIT', role, 'PID=', process.pid, 'CODE=', process.returncode, 'OWNED_EXTERNAL_STOP=', forced)
    emit('RUNNER_EXIT=', 0 if success else 1)
    trace.close()
