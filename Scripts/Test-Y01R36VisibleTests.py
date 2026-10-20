"""R36可见运行器离线契约；不启动UE、不操作桌面。"""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace
import uuid
import json

spec = importlib.util.spec_from_file_location('r36_visible', Path(__file__).with_name('Test-Y01R36Visible.py'))
r36 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r36)


class R36VisibleTests(unittest.TestCase):
    def test_default_map(self):
        self.assertEqual(r36.map_path(''), '/Game/Developers/Y01PlaygroundTests/R36/Maps/L_Y01R36')

    def test_isolated_run(self):
        self.assertEqual(r36.map_path('Network_01'), '/Game/Developers/Y01PlaygroundTests/R36/Runs/Network_01/Maps/L_Y01R36')

    def test_reject_foreign_path(self):
        for value in ('../main', '/Game/Y01Playground', 'a/b', 'a b'):
            with self.subTest(value=value), self.assertRaises(ValueError): r36.map_path(value)

    def test_commands_use_real_ds_and_two_d3d12(self):
        commands = r36.commands('R36-S1-UI-Python-Test', 18436, '', 10000)
        self.assertEqual(set(commands), {'Server', 'A', 'B'})
        self.assertIn('-server', commands['Server'])
        self.assertIn('-R36Fixture=controlled', commands['Server'])
        self.assertIn('-R36Items=10000', commands['Server'])
        for role in ('A', 'B'):
            self.assertIn('-d3d12', commands[role])
            self.assertNotIn('-nullrhi', commands[role])
            self.assertIn('127.0.0.1:18436', commands[role])
            self.assertFalse(any('R36Fixture=' in arg for arg in commands[role]))

    def test_reject_bad_launch(self):
        for args in (('unsafe', 18436, '', 17), ('R36-S1-UI-X', 80, '', 17),
                     ('R36-S1-UI-X', 18436, '', 0), ('R36-S1-UI-X', 18436, '', True)):
            with self.subTest(args=args), self.assertRaises(ValueError): r36.commands(*args)

    def test_no_capacity_truncation(self):
        for count in (1000, 10000, 10001):
            self.assertIn(f'-R36Items={count}', r36.commands('R36-S1-UI-Test', 18436, '', count).get('Server', []))

    def test_trace_complete_lines_only(self):
        self.assertEqual(r36.parse_trace('log Y01R36_TRACE {"kind":"VIEW","items":17}\nY01R36_TRACE {"kind":"VIEW"}'),
                         [{'kind': 'VIEW', 'items': 17}])

    def test_trace_corruption_rejected(self):
        with self.assertRaises(ValueError): r36.parse_trace('Y01R36_TRACE {bad}\n')

    def test_drag_has_real_motion(self):
        points = r36.drag_points((10, 20), (300, 200))
        self.assertGreaterEqual(len(points), 8)
        self.assertEqual(points[0], (10, 20)); self.assertEqual(points[-1], (300, 200))
        self.assertGreater(len(set(points)), 7)

    def test_drag_rejects_outside_viewport(self):
        for source, target in (((-1, 20), (100, 100)), ((10, 20), (1280, 300))):
            with self.assertRaises(ValueError): r36.drag_points(source, target)

    def test_same_commit_and_private_scope(self):
        a = dict(match='M', join=1, connection='C1', revision=2, digest='abc', items=17)
        b = dict(match='M', join=2, connection='C2', revision=2, digest='def', items=17)
        self.assertTrue(r36.validate_views([a, b], [a], [b], 17))
        with self.assertRaises(AssertionError): r36.validate_views([a, b], [a, b], [b], 17)
        with self.assertRaises(AssertionError): r36.validate_views([a, b], [dict(a, digest='wrong')], [b], 17)
        with self.assertRaises(AssertionError): r36.validate_views([a, b], [a], [a], 17)

    def test_process_identity_requires_creation_time(self):
        self.assertTrue(r36.same_process({'pid': 12, 'created': 34}, {'pid': 12, 'created': 34}))
        self.assertFalse(r36.same_process({'pid': 12, 'created': 34}, {'pid': 12, 'created': 35}))
        self.assertFalse(r36.same_process({'pid': 12}, {'pid': 12}))


class R36TracePollingTests(unittest.TestCase):
    """轮询仅解析新增完整行；原始历史与损坏诊断不能丢失。"""
    def setUp(self):
        import io
        class Log:
            def __init__(self):
                self.data = b''; self.present = True; self.inode = 1; self.read_bytes = 0
            def exists(self): return self.present
            def stat(self):
                if not self.present: raise FileNotFoundError('尚未创建或日志消失')
                return SimpleNamespace(st_size=len(self.data), st_ino=self.inode, st_dev=1)
            def read_text(self, **kwargs):
                self.read_bytes += len(self.data)
                return self.data.decode('utf-8', errors=kwargs.get('errors', 'strict'))
            def open(self, mode):
                self.stat()
                if mode != 'rb': raise AssertionError('增量游标必须按字节定位')
                owner = self
                class Stream(io.BytesIO):
                    def read(self, size=-1):
                        data = super().read(size); owner.read_bytes += len(data); return data
                return Stream(self.data)
        self.logs = {role: Log() for role in ('Server', 'A', 'B')}
        logs = self.logs
        class Root:
            def __truediv__(self, part):
                if part == 'Saved': return self
                return logs[part.rsplit('-', 1)[1][:-4]]
        self.patch = patch.object(r36, 'ROOT', Root()); self.patch.start(); self.addCleanup(self.patch.stop)
        self.runner = r36.Runner(); self.runner.args = SimpleNamespace(label='R36-S1-UI-Poll-Test')

    @staticmethod
    def line(row):
        return ('log Y01R36_TRACE ' + json.dumps(row, ensure_ascii=False) + '\r\n').encode('utf-8')

    def test_unchanged_poll_never_rereads_or_reparses_history(self):
        log = self.logs['A']; log.data = self.line(dict(kind='UI', revision=2))
        with patch.object(r36.json, 'loads', wraps=json.loads) as decode:
            self.runner.records(); self.runner.records(); self.runner.records()
        self.assertEqual(decode.call_count, 1, '相同完整行不能每次轮询重新解析')
        self.assertEqual(log.read_bytes, len(log.data), '无新增字节不能重读历史')

    def test_appends_only_new_bytes_and_preserves_all_record_objects(self):
        log = self.logs['A']; first = dict(kind='VIEW', connection='owner', revision=1)
        log.data = self.line(first); old = self.runner.records()['A'][0]
        following = [dict(kind='SCOPE', privateViews=2), dict(kind='UI', revision=2)]
        log.data += b'ordinary engine log\r\n' + b''.join(self.line(row) for row in following)
        rows = self.runner.records()['A']
        self.assertEqual(rows, [first] + following, '不能压缩掉历史Owner泄漏或中间UI证据')
        self.assertIs(rows[0], old, '已解析记录应复用，不重新构造整个历史')
        self.assertEqual(log.read_bytes, len(log.data))

    def test_split_utf8_and_crlf_wait_for_complete_record(self):
        log = self.logs['A']; row = dict(kind='UI', text='技能书')
        full = self.line(row); split = full.index('技'.encode('utf-8')) + 1
        log.data = full[:split]; self.assertEqual(self.runner.records()['A'], [])
        log.data = full[:-2]; self.assertEqual(self.runner.records()['A'], [])
        log.data = full; self.assertEqual(self.runner.records()['A'], [row])

    def test_initial_missing_log_can_appear_later(self):
        log = self.logs['B']; log.present = False
        self.assertEqual(self.runner.records()['B'], [])
        log.present = True; log.data = self.line(dict(kind='SCOPE', ownerLinked=True))
        self.assertEqual(len(self.runner.records()['B']), 1)

    def test_disappearing_log_must_not_silently_erase_evidence(self):
        log = self.logs['A']; log.data = self.line(dict(kind='SCOPE', privateViews=2))
        self.runner.records(); log.present = False
        with self.assertRaisesRegex(RuntimeError, '日志'): self.runner.records()

    def test_truncated_or_replaced_log_rejected(self):
        for change in ('truncate', 'replace'):
            with self.subTest(change=change):
                self.runner = r36.Runner(); self.runner.args = SimpleNamespace(label='R36-S1-UI-Poll-Test')
                log = self.logs['A']; log.data = self.line(dict(kind='VIEW', revision=2)); self.runner.records()
                if change == 'truncate': log.data = b''
                else: log.inode += 1
                with self.assertRaisesRegex(RuntimeError, '日志'): self.runner.records()

    def test_malformed_complete_record_still_fails_on_every_poll(self):
        log = self.logs['Server']; log.data = self.line(dict(kind='SCOPE'))
        self.runner.records(); log.data += b'Y01R36_TRACE {bad}\n'
        for _ in range(2):
            with self.assertRaises(ValueError): self.runner.records()

    def test_ten_thousand_records_are_not_capped_or_deduplicated(self):
        log = self.logs['A']; rows = [dict(kind='UI', revision=2, frame=i) for i in range(10000)]
        log.data = b''.join(self.line(row) for row in rows)
        self.assertEqual(self.runner.records()['A'], rows)
        log.data += self.line(rows[0])
        self.assertEqual(self.runner.records()['A'], rows + [rows[0]])


class R36LaunchSafetyTests(unittest.TestCase):
    def test_shared_ue_blocks_launch(self):
        with self.assertRaises(RuntimeError):
            r36.require_idle([{'Name': 'UnrealEditor.exe', 'ProcessId': 99}], set())

    def test_only_own_pid_allowed(self):
        self.assertTrue(r36.require_idle([{'Name': 'UnrealEditor.exe', 'ProcessId': 99}], {99}))
        self.assertTrue(r36.require_idle([{'Name': 'dotnet.exe', 'ProcessId': 12,
                                           'CommandLine': 'MSBuild.dll /nodemode:1 /nodeReuse:true'}], set()))

    def test_active_build_blocks_launch(self):
        with self.assertRaises(RuntimeError):
            r36.require_idle([{'Name': 'dotnet.exe', 'ProcessId': 12, 'CommandLine': 'UnrealBuildTool.dll Y01Editor'}], set())

    def test_foreign_process_never_closed(self):
        process = Mock(pid=99); process.poll.return_value = None
        with self.assertRaises(RuntimeError): r36.close_owned(process, {'pid': 99, 'created': 7}, 8)
        process.send_signal.assert_not_called(); process.terminate.assert_not_called(); process.kill.assert_not_called()

    def test_owned_process_normal_signal_only(self):
        process = Mock(pid=99); process.poll.return_value = None; process.wait.return_value = 0
        request = Mock()
        self.assertEqual(r36.close_owned(process, {'pid': 99, 'created': 7}, 7, request), 0)
        request.assert_called_once()
        process.send_signal.assert_not_called(); process.terminate.assert_not_called(); process.kill.assert_not_called()

    def test_exited_process_not_signalled(self):
        process = Mock(pid=99); process.poll.return_value = 0
        self.assertEqual(r36.close_owned(process, {'pid': 99, 'created': 7}, 7), 0)
        process.send_signal.assert_not_called()


class R36DriverTests(unittest.TestCase):
    def test_cli_s1_boundaries(self):
        args = r36.parse_args(['--label', 'R36-S1-UI-Cli', '--items', '10000', '--profile', 'compare', '--scenario', 'controlled'])
        self.assertEqual(getattr(args, 'items', None), 10000)
        self.assertEqual(getattr(args, 'profile', None), 'compare')
        with self.assertRaises(SystemExit): r36.parse_args(['--label', 'R36-S1-UI-Cli', '--scenario', 'natural'])

    def test_launch_checks_each_process_and_records_partial_ownership(self):
        owned, events, checks = {}, [], []
        def start(args):
            if args[0] == 'B': raise RuntimeError('模拟操作系统拒绝第三进程')
            return Mock(pid=len(owned) + 10)
        def scan(): checks.append(True); return []
        with self.assertRaises(RuntimeError):
            r36.launch_all({role: [role] for role in ('Server', 'A', 'B')}, owned, scan, start,
                           lambda p: 123, lambda role: None, lambda role, identity: events.append(role))
        self.assertEqual(set(owned), {'Server', 'A'})
        self.assertEqual(events, ['Server', 'A']); self.assertEqual(len(checks), 3)

    def test_no_launch_on_shared_occupancy(self):
        start = Mock()
        with self.assertRaises(RuntimeError):
            r36.launch_all({'Server': ['Server']}, {}, lambda: [{'Name': 'UnrealEditor.exe', 'ProcessId': 7}],
                           start, lambda p: 123, lambda role: None, lambda *args: None)
        start.assert_not_called()

    def test_drag_calls_mouse_motion_between_down_up(self):
        events = []
        r36.perform_drag((10, 20), (300, 200), lambda p: events.append(('move', p)),
                         lambda: events.append('down'), lambda: events.append('up'), lambda: None, lambda: None)
        self.assertIn('down', events); self.assertIn('up', events)
        self.assertEqual(events[0], ('move', (10, 20))); self.assertEqual(events[-1], 'up')
        self.assertGreaterEqual(len(events), 18)

    def test_focus_loss_stops_drag_before_input(self):
        move, down, up = Mock(), Mock(), Mock()
        def guard(): raise RuntimeError('失焦')
        with self.assertRaises(RuntimeError): r36.perform_drag((10, 20), (300, 200), move, down, up, guard, lambda: None)
        move.assert_not_called(); down.assert_not_called(); up.assert_not_called()


class R36SessionTests(unittest.TestCase):
    def test_profile_reaches_server_only(self):
        plan = r36.commands('R36-S1-UI-Profile', 18436, '', 17, 'compare')
        self.assertIn('-R36Profile=compare', plan['Server'])
        self.assertFalse(any('R36Profile=' in arg for arg in plan['A']))
        with self.assertRaises(ValueError): r36.commands('R36-S1-UI-Profile', 18436, '', 17, 'other')

    def test_latest_views_preserves_all_private_owners(self):
        rows = [dict(kind='VIEW', connection='a', revision=1), dict(kind='SCOPE'),
                dict(kind='VIEW', connection='b', revision=3), dict(kind='VIEW', connection='a', revision=2)]
        self.assertEqual({r['connection']: r['revision'] for r in r36.latest_views(rows)}, {'a': 2, 'b': 3})

    def test_progress_reports_observed_windows_not_fabricated_count(self):
        rows = [dict(kind='WIRE', transfer='t', connection='c', window=w, time=t, bytes=20000, end=e)
                for w, t, e in ((1, 10, 4096), (1, 10.5, 4096), (4, 12, 16384))]
        p = r36.wire_progress(rows).get('t', {})
        self.assertEqual(p.get('observed_windows'), 2)
        self.assertEqual(p.get('last_window'), 4)
        self.assertEqual(p.get('end'), 16384)
        self.assertEqual(p.get('seconds'), 2)
        self.assertEqual(p.get('bytes'), 20000)

    def test_session_success_verifies_before_close_and_result(self):
        events = []
        runner = Mock()
        for name in ('launch', 'verify', 'close'):
            getattr(runner, name).side_effect = lambda n=name: events.append(n)
        runner.finish.side_effect = lambda code: events.append(('finish', code))
        self.assertEqual(r36.run_session(runner), 0)
        self.assertEqual(events, ['launch', 'verify', 'close', ('finish', 0)])

    def test_session_partial_start_failure_still_closes_and_reports(self):
        runner = Mock(); runner.launch.side_effect = RuntimeError('第三进程启动失败')
        self.assertEqual(r36.run_session(runner), 1)
        runner.verify.assert_not_called(); runner.close.assert_called_once(); runner.finish.assert_called_once_with(1)
        runner.emit.assert_called()

    def test_normal_close_failure_cannot_report_success(self):
        runner = Mock(); runner.close.side_effect = RuntimeError('正常退出超时；不能强杀')
        self.assertEqual(r36.run_session(runner), 1)
        runner.finish.assert_called_once_with(1)


class R36AdapterTests(unittest.TestCase):
    def test_start_has_own_console_process_group_no_shell(self):
        with patch.object(r36.subprocess, 'Popen') as popen:
            r36.start_process(['unreal', 'project'])
            popen.assert_called_once()
            self.assertEqual(popen.call_args.args[0], ['unreal', 'project'])
            self.assertEqual(popen.call_args.kwargs.get('creationflags'), r36.subprocess.CREATE_NEW_PROCESS_GROUP)
            self.assertFalse(popen.call_args.kwargs.get('shell', False))

    def test_scan_uses_real_process_inventory(self):
        with patch.object(r36.subprocess, 'run', return_value=Mock(stdout='[{"Name":"UnrealEditor.exe","ProcessId":7}]')) as run:
            self.assertEqual(r36.scan_processes(), [{'Name': 'UnrealEditor.exe', 'ProcessId': 7}])
            self.assertIn('Get-CimInstance Win32_Process', ' '.join(run.call_args.args[0]))

    def test_scan_respects_constrained_powershell_encoding(self):
        with patch.object(r36.subprocess, 'run', return_value=Mock(stdout='[]')) as run:
            r36.scan_processes()
            self.assertNotIn('[Console]::', ' '.join(run.call_args.args[0]))
            self.assertNotIn('encoding', run.call_args.kwargs)
            self.assertFalse(run.call_args.kwargs.get('text', False))

    def test_hash_contract_requires_exact_seventy_packages(self):
        before = {str(i): str(i) for i in range(70)}
        self.assertTrue(r36.validate_packages(before, dict(before)))
        for after in ({}, dict(before, extra='new'), dict(before, **{'0': 'changed'})):
            with self.assertRaises(AssertionError): r36.validate_packages(before, after)

    def test_network_requires_owner_pawn_and_other_public_state(self):
        a = dict(kind='VIEW', match='m', join=1, connection='a', revision=1, digest='a', items=17)
        b = dict(a, join=2, connection='b', digest='b')
        scope = dict(kind='SCOPE', authority=False, states=2, privateViews=1, ownerLinked=True, pawn='Pawn')
        records = {'Server': [a, b], 'A': [a, scope], 'B': [b, scope]}
        self.assertTrue(r36.network_ready(records, 17))
        records['A'] = [a, dict(scope, ownerLinked=False)]
        self.assertFalse(r36.network_ready(records, 17))
        records['A'] = [a, b, dict(scope, privateViews=2)]
        with self.assertRaises(AssertionError): r36.network_ready(records, 17)


