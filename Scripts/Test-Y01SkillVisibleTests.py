"""U2验收器隔离回归：不启动UE、不发送输入；伪造数据只存在于本测试。"""
import argparse
import ast
import base64
import re
import copy
import importlib.util
import io
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch
from contextlib import redirect_stderr
import subprocess
import unittest

SCRIPT = Path(__file__).with_name('Test-Y01SkillVisible.py')


def load():
    if not SCRIPT.exists():
        return None
    spec = importlib.util.spec_from_file_location('skill_visible', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AcceptanceRegression(unittest.TestCase):
    def setUp(self):
        self.api = load()

    def api_function(self, name):
        result = getattr(self.api, name, None)
        self.assertTrue(callable(result), '缺少真正验收断言：' + name)
        return result

    def fixture(self):
        return [
            dict(kind='COMMIT', Actor='A', Mode=2, Speed=400, Damage=25, t=10., Radius=12, World=1., StartedAt=1., EndsAt=3.5),
            dict(kind='STATE', Actor='A', NetMode=1, Phase=0, t=10., Position=(0, 0, 90)),
            dict(kind='HIT', Actor='A', Target='M', HP=35, Damage=25, t=10.6, World=1.6, Position=(240, 0, 90)),
            dict(kind='STATE', Actor='A', NetMode=1, Phase=1, t=10.6, Position=(240, 0, 90)),
            dict(kind='EXPIRE', Actor='A', Mode=2, Phase=1, World=1.71, StartedAt=1., EndsAt=1.7, t=10.71),
            dict(kind='END', Actor='A', NetMode=1, Phase=1, t=10.71, World=1.71, StartedAt=1., EndsAt=1.7),
        ]

    def test_only_owned_clients_start_with_legacy_console_search(self):
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.args = SimpleNamespace(port=0, map='u2baseline', fast_observe=False)
        runner.output = SCRIPT.parent.parent / 'Saved'
        runner.logs, runner.owned = {}, {}
        runner.wait = lambda *args: True
        runner.focus = lambda role: None
        runner.emit = lambda *args, **kwargs: None
        runner.text = lambda role: 'D3D12'
        with patch.object(self.api.subprocess, 'Popen', return_value=SimpleNamespace(pid=123)) as launch, patch.object(self.api.time, 'sleep'), patch.object(Path, 'is_file', return_value=True):
            runner.launch()
        commands = [call.args[0] for call in launch.call_args_list]
        self.assertEqual(len(commands), 3)
        self.assertIn('/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground', commands[0])
        self.assertFalse(any('console.searchmode.legacy' in arg for arg in commands[0]))
        for command in commands[1:]:
            execs = [arg for arg in command if arg.startswith('-ExecCmds=')]
            self.assertEqual(execs, ['-ExecCmds=t.MaxFPS 60,console.searchmode.legacy 1'])
            self.assertIn('-d3d12', command)
            self.assertNotIn('-nullrhi', command)

    def test_fixed_damage_requires_explicit_non_growth_map(self):
        resolve = self.api_function('resolve_skill_map')
        self.assertEqual(resolve('u2baseline'), '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground')
        self.assertEqual(resolve('compare'), '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Maps/L_Y01Playground')
        for selection in (None, 'main', 'growthcompare', 'legacy', '/Game/Y01Playground/Maps/L_Y01Playground'):
            with self.subTest(selection=selection), self.assertRaisesRegex(ValueError, 'u2baseline'):
                resolve(selection)

    def test_fixed_damage_speed_matches_explicit_map(self):
        speed = self.api_function('skill_map_speed')
        self.assertEqual(speed('u2baseline'), 400)
        self.assertEqual(speed('compare'), 200)
        with self.assertRaises(ValueError): speed('main')

    def test_launcher_growth_and_baseline_do_not_remap_main(self):
        source = SCRIPT.with_name('Run-Y01Playground.ps1').read_text(encoding='utf-8-sig')
        definition = re.search(r'^function Resolve-PlaygroundMap\([^\n]+\) \{\n.*?^\}', source, re.M | re.S)
        self.assertIsNotNone(definition)
        command = ("$ErrorActionPreference='Stop'; " + definition[0] + "\n"
                   "Resolve-PlaygroundMap 'main' $false; Resolve-PlaygroundMap 'u2baseline' $false; "
                   "Resolve-PlaygroundMap 'growthcompare' $false; "
                   "foreach ($map in @('u2baseline','growthcompare')) {try {Resolve-PlaygroundMap $map $true; exit 9} catch {Write-Output 'LEGACY_ONLY'}}")
        encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
        result = self.api.subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.decode('ascii').splitlines(), [
            '/Game/Y01Playground/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground',
            '/Game/Developers/Y01PlaygroundTests/R33U3GrowthCompare/Maps/L_Y01Playground', 'LEGACY_ONLY', 'LEGACY_ONLY'])

    def test_automation_explicit_timeout_reaches_real_wait_command(self):
        source = SCRIPT.with_name('Run-Y01Automation.ps1').read_text(encoding='utf-8-sig')
        parameters = re.search(r'^param\([^\n]+\)', source).group(0)
        wait = re.search(r'^\s*Wait-Process[^\n]+', source, re.M).group(0)
        command = ("function Wait-Process {param($Id,$Timeout,$ErrorAction) Write-Output $Timeout}\n"
                   "function Harness {\n" + parameters + "\n$p = @{Id=123}\n" + wait + "\n}\n"
                   "Harness; Harness -TimeoutSeconds 900")
        encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
        result = self.api.subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.decode('ascii').splitlines(), ['150', '900'], '全量必须可显式延长等待，默认150与失败判定保留')
        self.assertIn('$report.failed -ne 0', source)
        self.assertIn('$report.notRun -ne 0', source)
        self.assertIn('$report.inProcess -ne 0', source)

    def clock_fixture(self, sample=20, ready=True):
        return (f'.Observer = (bReady={str(ready)},ServerSampleTime={sample},PreparationDeadline=24.000000,'
                'Training=(bInside=True,NextBatchAt=26.000000),'
                'Slots=((Row="BasicAttack",Deadline=21.500000),(Row="HeavyAttack",Deadline=23.250000)))')

    def test_snapshot_clock_uses_observer_sample_for_all_three_countdowns(self):
        parse = self.api_function('parse_observer_clock')
        row = '.Snapshot = (PreparationDeadline=999)\n' + self.clock_fixture()
        state = parse(row, [21.5, 23.25])
        self.assertEqual(state['server_sample_time'], 20)
        self.assertEqual(state['preparation_deadline'], 24)
        self.assertEqual(state['preparation_remaining'], 4)
        self.assertEqual(state['training_remaining'], 6)
        self.assertEqual(state['remaining'], [1.5, 3.25])
        self.assertEqual(parse(self.clock_fixture(30), [21.5, 23.25])['remaining'], [0, 0])

    def test_snapshot_clock_unsampled_and_not_ready_are_unknown_not_ready(self):
        parse = self.api_function('parse_observer_clock')
        for text in (self.clock_fixture(-1), self.clock_fixture(20, False),
                     self.clock_fixture().replace('ServerSampleTime=20,', '')):
            with self.subTest(text=text):
                state = parse(text, [21.5, 23.25])
                self.assertEqual(state['remaining'], [None, None])
                self.assertIsNone(state['preparation_remaining'])
                self.assertIsNone(state['training_remaining'])
        with self.assertRaises(AssertionError): parse('', [21.5, 23.25])

    def test_snapshot_clock_no_new_sample_keeps_value_after_disconnect(self):
        parse = self.api_function('parse_observer_clock')
        row = self.clock_fixture()
        before = parse(row, [21.5, 23.25])
        with patch.object(self.api.time, 'time', return_value=999999), patch.object(self.api.time, 'monotonic', return_value=999999):
            self.assertEqual(parse(row, [21.5, 23.25]), before)
        self.assertEqual(parse(self.clock_fixture(21), [21.5, 23.25])['remaining'], [.5, 2.25])

    def test_live_snapshot_cd_rejects_stale_sample_and_inflated_remaining(self):
        check = self.api_function('validate_snapshot_cd')
        state = dict(ready=True, server_sample_time=10.3, deadlines=[0, 12], remaining=[0, 1.7])
        commit = dict(World=10.)
        self.assertAlmostEqual(check(state, 1, commit)['remaining'], 1.7)
        for bad in (dict(state, server_sample_time=-1), dict(state, ready=False),
                    dict(state, server_sample_time=0, remaining=[0, 12]),
                    dict(state, remaining=[0, 15]), dict(state, remaining=[0, None])):
            with self.subTest(state=bad), self.assertRaises(AssertionError): check(bad, 1, commit)

    def test_boundary_ready_requires_live_inside_outside_positions_and_health(self):
        check = self.api_function('boundary_ready')
        state = dict(phase='Battle', position=(600, 0, 90),
                     monsters={'M': (450, 0, 90), 'N': (0, 0, 90)}, health={'M': 60, 'N': 60})
        self.assertTrue(check(state))
        for bad in (dict(state, phase='Preparing'), dict(state, monsters={}, health={}),
                    dict(state, health={'M': 60}), dict(state, monsters={'M': (900, 0, 90), 'N': (0, 0, 90)}),
                    dict(state, monsters={'M': (450, 0, 90), 'N': (400, 0, 90)})):
            with self.subTest(state=bad): self.assertFalse(check(bad))

    def test_main_ready_waits_for_live_replicated_in_range_targets(self):
        check = self.api_function('main_damage_ready')
        state = dict(phase='Battle', ready=True, position=(0, 0, 90), target='M',
                     monsters={'M': (200, 0, 90), 'N': (500, 0, 90), 'P': (0, 500, 90)},
                     health={'M': 60, 'N': 60, 'P': 60})
        self.assertTrue(check(state))
        for bad in (dict(state, phase='Preparing'), dict(state, ready=False), dict(state, monsters={}, health={}),
                    dict(state, health={'M': 60}), dict(state, health={'M': 35, 'N': 60, 'P': 60}),
                    dict(state, target=None), dict(state, monsters=dict(state['monsters'], M=(300, 0, 90))),
                    dict(state, monsters=dict(state['monsters'], M=(200, 0, 300)))):
            with self.subTest(state=bad): self.assertFalse(check(bad))

    def test_main_fixed_damage_rejects_empty_area_and_wrong_health_chain(self):
        check = self.api_function('validate_main_damage_chain')
        before = dict(health={'M': 60, 'N': 60, 'P': 60})
        area = dict(health={'M': 35, 'N': 60, 'P': 60})
        ball = dict(health={'M': 10, 'N': 60, 'P': 60})
        self.assertEqual(check(before, area, ball)['chain'], [60, 35, 10])
        for a, b, c in ((dict(health={}), before, area), (before, before, area),
                        (before, area, dict(health={'M': 35, 'N': 35, 'P': 60})),
                        (before, dict(health={'M': 34, 'N': 60, 'P': 60}), ball),
                        (before, area, dict(health={'M': 10})),
                        (before, area, dict(health={'M': 9, 'N': 60, 'P': 60}))):
            with self.subTest(before=a, area=b, ball=c), self.assertRaises(AssertionError): check(a, b, c)

    def test_main_scenario_waits_then_enforces_fixed_chain(self):
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.args = SimpleNamespace(scenario='main', hide_hud=False)
        waiting = dict(phase='Waiting', ready=True, deadlines=[0, 0])
        early = dict(phase='Battle', ready=True, position=(0, 0, 90), target=None, monsters={}, health={})
        ready = dict(early, target='M', monsters={'M': (200, 0, 90), 'N': (500, 0, 90), 'P': (0, 500, 90)}, health={'M': 60, 'N': 60, 'P': 60})
        queue = iter([waiting, waiting, waiting, early, early, ready])
        runner.observe = lambda: next(queue)
        runner.focus = runner.emit = runner.moves = runner.toggle_details = lambda *a, **k: None
        runner.n = SimpleNamespace(key=lambda *a: None)
        runner.record = lambda label, action, after: action()
        runner.text = lambda role: ''
        checks, attacks = [], []
        def wait(predicate, *args):
            for i in range(3):
                result = predicate()
                if result: return result
            self.fail('等待条件未使用完整复制数据')
        runner.wait = wait
        def attack(label, mode, before):
            attacks.append((label, before))
            return dict(health={'M': 35 if mode == 1 else 10, 'N': 60, 'P': 60})
        runner.attack = attack
        runner.check = lambda name, function: (checks.append(name), function())[1]
        runner.main_scenario()
        self.assertEqual(attacks[0][1], ready, '不得在Battle空怪快照上提前空放')
        self.assertIn('main-fixed-25-60-35-10', checks)

    def test_main_approach_uses_live_target_direction_without_relaxing_range(self):
        choose = self.api_function('main_approach_key')
        state = dict(phase='Battle', ready=True, position=(-150, -158, 90), target='M',
                     monsters={'M': (-17, 110, 90), 'N': (-15, 183, 90), 'P': (-66, 262, 90)},
                     health={'M': 60, 'N': 60, 'P': 60})
        original = copy.deepcopy(state)
        self.assertFalse(self.api.main_damage_ready(state))
        self.assertEqual(choose(state), 'D')
        for position, expected in (((-500, 110, 90), 'W'), ((500, 110, 90), 'S'), ((-17, 500, 90), 'A')):
            self.assertEqual(choose(dict(state, position=position)), expected)
        for bad in (dict(state, phase='Waiting'), dict(state, ready=False), dict(state, monsters={}, health={}),
                    dict(state, health={'M': 60}), dict(state, health={'M': 35, 'N': 60, 'P': 60}),
                    dict(state, target=None), dict(state, position=(-17, 0, 90)), dict(state, position=(-150, -158, 500))):
            with self.subTest(state=bad): self.assertIsNone(choose(bad))
        self.assertEqual(state, original)

    def test_main_scenario_approaches_peer_aggro_target_with_native_key_then_reobserves(self):
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.args = SimpleNamespace(scenario='main', hide_hud=False)
        waiting = dict(phase='Waiting', ready=True, deadlines=[0, 0])
        far = dict(phase='Battle', ready=True, position=(-150, -158, 90), target='M',
                   monsters={'M': (-17, 110, 90), 'N': (-15, 183, 90), 'P': (-66, 262, 90)},
                   health={'M': 60, 'N': 60, 'P': 60})
        close = dict(far, position=(-150, -50, 90))
        calls, attacks, initial = [], [], [waiting, waiting, waiting, far]
        def observe():
            if initial: return initial.pop(0)
            return close if any(call[0] == ord('D') for call in calls) else far
        runner.observe = observe
        runner.focus = runner.emit = runner.moves = runner.toggle_details = lambda *a, **k: None
        runner.n = SimpleNamespace(key=lambda *args: calls.append(args))
        runner.record = lambda label, action, after: action()
        runner.text = lambda role: ''
        runner.check = lambda name, function: function()
        def wait(predicate, *args):
            for _ in range(4):
                value = predicate()
                if value: return value
            self.fail('只等待追向另一玩家的怪，未使用原生移动靠近并重新读取')
        runner.wait = wait
        def attack(label, mode, before):
            attacks.append(before)
            return dict(health={'M': 35 if mode == 1 else 10, 'N': 60, 'P': 60})
        runner.attack = attack
        runner.main_scenario()
        self.assertEqual(attacks[0], close)
        movement = [call for call in calls if call[0] == ord('D')]
        self.assertEqual(len(movement), 1)
        self.assertGreater(movement[0][1], 0)
        self.assertLessEqual(movement[0][1], .2, '一次短步后必须重新观测，不能盲走')

    def test_attack_captures_nonzero_snapshot_before_png_drain(self):
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.args = SimpleNamespace(scenario='boundary')
        calls = []
        runner.n = SimpleNamespace(hold=lambda value: calls.append(('hold', value)))
        runner.attack_keys = lambda mode: calls.append(('attack', mode))
        runner.training_observe = lambda: calls.append(('observer',)) or {'remaining': [.9, .6]}
        method = getattr(runner, 'attack_and_sample', None)
        self.assertTrue(callable(method), '必须在截图落盘等待之前读取仍在CD中的快照')
        self.assertEqual(method(2), {'remaining': [.9, .6]})
        self.assertEqual(calls, [('attack', 2), ('hold', 1.1), ('observer',)])

    def test_boundary_needs_inside_damage_and_unchanged_outside(self):
        check = self.api_function('validate_area_contrast')
        before = dict(monsters={'M': (100, 0, 90), 'N': (600, 0, 90)}, health={'M': 60, 'N': 60})
        after = dict(monsters=before['monsters'], health={'M': 35, 'N': 60})
        hits = [dict(Target='M', Damage=25, HP=35)]
        self.assertEqual(check((0, 0, 90), before, after, hits)['outside'], ['N'])
        for bad in (dict(after, health={'M': 35, 'N': 35}), dict(after, monsters={'M': (100, 0, 90), 'N': (200, 0, 90)})):
            with self.assertRaises(AssertionError): check((0, 0, 90), before, bad, hits)
        with self.assertRaises(AssertionError): check((0, 0, 90), before, after, [])

    def test_midflight_hp_must_be_timestamped_before_hit(self):
        check = self.api_function('validate_midflight_health')
        samples = '[1970.01.01-00.00.10:250][1]0) Y01AttributeSet /Game/X:PersistentLevel.LocalM.Attributes.Health = (CurrentValue=60.000000)'
        self.assertEqual(check(self.fixture(), 'A', samples, 'LocalM', 60)['hp'], 60)
        for bad in (samples.replace(':250', ':750'), samples.replace('60.000000', '35.000000'), samples.replace('LocalM', 'Other'), ''):
            with self.assertRaises(AssertionError): check(self.fixture(), 'A', bad, 'LocalM', 60)

    def test_phase_uses_one_short_readonly_query(self):
        method = getattr(getattr(self.api, 'Runner', None), 'phase', None)
        self.assertTrue(callable(method), '缺少不阻塞四秒的单条Phase回读')
        runner = self.api.Runner.__new__(self.api.Runner)
        calls, data = [], {'text': ''}
        def command(value):
            calls.append(value)
            data['text'] += '.Snapshot = (Phase=Battle)\n'
        runner.focus = lambda role: None
        runner.text = lambda role: data['text']
        runner.n = SimpleNamespace(command=command)
        runner.wait = lambda predicate, description: predicate()
        runner.emit = lambda *args, **kwargs: None
        self.assertEqual(runner.phase(), 'Battle')
        self.assertEqual(calls, ['getall Y01GameState Snapshot'])

    def test_training_continuity_rejects_ended_despite_stale_battle_snapshot(self):
        check = self.api_function('validate_training_main_continuity')
        battle = ('[2026.09.28-08.10.00:900][1]LogTemp: Display: Y01Training OBSERVE Inside=1 Alive=2 HP=100\n'
                  '[2026.09.28-08.10.01:000][1]LogTemp: Display: Y01Flow Rep Phase=2 Wave=0 Completed=0 Revision=3\n')
        ended = '[2026.09.28-08.10.09:514][2]LogTemp: Display: Y01Flow Rep Phase=3 Wave=0 Completed=0 Revision=4\n'
        snapshot = '[2026.09.28-08.10.10:746][3]0) Y01GameState X.Snapshot = (Phase=Battle,Revision=3)\n'
        self.assertEqual(check(battle + snapshot)['phase'], 2)
        for bad in ('', snapshot, battle + ended + snapshot, battle + snapshot + ended):
            with self.subTest(text=bad), self.assertRaises(AssertionError):
                check(bad)

    def test_midflight_query_precedes_second_cooldown_key(self):
        method = getattr(getattr(self.api, 'Runner', None), 'attack_keys', None)
        self.assertTrue(callable(method), '缺少先捕获飞行中HP、再验CD拒绝的输入调度')
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.args = SimpleNamespace(scenario='boundary')
        calls = []
        runner.n = SimpleNamespace(key=lambda value: calls.append(('key', value)),
                                   hold=lambda value: calls.append(('hold', value)),
                                   command=lambda value: calls.append(('command', value)))
        runner.attack_keys(2)
        self.assertEqual(calls, [('key', ord('2')), ('command', 'getall Y01AttributeSet Health'), ('hold', .04), ('key', ord('2'))])

    def test_training_cast_waits_for_visible_replication_before_native_leave(self):
        method = getattr(getattr(self.api, 'Runner', None), 'cast_and_leave', None)
        self.assertTrue(callable(method), '缺少明确原生释放/可见间隔/返回顺序')
        runner = self.api.Runner.__new__(self.api.Runner)
        calls = []
        runner.n = SimpleNamespace(key=lambda value: calls.append(('key', value)),
                                   hold=lambda value: calls.append(('hold', value)),
                                   click_training=lambda value: calls.append(('click', value)))
        runner.cast_and_leave(2)
        self.assertEqual(calls, [('key', ord('2')), ('hold', .1), ('click', 0)])

    def test_pixel_count_excludes_hud_and_orange_or_cyan_objects(self):
        check = self.api_function('count_feedback_pixels')
        from PIL import Image
        image = Image.new('RGB', (1280, 800), (100, 100, 100))
        image.paste((255, 0, 0), (500, 100, 502, 102))
        image.paste((0, 255, 0), (640, 440, 652, 452))
        image.paste((255, 0, 0), (10, 10, 100, 100))
        image.paste((0, 255, 0), (600, 690, 700, 710))
        image.paste((255, 160, 0), (600, 300, 620, 320))
        image.paste((0, 255, 255), (640, 370, 670, 410))
        self.assertEqual(check(image), {'red': 4, 'green': 144})

    def test_audit_rehashes_real_png_instead_of_trusting_manifest(self):
        check = self.api_function('audit_frames')
        from PIL import Image
        with tempfile.TemporaryDirectory(dir=SCRIPT.parent.parent / 'Saved') as folder:
            directory = Path(folder)
            since = time.time_ns()
            time.sleep(.003)
            path = directory / 'A-0000.png'
            Image.new('RGB', (1280, 800)).save(path)
            stat = path.stat()
            record = dict(path=str(path), start_ns=since, end_ns=since + 1, verified=True, size=stat.st_size,
                          created_ns=stat.st_ctime_ns, modified_ns=stat.st_mtime_ns,
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            manifest = directory / 'A-frames.jsonl'
            manifest.write_text(json.dumps(record) + '\n', encoding='utf-8')
            self.assertEqual(check(directory)['files'], 1)
            import inspect
            self.assertIn('pixels', inspect.signature(check).parameters, '像素审计必须复用原始PNG哈希验证')
            audited = check(directory, pixels=True)
            self.assertEqual(audited['series'][0]['pixel_frames'][0]['green'], 0)
            self.assertEqual(audited['series'][0]['pixel_frames'][0]['sha256'], record['sha256'])
            record['sha256'] = 'forged'
            manifest.write_text(json.dumps(record) + '\n', encoding='utf-8')
            with self.assertRaises(AssertionError): check(directory)

    def test_netguid_maps_different_dynamic_actor_names(self):
        parse = self.api_function('parse_netguids')
        server = 'SerializeNewActor END: Finished Serializing. Actor: Y01Monster_0, FullNetGUIDPath: [31]Y01Monster_0, Channel: 9, IsLoading: 0, IsDynamic: 1'
        client = 'SerializeNewActor END: Finished Serializing. Actor: Y01Monster_1, FullNetGUIDPath: [31]Y01Monster_1, Channel: 9, IsLoading: 1, IsDynamic: 1'
        self.assertEqual(parse(server)['Y01Monster_0'], parse(client)['Y01Monster_1'])
        self.assertEqual(parse('unrelated'), {})

    def test_posted_readonly_rejects_gameplay_mutation(self):
        check = self.api_function('validate_readonly_command')
        check('getall Y01GameState Snapshot | getalllocation Y01Monster')
        for bad in ('set Y01Monster Health 0', 'ce ServerPlaygroundAction 1', 'getall X Y | quit', 'showhud'):
            with self.assertRaises(AssertionError): check(bad)

    def test_parse_real_log_timestamp_and_vector(self):
        parse = self.api_function('parse_events')
        events = parse('[2026.09.28-08.10.20:123][ 22]LogTemp: Display: Y01Skill HIT Actor=A Target=M Damage=25 HP=35 Position=X=12.500 Y=-2.000 Z=90.000\n')
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['Position'], (12.5, -2., 90.))
        self.assertEqual(events[0]['HP'], 35)
        self.assertAlmostEqual(events[0]['t'] % 60, 20.123, places=5)
        self.assertEqual(parse('Y01Skill HIT Actor=A HP=35'), [], '没有时间戳不能混进时序证据')

    def test_projectile_requires_delayed_single_damage_and_feedback(self):
        check = self.api_function('validate_projectile')
        self.assertEqual(check(self.fixture(), 'A', 400)['damage'], 25)
        for changes in ('instant', 'twice', 'feedback-missing', 'speed', 'no-end'):
            events = copy.deepcopy(self.fixture())
            if changes == 'instant': events[2]['World'] = 1.
            if changes == 'twice': events.insert(3, dict(events[2]))
            if changes == 'feedback-missing': events = [e for e in events if not (e['kind'] == 'STATE' and e['Phase'] == 1)]
            if changes == 'speed': events[0]['Speed'] = 200
            if changes == 'no-end': events.pop()
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                check(events, 'A', 400)

    def test_area_allows_multiple_targets_but_never_double_hits(self):
        check = self.api_function('validate_area')
        events = self.area_fixture()
        self.assertEqual(check(events, 'A')['targets'], ['M', 'N'])
        for bad in (events[:-1], events + [dict(events[1])], [dict(e, World=2) if e['kind'] == 'HIT' else e for e in events]):
            with self.assertRaises(AssertionError): check(bad, 'A')

    def area_fixture(self):
        return [dict(kind='COMMIT', Actor='A', Mode=1, Radius=250, Height=120, Damage=25, t=1, World=1, StartedAt=1, EndsAt=1.35),
                dict(kind='HIT', Actor='A', Target='M', HP=35, Damage=25, t=1, World=1),
                dict(kind='HIT', Actor='A', Target='N', HP=35, Damage=25, t=1.1, World=1.1),
                dict(kind='EXPIRE', Actor='A', Mode=1, Phase=0, t=1.36, World=1.36, StartedAt=1, EndsAt=1.35),
                dict(kind='END', Actor='A', Mode=1, Phase=0, t=1.36, World=1.36, StartedAt=1, EndsAt=1.35)]

    def test_parse_expire_and_full_precision_step(self):
        text = ('[2026.09.28-08.10.20:123][22]LogTemp: Display: Y01Skill STEP Actor=A World=1.040000 PrevWorld=1.000000 SampleWorld=1.040000 PrevX=0 PrevY=0 PrevZ=90 Position=X=16.000000 Y=0.000000 Z=90.000000 Mode=2 Phase=0 Terminal=0\n'
                '[2026.09.28-08.10.20:500][25]LogTemp: Display: Y01Skill EXPIRE Actor=A World=1.360000 StartedAt=1.000000 EndsAt=1.350000 Mode=1 Phase=0\n')
        rows = self.api.parse_events(text)
        self.assertEqual([e['kind'] for e in rows], ['STEP', 'EXPIRE'])
        self.assertEqual(rows[0]['Position'], (16., 0., 90.))
        self.assertEqual(rows[0]['SampleWorld'], 1.04)

    def test_area_uses_world_not_stretched_wall_clock(self):
        events = [dict(e, t=e['t'] * 3) for e in self.area_fixture()]
        self.assertAlmostEqual(self.api.validate_area(events, 'A')['duration'], .36)

    def test_projectile_feedback_uses_world_not_wall_clock(self):
        events = [dict(e, t=e['t'] * 4) for e in self.fixture()]
        result = self.api.validate_projectile(events, 'A', 400)
        self.assertAlmostEqual(result['flight_seconds'], .6)
        self.assertAlmostEqual(result['feedback_seconds'], .11)

    def test_world_and_natural_expiry_are_required(self):
        for source, mode in ((self.area_fixture(), 1), (self.fixture(), 2)):
            for change in ('missing-world', 'missing-expire', 'early-expire', 'wrong-deadline', 'expire-after-end'):
                events = copy.deepcopy(source)
                if change == 'missing-world': events[0].pop('World')
                if change == 'missing-expire': events = [e for e in events if e['kind'] != 'EXPIRE']
                if change == 'early-expire': events[-2]['World'] = 1.1
                if change == 'wrong-deadline': events[-1]['EndsAt'] += .3
                if change == 'expire-after-end': events[-2], events[-1] = events[-1], events[-2]
                with self.subTest(mode=mode, change=change), self.assertRaises(AssertionError):
                    if mode == 1: self.api.validate_area(events, 'A')
                    else: self.api.validate_projectile(events, 'A', 400)

    def test_speed_measures_only_complete_steps(self):
        check = self.api_function('validate_speed')
        for speed in (400, 200):
            rows = [dict(kind='STEP', Actor='A', World=1.04 + i * .04, PrevWorld=1 + i * .04,
                         SampleWorld=1.04 + i * .04, PrevX=i * speed * .04, PrevY=0, PrevZ=90,
                         Position=((i + 1) * speed * .04, 0, 90), Mode=2, Phase=0, Terminal=0) for i in range(3)]
            rows.append(dict(rows[-1], Terminal=1, Phase=1, Position=(1234, 0, 90)))
            result = check(rows, 'A', speed)
            self.assertEqual(result['count'], 3)
            self.assertAlmostEqual(result['mean_speed'], speed)
            for bad in (rows[-1:], [dict(e, SampleWorld=e['PrevWorld']) for e in rows],
                        [dict(e, Position=(1000, 0, 90)) for e in rows]):
                with self.assertRaises(AssertionError): check(bad, 'A', speed)

    def training_log(self, phase=2, hp=100, inside=1, alive=2):
        return (f'[2026.09.28-08.10.01:000][1]LogTemp: Display: Y01Flow Rep Phase={phase}\n'
                f'[2026.09.28-08.10.01:010][1]LogTemp: Display: Y01Training OBSERVE Inside={inside} Alive={alive} HP={hp}\n')

    def test_training_death_requires_ended_and_both_rooms_clean(self):
        check = self.api_function('validate_training_main_continuity')
        own = self.training_log(3, 100, 0, 0)
        peer = self.training_log(3, 0, 0, 0)
        stale = '0) Y01GameState X.Snapshot = (Phase=Battle,Revision=3)\n'
        import inspect
        self.assertIn('peer_text', inspect.signature(check).parameters, '需根据双端真实HP判断既定全局死亡规则')
        self.assertEqual(check(own + stale, peer)['phase'], 3)
        for a, b in ((self.training_log(), peer), (own, self.training_log(3, 0, 1, 2)),
                     (own, self.training_log(3, 100, 0, 0)), (own, '')):
            with self.assertRaises(AssertionError): check(a, b)

    def test_training_ended_requires_no_remaining_attack(self):
        check = self.api_function('validate_ended_attacks')
        events = self.fixture()
        self.assertEqual(check(events)['ended'], 1)
        for bad in (events[:-1], events + [dict(events[2], t=11, World=2)]):
            with self.assertRaises(AssertionError): check(bad)

    def test_cancellation_uses_world_and_forbids_expire_damage(self):
        check = self.api_function('validate_cancellation')
        events = [self.area_fixture()[0], dict(self.area_fixture()[-1], World=1.2, t=4)]
        self.assertAlmostEqual(check(events, 'A')['seconds'], .2)
        for bad in (events + [self.area_fixture()[1]], events + [self.area_fixture()[-2]],
                    [events[0], dict(events[1], World=1.4)]):
            with self.assertRaises(AssertionError): check(bad, 'A')

    def test_skill_details_native_button_coordinates(self):
        method = self.api_function('Runner').__dict__.get('toggle_details')
        self.assertTrue(callable(method), '缺少原生右侧技能说明切换')
        calls = []
        runner = self.api.Runner.__new__(self.api.Runner)
        runner.click_point = lambda x, y: calls.append((x, y))
        runner.emit = lambda *args, **kwargs: None
        runner.toggle_details()
        self.assertEqual(calls, [(1092, 183)])

    def test_rejection_requires_no_commit_no_state_and_no_cd_change(self):
        check = self.api_function('validate_rejection')
        check([], [10, 20], [10, 20])
        for events, deadline in ((self.fixture(), [10, 20]), ([], [11, 20]),
                                 ([dict(kind='STATE', Actor='fake')], [10, 20])):
            with self.assertRaises(AssertionError): check(events, [10, 20], deadline)

    def test_health_requires_same_identity_and_exact_snapshot_damage(self):
        check = self.api_function('validate_health')
        hits = [dict(Target='M', HP=35, Damage=25)]
        check({'M': 60, 'N': 60}, {'M': 35, 'N': 60}, hits)
        for after in ({'M': 35, 'N': 35}, {'M': 60, 'N': 60}, {'Other': 35, 'N': 60}):
            with self.assertRaises(AssertionError): check({'M': 60, 'N': 60}, after, hits)

    def test_peer_requires_active_feedback_end_for_same_attack(self):
        check = self.api_function('validate_peer')
        states = [dict(kind='STATE', Actor='A', NetMode=3, Phase=0, t=10),
                  dict(kind='STATE', Actor='A', NetMode=3, Phase=1, t=10.6),
                  dict(kind='END', Actor='A', NetMode=3, Phase=1, t=10.72)]
        check(states, 'A', True)
        for bad in (states[:-1], states[1:], [dict(e, Actor='B') for e in states], [states[0], states[2]]):
            with self.assertRaises(AssertionError): check(bad, 'A', True)

    def test_continuous_frames_reject_gap_and_duplicate_or_missing_evidence(self):
        check = self.api_function('validate_frame_series')
        frames = [dict(start_ns=n, end_ns=n + 10_000_000, sha256=str(i), verified=True)
                  for i, n in enumerate(range(0, 400_000_001, 20_000_000))]
        check(frames, 100_000_000, 350_000_000)
        for bad in ([], frames[::10], [dict(f, verified=False) for f in frames], [dict(f, sha256='same') for f in frames]):
            with self.assertRaises(AssertionError): check(bad, 100_000_000, 350_000_000)

    def test_snapshot_selects_local_pawn_not_first_remote_actor(self):
        parse = self.api_function('parse_observation')
        text = '''0) Y01PlaygroundController /Game/Test:PersistentLevel.C.Pawn = /Script/Y01Tests.Y01PlaygroundCharacter'/Game/Test:PersistentLevel.Y01PlaygroundCharacter_1'
0) Y01PlaygroundCharacter /Game/Test:PersistentLevel.Y01PlaygroundCharacter_0 (1.000000, 2.000000, 90.000000)
1) Y01PlaygroundCharacter /Game/Test:PersistentLevel.Y01PlaygroundCharacter_1 (500.000000, 600.000000, 90.000000)
0) Y01GameState /Game/Test:PersistentLevel.G.Snapshot = (Phase=Waiting)
0) Y01PlaygroundController /Game/Test:PersistentLevel.C.Observer = (bReady=True,Slots=((Row="BasicAttack",Deadline=0.000000),(Row="HeavyAttack",Deadline=2.000000)),Training=(bInside=False))
0) Y01Monster /Game/Test:PersistentLevel.Y01Monster_0 (650.000000, 600.000000, 90.000000)
0) Y01AttributeSet /Game/Test:PersistentLevel.Y01Monster_0.Attributes.Health = (CurrentValue=60.000000)
'''
        state = parse(text)
        self.assertEqual(state['position'], (500, 600, 90))
        self.assertEqual(state['monsters']['Y01Monster_0'], (650, 600, 90))
        clocked = parse(text.replace('bReady=True,Slots=', 'bReady=True,ServerSampleTime=1.25,PreparationDeadline=3,Slots='))
        self.assertEqual(clocked.get('server_sample_time'), 1.25)
        self.assertEqual(clocked.get('remaining'), [0, .75])
        with self.assertRaises(AssertionError): parse(text.replace('.Pawn = ', '.Missing = '))


