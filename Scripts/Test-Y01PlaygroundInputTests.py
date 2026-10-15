"""输入验收工具的隔离回归测试；加载函数而不启动UE或操作桌面。"""
import argparse
import ast
import base64
import hashlib
import json
import re
import ctypes as c
from ctypes import wintypes as w
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
import zlib
from unittest.mock import Mock

SCRIPT = Path(__file__).with_name('Test-Y01PlaygroundInput.py')


def functions():
    tree = ast.parse(SCRIPT.read_text(encoding='utf-8-sig'))
    tree.body = [node for node in tree.body if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.ClassDef))]
    scope = {'__file__': str(SCRIPT), 'emit': Mock(), 'window': 101,
             'p': types.SimpleNamespace(pid=202, poll=lambda: None), 'focus_checks': 0,
             'native_events': 0, 'aborted': False, 'activated': True}
    exec(compile(tree, str(SCRIPT), 'exec'), scope)
    scope['emit'] = Mock()
    return scope


class Console:
    """按本机Console.cpp：打开后拦字符，下次Pressed解锁，Enter Released提交。"""
    def __init__(self):
        self.open = False
        self.capture = False
        self.text = ''
        self.submitted = []
        self.toggles = 0

    def key(self, vk, seconds=0):
        if vk == 0xC0:
            self.open = not self.open
            self.capture = True
            self.toggles += 1
        elif self.open:
            self.capture = False
            if vk == 13:
                self.submitted.append(self.text)
                self.text = ''
                self.open = False

    def char(self, hwnd, message, value, unused):
        if self.open and not self.capture:
            self.text += chr(value)
        return 1

    def native_text(self, text):
        for char in text:
            self.capture = False
            self.char(101, 0x102, ord(char), 0)