class R36RunnerTests(unittest.TestCase):
    def runner(self):
        runner = r36.Runner()
        runner.args = SimpleNamespace(label='R36-S1-UI-Offline', port=18436, run='', items=17,
                                      profile='content', timeout=2, observe_only=True, validate_assets=False)
        runner.owned, runner.identities, runner.exits = {}, {}, []
        runner.emit = Mock(); runner.ready = Mock(); runner.created = Mock(return_value=7)
        return runner

    def test_verify_expands_both_panels_before_existing_actions(self):
        runner = self.runner()
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.snapshot = Mock(); runner.expand_panels = Mock()
        runner.verify()
        self.assertEqual(runner.expand_panels.call_args_list, [unittest.mock.call('A'), unittest.mock.call('B')])

    def test_expand_requires_real_click_and_new_layout(self):
        runner = self.runner()
        method = getattr(runner, 'expand_panels', None)
        self.assertTrue(callable(method), '需要真实展开入口，不能靠默认可见或坐标猜测')
        before = dict(kind='UI', time=1, panelsExpanded=False, TogglePanels=dict(laidOut=True, enabled=True, x=1200, y=30))
        stale = dict(before, panelsExpanded=True, bagGeometry=dict(laidOut=True))
        hidden = dict(stale, time=2, bagGeometry=dict(laidOut=False))
        after = dict(stale, time=3)
        runner.records = Mock(side_effect=[{'A': [before]}, {'A': [stale]}, {'A': [hidden]}, {'A': [after]}])
        runner.pointer = Mock()
        def wait(predicate, description):
            for _ in range(4):
                value = predicate()
                if value: return value
            self.fail(description)
        runner.wait = wait
        method('A')
        runner.pointer.assert_called_once_with('A', before['TogglePanels'])
        self.assertEqual(runner.records.call_count, 4)

    def test_expanded_panel_does_not_click_collapse(self):
        runner = self.runner()
        method = getattr(runner, 'expand_panels', None)
        self.assertTrue(callable(method), '需要幂等展开入口')
        runner.records = Mock(return_value={'A': [dict(kind='UI', time=1, panelsExpanded=True, bagGeometry=dict(laidOut=True))]})
        runner.wait = lambda predicate, description: predicate()
        runner.pointer = Mock()
        method('A')
        runner.pointer.assert_not_called()

    def test_real_runner_launch_uses_guarded_plan(self):
        runner = self.runner()
        with patch.object(r36, 'scan_processes', return_value=[]), patch.object(r36, 'start_process', side_effect=[Mock(pid=p) for p in (1, 2, 3)]):
            runner.launch()
        self.assertEqual(set(runner.owned), {'Server', 'A', 'B'})
        self.assertEqual(runner.ready.call_count, 3)
        self.assertEqual(runner.identities['A'], {'pid': 2, 'created': 7})

    def test_real_runner_closes_all_owned_even_if_one_exit_fails(self):
        runner = self.runner()
        runner.owned = {r: Mock(pid=i) for i, r in enumerate(('Server', 'A', 'B'), 1)}
        runner.identities = {r: dict(pid=p.pid, created=7) for r, p in runner.owned.items()}
        with patch.object(r36, 'close_owned', side_effect=[RuntimeError('无法正常关闭'), 0, 0]) as close:
            with self.assertRaises(RuntimeError): runner.close()
        self.assertEqual(close.call_count, 3)
        self.assertEqual(len(runner.exits), 2)

    def test_verify_rejects_unimplemented_visible_path(self):
        runner = self.runner(); runner.args.observe_only = False
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.snapshot = Mock(); runner.locate = Mock(); runner.logs = {}
        with self.assertRaises((RuntimeError, AssertionError)): runner.verify()

    def test_finish_persists_exit_and_remaining_ownership(self):
        runner = self.runner(); alive = Mock(pid=9); alive.poll.return_value = None; runner.owned = {'A': alive}
        runner.output = r36.ROOT / 'Saved' / ('R36-S1-UI-PythonFixture-' + uuid.uuid4().hex)
        runner.output.mkdir(); runner.started = 'before'
        runner.finish(1)
        target = runner.output / 'result.json'
        self.assertTrue(target.exists())
        result = json.loads(target.read_text(encoding='utf-8'))
        self.assertEqual(result['owned_remaining'], [9]); self.assertEqual(result['ExitCode'], 1)
        self.assertTrue(result['Finished'])


class R36WindowCloseTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    def test_client_close_uses_owned_window_not_console_signal(self):
        runner = self.runner(); p = Mock(pid=2); p.poll.return_value = None; p.wait.return_value = 0
        runner.owned = {'A': p}; runner.identities = {'A': dict(pid=2, created=7)}
        runner.n = Mock(); runner.n.locate.return_value = 55; runner.n.identity.return_value = {'pid': 2}
        runner.close()
        runner.n.user.PostMessageW.assert_called_once_with(55, 0x0010, 0, 0)
        p.send_signal.assert_not_called(); p.terminate.assert_not_called(); p.kill.assert_not_called()

    def test_foreign_window_cannot_be_closed(self):
        runner = self.runner(); p = Mock(pid=2); p.poll.return_value = None
        runner.owned = {'A': p}; runner.identities = {'A': dict(pid=2, created=7)}
        runner.n = Mock(); runner.n.locate.return_value = 55; runner.n.identity.return_value = {'pid': 99}
        with self.assertRaises(RuntimeError): runner.close()
        runner.n.user.PostMessageW.assert_not_called(); p.send_signal.assert_not_called()


class R36ProcessEncodingTests(unittest.TestCase):
    def test_utf8_console_switch_does_not_decode_with_cached_oem(self):
        expected = [{'ProcessId': 7, 'CommandLine': 'C:\\Users\\俞鑫\\.ncs\\upgrade.vbs'}]
        raw = json.dumps(expected, ensure_ascii=False).encode('utf-8')
        def transport(*args, **kwargs):
            return Mock(stdout=raw.decode('cp936') if kwargs.get('encoding') == 'oem' else raw)
        with patch.object(r36.subprocess, 'run', side_effect=transport):
            try:
                actual = r36.scan_processes()
            except (ValueError, RuntimeError) as error:
                self.fail('UE改变控制台代码页后仍应保留原始进程路径：' + str(error))
            self.assertEqual(actual, expected)

    def test_corrupt_scan_reports_original_fragment_without_launching(self):
        with patch.object(r36.subprocess, 'run', return_value=Mock(stdout=r'[{"CommandLine":"bad\q"}]')):
            with self.assertRaises(Exception) as caught:
                r36.scan_processes()
            self.assertRegex(str(caught.exception), '进程清单JSON.*CommandLine')

    def test_real_scan_preserves_unicode_command_line(self):
        import sys
        marker = 'R36中文全角反斜线＼路径'
        child = r36.subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()', marker], stdin=r36.subprocess.PIPE)
        try:
            try:
                rows = r36.scan_processes()
            except (ValueError, UnicodeError) as error:
                self.fail('真实进程清单必须可解码：' + str(error))
            matches = [p for p in rows if p['ProcessId'] == child.pid]
            self.assertEqual(len(matches), 1)
            self.assertIn(marker, matches[0]['CommandLine'], '不能让控制台代码页损坏进程身份和路径')
        finally:
            child.stdin.close()
            self.assertEqual(child.wait(timeout=10), 0)


class R36ServerCloseTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def test_missing_close_adapter_never_falls_back_to_break(self):
        p = Mock(pid=1); p.poll.return_value = None
        with self.assertRaises(RuntimeError): r36.close_owned(p, dict(pid=1, created=7), 7)
        p.send_signal.assert_not_called()

    def test_server_launch_binds_unique_run_and_token_only_to_server(self):
        runner = self.runner()
        with patch.object(r36, 'scan_processes', return_value=[]), patch.object(r36, 'start_process', side_effect=[Mock(pid=p) for p in (1, 2, 3)]):
            runner.launch()
        token = getattr(runner, 'stop_token', '')
        self.assertRegex(token, r'^[0-9a-f]{32}$')
        self.assertIn('-R36Run=' + runner.args.label, runner.plan['Server'])
        self.assertIn('-R36StopToken=' + token, runner.plan['Server'])
        self.assertFalse(any('R36Stop' in a or 'R36Run=' in a for a in runner.plan['A'] + runner.plan['B']))

    def test_server_close_requests_normal_engine_exit_not_break(self):
        runner = self.runner(); p = Mock(pid=1); p.poll.return_value = None; p.wait.return_value = 0
        runner.owned = {'Server': p}; runner.identities = {'Server': dict(pid=1, created=7)}
        runner.args.label = 'R36-S1-UI-StopPython-' + uuid.uuid4().hex
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.output.mkdir()
        runner.stop_token = uuid.uuid4().hex
        runner.close()
        target = runner.output / 'stop.request'
        self.assertTrue(target.exists(), 'DS必须收到本次显式停止文件')
        self.assertEqual(target.read_text(encoding='ascii'), runner.stop_token)
        p.send_signal.assert_not_called(); p.terminate.assert_not_called(); p.kill.assert_not_called()

    def test_nonzero_child_exit_cannot_be_reported_as_clean_close(self):
        runner = self.runner(); p = Mock(pid=1); p.poll.return_value = 3
        runner.owned = {'Server': p}; runner.identities = {'Server': dict(pid=1, created=7)}
        with self.assertRaises(RuntimeError): runner.close()
        self.assertEqual(runner.exits[0]['code'], 3)


class R36NativeActionTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def test_geometry_uses_actual_pixel_center_not_design_coordinates(self):
        self.assertEqual(r36.ui_point(dict(laidOut=True, x=66.6, y=187.2)), (67, 187))

    def test_missing_or_outside_geometry_refuses_input(self):
        for row in ({}, dict(laidOut=False, x=1, y=2), dict(laidOut=True, x=1280, y=20)):
            with self.subTest(row=row), self.assertRaises(ValueError): r36.ui_point(row)

    def test_receipt_requires_same_complete_authority_revision(self):
        a = dict(kind='VIEW', connection='a', revision=4, digest='same', seq=2)
        records = {'Server': [a], 'A': [a], 'B': []}
        self.assertEqual(r36.agreed_view(records, 'A', 2), a)
        for replacement in (dict(a, revision=3), dict(a, digest='half'), dict(a, connection='other')):
            records['A'] = [replacement]
            self.assertIsNone(r36.agreed_view(records, 'A', 2))
        records['A'] = [a]
        self.assertIsNone(r36.agreed_view(records, 'A', 1))

    def test_income_only_newer_authority_keeps_exact_committed_owner_evidence(self):
        view = dict(kind='VIEW', connection='a', match='m', join=1, revision=4, digest='four',
                    seq=2, response=2, expectedSeq=3, gold=201, sampleTime=1, items=1, health=300,
                    equipment=['item'], skills=[], offers=[], incomeRate=5)
        newer = dict(view, revision=5, digest='five', gold=202, sampleTime=1.2)
        records = {'Server': [view, newer], 'A': [view], 'B': []}
        self.assertEqual(r36.agreed_view(records, 'A', 2), view)
        for delta in (dict(seq=3), dict(health=0), dict(items=2), dict(incomeRate=4),
                      dict(offers=['purchased']), dict(equipment=['other']), dict(connection='other')):
            with self.subTest(delta=delta):
                records['Server'] = [view, dict(newer, **delta)]
                if 'connection' in delta:
                    records['A'] = [dict(view, connection='other')]
                self.assertIsNone(r36.agreed_view(records, 'A', 2))
                records['A'] = [view]
        records['Server'] = [dict(view, digest='wrong'), newer]
        self.assertIsNone(r36.agreed_view(records, 'A', 2))

    def pointer_runner(self):
        import ctypes
        from ctypes import wintypes
        runner = self.runner(); runner.windows = {'A': 55}; runner.owned = {'A': Mock(pid=2)}
        runner.owned_identity = Mock()
        runner.n = Mock(); runner.n.c = ctypes; runner.n.w = wintypes
        runner.snapshot = Mock()
        return runner

    def test_pointer_drag_sends_native_down_motion_up(self):
        runner = self.pointer_runner()
        with patch.object(r36.time, 'sleep'):
            runner.pointer('A', dict(laidOut=True, x=20, y=30), dict(laidOut=True, x=400, y=100))
        self.assertEqual([c.args[0] for c in runner.n.user.mouse_event.call_args_list], [2, 4])
        self.assertEqual(runner.n.user.SetCursorPos.call_count, 16)
        self.assertGreater(runner.owned_identity.call_count, 16)
        runner.n.activate.assert_called_once()

    def test_pointer_hover_never_clicks_and_keeps_owned_guard(self):
        runner = self.pointer_runner()
        with patch.object(r36.time, 'sleep'):
            try: runner.pointer('A', dict(laidOut=True, x=20, y=30), button='hover')
            except ValueError: self.fail('缺少复用安全输入的只悬停取图')
        runner.n.user.mouse_event.assert_not_called()
        runner.n.user.SetCursorPos.assert_called_once()
        self.assertGreaterEqual(runner.owned_identity.call_count, 3)
        runner.owned_identity.side_effect = RuntimeError('外来窗口')
        with self.assertRaises(RuntimeError): runner.pointer('A', dict(laidOut=True, x=20, y=30), button='hover')

    def test_pointer_foreign_identity_stops_before_activation_or_input(self):
        runner = self.pointer_runner(); runner.owned_identity.side_effect = RuntimeError('外来窗口')
        with self.assertRaises(RuntimeError): runner.pointer('A', dict(laidOut=True, x=20, y=30))
        runner.n.activate.assert_not_called(); runner.n.user.mouse_event.assert_not_called()