class TrainingEntryRegression(unittest.TestCase):
    """训练入口纯回归：只执行审计/参数前缀，所有进程与文件写入均替身隔离。"""
    script = SCRIPT.with_name('Test-Y01TrainingVisible.py')
    baseline = '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline'
    compare = '/Game/Developers/Y01PlaygroundTests/R33TrainingCompare'

    def setUp(self):
        self.source = self.script.read_text(encoding='utf-8-sig')
        self.tree = ast.parse(self.source)
        definitions = ast.Module(body=[node for node in self.tree.body if isinstance(
            node, (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.ClassDef))], type_ignores=[])
        self.scope = {'__file__': str(self.script)}
        exec(compile(definitions, str(self.script), 'exec'), self.scope)

    def api(self, name):
        value = self.scope.get(name)
        self.assertTrue(callable(value), '训练验收缺少前置保护：' + name)
        return value

    def fixture(self, selection='u2baseline'):
        root = self.compare if selection == 'compare' else self.baseline
        profile = root + '/DA_PlaygroundProfile.DA_PlaygroundProfile'
        table = '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Data/DT_Character.DT_Character'
        growth = (f'LogY01Playground: Display: GROWTH_AUDIT Stage=BEFORE Profile={profile} Character=Default '
                  f'Table={table} Row=Default Enabled=0 Seconds=0 Kills=0 Gold=0 HP=200 Attack=20 MoveSpeed=500 TestOnly=1\n')
        interval, count = (1, 1) if selection == 'compare' else (3, 2)
        training = (f'LogY01Playground: Display: TRAINING_BEFORE Profile={profile} Enabled=1 Table={root}/Data/DT_TrainingStage.DT_TrainingStage\n'
                    f'Row=Default Stage=1 StartSeconds=0 SpawnInterval={interval} MaxAlive=6 Reward=Default\n'
                    f'Spawns[0] Monster=Default Count={count} DelaySeconds=0\n'
                    'LogY01Playground: Display: TRAINING_REWARD Row=Default Reward=Default Gold=10 XP=5 Items=0\n')
        return growth, training

    def prefix(self, argv=(), growth=None, training=None, failure=None):
        selection = 'compare' if '--compare' in argv or 'compare' in argv else 'u2baseline'
        good_growth, good_training = self.fixture(selection)
        growth = good_growth if growth is None else growth
        training = good_training if training is None else training
        def audit(command, **kwargs):
            if failure is not None:
                if isinstance(failure, Exception):
                    raise failure
                return SimpleNamespace(returncode=failure, stdout='', stderr='审计失败')
            text = growth if '-GrowthAudit' in command else training if '-TrainingAudit' in command else '已只读校验地图'
            return SimpleNamespace(returncode=0, stdout=text, stderr='')
        # 停在第一个WinDLL之前；测试失败也不可能落到原桌面主体。
        boundary = next(i for i, node in enumerate(self.tree.body) if isinstance(node, ast.Assign)
                        and any(isinstance(target, ast.Name) and target.id == 'user' for target in node.targets))
        prefix = ast.Module(body=self.tree.body[:boundary], type_ignores=[])
        scope, error, status = {'__file__': str(self.script)}, io.StringIO(), 0
        with patch.object(sys, 'argv', [str(self.script), *argv]), patch.object(subprocess, 'run', side_effect=audit) as run, \
                patch.object(subprocess, 'Popen') as launch, patch.object(Path, 'mkdir') as mkdir, \
                patch.object(Path, 'open', return_value=io.StringIO()) as opened, redirect_stderr(error):
            try:
                exec(compile(prefix, str(self.script), 'exec'), scope)
            except SystemExit as exc:
                status = exc.code
        launch.assert_not_called()
        return SimpleNamespace(scope=scope, status=status, error=error.getvalue(), run=run, mkdir=mkdir, opened=opened)

    def test_default_entry_is_explicit_u2baseline_before_desktop(self):
        result = self.prefix()
        self.assertEqual(result.status, 0, result.error)
        self.assertEqual(result.scope.get('map_root'), self.baseline, '旧默认main会在Gold==0处误报')
        self.assertEqual(result.scope['args'].map, 'u2baseline')
        self.assertFalse(result.scope['args'].compare)
        self.assertEqual(result.run.call_count, 3, '必须先只读校验地图、成长和训练配置')

    def test_compare_keeps_old_one_second_one_monster_map(self):
        for argv in (('--compare',), ('--map', 'compare')):
            with self.subTest(argv=argv):
                result = self.prefix(argv)
                self.assertEqual(result.status, 0, result.error)
                self.assertEqual(result.scope.get('map_root'), self.compare)
                self.assertTrue(result.scope['args'].compare)

    def test_main_and_ambiguous_selection_reject_before_audit_or_output(self):
        resolve = self.api('resolve_training_map')
        self.assertEqual(resolve('u2baseline'), self.baseline + '/Maps/L_Y01Playground')
        self.assertEqual(resolve('compare'), self.compare + '/Maps/L_Y01Playground')
        for value in (None, 'main', 'legacy', 'growthcompare', '/Game/Y01Playground/Maps/L_Y01Playground'):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'u2baseline'):
                resolve(value)
        for argv in (('--map', 'main'), ('--map', 'u2baseline', '--compare')):
            result = self.prefix(argv)
            self.assertNotEqual(result.status, 0)
            result.run.assert_not_called()
            result.mkdir.assert_not_called()
            result.opened.assert_not_called()

    def test_audit_commands_are_readonly_and_target_exact_selected_root(self):
        for selection, argv in (('u2baseline', ()), ('compare', ('--compare',))):
            result = self.prefix(argv)
            commands = [call.args[0] for call in result.run.call_args_list]
            self.assertEqual(len(commands), 3, '不能跳过启动前审计')
            root = self.compare if selection == 'compare' else self.baseline
            for call in result.run.call_args_list:
                command = call.args[0]
                self.assertIn('-Root=' + root, command)
                for required in ('-run=Y01CreatePlayground', '-ValidateOnly', '-nullrhi', '-unattended', '-stdout', '-UTF8Output'):
                    self.assertIn(required, command)
                self.assertFalse(any(word in ' '.join(command) for word in ('Upgrade', 'CopyRoot', 'Repair', '-game', '-server')))
                self.assertTrue(call.kwargs['capture_output'])
                self.assertEqual(call.kwargs['encoding'], 'utf-8')
                self.assertGreater(call.kwargs['timeout'], 0)
            self.assertIn('-WithMap', commands[0])
            self.assertIn('-GrowthAudit', commands[1])
            self.assertIn('-TrainingAudit', commands[2])

    def test_closed_actual_character_reference_and_unused_growth_row_are_allowed(self):
        check = self.api('validate_training_audits')
        for selection in ('u2baseline', 'compare'):
            growth, training = self.fixture(selection)
            check(selection, growth, training)
            extra = growth.replace('Row=Default', 'Row=Unused').replace('Enabled=0', 'Enabled=1')
            check(selection, growth + extra, training)
            check(selection, growth.replace('R33U2Compare/Data/', 'R33U2Legacy/Data/'), training)
            check(selection, growth.replace('Character=Default', 'Character=Chosen').replace('Row=Default', 'Row=Chosen'), training)

    def test_growth_enabled_rejects_before_output_and_reports_actual_reference(self):
        growth, training = self.fixture('compare')
        growth = growth.replace('Enabled=0', 'Enabled=1').replace('Gold=0', 'Gold=1').replace(
            '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Data/', '/Game/Y01Playground/Data/')
        result = self.prefix(('--compare',), growth, training)
        self.assertNotEqual(result.status, 0, '当前共享main成长的旧对照必须前置拒绝')
        for text in ('不兼容', self.compare, '/Game/Y01Playground/Data/DT_Character', '精准隔离', 'u2baseline'):
            self.assertIn(text, result.error)
        result.mkdir.assert_not_called()
        result.opened.assert_not_called()

    def test_growth_audit_must_identify_exact_profile_and_selected_row(self):
        check = self.api('validate_training_audits')
        growth, training = self.fixture()
        for bad in ('', growth.replace(self.baseline, self.compare), growth.replace('Row=Default', 'Row=Other'),
                    growth + growth, growth.replace('Enabled=0', 'Enabled=1'), growth.replace('Enabled=0 ', ''),
                    growth.replace('HP=200', 'HP=nan'), growth.replace('Table=/Game/', 'Table=/Unknown/')):
            with self.subTest(growth=bad), self.assertRaises(ValueError):
                check('u2baseline', bad, training)

    def test_training_audit_rejects_changed_schedule_capacity_or_reward_shape(self):
        check = self.api('validate_training_audits')
        growth, training = self.fixture()
        changes = ((self.baseline, self.compare), ('Enabled=1', 'Enabled=0'), ('Stage=1', 'Stage=2'),
                   ('StartSeconds=0', 'StartSeconds=2'), ('SpawnInterval=3', 'SpawnInterval=1'),
                   ('MaxAlive=6', 'MaxAlive=5'), ('Count=2', 'Count=1'), ('DelaySeconds=0', 'DelaySeconds=1'),
                   ('Items=0', 'Items=1'), ('Monster=Default', 'Monster=None'), ('XP=5', 'XP=-1'),
                   ('TRAINING_REWARD Row=Default', 'TRAINING_REWARD Row=Other'))
        bad_logs = ['', training + 'Row=Later Stage=2 StartSeconds=5 SpawnInterval=1 MaxAlive=6 Reward=Default\n',
                    training + 'Spawns[1] Monster=Default Count=1 DelaySeconds=0\n']
        bad_logs += [training.replace(old, new) for old, new in changes]
        for bad in bad_logs:
            with self.subTest(training=bad), self.assertRaises(ValueError):
                check('u2baseline', growth, bad)

    def test_audit_failure_missing_output_and_timeout_do_not_reach_desktop(self):
        for kwargs in ({'failure': 1}, {'failure': subprocess.TimeoutExpired('只读审计', 120)},
                       {'failure': OSError('审计程序不可用')}, {'growth': ''}, {'training': ''}):
            with self.subTest(kwargs=kwargs):
                result = self.prefix(**kwargs)
                self.assertNotEqual(result.status, 0)
                result.mkdir.assert_not_called()
                result.opened.assert_not_called()

    def test_original_gold_xp_kills_zero_assertion_still_executes(self):
        assertion = "assert state(role)['Gold'] == 0 and state(role)['XP'] == 0 and state(role)['Kills'] == 0"
        expected = ast.dump(ast.parse(assertion).body[0])
        nodes = [node for node in ast.walk(self.tree) if isinstance(node, ast.Assert) and ast.dump(node) == expected]
        self.assertEqual(len(nodes), 1, '不能删除、放宽或用成长预期替换原零奖励断言')
        code = compile(ast.Module(body=nodes, type_ignores=[]), str(self.script), 'exec')
        state = dict(Gold=0, XP=0, Kills=0)
        scope = {'role': 'A', 'state': lambda role: state}
        exec(code, scope)
        for key in state:
            state[key] = 1
            with self.subTest(key=key), self.assertRaises(AssertionError):
                exec(code, scope)
            state[key] = 0

    def test_original_cleanup_spawn_and_two_room_assertions_are_preserved(self):
        assertions = {ast.unparse(node) for node in ast.walk(self.tree) if isinstance(node, ast.Assert)}
        for statement in ("assert state('A')['Alive'] == batch and state('B')['Inside'] == 0",
                          "assert state('B')['Alive'] == batch and state('A')['Room'] != state('B')['Room']",
                          "assert state('A')['Alive'] == 0 and state('B')['Inside'] == 1",
                          "assert state('A')['Alive'] == batch and state('A')['Stage'] == 1",
                          "assert text('Server').count('Y01Training UI_RPC') == 4",
                          "assert text('Server').count('Accepted=1') == 4",
                          'assert len(players) == 2',
                          'assert entries[0][1] == batch and entries[1][1] == batch * 2',
                          'assert abs(elapsed - expected_interval) < 0.5, (player, elapsed)'):
            self.assertIn(statement, assertions)
        self.assertIn("text('A').count('Y01Training UI_CLICK') == 3 and text('B').count('Y01Training UI_CLICK') == 1", self.source)
        self.assertIn("state('B').get('Alive', 0) >= batch * 3", self.source)
        self.assertIn('reversed(list(owned.items()))', self.source)
        self.assertIn('process.terminate()', self.source)
        self.assertIn("'OWNED_EXTERNAL_STOP=', forced", self.source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--result-log', type=Path)
    args = parser.parse_args()
    stream = io.StringIO()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
                               for case in (AcceptanceRegression, TrainingEntryRegression))
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
    report = stream.getvalue()
    print(report)
    if args.result_log:
        with args.result_log.open('x', encoding='utf-8') as handle:
            handle.write(report)
    sys.exit(not result.wasSuccessful())