class InputRegression(unittest.TestCase):
    def test_legacy_input_map_is_explicit_and_isolated(self):
        scope = functions()
        self.assertTrue(callable(scope.get('resolve_input_map')), '旧即时输入验收缺少显式legacy地图路径')
        self.assertEqual(scope['resolve_input_map']('legacy'),
                         '/Game/Developers/Y01PlaygroundTests/R33U2Legacy/Maps/L_Y01Playground')
        for invalid in (None, 'main', 'compare', '/Game/Y01Playground/Maps/L_Y01Playground'):
            with self.assertRaises(ValueError):
                scope['resolve_input_map'](invalid)

    def test_input_rejects_new_main_before_any_desktop_operation(self):
        result = subprocess.run([sys.executable, '-X', 'utf8', str(SCRIPT), '--map', 'main'], capture_output=True, text=True, encoding='utf-8')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--map legacy', result.stderr)
        self.assertNotIn('OWNED_START', result.stdout)

    def test_launcher_file_rejects_main_verify_before_start(self):
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-File',
                                 str(SCRIPT.with_name('Run-Y01Playground.ps1')), '-Map', 'main', '-Verify', '-Headless'],
                                capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'-Map legacy', result.stderr, result.stderr.decode('mbcs', errors='backslashreplace'))
        self.assertNotIn(b'ParserError', result.stderr)
        self.assertNotIn(b'PID=', result.stdout)

    def test_launcher_map_resolution_preserves_default_and_guards_verify(self):
        launcher = SCRIPT.with_name('Run-Y01Playground.ps1')
        source = launcher.read_text(encoding='utf-8-sig')
        self.assertIn('function Resolve-PlaygroundMap', source, '启动器缺少可独立验证的地图/Verify兼容策略')
        definition = re.search(r'^function Resolve-PlaygroundMap\([^\n]+\) \{\n.*?^\}', source, re.M | re.S)
        self.assertIsNotNone(definition, '必须隔离纯函数，不能执行启动器主体')
        command = ("$ErrorActionPreference='Stop'; " + definition[0] + "\n"
                   "Resolve-PlaygroundMap 'main' $false; Resolve-PlaygroundMap 'legacy' $true; Resolve-PlaygroundMap 'compare' $false; "
                   "Resolve-PlaygroundMap 'r33growth' $false; Resolve-PlaygroundMap 'r34compare' $false; "
                   "Resolve-PlaygroundMap 'r34ranged' $false; Resolve-PlaygroundMap 'trainingcompare' $false; "
                   "try {Resolve-PlaygroundMap 'main' $true; exit 9} catch {Write-Output 'LEGACY_REJECT_OK'}")
        encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode('mbcs', errors='backslashreplace'))
        self.assertEqual(result.stdout.decode('ascii').splitlines(), [
            '/Game/Y01Playground/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33U2Legacy/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33Growth/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R34Compare/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R34Ranged/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33TrainingCompare/Maps/L_Y01Playground', 'LEGACY_REJECT_OK'])

    def console_scope(self):
        scope, console = functions(), Console()
        scope.update(key=console.key, type_text=console.native_text,
                     user=types.SimpleNamespace(PostMessageW=console.char),
                     time=types.SimpleNamespace(sleep=lambda _: None), emit=Mock())
        return scope, console

    def test_command_submits_actual_text(self):
        scope, console = self.console_scope()
        scope['command']('getall Y01PlaygroundController Observer')
        self.assertEqual(console.submitted, ['getall Y01PlaygroundController Observer'])

    def test_enter_does_not_reopen_console(self):
        scope, console = self.console_scope()
        scope['command']('shot showui')
        self.assertFalse(console.open, 'Enter提交后不得再按波浪键')
        self.assertEqual(console.toggles, 1)

    def test_missing_screenshot_is_failure(self):
        scope = functions()
        with tempfile.TemporaryDirectory() as directory:
            scope.update(root=Path(directory), output=Path(directory), label='test', shot_index=0,
                         command=Mock(), text=lambda: '', focused=Mock(),
                         time=types.SimpleNamespace(sleep=lambda _: None, time_ns=time.time_ns),
                         wait=Mock(side_effect=TimeoutError('截图未落盘')), emit=Mock())
            with self.assertRaises((TimeoutError, RuntimeError, AssertionError)):
                scope['capture']('Waiting')

    def test_each_native_event_checks_before_and_after(self):
        scope = functions()
        self.assertTrue(callable(scope.get('key_event')), '缺少逐事件前后台身份保护')
        focus, send = Mock(), Mock()
        scope.update(focused=focus, user=types.SimpleNamespace(keybd_event=send), emit=Mock())
        scope['key_event'](87, 0)
        scope['key_event'](87, 2)
        self.assertEqual(focus.call_count, 4)
        self.assertEqual(send.call_count, 2)

    def test_foreground_loss_prevents_event(self):
        scope = functions()
        self.assertTrue(callable(scope.get('key_event')), '缺少逐事件前后台身份保护')
        send = Mock()
        scope.update(focused=Mock(side_effect=RuntimeError('前台丢失')), user=types.SimpleNamespace(keybd_event=send))
        with self.assertRaises(RuntimeError):
            scope['key_event'](87, 0)
        send.assert_not_called()

    def test_foreground_loss_never_reactivates(self):
        scope = functions()
        user = Mock()
        user.GetForegroundWindow.return_value = 999
        user.IsWindow.return_value = True
        scope.update(user=user, kernel=Mock(), identity=lambda hwnd: {'hwnd': hwnd, 'pid': 202 if hwnd == 101 else 303, 'tid': 5},
                     time=types.SimpleNamespace(sleep=lambda _: None), emit=Mock())
        with self.assertRaises(RuntimeError):
            scope['focused']('unit') if 'reason' in scope['focused'].__code__.co_varnames else scope['focused']()
        user.SetForegroundWindow.assert_not_called()

    def test_same_hwnd_wrong_pid_is_failure(self):
        scope = functions()
        user = Mock()
        user.GetForegroundWindow.return_value = 101
        scope.update(user=user, identity=lambda hwnd: {'hwnd': hwnd, 'pid': 303, 'tid': 5}, emit=Mock())
        with self.assertRaises(RuntimeError):
            scope['focused']('unit') if 'reason' in scope['focused'].__code__.co_varnames else scope['focused']()

    def test_png_rejects_old_empty_and_truncated(self):
        scope = functions()
        self.assertTrue(callable(scope.get('valid_png')), '截图必须验证非空、新时间及完整PNG')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frame.png'
            start = time.time_ns()
            self.assertFalse(scope['valid_png'](path, start))
            # Windows文件创建时间可能落后高精度time_ns不足1ms；隔开测试夹具创建，保留严格新鲜度断言。
            time.sleep(0.003)
            path.write_bytes(b'')
            self.assertFalse(scope['valid_png'](path, start))
            path.write_bytes(b'\x89PNG\r\n\x1a\n')
            self.assertFalse(scope['valid_png'](path, start))
            def chunk(kind, data):
                return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
            path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 1280, 800, 8, 6, 0, 0, 0))
                             + chunk(b'IDAT', zlib.compress((b'\0' + b'\0' * 5120) * 800)) + chunk(b'IEND', b''))
            self.assertTrue(scope['valid_png'](path, start))
            self.assertFalse(scope['valid_png'](path, time.time_ns() + 10**9))

    def test_skill_requires_damage_and_its_own_deadline(self):
        scope = functions()
        self.assertTrue(callable(scope.get('check_skill')), '必须断言实际血量与对应技能CD')
        before = {'health': {'Y01Monster_0': 60}, 'deadlines': [0, 0], 'target': 'Y01Monster_0'}
        after = {'health': {'Y01Monster_0': 35}, 'deadlines': [10, 0], 'target': 'Y01Monster_0'}
        scope['check_skill'](before, after, 0)
        for bad in ({**after, 'health': before['health']}, {**after, 'deadlines': [0, 10]},
                    {**after, 'health': {'Y01Monster_1': 35}}):
            with self.assertRaises((AssertionError, RuntimeError)):
                scope['check_skill'](before, bad, 0)

    def test_skill_tracks_damage_identity_when_native_target_changes(self):
        scope = functions()
        before = {'health': {'Y01Monster_4': 35, 'Y01Monster_5': 60},
                  'deadlines': [17.2, 13.5], 'target': 'Y01Monster_4'}
        after = {'health': {'Y01Monster_4': 35, 'Y01Monster_5': 35},
                 'deadlines': [17.2, 19.3], 'target': 'Y01Monster_5'}
        scope['check_skill'](before, after, 1)
        with self.assertRaises(AssertionError):
            scope['check_skill'](before, {**after, 'health': {'Y01Monster_4': 10, 'Y01Monster_5': 35}}, 1)
        with self.assertRaises(AssertionError):
            scope['check_skill'](before, {**after, 'health': {'Y01Monster_6': 35}}, 1)

    def test_movement_uses_frame_coordinates_not_old_telemetry(self):
        scope = functions()
        stale = Mock(return_value={'X': -133, 'HP': 200})
        scope.update(telemetry=stale, observe=Mock(return_value={'x': -133}), key=Mock(),
                     capture=Mock(side_effect=[{'x': -55}, {'x': -211}]),
                     wait=Mock(side_effect=AssertionError('不应轮询旧遥测冒充同帧位置')))
        scope['moves']('Returned')
        stale.assert_not_called()

    def test_no_actual_movement_is_failure(self):
        scope = functions()
        scope.update(observe=Mock(return_value={'x': -150}), key=Mock(),
                     telemetry=Mock(return_value={'X': -150, 'HP': 200}),
                     wait=Mock(side_effect=AssertionError('没有位移')),
                     capture=Mock(return_value={'x': -150}))
        with self.assertRaises(AssertionError):
            scope['moves']('Main')

    def test_skills_uses_latest_pre_key_capture(self):
        scope = functions()
        stale = {'target': 'Y01Monster_4'}
        latest = {'target': 'Y01Monster_3'}
        after1, after2 = {'step': 1}, {'step': 2}
        check = Mock()
        scope.update(until_state=Mock(return_value=stale), capture=Mock(side_effect=[latest, after1, after2]),
                     key=Mock(), check_skill=check)
        scope['skills']('Training')
        self.assertEqual(check.call_args_list[0].args, (latest, after1, 0), '截图之后不得仍沿用截图之前的旧索敌')

    def test_native_text_emits_key_pairs_not_posted_chars(self):
        scope = functions()
        events = Mock()
        user = Mock()
        user.GetKeyboardLayout.return_value = 1
        user.VkKeyScanExW.side_effect = lambda char, layout: {'a': 65, '|': 0x100 | 0xDC}[char]
        scope.update(user=user, identity=lambda hwnd: {'tid': 5}, key_event=events)
        scope['type_text']('a|')
        self.assertEqual([call.args for call in events.call_args_list],
                         [(65, 0), (65, 2), (16, 0), (220, 0), (220, 2), (16, 2)])
        user.PostMessageW.assert_not_called()

    def test_foreground_loss_is_latched(self):
        scope = functions()
        user = Mock()
        user.GetForegroundWindow.return_value = 999
        user.IsWindow.return_value = True
        scope.update(user=user, identity=lambda hwnd: {'hwnd': hwnd, 'pid': 202, 'tid': 5})
        with self.assertRaises(RuntimeError):
            scope['focused']('lost')
        user.GetForegroundWindow.return_value = 101
        with self.assertRaises(RuntimeError):
            scope['focused']('restored-but-must-stop')

    def test_cleanup_never_closes_reused_window(self):
        scope = functions()
        user, process = Mock(), Mock()
        process.pid = 202
        process.poll.return_value = None
        scope.update(user=user, p=process, identity=lambda hwnd: {'hwnd': hwnd, 'pid': 303, 'tid': 5})
        scope['cleanup']()
        user.PostMessageW.assert_not_called()
        process.wait.assert_called_once_with(timeout=8)

    def test_snapshot_without_fresh_phase_is_not_success(self):
        scope = functions()
        with self.assertRaises(RuntimeError):
            scope['parse_snapshot']('Command not recognized: w')

    def test_fresh_snapshot_parser_preserves_target_and_slots(self):
        scope = functions()
        self.assertTrue(callable(scope.get('parse_snapshot')), '必须解析真实回读，而非按键计数')
        snapshot = scope['parse_snapshot']('''0) Y01PlaygroundCharacter /Game/Test:PersistentLevel.Y01PlaygroundCharacter_0 (-133.000000, 0.000000, 90.000000)
0) Y01GameState /Game/Test:PersistentLevel.Y01GameState_0.Snapshot = (Phase=Waiting,Revision=0)
0) Y01PlaygroundController /Game/Test:PersistentLevel.Y01PlaygroundController_0.Observer = (bReady=True,Training=(bInside=False),Notice="",Slots=((Row="BasicAttack",bActive=True,Deadline=10.000000),(Row="HeavyAttack",bActive=True,Deadline=0.000000),(),()),Target="/Script/Y01.Y01Monster'/Game/Test:PersistentLevel.Y01Monster_0'")
0) Y01AttributeSet /Game/Test:PersistentLevel.Y01Monster_0.Attributes.Health = (BaseValue=35.000000,CurrentValue=35.000000)
''')
        self.assertEqual(snapshot['phase'], 'Waiting')
        self.assertIn('x', snapshot, '缺少同帧原生坐标')
        self.assertEqual(snapshot['x'], -133)
        self.assertEqual(snapshot['target'], 'Y01Monster_0')
        self.assertEqual(snapshot['health']['Y01Monster_0'], 35)
        self.assertEqual(snapshot['deadlines'], [10, 0])