class R36ActionDriverTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def test_settled_ui_requires_authority_and_same_ui_revision(self):
        v = dict(kind='VIEW', connection='a', revision=4, digest='same', seq=2)
        ui = dict(kind='UI', revision=4, pending=False, cells=[])
        rows = {'Server': [v], 'A': [v, ui], 'B': []}
        self.assertEqual(r36.settled_ui(rows, 'A', 2), (ui, v))
        for bad in (dict(ui, pending=True), dict(ui, revision=3)):
            rows['A'] = [v, bad]; self.assertIsNone(r36.settled_ui(rows, 'A', 2))

    def test_cell_selection_uses_visible_identity_and_order_not_name(self):
        cells = [dict(zone=0, item=str(i), type=7, order=i, laidOut=True, x=40, y=80) for i in (9, 2)]
        cells.append(dict(cells[1], item='hidden', order=1, laidOut=False))
        self.assertEqual(r36.choose_cell(dict(cells=cells), 0, type=7), cells[1])
        self.assertEqual(r36.choose_cell(dict(cells=cells), 0, item='9'), cells[0])
        with self.assertRaises(AssertionError): r36.choose_cell(dict(cells=cells), 2, slot=0)

    def test_interaction_waits_for_settled_not_optimistic_view(self):
        runner = self.runner(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.wait = Mock(return_value=('ui', 'view'))
        self.assertEqual(runner.interaction_state(), ('ui', 'view'))
        self.assertIsNone(runner.wait.call_args.args[0]())

    def test_malformed_wait_result_cannot_reach_native_input(self):
        runner = self.runner(); runner.wait = Mock(return_value=None)
        with self.assertRaises(AssertionError): runner.interaction_state()

    def action_runner(self, code=0):
        runner = self.runner()
        before = dict(seq=0, expectedSeq=1, items=17, bag=17, equipment=['empty'] * 6, skills=[])
        after = dict(before, seq=1, code=code, bag=16, equipment=['item'] + ['empty'] * 5)
        other = dict(before, connection='b')
        ui = dict(canDrag=True, confirmation=False)
        runner.interaction_state = Mock(side_effect=[(ui, before), (ui, other), (ui, after), (ui, other)])
        runner.pointer = Mock(); runner.snapshot = Mock()
        return runner, before, after

    def test_action_requires_expected_receipt_and_checks_authoritative_delta(self):
        runner, before, after = self.action_runner(); check = Mock(return_value=True)
        self.assertEqual(runner.action('equip', lambda ui: 'source', lambda ui: 'target', check=check), after)
        runner.pointer.assert_called_once_with('A', 'source', 'target', button='left')
        check.assert_called_once_with(before, after)
        self.assertIn(unittest.mock.call('A', 1), runner.interaction_state.call_args_list)
        runner.snapshot.assert_called_once_with('equip')

    def test_confirmation_accepts_real_geometry_without_legacy_boolean(self):
        function = getattr(r36, 'confirmation_open', None)
        self.assertTrue(callable(function), '真实Confirmation几何缺少兼容适配')
        self.assertTrue(function({'Confirmation': dict(visible=True, laidOut=True)}))
        self.assertFalse(function({'Confirmation': dict(visible=False, laidOut=False)}))
        self.assertFalse(function({'Confirmation': dict(visible=True, laidOut=False)}))
        self.assertFalse(function({}))

    def test_confirmation_retains_legacy_boolean_compatibility(self):
        function = getattr(r36, 'confirmation_open', None)
        self.assertTrue(callable(function))
        self.assertTrue(function(dict(confirmation=True)))
        self.assertFalse(function(dict(confirmation=False)))

    def test_action_accepts_visible_confirmation_when_drag_is_blocked(self):
        runner, _, after = self.action_runner()
        rows = list(runner.interaction_state.side_effect)
        rows[0] = (dict(canDrag=False, Confirmation=dict(visible=True, laidOut=True)), rows[0][1])
        runner.interaction_state.side_effect = rows
        self.assertEqual(runner.action('confirm', lambda ui: 'confirm-button'), after)
        runner.pointer.assert_called_once_with('A', 'confirm-button', None, button='left')

    def test_action_server_rejection_is_not_success(self):
        runner, _, _ = self.action_runner(code=4)
        with self.assertRaises(AssertionError): runner.action('equip', lambda ui: 'source')
        runner.snapshot.assert_not_called()

    def test_action_checks_other_owner_unchanged(self):
        runner, _, _ = self.action_runner()
        rows = list(runner.interaction_state.side_effect); rows[-1] = (rows[-1][0], dict(rows[-1][1], items=99))
        runner.interaction_state.side_effect = rows
        with self.assertRaises(AssertionError): runner.action('equip', lambda ui: 'source')

    def test_controlled_equipment_sequence_uses_real_pointer_start_and_three_actions(self):
        runner = self.runner(); ui = dict(Advance=dict(laidOut=True, enabled=True, x=20, y=30), canDrag=False,
            automaticWaves=False, waveClockSource='AuthoritySample', waveServerTime=0)
        runner.wave_state = Mock(side_effect=[dict(phase=0, index=-1, time=0), dict(phase=1, index=0, time=1)])
        runner.interaction_state = Mock(side_effect=[(ui, {}), (ui, {}), (dict(ui, canDrag=True), {})])
        runner.pointer = Mock(); runner.wait = Mock(side_effect=lambda predicate, description: predicate())
        runner.action = Mock(return_value={})
        runner.controlled_actions()
        runner.pointer.assert_called_once_with('A', ui['Advance'])
        self.assertEqual([c.args[0] for c in runner.action.call_args_list], ['02-Equip', '03-Replace', '04-Unequip'])

    def test_verify_dispatches_controlled_actions_after_initial_capture(self):
        runner = self.runner(); runner.args.observe_only = False
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.controlled_actions = Mock(); runner.skill_actions = Mock()
        error = None
        try: runner.verify()
        except RuntimeError as e: error = str(e)
        self.assertIsNone(error, '已实现的真实场景必须从正式入口可达')
        runner.controlled_actions.assert_called_once()


class R36SkillDriverTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def ui(self):
        return dict(cells=[dict(zone=0, type=5, definition=d, item=str(i), order=i,
                               laidOut=True, x=40, y=80) for i, d in enumerate(('A', 'B', 'A'), 1)])

    def test_book_selection_by_definition_not_display_name(self):
        ui = self.ui()
        self.assertEqual(r36.choose_book(ui, skill='B'), ui['cells'][1])
        self.assertEqual(r36.choose_book(ui, exclude=('A',)), ui['cells'][1])
        with self.assertRaises(AssertionError): r36.choose_book(ui, skill='missing')

    def test_learning_sequence_requires_prepare_cancel_confirm_and_remove(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=(self.ui(), {}))
        runner.action = Mock(return_value=dict(skills=[dict(id='first')]))
        runner.skill_actions()
        self.assertEqual([c.args[0] for c in runner.action.call_args_list],
                         ['05-Learn', '06-Duplicate', '07-PrepareCancel', '08-Cancel',
                          '09-PrepareConfirm', '10-Confirm', '11-Remove', '12-Relearn'])
        self.assertEqual(runner.action.call_args_list[1].kwargs['code'], 26)
        self.assertEqual(runner.action.call_args_list[6].kwargs['button'], 'right')
        self.assertTrue(all(call.kwargs.get('check') for call in runner.action.call_args_list))


class R36SkillDeltaTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    ui = R36SkillDriverTests.ui

    def calls(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=(self.ui(), {}))
        runner.action = Mock(return_value=dict(skills=[dict(id='first')]))
        runner.skill_actions()
        calls = runner.action.call_args_list
        self.assertEqual(len(calls), 8, '必须执行八步且提供可检验的业务delta')
        return calls

    def test_learning_checks_consumption_identity_ready_and_other_slots(self):
        import copy
        calls = self.calls()
        empty = dict(id='0' * 32, skill='None', deadline=0)
        before = dict(items=17, bag=17, equipment=['0' * 32] * 6,
                      skills=[dict(empty) for _ in range(4)], ticket='0' * 32)
        after = copy.deepcopy(before); after.update(items=16, bag=16)
        after['skills'][0] = dict(id='new', skill='A', deadline=0)
        check = calls[0].kwargs['check']
        self.assertTrue(check(before, after))
        for changes in (dict(items=17), dict(bag=17), dict(equipment=['wrong'] * 6)):
            self.assertFalse(check(before, dict(after, **changes)))
        for changes in (dict(id='0' * 32), dict(skill='B'), dict(deadline=8)):
            bad = copy.deepcopy(after); bad['skills'][0].update(changes)
            self.assertFalse(check(before, bad))
        bad = copy.deepcopy(after); bad['skills'][1] = dict(after['skills'][0])
        self.assertFalse(check(before, bad))
        relearn = calls[7].kwargs['check']
        self.assertTrue(relearn(before, after))
        old = copy.deepcopy(after); old['skills'][0]['id'] = 'first'
        self.assertFalse(relearn(before, old), '重学不得恢复旧LearnedId')

    def test_duplicate_prepare_cancel_confirm_and_remove_deltas(self):
        import copy
        calls = self.calls(); zero = '0' * 32
        before = dict(items=16, bag=16, equipment=[zero] * 6, ticket=zero,
                      skills=[dict(id='first', skill='A', deadline=18)] + [dict(id=zero, skill='None', deadline=0) for _ in range(3)])
        prepared = copy.deepcopy(before); prepared['ticket'] = 'ticket'
        for index, after in ((1, before), (2, prepared), (3, before), (4, prepared)):
            check = calls[index].kwargs['check']
            self.assertTrue(check(prepared if index == 3 else before, after))
            self.assertFalse(check(before, dict(after, items=15)))
            bad = copy.deepcopy(after); bad['skills'][0]['deadline'] = 0
            self.assertFalse(check(before, bad), '拒绝/准备/取消不能刷新旧CD')
        self.assertFalse(calls[2].kwargs['check'](before, before), '覆盖准备必须有有效票据')
        self.assertFalse(calls[3].kwargs['check'](prepared, prepared), '取消必须结束票据')
        confirmed = copy.deepcopy(before); confirmed.update(items=15, bag=15)
        confirmed['skills'][0] = dict(id='second', skill='B', deadline=0)
        self.assertTrue(calls[5].kwargs['check'](prepared, confirmed))
        self.assertFalse(calls[5].kwargs['check'](prepared, dict(confirmed, bag=16)))
        removed = copy.deepcopy(before); removed['skills'][0] = dict(id=zero, skill='None', deadline=0)
        self.assertTrue(calls[6].kwargs['check'](before, removed))
        self.assertFalse(calls[6].kwargs['check'](before, dict(removed, bag=17)), '移除不能返书')
        self.assertFalse(calls[6].kwargs['check'](before, before))

    def test_book_selection_excludes_hidden_wrong_type_and_missing_identity(self):
        ui = self.ui(); ui['cells'][:0] = [dict(ui['cells'][0], order=0, laidOut=False),
                                         dict(ui['cells'][0], order=0, type=7)]
        selected = r36.choose_book(ui)
        self.assertIsNotNone(selected, '必须选择真实已布局技能书')
        self.assertEqual(selected['order'], 1)
        with self.assertRaises(AssertionError): r36.choose_book(dict(cells=[]))

    def test_verify_dispatches_learning_only_after_equipment_and_never_observe(self):
        for observe in (False, True):
            runner = self.runner(); runner.args.observe_only = observe; events = []
            runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
            runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(side_effect=lambda label: events.append('capture'))
            runner.controlled_actions = Mock(side_effect=lambda: events.append('equipment'))
            runner.skill_actions = Mock(side_effect=lambda: events.append('skills'))
            runner.verify()
            self.assertEqual(events, ['capture'] if observe else ['capture', 'equipment', 'skills'])


class R36ScrollCoverageTests(unittest.TestCase):
    def sample(self, orders=(1, 2), widgets=('w1', 'w2'), total=4):
        return dict(revision=7, total=total, pending=False,
                    bagGeometry=dict(laidOut=True, rect=[0, 0, 300, 300]),
                    cells=[dict(zone=0, laidOut=True, item='item' + str(i), order=i, widget=w,
                                x=50, y=50, rect=[10, 10, 90, 90]) for i, w in zip(orders, widgets)])

    def test_only_fully_visible_bag_cells_count(self):
        ui = self.sample()
        ui['cells'] += [dict(ui['cells'][0], item='partial', rect=[10, 250, 90, 330]),
                        dict(ui['cells'][0], item='outside', rect=[-20, 10, 20, 90]),
                        dict(ui['cells'][0], item='slot', zone=1),
                        dict(ui['cells'][0], item='hidden', laidOut=False)]
        self.assertEqual([c['item'] for c in r36.visible_bag_cells(ui)], ['item1', 'item2'])

    def test_missing_or_nonfinite_rect_cannot_claim_visibility(self):
        for rect in (None, [0, 0, float('nan'), 20], [0, 0, 0, 20], [0, 0, 10]):
            ui = self.sample(); ui['cells'][0]['rect'] = rect
            with self.subTest(rect=rect), self.assertRaises(ValueError): r36.visible_bag_cells(ui)

    def test_full_coverage_requires_every_instance_and_widget_reuse(self):
        coverage = r36.ScrollCoverage(4, 7)
        coverage.observe(self.sample())
        with self.assertRaises(AssertionError): coverage.finish()
        coverage.observe(self.sample((3, 4)))
        self.assertEqual(coverage.finish(), dict(items=4, orders=4, widgets=2, reused_widgets=2, peak_entries=2))

    def test_duplicates_and_last_page_do_not_prove_full_coverage(self):
        coverage = r36.ScrollCoverage(4, 7)
        for _ in range(3): coverage.observe(self.sample((3, 4)))
        with self.assertRaises(AssertionError): coverage.finish()

    def test_revision_pending_or_count_change_invalidates_run(self):
        for changes in (dict(revision=8), dict(total=3), dict(pending=True)):
            coverage = r36.ScrollCoverage(4, 7)
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                coverage.observe(dict(self.sample(), **changes))

    def test_order_identity_collision_and_missing_identity_rejected(self):
        for changes in (dict(item=''), dict(item='0' * 32), dict(order=0), dict(order=5), dict(widget='')):
            coverage = r36.ScrollCoverage(4, 7); ui = self.sample(); ui['cells'][0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(AssertionError): coverage.observe(ui)
        coverage = r36.ScrollCoverage(4, 7); coverage.observe(self.sample())
        ui = self.sample(); ui['cells'][0]['item'] = 'wrong'
        with self.assertRaises(AssertionError): coverage.observe(ui)

    def test_new_widget_for_every_page_does_not_prove_reuse(self):
        coverage = r36.ScrollCoverage(4, 7)
        coverage.observe(self.sample()); coverage.observe(self.sample((3, 4), ('w3', 'w4')))
        with self.assertRaises(AssertionError): coverage.finish()


class R36NativeWheelTests(unittest.TestCase):
    pointer_runner = R36NativeActionTests.pointer_runner
    runner = R36RunnerTests.runner

    def ui(self):
        return dict(bagGeometry=dict(laidOut=True, x=150, y=150, rect=[0, 0, 300, 300]), cells=[])

    def test_native_wheel_moves_to_bag_and_sends_signed_detents(self):
        runner = self.pointer_runner()
        runner.wheel('A', self.ui(), -6)
        runner.n.activate.assert_called_once()
        runner.n.user.SetCursorPos.assert_called_once_with(150, 150)
        runner.n.user.mouse_event.assert_called_once_with(0x0800, 0, 0, (-720) & 0xffffffff, 0)
        self.assertGreaterEqual(runner.owned_identity.call_count, 3)
        self.assertGreaterEqual(runner.n.focused.call_count, 2)

    def test_invalid_geometry_or_detents_never_send_input(self):
        for ticks in (0, True, 1.5, -9):
            runner = self.pointer_runner()
            with self.subTest(ticks=ticks), self.assertRaises(ValueError): runner.wheel('A', self.ui(), ticks)
            runner.n.user.mouse_event.assert_not_called()
        runner = self.pointer_runner(); ui = self.ui(); ui['bagGeometry']['rect'] = None
        with self.assertRaises(ValueError): runner.wheel('A', ui)
        runner.n.activate.assert_not_called()

    def test_foreign_identity_blocks_activation_and_wheel(self):
        runner = self.pointer_runner(); runner.owned_identity.side_effect = RuntimeError('不是本次窗口')
        with self.assertRaises(RuntimeError): runner.wheel('A', self.ui())
        runner.n.activate.assert_not_called(); runner.n.user.mouse_event.assert_not_called()

    def test_focus_loss_after_pointer_move_blocks_wheel(self):
        runner = self.pointer_runner(); runner.n.focused.side_effect = [None, RuntimeError('移动后失焦')]
        with self.assertRaises(RuntimeError): runner.wheel('A', self.ui())
        runner.n.user.SetCursorPos.assert_called_once(); runner.n.user.mouse_event.assert_not_called()


class R36ScrollDriverTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    sample = R36ScrollCoverageTests.sample

    def scroll_runner(self):
        runner = self.runner(); runner.args.items = 4
        first = dict(self.sample(), time=1., offset=0)
        second = dict(self.sample((3, 4)), time=2., offset=2)
        view = dict(connection='a', revision=7, digest='whole')
        runner.interaction_state = Mock(return_value=(first, view))
        runner.wheel = Mock(); runner.snapshot = Mock()
        rows = iter([(second, view)])
        def wait(predicate, description, seconds=None):
            ui, current = next(rows)
            with patch.object(r36, 'settled_ui', return_value=(ui, current)):
                runner.records = Mock(return_value={})
                result = predicate()
            self.assertTrue(result, '新鲜且完整的滚动样本必须可接纳')
            return result
        runner.wait = Mock(side_effect=wait)
        return runner, first, second, view

    def test_driver_uses_wheel_and_finishes_only_after_every_item(self):
        runner, first, _, _ = self.scroll_runner()
        result = runner.scroll_bag('A')
        self.assertEqual(result, dict(items=4, orders=4, widgets=2, reused_widgets=2, peak_entries=2))
        runner.wheel.assert_called_once_with('A', first)
        self.assertTrue(any(c.args[0] == 'SCROLL_PASS' for c in runner.emit.call_args_list))

    def test_driver_rejects_stale_sample_and_owner_change(self):
        runner, first, second, view = self.scroll_runner()
        def wait(predicate, description, seconds=None):
            runner.records = Mock(return_value={})
            with patch.object(r36, 'settled_ui', return_value=(first, view)):
                self.assertFalse(predicate(), '输入之前的旧Trace不能证明滚轮结果')
            with patch.object(r36, 'settled_ui', return_value=(second, dict(view, connection='foreign'))):
                with self.assertRaises(AssertionError): predicate()
            raise RuntimeError('本测试停止在身份保护')
        runner.wait = Mock(side_effect=wait)
        with self.assertRaises(RuntimeError): runner.scroll_bag('A')

    def test_verify_dispatches_scroll_for_both_owners(self):
        runner = self.runner(); runner.args.scroll_all = True
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.scroll_bag = Mock()
        runner.verify()
        self.assertEqual([c.args for c in runner.scroll_bag.call_args_list], [('A',), ('B',)])

    def test_cli_scroll_requires_initial_observation_mode(self):
        args = r36.parse_args(['--label', 'R36-S1-UI-ScrollCli', '--observe-only', '--scroll-all'])
        self.assertTrue(args.scroll_all)
        with self.assertRaises(SystemExit): r36.parse_args(['--label', 'R36-S1-UI-ScrollCli', '--scroll-all'])


class R36DragCaptureTests(unittest.TestCase):
    pointer_runner = R36NativeActionTests.pointer_runner
    runner = R36RunnerTests.runner

    def test_capture_is_between_real_motion_and_mouse_up(self):
        events = []
        r36.perform_drag((10, 20), (300, 200), lambda p: events.append(('move', p)),
                         lambda: events.append('down'), lambda: events.append('up'),
                         lambda: None, lambda: None, observe=lambda: events.append('capture'))
        self.assertEqual(events.count('capture'), 1)
        if 'capture' in events:
            at = events.index('capture')
            self.assertLess(events.index('down'), at)
            self.assertLess(at, events.index('up'))
            self.assertNotEqual(events[at - 1], ('move', (300, 200)))
            self.assertGreater(at, 3)

    def test_native_pointer_captures_held_drag_with_unique_label(self):
        runner = self.pointer_runner(); captures = []
        runner.snapshot.side_effect = lambda label: captures.append((label,
            [c.args[0] for c in runner.n.user.mouse_event.call_args_list], runner.n.user.SetCursorPos.call_count))
        with patch.object(r36.time, 'sleep'):
            for _ in range(2):
                runner.n.user.reset_mock()
                runner.pointer('A', dict(laidOut=True, x=20, y=30), dict(laidOut=True, x=400, y=100))
        self.assertEqual(len(captures), 2, '不能仅在拖动结束后截图')
        if len(captures) == 2:
            self.assertNotEqual(captures[0][0], captures[1][0])
            for label, flags, moves in captures:
                self.assertRegex(label, r'^Drag-A-[0-9a-f]{32}$')
                self.assertEqual(flags, [2], '截图时鼠标必须仍按下')
                self.assertGreater(moves, 2); self.assertLess(moves, 16)


class R36NativeSkillKeyTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    pointer_runner = R36NativeActionTests.pointer_runner

    def test_skill_key_reuses_native_adapter_for_each_slot(self):
        for slot in range(4):
            runner = self.pointer_runner(); runner.n.user.GetAsyncKeyState.return_value = 0
            runner.skill_key('A', slot)
            runner.n.activate.assert_called_once()
            runner.n.key.assert_called_once_with(0x31 + slot)
            runner.n.command.assert_not_called()
            self.assertEqual(runner.n.p, runner.owned['A'])
            self.assertEqual(runner.n.window, runner.windows['A'])
            self.assertGreaterEqual(runner.owned_identity.call_count, 3)
            self.assertGreaterEqual(runner.n.focused.call_count, 2)

    def test_invalid_slot_sends_nothing(self):
        for slot in (-1, 4, True, 1.5, '1'):
            runner = self.pointer_runner()
            with self.subTest(slot=slot), self.assertRaises(ValueError): runner.skill_key('A', slot)
            runner.n.activate.assert_not_called(); runner.n.key.assert_not_called()

    def test_foreign_window_stops_before_activation(self):
        runner = self.pointer_runner(); runner.owned_identity.side_effect = RuntimeError('外来窗口')
        with self.assertRaises(RuntimeError): runner.skill_key('A', 0)
        runner.n.activate.assert_not_called(); runner.n.key.assert_not_called()

    def test_focus_loss_never_sends_skill_key(self):
        runner = self.pointer_runner(); runner.n.focused.side_effect = RuntimeError('失焦')
        with self.assertRaises(RuntimeError): runner.skill_key('A', 0)
        runner.n.key.assert_not_called()

    def test_held_user_key_is_not_released_or_overlapped(self):
        for held in (0x10, 0x11, 0x12, 0x5B, 0x5C, 0x31):
            runner = self.pointer_runner()
            runner.n.user.GetAsyncKeyState.side_effect = lambda vk: 0x8000 if vk == held else 0
            with self.subTest(held=held), self.assertRaises(RuntimeError): runner.skill_key('A', 0)
            runner.n.key.assert_not_called(); runner.n.key_event.assert_not_called()


class R36CombatApproachTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    pointer_runner = R36NativeActionTests.pointer_runner

    def sample(self, valid=False):
        view = dict(kind='VIEW', connection='a', revision=4, digest='v', health=300)
        scope = dict(kind='SCOPE', authority=True, ownerLinked=True, phase=2, inside=False,
                     time=100, pawnX=-150, pawnY=-75, targetDistance=398 if valid else -1, targetValid=valid)
        ui = dict(kind='UI', revision=4, pending=False)
        return scope, dict(Server=[view, scope], A=[view, ui])

    def test_scope_uses_own_authoritative_view_not_other_player(self):
        scope, rows = self.sample()
        rows['Server'] += [dict(rows['Server'][0], connection='b'), dict(scope, targetValid=True)]
        self.assertEqual(r36.combat_scope(rows, 'A'), scope)

    def test_scope_rejects_wrong_phase_unlinked_and_bad_coordinates(self):
        scope, rows = self.sample()
        for changes in (dict(phase=1), dict(authority=False), dict(ownerLinked=False), dict(inside=True)):
            rows['Server'][-1] = dict(scope, **changes)
            self.assertIsNone(r36.combat_scope(rows, 'A'))
        for value in (float('nan'), float('inf'), None, True):
            rows['Server'][-1] = dict(scope, pawnX=value)
            with self.assertRaises(ValueError): r36.combat_scope(rows, 'A')

    def test_native_movement_is_bounded_owned_and_no_console(self):
        runner = self.pointer_runner(); runner.n.user.GetAsyncKeyState.return_value = 0
        runner.move_key('A', 'W', .2)
        runner.n.key.assert_called_once_with(0x57, .2)
        runner.n.command.assert_not_called()
        self.assertGreaterEqual(runner.owned_identity.call_count, 3)
        for key, seconds in (('F', .2), ('W', 0), ('W', .21), ('W', True)):
            with self.assertRaises(ValueError): runner.move_key('A', key, seconds)

    def test_movement_never_overlaps_user_held_key_or_foreign_window(self):
        runner = self.pointer_runner(); runner.n.user.GetAsyncKeyState.return_value = 0x8000
        with self.assertRaises(RuntimeError): runner.move_key('A', 'W', .2)
        runner.n.key.assert_not_called()
        runner = self.pointer_runner(); runner.owned_identity.side_effect = RuntimeError('外部窗口')
        with self.assertRaises(RuntimeError): runner.move_key('A', 'W', .2)
        runner.n.activate.assert_not_called(); runner.n.key.assert_not_called()

    def test_approach_reads_fresh_target_before_returning(self):
        runner = r36.Runner(); scope, rows = self.sample()
        runner.records = Mock(return_value=rows); runner.emit = Mock(); runner.snapshot = Mock()
        moved = dict(scope, pawnX=-55, time=101, targetValid=True, targetDistance=442)
        runner.move_key = Mock(side_effect=lambda *args: rows['Server'].__setitem__(-1, moved))
        runner.wait = Mock(side_effect=lambda predicate, *args, **kwargs: predicate())
        runner.ensure_cd_target()
        runner.move_key.assert_called_once_with('A', 'W', .2)
        self.assertEqual(runner.emit.call_args.args[0], 'COMBAT_TARGET_READY')
        self.assertTrue(runner.emit.call_args.kwargs['scope']['targetValid'])

    def test_valid_target_does_not_move(self):
        runner = r36.Runner(); scope, rows = self.sample(True)
        runner.records = Mock(return_value=rows); runner.move_key = Mock(); runner.emit = Mock(); runner.snapshot = Mock()
        runner.wait = Mock(side_effect=lambda predicate, *args, **kwargs: predicate())
        runner.ensure_cd_target(); runner.move_key.assert_not_called()

    def test_missing_target_at_arena_center_is_not_guessed_success(self):
        runner = r36.Runner(); scope, rows = self.sample()
        rows['Server'][-1] = dict(scope, pawnX=0, pawnY=0)
        runner.records = Mock(return_value=rows); runner.move_key = Mock(); runner.emit = Mock(); runner.snapshot = Mock()
        runner.wait = Mock(side_effect=lambda predicate, *args, **kwargs: predicate())
        with self.assertRaises(RuntimeError): runner.ensure_cd_target()
        runner.move_key.assert_not_called()


class R36CooldownObservationTests(unittest.TestCase):
    def sample(self):
        skill = dict(id='learned-a', skill='A', deadline=0)
        before = dict(kind='VIEW', connection='a', revision=4, digest='old', skills=[skill], items=16, bag=16)
        after = dict(before, revision=5, digest='new', skills=[dict(skill, deadline=18)])
        ui = dict(kind='UI', revision=5, pending=False, serverTime=11, time=101)
        return before, after, ui, {'Server': [after], 'A': [after, ui]}

    def test_new_authoritative_deadline_is_observable(self):
        before, after, ui, rows = self.sample()
        self.assertEqual(r36.cooldown_activation(rows, 'A', 0, before, 100), (ui, after))

    def test_stale_ui_or_uncommitted_view_is_not_activation(self):
        before, after, ui, rows = self.sample()
        for change in (dict(time=100), dict(pending=True), dict(revision=4)):
            rows['A'] = [after, dict(ui, **change)]
            self.assertIsNone(r36.cooldown_activation(rows, 'A', 0, before, 100))
        rows['A'] = [dict(after, digest='partial'), ui]
        self.assertIsNone(r36.cooldown_activation(rows, 'A', 0, before, 100))

    def test_old_or_expired_deadline_cannot_pass(self):
        before, after, ui, rows = self.sample()
        for deadline in (0, 10, float('nan'), float('inf')):
            current = dict(after, skills=[dict(after['skills'][0], deadline=deadline)])
            rows.update(Server=[current], A=[current, ui])
            self.assertIsNone(r36.cooldown_activation(rows, 'A', 0, before, 100))
        rows.update(Server=[after], A=[after, ui])
        old = dict(before, skills=after['skills'])
        self.assertIsNone(r36.cooldown_activation(rows, 'A', 0, old, 100))

    def test_changed_owner_or_learned_identity_is_rejected(self):
        before, after, ui, rows = self.sample()
        for change in (dict(connection='foreign'), dict(skills=[dict(after['skills'][0], id='other')]),
                       dict(skills=[dict(after['skills'][0], skill='B')])):
            current = dict(after, **change); rows.update(Server=[current], A=[current, ui])
            with self.subTest(change=change), self.assertRaises(AssertionError):
                r36.cooldown_activation(rows, 'A', 0, before, 100)

    def test_sync_time_must_be_finite_and_nonnegative(self):
        before, after, ui, rows = self.sample()
        for value in (float('nan'), float('inf'), -1, None, True):
            rows['A'] = [after, dict(ui, serverTime=value)]
            with self.subTest(value=value), self.assertRaises(ValueError):
                r36.cooldown_activation(rows, 'A', 0, before, 100)


class R36CooldownDriverTests(unittest.TestCase):
    runner = R36RunnerTests.runner
    ui = R36SkillDriverTests.ui
    sample = R36CooldownObservationTests.sample

    def test_optional_cd_path_activates_new_and_relearned_identities(self):
        runner = self.runner(); runner.args.cooldown = True
        runner.interaction_state = Mock(return_value=(dict(self.ui(), automaticWaves=False,
            waveClockSource='AuthoritySample', waveServerTime=0, Advance=dict(laidOut=True, enabled=True, x=20, y=30)), {}))
        runner.wave_state = Mock(side_effect=[dict(phase=0, index=-1, time=0), dict(phase=1, index=0, time=1)])
        runner.wait = lambda f, *a, **k: f()
        runner.pointer = Mock(); runner.activate_skill = Mock()
        events = []
        runner.action = Mock(side_effect=lambda label, *a, **k: events.append(label) or dict(skills=[dict(id='first')]))
        runner.activate_skill.side_effect = lambda label, slot: events.append(label)
        runner.skill_actions()
        self.assertEqual(events, ['05-Learn', 'CD-First', '06-Duplicate', '07-PrepareCancel', '08-Cancel',
                                 '09-PrepareConfirm', '10-Confirm', 'CD-Replacement', '11-Remove', '12-Relearn', 'CD-Relearn'])
        runner.pointer.assert_called_once_with('A', runner.interaction_state.return_value[0]['Advance'])

    def test_default_learning_path_does_not_activate(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=(self.ui(), {}))
        runner.action = Mock(return_value=dict(skills=[dict(id='first')]))
        runner.activate_skill = Mock(); runner.pointer = Mock()
        runner.skill_actions()
        runner.activate_skill.assert_not_called(); runner.pointer.assert_not_called()

    def test_cd_cli_requires_explicit_run_and_interaction(self):
        args = r36.parse_args(['--label', 'R36-S1-UI-CDCli', '--run', 'CD_run', '--cooldown'])
        self.assertTrue(args.cooldown)
        for suffix in ([], ['--run', 'CD_run', '--observe-only'], ['--run', 'CD_run', '--validate-assets']):
            with self.subTest(suffix=suffix), self.assertRaises(SystemExit):
                r36.parse_args(['--label', 'R36-S1-UI-CDCli', '--cooldown'] + suffix)

    def activation_runner(self):
        runner = self.runner(); before, after, ui, records = self.sample()
        old_ui = dict(ui, time=100, serverTime=10, revision=4)
        runner.interaction_state = Mock(return_value=(old_ui, before))
        runner.skill_key = Mock(); runner.snapshot = Mock(); runner.records = Mock(return_value=records)
        runner.ensure_cd_target = Mock()
        def wait(predicate, description, seconds=None):
            self.assertTrue(predicate(), '真实激活与递减必须有可接受的权威样本')
            if '递减' in description:
                return dict(ui, time=102, serverTime=11.5), after
            return ui, after
        runner.wait = Mock(side_effect=wait)
        return runner, before, after, ui, records

    def test_activation_requires_new_commit_and_observed_countdown(self):
        runner, before, after, ui, records = self.activation_runner()
        runner.wait.side_effect = None
        calls = []
        def wait(predicate, description, seconds=None):
            if calls: records['A'][-1] = dict(ui, time=102, serverTime=11.5)
            result = predicate(); self.assertTrue(result); calls.append(description); return result
        runner.wait.side_effect = wait
        runner.activate_skill('CD-First', 0)
        runner.skill_key.assert_called_once_with('A', 0)
        self.assertEqual(len(calls), 2)
        runner.ensure_cd_target.assert_called_once_with()
        runner.snapshot.assert_called_once_with('CD-First')
        self.assertTrue(any(c.args[0] == 'COOLDOWN_PASS' for c in runner.emit.call_args_list))

    def test_activation_cannot_pass_after_timeout(self):
        runner, *_ = self.activation_runner(); runner.wait.side_effect = TimeoutError('没有权威CD提交')
        with self.assertRaises(TimeoutError): runner.activate_skill('CD-First', 0)
        runner.snapshot.assert_not_called()

    def test_countdown_cannot_accept_deadline_refresh_or_consumption(self):
        for change in ('deadline', 'items'):
            runner, before, after, ui, records = self.activation_runner(); count = []
            def wait(predicate, description, seconds=None):
                if count:
                    bad = dict(after, items=15) if change == 'items' else dict(after, skills=[dict(after['skills'][0], deadline=19)])
                    records.update(Server=[bad], A=[bad, dict(ui, time=102, serverTime=11.5)])
                count.append(1); return predicate()
            runner.wait.side_effect = wait
            with self.subTest(change=change), self.assertRaises(AssertionError): runner.activate_skill('CD-First', 0)
            runner.snapshot.assert_not_called()


class R36BurstObservationTests(unittest.TestCase):
    def sample(self):
        import copy
        zero = '0' * 32
        base = dict(kind='VIEW', match='m', join=1, connection='a', revision=2, digest='base',
                    items=17, bag=17, response=0, seq=0, expectedSeq=1, code=0, requestItem=zero,
                    equipment=[zero] * 6, skills=[dict(id=zero, skill='None', deadline=0) for _ in range(4)])
        requests = [dict(seq=i + 1, operation=0 if i < 6 else 2, item='item' + str(i),
                         slot=i if i < 6 else i - 6, definition='equipment' if i < 6 else 'skill' + str(i)) for i in range(8)]
        commits = []; current = copy.deepcopy(base)
        for i, request in enumerate(requests):
            current = copy.deepcopy(current)
            current.update(revision=3 + i, digest='commit' + str(i), response=i + 1, seq=i + 1,
                           expectedSeq=i + 2, requestItem=request['item'], bag=16 - i)
            if i < 6: current['equipment'][i] = request['item']
            else:
                current['items'] -= 1
                current['skills'][i - 6] = dict(id='learned' + str(i), skill=request['definition'], deadline=0)
            commits.append(dict(kind='REQUEST_COMMIT', view=current, inFlightRevision=3,
                                requestPresent=i < 6, frame=100))
        other = dict(copy.deepcopy(base), connection='b', join=2, digest='other')
        scope = dict(kind='SCOPE', privateViews=1, authority=False, ownerLinked=True, states=2, pawn='pawn')
        final = commits[-1]['view']
        plan = dict(kind='BURST_SENT', connection='a', base=base, requests=requests)
        records = dict(Server=[base, other] + commits + [final],
                       A=[base, scope, plan, commits[0]['view'], final,
                          dict(kind='UI', revision=final['revision'], total=9, pending=False)], B=[other, scope])
        return records

    def evaluate(self, rows):
        function = getattr(r36, 'burst_result', None)
        self.assertTrue(callable(function), '缺少快速提交完整性观测接缝')
        return function(rows, 17)

    def test_burst_requires_rpc_receipts_merge_delete_and_complete_view(self):
        result = self.evaluate(self.sample())
        self.assertIsNotNone(result, '完整快速提交证据应通过')
        self.assertEqual(result['requests'], 8)
        self.assertEqual(result['deleted_books'], 2)
        self.assertEqual(result['skipped_commits'], 6)
        self.assertEqual(result['final_items'], 15)
        self.assertEqual(result['final_bag'], 9)

    def test_incomplete_final_or_missing_receipt_keeps_waiting(self):
        for role in ('A', 'Server'):
            rows = self.sample()
            if role == 'A': rows['A'] = rows['A'][:-2]
            else: rows['Server'] = [r for r in rows['Server'] if not (r['kind'] == 'REQUEST_COMMIT' and r['view']['seq'] == 4)]
            with self.subTest(role=role): self.assertIsNone(self.evaluate(rows))

    def test_rejected_or_wrong_consumption_cannot_pass(self):
        for field, value in (('code', 26), ('items', 17), ('bag', 10)):
            rows = self.sample(); rows['Server'][-2]['view'][field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError): self.evaluate(rows)
        rows = self.sample(); rows['Server'][-2]['requestPresent'] = True
        with self.assertRaises(AssertionError): self.evaluate(rows)

    def test_sampled_half_view_or_mixed_ui_cannot_pass(self):
        for entry in (dict(kind='VIEW', connection='a', revision=6, digest='half'),
                      dict(kind='UI', revision=10, total=10, pending=False)):
            rows = self.sample(); rows['A'].insert(-1, entry)
            with self.subTest(kind=entry['kind']), self.assertRaises(AssertionError): self.evaluate(rows)

    def test_no_merge_or_nonowner_leak_is_rejected(self):
        for mode in ('no_merge', 'leak', 'other_mutated'):
            rows = self.sample()
            if mode == 'no_merge':
                for row in rows['Server']:
                    if row['kind'] == 'REQUEST_COMMIT': row['inFlightRevision'] = row['view']['revision']
            elif mode == 'leak': rows['B'].append(dict(kind='SCOPE', privateViews=2))
            else: rows['B'].append(dict(rows['B'][0], digest='wrong', items=16))
            with self.subTest(mode=mode), self.assertRaises(AssertionError): self.evaluate(rows)

    def test_request_plan_must_be_unique_contiguous_identity_intents(self):
        for field, value in (('seq', 1), ('item', 'item0'), ('operation', 5), ('slot', 0)):
            rows = self.sample(); rows['A'][2]['requests'][3][field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError): self.evaluate(rows)


class R36BurstDriverTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def test_cli_burst_is_explicit_and_separate_from_other_scenarios(self):
        try:
            args = r36.parse_args(['--label', 'R36-S1-UI-BurstCli', '--burst', '--observe-only'])
        except SystemExit:
            self.fail('CLI应支持显式controlled快速验证')
        self.assertTrue(args.burst)
        for extra in ([], ['--observe-only', '--scroll-all'], ['--observe-only', '--validate-assets'], ['--cooldown', '--run', 'CD']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit):
                r36.parse_args(['--label', 'R36-S1-UI-BurstCli', '--burst'] + extra)

    def test_burst_flag_is_only_on_controlled_sender(self):
        runner = self.runner(); runner.args.burst = True
        with patch.object(r36, 'scan_processes', return_value=[]), patch.object(r36, 'start_process', side_effect=[Mock(pid=p) for p in (1, 2, 3)]):
            runner.launch()
        self.assertIn('-R36Burst=controlled', runner.plan['A'])
        self.assertFalse(any('R36Burst' in arg for role in ('B', 'Server') for arg in runner.plan[role]))

    def test_burst_verification_is_separate_from_mouse_drag_scenario(self):
        runner = self.runner(); runner.args.burst = True
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.burst_actions = Mock()
        runner.controlled_actions = Mock(); runner.skill_actions = Mock()
        runner.verify()
        runner.burst_actions.assert_called_once()
        runner.controlled_actions.assert_not_called(); runner.skill_actions.assert_not_called()

    def test_driver_waits_for_full_burst_evidence_before_capture(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=(dict(canDrag=False, automaticWaves=False,
            waveClockSource='AuthoritySample', waveServerTime=0, Advance=dict(x=5, laidOut=True, enabled=True)), {}))
        runner.wave_state = Mock(side_effect=[dict(phase=0, index=-1, time=0), dict(phase=1, index=0, time=1)])
        runner.pointer = Mock(); runner.snapshot = Mock(); runner.records = Mock(return_value=R36BurstObservationTests().sample())
        def wait(predicate, description, seconds=None):
            result = predicate(); self.assertIsNotNone(result); return result
        runner.wait = Mock(side_effect=wait)
        method = getattr(runner, 'burst_actions', None)
        self.assertTrue(callable(method), '需要独立controlled快速验证调度')
        method()
        runner.pointer.assert_called_once_with('A', {'x': 5, 'laidOut': True, 'enabled': True})
        runner.snapshot.assert_called_once_with('Burst-Final')
        self.assertTrue(any(c.args[0] == 'BURST_PASS' for c in runner.emit.call_args_list))

    def test_burst_timeout_never_captures_success(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=({'canDrag': True}, {}))
        runner.snapshot = Mock(); runner.wait = Mock(side_effect=TimeoutError('缺完整快速提交'))
        method = getattr(runner, 'burst_actions', None)
        self.assertTrue(callable(method), '需要独立controlled快速验证调度')
        with self.assertRaises(TimeoutError): method()
        runner.snapshot.assert_not_called()


class R36S4DriverTests(unittest.TestCase):
    def test_content_check_cli_is_natural_s4_content_only(self):
        base = ['--label', 'R36-S4-New-Check', '--run', 'S4_New', '--scenario', 'natural', '--items', '0',
                '--content-check', 'R36-S4-Selection']
        try: args = r36.parse_args(base)
        except SystemExit: self.fail('缺少仅本地Saved的新内容验收选择入口')
        self.assertEqual(args.content_check, 'R36-S4-Selection')
        for extra in (['--profile', 'evidence'], ['--scenario', 'controlled'], ['--observe-only'],
                      ['--content-check', '../escape'], ['--label', 'R36-S3-Visible-X']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): r36.parse_args(base + extra)

    def test_content_check_load_requires_matching_run_and_real_random_candidates(self):
        load = getattr(r36, 'load_content_check', None)
        self.assertTrue(callable(load), '需要固定Saved目录与完整选择校验')
        plan = dict(run='S4_New', equipment_offer='eq', book_offer='book', direct_offer='direct', random_offer='random',
                    equipment='arbitrary_equipment', skill='arbitrary_skill', equipment_attack=9,
                    random_candidates=['arbitrary_equipment', 'old_equipment'], quality=0)
        with patch.object(Path, 'read_text', return_value=json.dumps(plan)):
            self.assertEqual(load('R36-S4-Selection', 'S4_New'), plan)
            with self.assertRaises(ValueError): load('R36-S4-Selection', 'S4_Other')
        with patch.object(Path, 'read_text', return_value=json.dumps(dict(plan, quality=4))):
            try: highest = load('R36-S4-Selection', 'S4_New')
            except ValueError as error: self.fail('原生第五品质不应被拒绝：' + str(error))
            self.assertEqual(highest['quality'], 4)
        for bad in (dict(plan, random_candidates=['arbitrary_equipment']), dict(plan, equipment_attack=float('nan')),
                    dict(plan, quality=5), dict(plan, quality=True)):
            with patch.object(Path, 'read_text', return_value=json.dumps(bad)), self.assertRaises(ValueError):
                load('R36-S4-Selection', 'S4_New')
        with self.assertRaises(ValueError): load('../escape', 'S4_New')

    def test_natural_random_repeats_purchases_not_seed_or_grants(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'purchase_until', None)
        self.assertTrue(callable(method), '需要自然随机逐笔购买直到目标而非指定seed')
        runner.purchase = Mock(side_effect=[dict(productDefinition='old', productQuality=0),
                                           dict(productDefinition='new', productQuality=0)])
        result = method(7, 'new', ['old', 'new'], 0)
        self.assertEqual(result['productDefinition'], 'new')
        self.assertEqual([c.args[0] for c in runner.purchase.call_args_list], [7, 7])
        runner.purchase = Mock(return_value=dict(productDefinition='outside', productQuality=0))
        with self.assertRaises(AssertionError): method(7, 'new', ['old', 'new'], 0)

    def test_content_purchase_uses_explicit_generic_selection_after_deliveries(self):
        runner = R36RunnerTests().runner(); runner.args.content_check = 'R36-S4-Selection'; runner.args.run = 'S4_New'
        runner.wait = Mock(); runner.configured_purchase = Mock(); runner.purchase = Mock(return_value={'productItem': 'item'})
        runner.offer_index = Mock(return_value=0); runner.action = Mock()
        with patch.object(r36, 'load_content_check', create=True, return_value={'run': 'S4_New'}):
            runner.content_purchase()
        runner.configured_purchase.assert_called_once_with({'run': 'S4_New'})
        runner.purchase.assert_not_called()

    def test_configured_purchase_resolves_generic_offers_and_checks_effect_and_learning(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'configured_purchase', None)
        self.assertTrue(callable(method), '需要按公开商品引用执行通用购买链')
        plan = dict(equipment_offer='eq', book_offer='book', direct_offer='direct', random_offer='random',
                    equipment='new_eq', skill='new_skill', equipment_attack=9, random_candidates=['old', 'new_eq'], quality=0)
        offers = [dict(id=n, kind=k) for n, k in [('random', 0), ('direct', 1), ('book', 0), ('eq', 0)]]
        config = [dict(o, itemType=5 if o['id'] == 'book' else 7) for o in offers]
        runner.wave_state = Mock(return_value=dict(shopConfig=config))
        runner.interaction_state = Mock(return_value=({}, dict(offers=offers)))
        runner.purchase = Mock(side_effect=[dict(productItem='equipment', productDefinition='new_eq', productQuality=0),
                                            dict(productItem='book', productDefinition='new_skill', productQuality=0), {}])
        runner.action = Mock(); runner.disabled_click = Mock(); runner.purchase_until = Mock(return_value={}); runner.snapshot = Mock()
        method(plan)
        self.assertEqual([c.args[0] for c in runner.purchase.call_args_list], [3, 2, 1])
        runner.purchase_until.assert_called_once_with(0, 'new_eq', ['old', 'new_eq'], 0)
        runner.disabled_click.assert_called_once_with('Purchase_1', 'ShopList', 'S4-Added-DirectLimit')
        equip, learn = [c.kwargs['check'] for c in runner.action.call_args_list]
        before = dict(attack=30, incomeRate=1, bag=1, items=1, equipment=['0'], skills=[dict(id='0', skill='None')])
        after = dict(before, attack=39, bag=0, equipment=['equipment'])
        self.assertTrue(equip(before, after)); self.assertFalse(equip(before, dict(after, attack=35)))
        after = dict(before, items=0, bag=0, skills=[dict(id='learned', skill='new_skill')])
        self.assertTrue(learn(before, after)); self.assertFalse(learn(before, dict(after, skills=before['skills'])))

    def test_content_skill_requires_fresh_authority_cooldown_after_native_key(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'content_skill', None)
        self.assertTrue(callable(method), '按键不是释放成功，必须核对真实权威CD提交')
        before = dict(items=1, bag=0, skills=[dict(id='learned', skill='arbitrary_active', deadline=0)])
        active = dict(before, skills=[dict(id='learned', skill='arbitrary_active', deadline=13)])
        runner.interaction_state = Mock(return_value=({'time': 9}, before))
        runner.skill_key = Mock(); runner.records = Mock(return_value={}); runner.snapshot = Mock()
        runner.wait = lambda predicate, description: predicate()
        with patch.object(r36, 'natural_skill_activation', create=True, return_value=({'serverTime': 10}, active)) as confirm, \
                patch.object(r36, 'cooldown_activation', return_value=({'serverTime': 10}, active)):
            method(0)
            confirm.assert_called_once_with({}, 'A', 0, before, 9)
        runner.skill_key.assert_called_once_with('A', 0)
        runner.emit.assert_called_once()
        self.assertEqual(runner.emit.call_args.args[0], 'S4_ACTIVE_SKILL_PASS')
        self.assertFalse(runner.emit.call_args.kwargs['controlled'])

    def test_natural_skill_pairs_committed_ui_while_newer_combat_changes_inventory(self):
        observe = getattr(r36, 'natural_skill_activation', None)
        self.assertTrue(callable(observe), '自然战斗应匹配完整历史提交，不要求战斗冻结在最新版本')
        skill = dict(id='learned', skill='arbitrary_active', deadline=13)
        view = dict(kind='VIEW', connection='owner', revision=2, digest='exact', skills=[skill], items=2, bag=1)
        before = dict(view, revision=1, skills=[dict(skill, deadline=0)])
        newer = dict(view, revision=3, digest='new', items=3, bag=2)
        ui = dict(kind='UI', revision=2, pending=False, time=11, serverTime=11)
        records = dict(A=[view, ui], Server=[view, newer])
        self.assertEqual(observe(records, 'A', 0, before, 10), (ui, view))
        for bad in (dict(ui, pending=True), dict(ui, time=9), dict(ui, serverTime=14), dict(ui, revision=1)):
            self.assertIsNone(observe(dict(records, A=[view, bad]), 'A', 0, before, 10))
        self.assertIsNone(observe(dict(records, A=[dict(view, digest='wrong'), ui]), 'A', 0, before, 10))
        self.assertIsNone(observe(dict(records, Server=[view, dict(newer, skills=[dict(skill, id='replacement')])]), 'A', 0, before, 10))

    def test_three_profiles_accept_unique_s4_natural_labels_without_grants(self):
        for profile in ('content', 'evidence', 'compare'):
            try:
                plan = r36.commands('R36-S4-Arbitrary-' + uuid.uuid4().hex, 18436, 'S4_Content_Test', 0, profile, 'natural')
            except ValueError as error: self.fail(str(error))
            self.assertIn('-R36Scenario=natural', plan['Server'])
            for role, command in plan.items():
                self.assertIn('-R36Profile=' + profile, command)
                self.assertTrue(any(a.startswith('-R36Run=R36-S4-') for a in command))
                self.assertFalse(any('R36Fixture=' in a or 'R36Items=' in a or 'seed' in a.lower() for a in command))
                if role != 'Server': self.assertIn('-d3d12', command)

    def test_s4_rejects_unsafe_label_foreign_run_and_nonempty_natural(self):
        for label, run, count in (('R36-S4-../escape', 'S4_Test', 0), ('R36-S4-', 'S4_Test', 0),
                                  ('R36-S4-X', '', 0), ('R36-S4-X', 'S3_Old', 0),
                                  ('R36-S4-X', 'S4_Test', 1), ('R36-S4-X', 'S4_Test', True)):
            with self.subTest(label=label, run=run, count=count), self.assertRaises(ValueError):
                r36.commands(label, 18436, run, count, 'content', 'natural')

    def test_s4_stop_uses_fixed_saved_directory_and_exact_token(self):
        runner = R36RunnerTests().runner(); runner.args.label = 'R36-S4-NewPurpose-' + uuid.uuid4().hex
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.stop_token = uuid.uuid4().hex
        with patch.object(Path, 'open', unittest.mock.mock_open()) as opened:
            try: runner.close_server()
            except RuntimeError as error: self.fail(str(error))
            opened.assert_called_once_with('x', encoding='ascii')
            opened().write.assert_called_once_with(runner.stop_token)
        for token in ('0' * 32, 'a' * 31, 'a' * 33, '../stop', 'G' * 32):
            runner.stop_token = token
            with self.subTest(token=token), self.assertRaises(RuntimeError): runner.close_server()
        runner.stop_token = uuid.uuid4().hex; runner.output = r36.ROOT / 'Saved' / 'foreign'
        with self.assertRaises(RuntimeError): runner.close_server()

    def test_s4_natural_dispatch_does_not_reuse_s2_or_controlled_chain(self):
        for profile, method in (('content', 'content_actions'), ('evidence', 'source_actions'), ('compare', 'source_actions')):
            runner = R36RunnerTests().runner(); runner.args.label = 'R36-S4-NewPurpose-Test'
            runner.args.scenario = 'natural'; runner.args.profile = profile; runner.args.observe_only = False
            runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
            runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock()
            runner.natural_actions = Mock(); runner.controlled_actions = Mock(); runner.skill_actions = Mock()
            runner.content_actions = Mock(); runner.source_actions = Mock()
            runner.verify()
            getattr(runner, method).assert_called_once()
            runner.natural_actions.assert_not_called(); runner.controlled_actions.assert_not_called(); runner.skill_actions.assert_not_called()

    def test_direct_purchase_uses_published_effects_not_five_one(self):
        runner = R36RunnerTests().runner(); runner.args.label = 'R36-S4-NewPurpose-Test'
        runner.button_in_list = Mock(return_value={})
        before = dict(gold=200, strength=0, items=0, bag=0, equipment=[], skills=[], attack=30, incomeRate=1)
        runner.interaction_state = Mock(return_value=({}, dict(before, offers=[dict(id='new', price=10, kind=1, attackEffect=2, incomeEffect=0)])))
        after = dict(before, gold=191, attack=32, requestOffer='new', productItem='0' * 32)
        def action(label, source, **kwargs):
            self.assertTrue(kwargs['check'](before, after))
            self.assertFalse(kwargs['check'](before, dict(after, attack=35, incomeRate=2)))
            return after
        runner.action = Mock(side_effect=action)
        runner.purchase(0, 'New-Direct')

    def test_source_driver_has_separate_natural_four_source_chain(self):
        runner = R36RunnerTests().runner(); runner.args.profile = 'compare'; runner.args.scenario = 'natural'
        method = getattr(runner, 'source_actions', None)
        self.assertTrue(callable(method), 'S4四来源不能复用S2合成或全槽链')
        initial = dict(gold=200, items=0, bag=0, attack=30, incomeRate=1, strength=0)
        full = dict(initial, attack=52, incomeRate=6)
        off = dict(initial, attack=45, incomeRate=4)
        runner.interaction_state = Mock(side_effect=[({'canDrag': False, 'Advance': {}}, initial),
            ({}, initial), ({}, full), ({}, initial), ({}, off)])
        runner.pointer = Mock(); runner.wait = Mock(); runner.snapshot = Mock(); runner.advance_manual = Mock()
        runner.offer_index = Mock(side_effect=[0, 1, 2]); runner.purchase = Mock(side_effect=[dict(productItem='eq'), dict(productItem='book'), {}])
        runner.action = Mock(); runner.button_in_list = Mock(); runner.natural_actions = Mock()
        method()
        self.assertEqual([c.args[0] for c in runner.purchase.call_args_list], [0, 1, 2])
        self.assertEqual([c.args[0] for c in runner.action.call_args_list], ['S4-Equip', 'S4-Learn', 'S4-Talent', 'S4-Unequip'])
        runner.natural_actions.assert_not_called(); runner.pointer.assert_not_called()
        runner.advance_manual.assert_called_once_with()

    def test_content_driver_requires_empty_zero_and_preserves_ordered_wave_count(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'content_actions', None)
        self.assertTrue(callable(method), '需要独立自然多波输入链')
        initial = dict(gold=0, items=0, bag=0, attack=30, incomeRate=1, strength=0, health=300)
        runner.interaction_state = Mock(return_value=({}, initial))
        config = dict(orderedWaves=[dict(id='one'), dict(id='two'), dict(id='three'), dict(id='new-four')])
        runner.wave_state = Mock(return_value=config); runner.play_wave = Mock(); runner.content_purchase = Mock()
        runner.records = Mock(return_value={'Server': []}); runner.snapshot = Mock()
        with patch.object(r36, 'validate_wave_samples', create=True, return_value=True): method()
        self.assertEqual([c.args[0] for c in runner.play_wave.call_args_list], [0, 1, 2, 3])
        runner.content_purchase.assert_called_once()
        runner.interaction_state.return_value = ({}, dict(initial, gold=200))
        with self.assertRaises(AssertionError): method()

    def test_wave_wallet_waits_for_cleared_authority_revision_not_old_agreement(self):
        runner = r36.Runner(); runner.emit = Mock()
        before = dict(kind='VIEW', connection='a', revision=23, gold=18, incomeRate=1, experience=0)
        after = dict(before, revision=24, gold=318)
        old_ledger = dict(kind='ECONOMY', connection='a', revision=23, balance=18, rate=1,
                          totalIncome='18', totalSpending='0', fault=0)
        ledger = dict(old_ledger, revision=24, balance=318)
        wave = dict(kind='WAVE', cleared=True, completed=1, initialGold=0,
                    orderedWaves=[dict(rewardGold=300, rewardXP=0)])
        rows = [before, old_ledger, dict(wave, cleared=False, completed=0), after, ledger, wave]
        runner.records = Mock(return_value=dict(Server=rows))
        phase = {'fresh': False}
        def wait(predicate, description):
            if not phase['fresh']:
                self.assertIsNone(predicate(), '已一致但清波前的钱包必须继续等待')
                phase['fresh'] = True
            return predicate()
        runner.wait = wait
        with patch.object(r36, 'settled_ui', side_effect=lambda *a: ({}, after if phase['fresh'] else before)):
            result = runner.wave_wallets(wave)
        self.assertEqual(result['A']['full_wave_reward'], 300)
        self.assertEqual(result['B']['view']['revision'], 24)

    def test_reward_accounting_separates_initial_income_spending_and_each_full_reward(self):
        validate = getattr(r36, 'validate_wave_wallet', None)
        self.assertTrue(callable(validate), '需要独立自然清波账本，而非S2初始200断言')
        view = dict(connection='owner', revision=7, gold=585, incomeRate=1, experience=0)
        ledger = dict(connection='owner', revision=7, balance=585, rate=1, totalIncome='35', totalSpending='50', fault=0)
        self.assertEqual(validate(view, ledger, 0, 600), 600)
        for change in (dict(gold=285), dict(experience=1), dict(revision=8)):
            with self.assertRaises(AssertionError): validate(dict(view, **change), ledger, 0, 600)

    def test_wave_evidence_requires_pending_delayed_spawn_and_final_success(self):
        validate = getattr(r36, 'validate_wave_samples', None)
        self.assertTrue(callable(validate), '需要真实波次只读证据验证')
        config = dict(id='custom', prepare=15, spawns=[dict(definition='first', count=2, delay=0), dict(definition='late', count=1, delay=2)])
        monsters = [dict(actor='a', definition='first', health=0), dict(actor='b', definition='first', health=0)]
        rows = [dict(index=0, phase=2, time=20, phaseTime=20, pending=1, cleared=False, completed=0, monsters=monsters),
                dict(index=0, phase=2, time=22, phaseTime=20, pending=0, cleared=False, completed=0, monsters=monsters + [dict(actor='c', definition='late', health=90)]),
                dict(index=0, phase=3, result=1, time=30, phaseTime=30, pending=0, cleared=True, completed=1, monsters=monsters + [dict(actor='c', definition='late', health=0)])]
        self.assertTrue(validate(rows, [config]))
        for bad in ([dict(rows[0], cleared=True)] + rows[1:], [rows[0], dict(rows[1], time=21)] + rows[2:], rows[:-1]):
            with self.assertRaises(AssertionError): validate(bad, [config])


class R36S2DriverTests(unittest.TestCase):
    def test_s2_controlled_labels_keep_old_protocol(self):
        for label in ('R36-S2-Visible-Test', 'R36-S2-Runtime-Test'):
            try: plan = r36.commands(label, 18436, 'S2_Test', 500, 'evidence')
            except ValueError as error: self.fail(str(error))
            self.assertIn('-R36Items=500', plan['Server'])
            self.assertIn('-R36Fixture=controlled', plan['Server'])
            self.assertIn('-server', plan['Server'])
            for role in ('A', 'B'): self.assertIn('-d3d12', plan[role])

    def test_natural_cli_empty_inventory_and_no_controlled_grants(self):
        try:
            args = r36.parse_args(['--label', 'R36-S2-Visible-Natural', '--run', 'S2_Test',
                                   '--scenario', 'natural', '--profile', 'evidence', '--items', '0'])
        except SystemExit: self.fail('缺少明确S2自然空包入口')
        runner = R36RunnerTests().runner(); runner.args = args
        with patch.object(r36, 'scan_processes', return_value=[]), patch.object(r36, 'start_process', side_effect=[Mock(pid=p) for p in (1, 2, 3)]):
            runner.launch()
        self.assertIn('-R36Scenario=natural', runner.plan['Server'])
        self.assertFalse(any('R36Fixture=' in x or 'R36Items=' in x for x in runner.plan['Server']))
        self.assertFalse(any('seed' in x.lower() for command in runner.plan.values() for x in command))

    def test_natural_rejects_ambiguous_or_controlled_settings(self):
        base = ['--label', 'R36-S2-Visible-Natural', '--run', 'S2_Test', '--scenario', 'natural', '--profile', 'evidence', '--items', '0']
        for extra in (['--items', '500'], ['--profile', 'content'], ['--burst', '--observe-only'], ['--scroll-all'], ['--run', '']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): r36.parse_args(base + extra)
        with self.assertRaises(SystemExit): r36.parse_args(base + ['--label', 'R36-S1-UI-Natural'])

    def test_500_scroll_can_continue_to_incremental_without_changing_old_cli(self):
        try:
            args = r36.parse_args(['--label', 'R36-S2-Visible-500', '--run', 'S2_Test', '--items', '500', '--scroll-all', '--incremental'])
        except SystemExit: self.fail('500全滚动之后需要真实增量操作入口')
        self.assertFalse(args.observe_only); self.assertTrue(args.incremental)
        runner = R36RunnerTests().runner(); runner.args = args
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.scroll_bag = Mock()
        runner.controlled_actions = Mock(); runner.skill_actions = Mock()
        runner.verify()
        self.assertEqual(runner.scroll_bag.call_args_list, [unittest.mock.call('A'), unittest.mock.call('B')])
        runner.controlled_actions.assert_called_once(); runner.skill_actions.assert_not_called()

    def test_s2_stop_request_stays_in_own_saved_label(self):
        runner = R36RunnerTests().runner(); runner.args.label = 'R36-S2-Visible-Stop'
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.stop_token = uuid.uuid4().hex
        with patch.object(Path, 'open', unittest.mock.mock_open()) as opened:
            runner.close_server()
        opened.assert_called_once_with('x', encoding='ascii')


class R36S2InteractionTests(unittest.TestCase):
    runner = R36RunnerTests.runner

    def test_natural_dispatch_never_uses_controlled_actions(self):
        runner = self.runner(); runner.args.scenario = 'natural'; runner.args.observe_only = False
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.natural_actions = Mock()
        runner.controlled_actions = Mock(); runner.skill_actions = Mock()
        runner.verify()
        runner.natural_actions.assert_called_once(); runner.controlled_actions.assert_not_called()
        runner.skill_actions.assert_not_called()
        event = next(c for c in runner.emit.call_args_list if c.args[0] == 'NETWORK_INITIAL_PASS')
        self.assertFalse(event.kwargs['controlled']); self.assertTrue(event.kwargs['natural_purchase'])

    def test_unpainted_shop_child_scrolls_by_visible_sibling_order(self):
        for target, sibling, ticks in ((5, 1, -2), (0, 5, 2)):
            with self.subTest(target=target):
                runner = self.runner(); runner.args.timeout = 5
                name = 'Purchase_' + str(target)
                area = dict(laidOut=True, rect=[380, 121, 560, 303], x=470, y=212)
                cell = dict(laidOut=True, rect=[380, 160, 550, 208], x=465, y=184)
                before = dict(time=1, ShopList=area, **{name: dict(laidOut=False, rect=[0, 0, 0, 0]),
                              'Purchase_' + str(sibling): cell})
                after = dict(before, time=2, **{name: cell})
                runner.interaction_state = Mock(side_effect=[(before, {}), (after, {})])
                runner.wheel = Mock(); runner.wait = Mock(); runner.pointer = Mock()
                self.assertEqual(runner.button_in_list(name, 'ShopList'), cell)
                runner.wheel.assert_called_once_with('A', dict(before, bagGeometry=area, cells=[]), ticks)
                runner.pointer.assert_not_called()

    def test_clipped_shop_button_is_not_clickable_until_inside_list(self):
        function = getattr(r36, 'visible_control', None)
        self.assertTrue(callable(function), '需要真实裁剪边界判定，不能点击列表外按钮')
        ui = {'ShopList': dict(laidOut=True, rect=[500, 160, 750, 410]),
              'Purchase_3': dict(laidOut=True, rect=[500, 400, 750, 464], x=625, y=432)}
        self.assertFalse(function(ui, 'Purchase_3', 'ShopList'))
        ui['Purchase_3'].update(rect=[500, 300, 750, 364], y=332)
        self.assertTrue(function(ui, 'Purchase_3', 'ShopList'))

    def test_natural_delta_separates_spending_and_income(self):
        function = getattr(r36, 'economy_delta', None)
        self.assertTrue(callable(function), '需要明确自然收入与支出的观测分账')
        before = dict(gold=200, strength=0, attack=30, incomeRate=1)
        after = dict(gold=192, strength=0, attack=30, incomeRate=1)
        self.assertEqual(function(before, after, 10), 2)
        with self.assertRaises(AssertionError): function(before, dict(after, gold=189), 10)
        with self.assertRaises(AssertionError): function(before, dict(after, strength=1), 10)

    def test_material_action_uses_enabled_button_without_requiring_drag(self):
        runner = self.runner(); runner.args.scenario = 'natural'
        ui = dict(canDrag=False, Synthesize=dict(enabled=True, x=1))
        before = dict(expectedSeq=9, items=5, bag=5, equipment=[], skills=[])
        after = dict(before, code=0, items=1, bag=1)
        runner.interaction_state = Mock(side_effect=[(ui, before), ({}, before), ({}, after), ({}, before)])
        runner.pointer = Mock(); runner.snapshot = Mock()
        runner.action('Craft', lambda u: u['Synthesize'], check=lambda b, a: a['items'] == b['items'] - 4)
        event = next(c for c in runner.emit.call_args_list if c.args[0] == 'ACTION_PASS')
        self.assertFalse(event.kwargs['controlled']); self.assertTrue(event.kwargs['natural_purchase'])

    def test_500_equip_checks_actual_effect_not_old_fixture_ten(self):
        runner = self.runner(); runner.interaction_state = Mock(return_value=({'canDrag': True}, {}))
        ui = dict(cells=[dict(zone=0, type=7, laidOut=True, x=40, y=220, item='new', attackEffect=5, incomeEffect=1)])
        def action(label, source, target=None, **kwargs):
            if label == '02-Equip':
                source(ui)
                before = dict(items=500, bag=500, attack=30, incomeRate=1)
                after = dict(items=500, bag=499, attack=35, incomeRate=2, equipment=['new'], requestItem='new')
                self.assertTrue(kwargs['check'](before, after), '不能硬编码旧夹具攻击10')
            return {}
        runner.action = Mock(side_effect=action)
        runner.controlled_actions()

    def test_natural_purchase_driver_uses_real_button_and_product_receipt(self):
        runner = self.runner(); runner.button_in_list = Mock(return_value={})
        runner.interaction_state = Mock(return_value=({}, dict(gold=200, strength=0, offers=[dict(id='offer', price=10, kind=0)])))
        def action(label, source, **kwargs):
            self.assertEqual(source({'Purchase_0': 'actual-button'}), 'actual-button')
            b = dict(gold=200, strength=0, items=0, bag=0, equipment=[], skills=[], attack=30, incomeRate=1)
            a = dict(b, gold=192, items=1, bag=1, productItem='actual-product', requestOffer='offer')
            self.assertTrue(kwargs['check'](b, a))
            self.assertFalse(kwargs['check'](b, dict(a, attack=35)))
            return a
        runner.action = Mock(side_effect=action)
        method = getattr(runner, 'purchase', None)
        self.assertTrue(callable(method), '自然购买入口缺失')
        self.assertEqual(method(0, 'Buy')['productItem'], 'actual-product')
        runner.button_in_list.assert_called_with('Purchase_0', 'ShopList')

    def test_direct_kind_one_and_random_tier_kind_two_are_distinct(self):
        for kind in (1, 2):
            runner = self.runner(); runner.button_in_list = Mock(return_value={})
            b = dict(gold=200, strength=0, items=0, bag=0, equipment=[], skills=[], attack=30, incomeRate=1)
            runner.interaction_state = Mock(return_value=({}, dict(b, offers=[dict(id='o', price=50, kind=kind, attackEffect=5, incomeEffect=1)])))
            after = dict(b, gold=151, requestOffer='o', productItem='new')
            after.update(dict(attack=35, incomeRate=2) if kind == 1 else dict(items=1, bag=1))
            def action(label, source, **kwargs):
                self.assertTrue(kwargs['check'](b, after), 'DirectEffect=1，RandomTier=2')
                return after
            runner.action = Mock(side_effect=action)
            with self.subTest(kind=kind): runner.purchase(0, 'Buy')

    def test_natural_four_sources_requires_actual_buy_drag_talent_and_unequip(self):
        runner = self.runner(); runner.args.scenario = 'natural'; runner.pointer = Mock(); runner.wait = Mock()
        runner.advance_manual = Mock()
        initial = dict(gold=200, items=0, bag=0, attack=30, incomeRate=1, strength=0)
        runner.interaction_state = Mock(return_value=({'canDrag': False, 'Advance': 'start'}, initial))
        runner.purchase = Mock(side_effect=[dict(productItem='eq'), dict(productItem='book'), {}, {}])
        runner.action = Mock(return_value={}); runner.button_in_list = Mock(); runner.disabled_click = Mock()
        runner.craft_actions = Mock(); runner.full_slots_purchase = Mock()
        function = getattr(runner, 'natural_actions', None)
        self.assertTrue(callable(function), '自然四来源完整主流程缺失')
        function()
        self.assertEqual([c.args[0] for c in runner.purchase.call_args_list], [0, 1, 2, 3])
        self.assertEqual([c.args[0] for c in runner.action.call_args_list], ['Natural-Equip', 'Natural-Learn', 'Natural-Talent', 'Natural-Unequip'])
        runner.disabled_click.assert_called_once_with('Purchase_2', 'ShopList', 'Natural-Limit')
        runner.craft_actions.assert_called_once(); runner.full_slots_purchase.assert_called_once()
        runner.pointer.assert_not_called(); runner.advance_manual.assert_called_once_with()

    def test_craft_clicks_auto_synthesis_once_without_manual_selection(self):
        runner = self.runner(); runner.purchase = Mock()
        cells = [dict(zone=0, type=7, quality=0, item=str(i), laidOut=True, rect=[10, 10, 20, 20]) for i in range(1, 6)]
        ui = dict(cells=cells, bagGeometry=dict(laidOut=True, rect=[0, 0, 100, 100]))
        runner.interaction_state = Mock(return_value=(ui, {}))
        def action(label, source, **kwargs):
            b = dict(items=5, bag=5, equipment=[], skills=[], attack=45, incomeRate=4, gold=100, strength=0)
            a = dict(b, items=1, bag=1, productQuality=1, productItem='out')
            self.assertTrue(kwargs['check'](b, a)); self.assertFalse(kwargs['check'](b, dict(a, bag=2)))
            return a
        runner.action = Mock(side_effect=action)
        method = getattr(runner, 'craft_actions', None)
        self.assertTrue(callable(method), '真实一键合成流程缺失')
        method()
        self.assertEqual(runner.purchase.call_count, 4)
        self.assertEqual(runner.action.call_count, 1)
        self.assertFalse(hasattr(runner, 'select_material'))


class R36S2LifecycleDriverTests(unittest.TestCase):
    def args(self):
        try:
            return r36.parse_args(['--label', 'R36-S2-Visible-LifeTest', '--run', 'S2_Test',
                                   '--profile', 'evidence', '--items', '17', '--lifecycle'])
        except SystemExit: self.fail('缺少独立controlled生命周期入口')

    def test_explicit_server_only_lifecycle_and_no_mixed_natural(self):
        args = self.args(); runner = R36RunnerTests().runner(); runner.args = args
        with patch.object(r36, 'scan_processes', return_value=[]), patch.object(r36, 'start_process', side_effect=[Mock(pid=p) for p in (1, 2, 3)]):
            runner.launch()
        self.assertIn('-R36Lifecycle=controlled', runner.plan['Server'])
        for role in ('A', 'B'): self.assertFalse(any('R36Lifecycle' in x for x in runner.plan[role]))
        base = ['--label', args.label, '--run', args.run, '--lifecycle', '--profile', 'evidence']
        for extra in (['--scenario', 'natural', '--items', '0'], ['--scroll-all', '--incremental'], ['--observe-only'], ['--cooldown'], ['--run', '']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): r36.parse_args(base + extra)

    def test_lifecycle_dispatch_separate_from_purchase_and_skill_scenarios(self):
        runner = R36RunnerTests().runner(); runner.args = self.args()
        runner.wait = Mock(); runner.records = Mock(return_value={'Server': [], 'A': [], 'B': []})
        runner.locate = Mock(); runner.expand_panels = Mock(); runner.snapshot = Mock(); runner.lifecycle_actions = Mock()
        runner.natural_actions = Mock(); runner.controlled_actions = Mock(); runner.skill_actions = Mock()
        runner.verify(); runner.lifecycle_actions.assert_called_once()
        runner.natural_actions.assert_not_called(); runner.controlled_actions.assert_not_called(); runner.skill_actions.assert_not_called()

    def test_lifecycle_signal_cannot_write_other_run_or_arbitrary_action(self):
        runner = R36RunnerTests().runner(); runner.args = self.args()
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.stop_token = uuid.uuid4().hex
        method = getattr(runner, 'request_lifecycle', None)
        self.assertTrue(callable(method), '缺少仅本次令牌的生命周期请求')
        with patch.object(Path, 'open', unittest.mock.mock_open()) as opened:
            method('death'); opened.assert_called_once_with('x', encoding='ascii')
        for name in ('../stop', 'unknown', 'stop'):
            with self.subTest(name=name), self.assertRaises(ValueError): method(name)
        runner.output = r36.ROOT / 'Saved' / 'other'
        with patch.object(Path, 'open') as opened, self.assertRaises(ValueError): method('ended')
        opened.assert_not_called()

    def test_observation_waits_for_each_owner_running_ledger(self):
        runner = r36.Runner(); runner.emit = Mock(); runner.snapshot = Mock()
        clock = dict(phase=1, time=0)
        def state(role):
            t = clock['time']
            return ({'time': t}, {'health': 300}, {'inside': False},
                    dict(connection=role, balance=200+t, totalIncome=str(t), totalSpending='0',
                         rate=1, lastTime=t, remainder=0, phase=clock['phase'], fault=0))
        runner.lifecycle_state = state
        def wait(predicate, label):
            if label == '完整生命周期账本':
                clock.update(phase=1, time=0)
                self.assertFalse(predicate(), '另一Owner仍是Waiting时不能开始Running区间')
                clock['phase'] = 2
            else: clock.update(phase=2, time=3)
            return predicate()
        runner.wait = wait
        result = runner.lifecycle_observe('Preparing')
        self.assertEqual(result['B'][3]['totalIncome'], '3')

    def test_income_interval_validates_exact_ledger_not_only_gold_difference(self):
        check = getattr(r36, 'validate_income_interval', None)
        self.assertTrue(callable(check), '需要服务器收入支出与小数精确分账')
        before = dict(connection='a', balance=200, totalIncome='0', totalSpending='0', rate=5, lastTime=0, remainder=0, phase=2, fault=0)
        after = dict(before, balance=211, totalIncome='11', lastTime=2.25, remainder=.25)
        self.assertEqual(check(before, after)['income'], 11)
        for change in (dict(balance=212), dict(totalIncome='12'), dict(remainder=.5), dict(connection='b'), dict(totalSpending='1'), dict(rate=4), dict(fault=1)):
            with self.subTest(change=change), self.assertRaises(AssertionError): check(before, dict(after, **change))
        stopped = dict(after, phase=3, rate=0)
        self.assertEqual(check(stopped, stopped, stopped=True)['income'], 0)
        with self.assertRaises(AssertionError): check(stopped, dict(stopped, lastTime=3), stopped=True)


class R36DeliveryTraceTests(unittest.TestCase):
    def setUp(self):
        from copy import deepcopy
        record = dict(id='D', kill='K', item='I', match='M', join=1, definition='Equip', type=2,
                      quality=0, origin=[1, 2, 3], start=2., eta=3., state=0, cancel=0, fault=0,
                      retry=0, delay=0., deliveredTime=0.)
        self.before = dict(match='M', connection='C', join=1, revision=1, items=0, sampleTime=2.5,
                           deliveryEnabled=True, deliveryEnded=False, deliveryFault=0,
                           deliveries=[record], arrivals=[])
        self.after = deepcopy(self.before)
        self.after.update(revision=2, items=1, sampleTime=3.25, arrivals=[dict(item='I', delivery='D')])
        self.after['deliveries'][0].update(state=1, deliveredTime=3.25, delay=.25)

    def test_authoritative_transition(self):
        self.assertTrue(r36.validate_delivery_transition(self.before, self.after))

    def test_identity_not_definition_dedup(self):
        self.after['deliveries'][0]['id'] = 'other'
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))

    def test_recipient_cannot_change(self):
        self.after['join'] = 2
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))

    def test_eta_not_client_animation_time(self):
        self.after['deliveries'][0]['deliveredTime'] = 2.99
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))

    def test_retry_cannot_reroll_or_retime(self):
        for field, value in (('definition', 'Other'), ('item', 'Other'), ('eta', 4.)):
            old = self.after['deliveries'][0][field]
            self.after['deliveries'][0][field] = value
            self.assertFalse(r36.validate_delivery_transition(self.before, self.after))
            self.after['deliveries'][0][field] = old

    def test_fault_not_delivered(self):
        self.after['deliveries'][0].update(state=2, fault=22)
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))

    def test_duplicate_delivery_rejected(self):
        self.after['deliveries'].append(dict(self.after['deliveries'][0]))
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))

    def test_arrival_marker_required(self):
        self.after['arrivals'] = []
        self.assertFalse(r36.validate_delivery_transition(self.before, self.after))


class R36PresentationS3Tests(unittest.TestCase):
    def args(self, mode='drop', scenario='controlled', profile='content'):
        return ['--label', 'R36-S3-Visible-Python', '--run', 'S3_Integration', '--items', '0',
                '--presentation', mode, '--scenario', scenario, '--profile', profile]

    def test_s3_controlled_empty_and_labels(self):
        for prefix in ('Visible', 'Presentation'):
            label = 'R36-S3-' + prefix + '-Offline'
            plan = r36.commands(label, 18436, 'S3_Integration', 0)
            self.assertIn('-R36Items=0', plan['Server'])
            for role in ('Server', 'A', 'B'): self.assertIn('-R36Run=' + label, plan[role])

    def test_s3_natural_baseline_and_growth_compare(self):
        for profile in ('evidence', 'compare'):
            plan = r36.commands('R36-S3-Visible-Natural', 18436, 'S3_Integration', 0, profile, 'natural')
            self.assertIn('-R36Scenario=natural', plan['Server'])
            self.assertFalse(any('R36Fixture' in arg or 'R36Items' in arg or 'seed' in arg.lower() for arg in plan['Server']))

    def test_s3_natural_rejects_controlled_content_or_items(self):
        for profile, count, run in (('content', 0, 'S3'), ('evidence', 1, 'S3'), ('evidence', 0, '')):
            with self.assertRaises(ValueError): r36.commands('R36-S3-Visible-N', 18436, run, count, profile, 'natural')

    def test_s3_modes_parse(self):
        for mode in ('drop', 'disabled', 'region', 'death', 'growth'):
            args = self.args(mode, profile='compare' if mode == 'growth' else 'content')
            if mode == 'death': args.append('--lifecycle')
            self.assertEqual(r36.parse_args(args).presentation, mode)

    def test_s3_modes_reject_old_or_mixed_drivers(self):
        for extra in (['--burst'], ['--cooldown'], ['--observe-only'], ['--incremental'], ['--scroll-all'], ['--validate-assets']):
            with self.assertRaises(SystemExit): r36.parse_args(self.args() + extra)
        with self.assertRaises(SystemExit): r36.parse_args(self.args('death'))
        with self.assertRaises(SystemExit): r36.parse_args(self.args('growth', 'natural', 'content'))

    def test_stop_s3_keeps_owned_token(self):
        runner = r36.Runner(); runner.args = SimpleNamespace(label='R36-S3-Presentation-Stop-' + uuid.uuid4().hex)
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.output.mkdir()
        runner.stop_token = uuid.uuid4().hex; runner.emit = Mock(); runner.close_server()
        self.assertEqual((runner.output / 'stop.request').read_text(), runner.stop_token)

    def test_lifecycle_s3_uses_existing_guard(self):
        runner = r36.Runner(); runner.args = SimpleNamespace(label='R36-S3-Visible-Life-' + uuid.uuid4().hex,
            run='S3', lifecycle=True, scenario='controlled')
        runner.output = r36.ROOT / 'Saved' / runner.args.label; runner.output.mkdir()
        runner.stop_token = uuid.uuid4().hex; runner.emit = Mock(); runner.request_lifecycle('death')
        self.assertEqual((runner.output / 'death.request').read_text(), runner.stop_token)
        runner.args.scenario = 'natural'
        with self.assertRaises(ValueError): runner.request_lifecycle('ended')

    def test_growth_purchase_accounting_keeps_strength(self):
        try: value = r36.economy_delta(dict(gold=20, strength=3), dict(gold=11, strength=3), 10, allow_growth=True)
        except TypeError: self.fail('S3正常购买必须保留既有力量，不沿用S2零力量断言')
        self.assertEqual(value, 1)
        with self.assertRaises(AssertionError):
            r36.economy_delta(dict(gold=20, strength=3), dict(gold=11, strength=4), 10, allow_growth=True)

    def test_owner_training_scope_not_other_player(self):
        function = getattr(r36, 'training_scope', None); self.assertTrue(callable(function))
        own = dict(kind='SCOPE', authority=True, ownerLinked=True, inside=True, trainingKills=3, trainingAlive=0)
        records = dict(A=[dict(kind='VIEW', connection='a')], Server=[dict(kind='VIEW', connection='a'), own,
            dict(kind='VIEW', connection='b'), dict(own, trainingKills=100)])
        self.assertEqual(function(records, 'A')['trainingKills'], 3)

    def test_growth_instance_exact_source_and_untouched_duplicate(self):
        function = getattr(r36, 'validate_s3_growth', None); self.assertTrue(callable(function))
        rule = dict(target=0, attribute=0, accumulated=3, appliedCount='3')
        view = dict(match='M', join=1, attack=43, strength=3, equipment=['i'], growth=dict(strength=3, fault=0,
            playerRules=[dict(source='R36/M/P1/Equipment/i', accumulated=3, appliedCount='3')]),
            itemDetails=[dict(item='i', definition='S3Blade', growth=[rule]),
                         dict(item='j', definition='S3Blade', growth=[dict(rule, accumulated=0, appliedCount='0')])])
        self.assertTrue(function(view, 'i', 'j', 3, True))
        view['growth']['playerRules'][0]['source'] = 'R36/M/P2/Equipment/i'
        with self.assertRaises(AssertionError): function(view, 'i', 'j', 3, True)
        view['growth']['playerRules'][0]['source'] = 'R36/M/P1/Equipment/i'
        match = 'FB7248C94A9F2AF7DBF44F91FF90D869'; item = '9EAE46CE4BC39D513DD005BF364DB21B'
        view['match'] = match; view['equipment'][0] = item; view['itemDetails'][0]['item'] = item
        view['growth']['playerRules'][0]['source'] = 'R36/FB7248C9-4A9F-2AF7-DBF4-4F91FF90D869/P1/Equipment/9EAE46CE-4BC3-9D51-3DD0-05BF364DB21B'
        self.assertTrue(function(view, item, 'j', 3, True))
        view['match'] = 'M'; view['equipment'][0] = 'i'; view['itemDetails'][0]['item'] = 'i'
        view['growth']['playerRules'][0]['source'] = 'R36/M/P1/Equipment/i'
        view['itemDetails'][1]['growth'][0]['accumulated'] = 3
        with self.assertRaises(AssertionError): function(view, 'i', 'j', 3, True)

    def test_presentation_dispatch_before_legacy_actions(self):
        runner = R36RunnerTests().runner(); runner.args.presentation = 'drop'; runner.args.observe_only = False
        runner.wait = Mock(return_value=True); runner.records = Mock(return_value=dict(Server=[], A=[], B=[]))
        runner.snapshot = Mock(); runner.locate = Mock(); runner.expand_panels = Mock(); runner.presentation_actions = Mock(); runner.controlled_actions = Mock()
        runner.verify(); runner.presentation_actions.assert_called_once(); runner.controlled_actions.assert_not_called()