class EvidenceAudit(unittest.TestCase):
    """只读复核已完成的原生运行，不启动游戏、不发送任何输入。"""
    def __init__(self, directory):
        super().__init__('runTest')
        self.directory = Path(directory).resolve()

    def shortDescription(self):
        return '原生证据逐条校验：' + self.directory.name

    def runTest(self):
        directory = self.directory
        lines = (directory / 'runner.log').read_text(encoding='utf-8').splitlines()
        client = (directory / 'Client.log').read_text(encoding='utf-8')
        pid = int(re.fullmatch(r'OWNED_START pid= (\d+) COMMAND= .+', lines[0])[1])
        self.assertEqual(lines[1], 'SCRIPT_SHA256 ' + hashlib.sha256(SCRIPT.read_bytes()).hexdigest())
        summary = re.fullmatch(r'NATIVE_SCENARIO_PASS scenario= (main|training) screenshots= (\d+) foreground_checks= (\d+) native_key_events= (\d+)', lines[-2])
        self.assertIsNotNone(summary, '必须完整成功退出，不能只拿中途记录冒充通过')
        mode, shots, checks, events = summary.groups()
        self.assertEqual(lines[-3], f'OWNED_EXIT pid= {pid} code= 0 forced= False')
        self.assertEqual(lines[-1], 'RUNNER_EXIT= 0 EVIDENCE= ' + str(directory))
        self.assertNotIn('-Y01PlaygroundVerify', lines[0])
        for forbidden in ('Command not recognized:', 'Y01Playground INPUT_PASS', 'VERIFY_CLIENT_PASS'):
            self.assertNotIn(forbidden, client)
        counts, held, hwnd, previous_time = {'foreground': 0, 'key': 0}, set(), None, 0
        mouse_reasons = []
        records = []
        for index, line in enumerate(lines):
            if line.startswith('FOREGROUND '):
                match = re.fullmatch(r'FOREGROUND (\d+) (.+?) target= (\{.+?\}) actual= (\{.+?\}) MATCH= True', line)
                self.assertIsNotNone(match, line)
                number, reason, target, actual = match.groups()
                target, actual = ast.literal_eval(target), ast.literal_eval(actual)
                counts['foreground'] += 1
                self.assertEqual(int(number), counts['foreground'])
                hwnd = target['hwnd'] if hwnd is None else hwnd
                self.assertGreater(hwnd, 0)
                self.assertEqual((target['hwnd'], target['pid']), (hwnd, pid))
                self.assertEqual(actual, target)
                if reason.startswith('mouse-'):
                    mouse_reasons.append(reason)
            elif line.startswith('NATIVE_KEY '):
                match = re.fullmatch(r'NATIVE_KEY (\d+) vk= (\d+) flags= ([02]) monotonic= ([\d.]+)', line)
                self.assertIsNotNone(match, line)
                number, vk, flags, stamp = match.groups()
                counts['key'] += 1
                self.assertEqual(int(number), counts['key'])
                for distance, suffix in ((2, 'before'), (1, 'after')):
                    self.assertRegex(lines[index - distance], rf'^FOREGROUND \d+ key-{vk}-{flags}-{suffix} target= ')
                self.assertNotIn(int(vk), (0x11, 0x12, 0x5B, 0x5C, 0xA2, 0xA3, 0xA4, 0xA5))
                self.assertGreaterEqual(float(stamp), previous_time)
                previous_time = float(stamp)
                if flags == '0':
                    self.assertNotIn(vk, held)
                    held.add(vk)
                else:
                    self.assertIn(vk, held)
                    held.remove(vk)
            elif line.startswith('SCREENSHOT '):
                records.append(line)
        self.assertFalse(held, '成功结束不得残留按下的键')
        self.assertEqual(counts, {'foreground': int(checks), 'key': int(events)})
        mouse_pair = ['mouse-position-before', 'mouse-position-after', 'mouse-2-before',
                      'mouse-2-after', 'mouse-4-before', 'mouse-4-after']
        self.assertEqual(mouse_reasons, mouse_pair * (2 if mode == 'training' else 0))
        self.assertEqual(client.count('Y01Training UI_CLICK'), 2 if mode == 'training' else 0)
        stages = ['Waiting', 'Main-W', 'Main-S', 'Preparing', 'Battle', 'Main-Target', 'Main-Skill1', 'Main-Skill2']
        if mode == 'training':
            stages += ['Training-Entered', 'Training-W', 'Training-S', 'Training-Target',
                       'Training-Skill1', 'Training-Skill2', 'Training-Returned', 'Returned-W', 'Returned-S']
        stages += ['F5']
        self.assertEqual(len(records), int(shots))
        self.assertEqual(len(records), len(stages))
        scope, states, hashes = functions(), {}, set()
        previous_request = 0
        for number, (stage, line) in enumerate(zip(stages, records), 1):
            match = re.fullmatch(r'SCREENSHOT (.+) size= (\d+) created_ns= (\d+) mtime_ns= (\d+) requested_ns= (\d+) sha256= ([a-f0-9]{64}) STATE= (.+)', line)
            self.assertIsNotNone(match, line)
            filename, size, created, modified, requested, digest, state = match.groups()
            path = Path(filename)
            self.assertEqual(path, directory / f'{number:02d}-{stage}.png')
            self.assertTrue(scope['valid_png'](path, int(requested)), filename)
            stat = path.stat()
            self.assertEqual((stat.st_size, stat.st_ctime_ns, stat.st_mtime_ns), tuple(map(int, (size, created, modified))))
            self.assertGreater(int(requested), previous_request)
            previous_request = int(requested)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)
            self.assertNotIn(digest, hashes)
            hashes.add(digest)
            states[path.stem] = json.loads(state)
        self.assertEqual({p.stem for p in directory.glob('*.png')}, set(states))
        # OutputDeviceHelper在缓冲日志输出时读取GFrameCounter，前缀不是采样帧ID。
        # UPlayer同步执行整组查询；逐组核验最新Observer/Health/坐标/Snapshot及随后截图。
        query_rows, traced = [], []
        for line in client.splitlines():
            entry = re.match(r'^\[[^\]]+\]\[\s*\d+\](.*)', line)
            if not entry:
                continue
            body = entry[1]
            if '.Observer = ' in body:
                query_rows = [body]
            elif any(marker in body for marker in ('.Health = ', '.Snapshot = ', ') Y01PlaygroundCharacter ')):
                query_rows.append(body)
            shot = re.search(r'Tracing Screenshot "([^"]+)" taken with size: 1280 x 800', body)
            if shot:
                self.assertGreaterEqual(len(query_rows), 3)
                self.assertIn('.Observer = ', query_rows[0])
                self.assertTrue(all('.Health = ' in row for row in query_rows[1:-2]))
                self.assertIn(') Y01PlaygroundCharacter ', query_rows[-2])
                self.assertIn('.Snapshot = ', query_rows[-1])
                self.assertEqual(scope['parse_snapshot']('\n'.join(query_rows)), states[shot[1]])
                traced.append(shot[1])
                query_rows = []
        self.assertEqual(traced, list(states))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result_log', nargs='?', type=Path)
    parser.add_argument('--evidence', action='append', default=[], help='附加只读校验一个已有原生证据目录；可重复指定')
    args = parser.parse_args()
    result_log = args.result_log
    stream = io.StringIO()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InputRegression)
    for directory in args.evidence:
        suite.addTest(EvidenceAudit(directory))
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
    report = stream.getvalue()
    print(report)
    if result_log:
        with result_log.open('x', encoding='utf-8') as handle:
            handle.write(report)
    sys.exit(not result.wasSuccessful())