class R36S3ActionTests(unittest.TestCase):
    def test_growth_node_captures_both_instance_tooltips_without_transaction(self):
        runner = r36.Runner(); runner.emit = Mock(); runner.snapshot = Mock(); runner.pointer = Mock()
        cells = [dict(zone=1, item='i', laidOut=True, x=30, y=30), dict(zone=0, item='j', laidOut=True, x=60, y=60)]
        ui = dict(cells=cells); view = dict(items=0, bag=0, strength=0, attack=30, growth=dict(playerRules=[]), deliveries=[])
        runner.interaction_state = Mock(return_value=(ui, view))
        with patch.object(r36, 'validate_s3_growth', return_value=True): runner.s3_growth_node('Node', 'i', 'j', 3, True)
        self.assertEqual(runner.pointer.call_args_list, [unittest.mock.call('A', cells[0], button='hover'), unittest.mock.call('A', cells[1], button='hover')])
        self.assertEqual([c.args[0] for c in runner.snapshot.call_args_list], ['Node', 'Node-ItemDetails', 'Node-DuplicateDetails'])

    def test_training_wait_uses_actual_kills_and_leaves_before_next_spawn(self):
        runner = r36.Runner(); method = getattr(runner, 's3_train_kills', None); self.assertTrue(callable(method))
        runner.s3_region = Mock(); runner.emit = Mock()
        before = dict(trainingKills=4, trainingAlive=0); after = dict(trainingKills=7, trainingAlive=0)
        runner.records = Mock(return_value={})
        runner.wait = lambda predicate, *a, **k: predicate()
        with patch.object(r36, 'training_scope', side_effect=[before, after, after]): method(3)
        self.assertEqual([c.args[0] for c in runner.s3_region.call_args_list], [True, False])

    def test_region_input_requires_own_authoritative_scope(self):
        runner = r36.Runner(); method = getattr(runner, 's3_region', None); self.assertTrue(callable(method))
        runner.records = Mock(return_value={}); runner.pointer = Mock(); runner.emit = Mock()
        runner.interaction_state = Mock(return_value=(dict(time=1, Training=dict(laidOut=True)), {}))
        runner.wait = lambda predicate, *a, **k: predicate()
        with patch.object(r36, 'training_scope', side_effect=[dict(inside=False, time=1), dict(inside=True, time=2)]): method(True)
        runner.pointer.assert_called_once()

    def test_flights_always_on_without_toggle(self):
        runner = r36.Runner(); method = getattr(runner, 's3_toggle_flights', None); self.assertTrue(callable(method))
        view = dict(expectedSeq=1, response=0, items=0, bag=0)
        runner.interaction_state = Mock(return_value=(dict(time=1), view)); runner.pointer = Mock(); runner.emit = Mock()
        method(True); runner.pointer.assert_not_called()
        with self.assertRaises(AssertionError): method(False)
        runner.interaction_state = Mock(return_value=(dict(time=1, ToggleFlights=dict(laidOut=True)), view))
        with self.assertRaises(AssertionError): method(True)

    def test_drop_evidence_waits_for_real_transition_not_ui_alone(self):
        function = getattr(r36, 's3_delivery_evidence', None); self.assertTrue(callable(function))
        fixture = R36DeliveryTraceTests(); fixture.setUp()
        records = dict(Server=[dict(fixture.before, kind='VIEW')], A=[dict(fixture.after, kind='VIEW')],
                       B=[dict(kind='VIEW', connection='B', items=0, bag=0, strength=0, deliveries=[])])
        self.assertIsNone(function(records))

    def test_recorder_always_stops_on_failed_visible_action(self):
        runner = r36.Runner(); method = getattr(runner, 's3_capture', None); self.assertTrue(callable(method))
        recorder = Mock(); recorder.frames = dict(A=[], B=[])
        runner.helpers = SimpleNamespace(skill=SimpleNamespace(Recorder=Mock(return_value=recorder))); runner.emit = Mock()
        with self.assertRaisesRegex(RuntimeError, '真实失败'): method('S3', Mock(side_effect=RuntimeError('真实失败')))
        recorder.start.assert_called_once(); recorder.stop.assert_called_once()

    def test_growth_driver_uses_two_purchases_three_then_two_kills(self):
        runner = r36.Runner(); method = getattr(runner, 's3_growth', None); self.assertTrue(callable(method))
        runner.purchase = Mock(side_effect=[dict(productItem='i'), dict(productItem='j')]); runner.action = Mock()
        runner.s3_train_kills = Mock(); runner.s3_growth_node = Mock()
        method()
        self.assertEqual(runner.purchase.call_count, 2)
        self.assertEqual([c.args[0] for c in runner.s3_train_kills.call_args_list], [3, 2])
        self.assertEqual([(c.args[3], c.args[4]) for c in runner.s3_growth_node.call_args_list], [(0, True), (3, True), (3, False), (3, False), (3, True)])
        self.assertEqual(runner.action.call_count, 3)

    def test_drop_evidence_requires_authoritative_eta_and_private_owner(self):
        function = getattr(r36, 's3_delivery_evidence', None); self.assertTrue(callable(function))
        fixture = R36DeliveryTraceTests(); fixture.setUp()
        before = dict(fixture.before, kind='VIEW'); after = dict(fixture.after, kind='VIEW')
        other = dict(kind='VIEW', connection='B', items=0, bag=0, strength=0, deliveries=[])
        records = dict(Server=[before, after], A=[after, dict(kind='UI', flightGlyphs=[dict(laidOut=True)],
            cells=[dict(item='I', zone=0, flashRemaining=.5, laidOut=True)])], B=[other])
        result = function(records)
        self.assertTrue(result['flight_observed']); self.assertTrue(result['flash_observed'])
        records['B'].append(dict(other, items=1))
        with self.assertRaises(AssertionError): function(records)

    def test_drop_driver_and_initial_guard_exist(self):
        runner = r36.Runner(); method = getattr(runner, 'presentation_actions', None); self.assertTrue(callable(method))
        self.assertTrue(callable(getattr(runner, 's3_drop', None)))
        runner.args = SimpleNamespace(presentation='drop'); runner.interaction_state = Mock(return_value=({}, dict(items=1)))
        runner.s3_drop = Mock()
        with self.assertRaises(AssertionError): method()
        runner.s3_drop.assert_not_called()


class R36TimingTests(unittest.TestCase):
    def function(self, name):
        value = getattr(r36, name, None)
        self.assertTrue(callable(value), '缺少分段计时边界：' + name)
        return value

    def test_clock_alignment_measures_offset_not_assumes_equal(self):
        align = self.function('clock_alignment')
        rows = [dict(time=1000 + t, clockEnd=1000 + t + .002, qpc=str(int(t * 10000 + 10)),
                     secondsPerCycle=.0001, pid=7, event=i + 1) for i, t in enumerate((1, 2, 3))]
        value = align(rows)
        self.assertAlmostEqual(value['offset'], 1000, places=6)
        self.assertAlmostEqual(value['uncertainty'], .001, places=6)
        with self.assertRaises(AssertionError): align(rows + [dict(rows[-1], event=4, time=1004, clockEnd=1004.001)])
        with self.assertRaises(AssertionError): align([rows[0]])
        with self.assertRaises(AssertionError): align([rows[0], dict(rows[1], pid=8)])
        with self.assertRaises(AssertionError): align([rows[0], dict(rows[1], event=16384)])

    def test_clock_alignment_rejects_nonfinite_and_reverse(self):
        align = self.function('clock_alignment')
        good = dict(time=11, clockEnd=11.001, qpc='1000', secondsPerCycle=.001, pid=1, event=1)
        for change in (dict(time=float('nan')), dict(clockEnd=10), dict(secondsPerCycle=0), dict(qpc='broken')):
            with self.subTest(change=change), self.assertRaises((AssertionError, ValueError)):
                align([good, dict(good, event=2, **change)])

    def test_timing_mode_is_explicit_500_evidence_not_capacity(self):
        args = ['--label', 'R36-S4-Timing-Test', '--run', 'S4_Timing_Test', '--items', '500',
                '--profile', 'evidence', '--scroll-all', '--timing']
        try: parsed = r36.parse_args(args)
        except SystemExit: self.fail('缺少显式500专项入口')
        self.assertTrue(parsed.timing)
        for extra in (['--scenario', 'natural'], ['--items', '501'], ['--profile', 'content'], ['--observe-only']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): r36.parse_args(args + extra)
        self.assertIn('-R36Items=501', r36.commands('R36-S4-Other', 18436, 'S4_Other', 501)['Server'])

    def test_timing_layout_rejects_mixed_version_or_foreign_item(self):
        validate = self.function('timing_ui_complete')
        view = dict(connection='A', revision=4, response=0, bag=1, equipment=['0'] * 6,
                    itemDetails=[dict(item='own', location=0)])
        ui = dict(connection='A', revision=4, response=0, pending=False, total=1,
                  bagGeometry=dict(laidOut=True, rect=[0, 0, 100, 100]),
                  cells=[dict(zone=1, slot=i, item='0', laidOut=True) for i in range(6)] +
                        [dict(zone=2, slot=i, item='0', laidOut=True) for i in range(4)] +
                        [dict(zone=0, item='own', laidOut=True, rect=[0, 0, 10, 10])])
        self.assertTrue(validate(ui, view))
        for change in (dict(revision=3), dict(connection='B'), dict(total=2), dict(pending=True)):
            self.assertFalse(validate(dict(ui, **change), view))
        ui['cells'][-1]['item'] = 'foreign'
        self.assertFalse(validate(ui, view))

    def test_timing_segments_requires_exact_transfer_revision_and_request(self):
        segment = self.function('timing_segments')
        view = dict(connection='A', match='M', join=1, revision=4, response=1, seq=2, bag=1,
                    equipment=['0'] * 6, itemDetails=[dict(item='own', location=0)])
        def event(stage, t, start=0, **extra):
            return dict(kind='TIMING', stage=stage, time=t, start=start, connection='A', match='M', join=1,
                        revision=4, transfer='T', window=1, bytes=100, seq=2, **extra)
        ui = dict(kind='UI', time=203.5, connection='A', revision=4, response=1, pending=False, total=1,
                  bagGeometry=dict(laidOut=True, rect=[0, 0, 100, 100]),
                  cells=[dict(zone=1, slot=i, item='0', laidOut=True) for i in range(6)] +
                        [dict(zone=2, slot=i, item='0', laidOut=True) for i in range(4)] +
                        [dict(zone=0, item='own', laidOut=True, rect=[0, 0, 10, 10])])
        records = dict(Server=[event('REQUEST_ENTER', 100.8), event('COMMIT', 101, 100.9),
            event('ENCODE', 101.2, 101.1), event('SEND_WINDOW', 101.3), event('REQUEST_DONE', 101.4),
            event('ACK_WINDOW', 103)], A=[event('REQUEST_SENT', 200.5), event('RECEIVE_WINDOW', 202),
            event('ASSEMBLED', 202.1), event('PUBLISHED', 202.5, 202.2), event('UI_PRESENT', 203, 202.6), ui])
        clocks = dict(Server=dict(offset=100, uncertainty=0), A=dict(offset=200, uncertainty=0))
        result = segment(records, 'A', view, clocks, seq=2)
        self.assertAlmostEqual(result['seconds']['send_to_assembled_scheduled'], .8)
        self.assertAlmostEqual(result['seconds']['decode_validate_publish'], .3)
        self.assertAlmostEqual(result['seconds']['request_to_server_enter'], .3)
        self.assertAlmostEqual(result['seconds']['request_to_layout_observed'], 3)
        for stage, key, value in [('ASSEMBLED', 'transfer', 'wrong'), ('PUBLISHED', 'revision', 3),
                                  ('REQUEST_ENTER', 'seq', 3)]:
            bad = {r: [dict(row, **{key: value}) if row.get('stage') == stage else row for row in rows]
                   for r, rows in records.items()}
            with self.subTest(stage=stage), self.assertRaises(AssertionError): segment(bad, 'A', view, clocks, seq=2)

    def test_timing_coalesced_request_keeps_original_commit_boundary(self):
        segment = self.function('timing_segments')
        original = r36.timing_segments
        captured = []
        def collect(records, role, view, clocks, seq=None):
            if not captured: captured.append((records, role, view, clocks, seq))
            return original(records, role, view, clocks, seq)
        with patch.object(r36, 'timing_segments', collect):
            self.test_timing_segments_requires_exact_transfer_revision_and_request()
        records, role, view, clocks, seq = captured[0]
        records['Server'] = [dict(r, revision=3) if r['stage'] in ('COMMIT', 'REQUEST_DONE') else r for r in records['Server']]
        result = segment(records, role, view, clocks, seq)
        self.assertEqual(result['boundaries']['commit']['revision'], 3)
        self.assertEqual(result['revision'], 4)
        self.assertAlmostEqual(result['seconds']['server_enter_to_commit'], .2)

    def test_timing_book_search_scrolls_instead_of_assuming_first_page(self):
        runner = r36.Runner(); method = getattr(runner, 'timing_book', None)
        self.assertTrue(callable(method), '书不在当前页时需要真实滚动查找')
        def ui(offset, kind):
            return dict(offset=offset, time=offset + 1, bagGeometry=dict(laidOut=True, rect=[0, 0, 100, 100]),
                        cells=[dict(zone=0, type=kind, definition='D', item='I', laidOut=True, rect=[0, 0, 20, 20], x=10, y=10)])
        runner.args = SimpleNamespace(timeout=180)
        runner.interaction_state = Mock(side_effect=[(ui(0, 7), {}), (ui(6, 5), {})])
        runner.wheel = Mock(); runner.wait = Mock()
        self.assertEqual(method()['type'], 5)
        runner.wheel.assert_called_once()

    def test_timing_audit_distinguishes_bootstrap_from_500_snapshots(self):
        audit = self.function('timing_authority_view')
        first = dict(connection='A', match='M', join=1, revision=5, items=500)
        boot = dict(first, revision=1, items=0, bag=0, seq=0, response=0, itemDetails=[],
                    equipment=['0'] * 6, skills=[dict(id='0')] * 4)
        self.assertEqual(audit(boot, first, {}), 'bootstrap_not_sampled')
        full = dict(first, digest='D')
        self.assertEqual(audit(full, first, {('A', 5): full}), 'full_digest')
        for bad in (dict(boot, items=1), dict(boot, connection='B'), full):
            with self.assertRaises(AssertionError): audit(bad, first, {})
        with self.assertRaises(AssertionError): audit(full, first, {('A', 5): dict(full, digest='wrong')})

    def test_timing_authority_event_recovers_only_exact_digest(self):
        build = self.function('timing_authority_map')
        view = dict(kind='VIEW', connection='A', match='M', join=1, revision=7, digest='D', items=500)
        event = dict(view, kind='TIMING', stage='COMMIT')
        records = dict(Server=[event], A=[view], B=[])
        self.assertEqual(build(records)[('A', 7)], view)
        for bad in (dict(event, digest='other'), dict(event, connection='B'), dict(event, revision=8)):
            with self.assertRaises(AssertionError): build(dict(records, Server=[bad]))

    def test_timing_actions_native_equipment_purchase_and_delete(self):
        self.assertTrue(callable(getattr(r36.Runner, 'timing_actions', None)), '缺少实际鼠标增删驱动')
        self.assertTrue(callable(getattr(r36.Runner, 'timing_report', None)), '缺少精确边界报告')


class HUD08AdapterTests(unittest.TestCase):
    def ui(self):
        def rect(name, x, y, width=50, height=40):
            return dict(widget=name, visible=True, laidOut=True, enabled=True,
                        rect=[x, y, x + width, y + height], x=x + width / 2, y=y + height / 2)
        ui = dict(kind='UI', time=1, characterExpanded=False, backpackExpanded=False, shopExpanded=False,
                  panelsExpanded=False, panelsExpandedMeaning='backpack-only-compatibility',
                  canToggleCharacter=True, canToggleBackpack=True, canToggleShop=True, canDragBackpack=False,
                  cells=[dict(rect(f'EquipmentSlot_{i}', 10 + i * 55, 650), zone=1, slot=i) for i in range(6)]
                        + [dict(rect(f'SkillSlot_{i}', 400 + i * 55, 650), zone=2, slot=i) for i in range(4)])
        for i, name in enumerate(('ToggleCharacter', 'ToggleBackpack', 'ToggleShop', 'TopVitals', 'WaveCountdown')):
            ui[name] = rect(name, 700 + i * 80, 10)
        for name in ('CharacterPanel', 'BackpackPanel', 'ShopPanel', 'bagGeometry'):
            ui[name] = dict(visible=False, laidOut=False, rect=[0, 0, 0, 0])
        return ui

    def test_hud_schema_has_independent_panels_and_permanent_slots(self):
        function = getattr(r36, 'hud_record', None)
        self.assertTrue(callable(function), '缺少按PanelRecord真实schema归一化的证据')
        record = function(self.ui(), (1280, 720))
        self.assertFalse(record['panel']['panelsExpanded'])
        self.assertEqual(record['panel']['panelsExpandedMeaning'], 'backpack-only-compatibility')
        self.assertEqual(len(record['geometry']['equipment']), 6)
        self.assertEqual(len(record['geometry']['skills']), 4)
        self.assertIn('TopVitals', record['controls'])
        self.assertNotIn('TogglePanels', record['controls'])

    def test_hud_rejects_mislabelled_legacy_and_offscreen_slot(self):
        function = getattr(r36, 'hud_record', None); self.assertTrue(callable(function))
        ui = self.ui(); ui['panelsExpanded'] = True
        with self.assertRaises(AssertionError): function(ui, (1280, 720))
        ui = self.ui(); ui['cells'][0]['rect'][3] = 721
        with self.assertRaises(AssertionError): function(ui, (1280, 720))
        ui = self.ui(); ui['ShopPanel'] = dict(visible=True, laidOut=True, rect=[0, 600, 700, 720])
        with self.assertRaises(AssertionError): function(ui, (1280, 720))

    def test_independent_toggle_clicks_only_its_button_and_waits_fresh(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'set_panel', None); self.assertTrue(callable(method))
        before = self.ui(); after = dict(before, time=2, characterExpanded=True,
            CharacterPanel=dict(visible=True, laidOut=True, rect=[10, 100, 200, 400]))
        runner.records = Mock(side_effect=[{'A': [before]}, {'A': [after]}])
        runner.pointer = Mock(); runner.wait = lambda f, *a, **k: f()
        method('A', 'character', True)
        runner.pointer.assert_called_once_with('A', before['ToggleCharacter'])

    def test_toggle_never_collapses_another_open_panel(self):
        runner = R36RunnerTests().runner()
        method = getattr(runner, 'set_panel', None); self.assertTrue(callable(method))
        before = dict(self.ui(), characterExpanded=True)
        after = dict(before, time=2, characterExpanded=False, backpackExpanded=True, panelsExpanded=True,
                     bagGeometry=dict(laidOut=True), BackpackPanel=dict(laidOut=True))
        runner.records = Mock(side_effect=[{'A': [before]}, {'A': [after]}])
        runner.pointer = Mock(); runner.wait = lambda f, *a, **k: f()
        with self.assertRaises(AssertionError): method('A', 'backpack', True)

    def test_only_authoritative_automatic_never_sends_advance(self):
        for scenario, automatic in (('natural', True), ('controlled', True)):
            runner = R36RunnerTests().runner(); runner.args.scenario = scenario
            method = getattr(runner, 'advance_manual', None); self.assertTrue(callable(method))
            runner.pointer = Mock(); runner.interaction_state = Mock(return_value=(dict(automaticWaves=automatic), {}))
            runner.n = Mock()
            self.assertFalse(method())
            runner.pointer.assert_not_called(); runner.n.key.assert_not_called()

    def manual_runner(self, scenario='controlled', button=True):
        runner = R36RunnerTests().runner(); runner.args.scenario = scenario
        control = dict(laidOut=True, enabled=True, x=20, y=30)
        ui = dict(automaticWaves=False, waveClockSource='AuthoritySample', waveServerTime=0,
                  time=1, waveRevision=1, TopVitals=dict(laidOut=True, rect=[16,18,426,86], x=221, y=52))
        if button: ui['Advance'] = control
        runner.interaction_state = Mock(return_value=(ui, {}))
        runner.pointer = Mock(); runner.wave_state = Mock(side_effect=[dict(phase=0, index=-1, time=0), dict(phase=1, index=0, time=1)])
        runner.wait = lambda f, *a, **k: f()
        runner.owned_identity = Mock(); runner.owned = {'A': Mock()}; runner.windows = {'A': 7}
        runner.n = Mock(); runner.n.user.GetAsyncKeyState.return_value = 0
        return runner, ui

    def test_controlled_manual_retains_existing_button(self):
        runner, ui = self.manual_runner()
        self.assertTrue(runner.advance_manual())
        runner.pointer.assert_called_once_with('A', ui['Advance'])

    def test_natural_evidence_and_compare_still_advance_manual(self):
        for profile in ('evidence', 'compare'):
            runner, ui = self.manual_runner('natural'); runner.args.profile = profile
            self.assertTrue(runner.advance_manual())
            runner.pointer.assert_called_once_with('A', ui['Advance'])

    def test_manual_without_button_clicks_noninteractive_viewport_before_enter(self):
        runner, ui = self.manual_runner(button=False)
        order = Mock(); runner.pointer.side_effect = lambda *a, **k: order.pointer()
        runner.n.key.side_effect = lambda *a: order.key()
        self.assertTrue(runner.advance_manual())
        self.assertEqual(order.mock_calls, [unittest.mock.call.pointer(), unittest.mock.call.key()])
        runner.pointer.assert_called_once_with('A', ui['TopVitals'])
        runner.n.key.assert_called_once_with(0x0D)
        self.assertEqual(runner.wave_state.call_count, 2)

    def test_manual_unknown_policy_waits_without_input(self):
        for change in ({'automaticWaves': None}, {'automaticWaves': 0}, {'waveClockSource': 'Unknown'}, {'waveServerTime': -1}):
            runner, ui = self.manual_runner(); ui.update(change)
            def waiting(f, *a, **k):
                self.assertFalse(f()); raise TimeoutError('等待可靠策略')
            runner.wait = waiting
            with self.assertRaisesRegex(TimeoutError, '可靠策略'): runner.advance_manual()
            runner.pointer.assert_not_called(); runner.n.key.assert_not_called()

    def test_manual_waits_until_policy_arrives_and_blocks_auto(self):
        runner, ui = self.manual_runner(); ui['waveClockSource'] = 'Unknown'
        def waiting(f, *a, **k):
            self.assertFalse(f()); ui.update(waveClockSource='ObserverHeld', automaticWaves=True)
            return f()
        runner.wait = waiting
        self.assertFalse(runner.advance_manual())
        runner.pointer.assert_not_called(); runner.n.key.assert_not_called()

    def test_manual_enter_does_not_claim_success_without_authoritative_phase_change(self):
        runner, _ = self.manual_runner(button=False)
        runner.wave_state = Mock(return_value=dict(phase=0, index=-1, time=0))
        def waiting(f, description, **k):
            result = f()
            if not result: raise TimeoutError('权威阶段未改变')
            return result
        runner.wait = waiting
        with self.assertRaisesRegex(TimeoutError, '权威阶段未改变'): runner.advance_manual()

    def test_manual_refuses_missing_safe_viewport_geometry(self):
        runner, ui = self.manual_runner(button=False); ui['TopVitals']['laidOut'] = False
        with self.assertRaises(AssertionError): runner.advance_manual()
        runner.n.key.assert_not_called()

    def test_legacy_wave_entry_uses_manual_gate(self):
        runner = R36RunnerTests().runner()
        runner.interaction_state = Mock(return_value=({}, {})); runner.pointer = Mock()
        runner.advance_manual = Mock(side_effect=RuntimeError('推进门禁'))
        with self.assertRaisesRegex(RuntimeError, '推进门禁'): runner.play_wave(0)
        runner.pointer.assert_not_called()

    def test_legacy_burst_entry_uses_manual_gate(self):
        runner = R36RunnerTests().runner()
        runner.interaction_state = Mock(return_value=({'canDrag': False}, {})); runner.pointer = Mock()
        runner.advance_manual = Mock(side_effect=RuntimeError('推进门禁'))
        with self.assertRaisesRegex(RuntimeError, '推进门禁'): runner.burst_actions()
        runner.pointer.assert_not_called()

    def test_legacy_lifecycle_entry_uses_manual_gate(self):
        runner = R36RunnerTests().runner(); runner.lifecycle_rejected = Mock()
        runner.interaction_state = Mock(return_value=({}, {})); runner.pointer = Mock()
        runner.advance_manual = Mock(side_effect=RuntimeError('推进门禁'))
        with self.assertRaisesRegex(RuntimeError, '推进门禁'): runner.lifecycle_actions()
        runner.pointer.assert_not_called()

    def test_capture_emits_normalized_hud_for_each_new_schema_window(self):
        runner = R36RunnerTests().runner(); recorder = Mock(frames={})
        runner.helpers = SimpleNamespace(skill=SimpleNamespace(Recorder=Mock(return_value=recorder)))
        runner.records = Mock(return_value={'A': [self.ui()], 'B': [self.ui()], 'Server': []})
        runner.snapshot('schema')
        records = [c.kwargs for c in runner.emit.call_args_list if c.args[0] == 'HUD_SNAPSHOT']
        self.assertEqual({r['role'] for r in records}, {'A', 'B'})
        self.assertTrue(all(len(r['hud']['geometry']['equipment']) == 6 for r in records))

    def waves(self):
        configs = [dict(id='W1', prepare=2, spawns=[dict(count=2, delay=0), dict(count=1, delay=3)]),
                   dict(id='W2', prepare=2, spawns=[dict(count=1, delay=0)])]
        def state(key, started, start, spawned, alive, pending):
            return dict(key=key, id=f'W{key+1}', started=started, startTime=start, total=3 if key == 0 else 1,
                        spawned=spawned, alive=alive, pending=pending, rewarded=False)
        def row(t, index, deadline, states):
            return dict(kind='WAVE', time=t, automaticWaves=True, phase=2, index=index,
                        nextWaveIndex=index + 1 if index == 0 else -1, nextWaveDeadline=deadline, totalWaves=2,
                        completed=0, waveStates=states, totalAliveMonsters=sum(s['alive'] for s in states),
                        totalPendingSpawns=sum(s['pending'] for s in states))
        return configs, [row(2, 0, 4, [state(0, True, 2, 2, 2, 1), state(1, False, 0, 0, 0, 0)]),
                         row(4, 1, 0, [state(0, True, 2, 2, 2, 1), state(1, True, 4, 1, 1, 0)]),
                         row(5, 1, 0, [state(0, True, 2, 3, 3, 0), state(1, True, 4, 1, 1, 0)])]

    def test_automatic_overlap_and_delayed_old_wave_are_verified(self):
        function = getattr(r36, 'validate_automatic_wave_samples', None); self.assertTrue(callable(function))
        configs, rows = self.waves()
        result = function(rows, configs, require_overlap=True)
        self.assertEqual(result['started_waves'], 2)
        self.assertEqual(result['overlap']['waveStates'][0]['pending'], 1)

    def test_automatic_rejects_early_spawn_and_early_next_wave(self):
        function = getattr(r36, 'validate_automatic_wave_samples', None); self.assertTrue(callable(function))
        configs, rows = self.waves(); rows[1]['waveStates'][0].update(spawned=3, pending=0)
        with self.assertRaises(AssertionError): function(rows, configs)
        configs, rows = self.waves(); rows[1]['waveStates'][1]['startTime'] = 3.9
        with self.assertRaises(AssertionError): function(rows, configs)

    def test_automatic_content_dispatch_skips_manual_play_wave(self):
        runner = R36RunnerTests().runner(); runner.args.scenario = 'natural'
        runner.wave_state = Mock(return_value=dict(automaticWaves=True))
        runner.wait = lambda f, *a, **k: f()
        runner.automatic_actions = Mock(); runner.play_wave = Mock(); runner.interaction_state = Mock()
        runner.content_actions()
        runner.automatic_actions.assert_called_once(); runner.play_wave.assert_not_called()


if __name__ == '__main__': unittest.main()
