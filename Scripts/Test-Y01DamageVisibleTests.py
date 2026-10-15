"""伤害数字薄驱动纯测试，不启动UE、不发送输入。"""
import importlib.util
from pathlib import Path
import tempfile
import json
import unittest

PATH = Path(__file__).with_name('Test-Y01DamageVisible.py')


class DamageVisibleTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(PATH.is_file(), '尚未实现伤害数字薄驱动')
        spec = importlib.util.spec_from_file_location('damage_visible_tested', PATH)
        self.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.m)

    def line(self, kind, fields='', time='00:000', family='Y01DamageNumber'):
        return f'[2026.09.29-10.00.{time}][  1]LogTemp: Display: {family} {kind} {fields}\n'

    def test_parser_preserves_identity_and_ignores_partial(self):
        text = self.line('BORROW', 'Id=9007199254740993 Epoch=001234 Region=0 Actual=2.5 Slot=0 Serial=1')
        rows = self.m.parse_damage(text + self.line('RETURN', 'Id=5')[:-1])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['Id'], 9007199254740993)
        self.assertEqual(rows[0]['Epoch'], '001234')
        self.assertEqual(rows[0]['Actual'], 2.5)
        self.assertGreater(rows[0]['t'], 0)

    def pool(self):
        return (self.line('PREWARM', 'Count=32 Cap=128') +
                self.line('BORROW', 'Id=1 Epoch=abc Region=0 Slot=0 Serial=1 Oldest=0 Actual=30') +
                self.line('RETURN', 'Id=1 Serial=1 Age=0.81', '00:810') +
                self.line('BORROW', 'Id=2 Epoch=abc Region=0 Slot=0 Serial=2 Oldest=0 Actual=10', '01:000') +
                self.line('RETURN', 'Id=2 Serial=2 Age=0.80', '01:800') +
                self.line('RELEASE', 'Count=32', '02:000'))

    def test_pool_repeated_slot_stable_32(self):
        result = self.m.pool_audit(self.m.parse_damage(self.pool()))
        self.assertEqual(result['release_counts'], [32])
        self.assertEqual(result['repeated_slots'], [0])
        self.assertEqual(result['natural_returns'], 2)

    def test_pool_rejects_unmatched_return(self):
        with self.assertRaises(AssertionError):
            self.m.pool_audit(self.m.parse_damage(self.pool().replace('Id=1 Serial=1 Age', 'Id=3 Serial=1 Age')))

    def test_pool_capacity_and_oldest_simulation(self):
        text = self.line('PREWARM', 'Count=32 Cap=128')
        for slot in range(128):
            text += self.line('BORROW', f'Id={slot+1} Slot={slot} Serial={slot+1} Oldest=0')
        text += self.line('RETURN', 'Id=1 Serial=1 Age=0.01')
        text += self.line('BORROW', 'Id=129 Slot=0 Serial=129 Oldest=1')
        for slot in range(1, 128):
            text += self.line('RETURN', f'Id={slot+1} Serial={slot+1} Age=0.81')
        text += self.line('RETURN', 'Id=129 Serial=129 Age=0.80')
        text += self.line('RELEASE', 'Count=128')
        result = self.m.pool_audit(self.m.parse_damage(text))
        self.assertEqual(result['max_active'], 128)
        self.assertEqual(result['oldest_reuses'], 1)
        with self.assertRaises(AssertionError):
            self.m.pool_audit(self.m.parse_damage(text.replace('Id=129 Slot=0', 'Id=129 Slot=4')))

    def test_link_requires_matching_actual_display_and_type(self):
        fields = 'Player=7 Id=1 Epoch=abc Region=0 Actual=25 Display=35 Type=Damage'
        server = self.m.parse_damage(self.line('SEND', fields, family='Y01Damage'))
        client = self.m.parse_damage(self.line('RECEIVE', fields, family='Y01Damage') +
                                     self.line('BORROW', fields + ' Slot=0 Serial=1'))
        self.assertEqual(self.m.delivery_audit(server, {'A': client})['deliveries'], 1)
        for field, wrong in (('Actual', 24), ('Display', 34), ('Type', 'Healing')):
            original = client[-1][field]
            client[-1][field] = wrong
            with self.assertRaises(AssertionError, msg=f'{field}不一致必须失败'):
                self.m.delivery_audit(server, {'A': client})
            client[-1][field] = original

    def test_link_accepts_effective_healing_payload(self):
        fields = 'Player=7 Id=2 Epoch=abc Region=0 Actual=0 Display=25 Type=Healing'
        server = self.m.parse_damage(self.line('SEND', fields, family='Y01Damage'))
        client = self.m.parse_damage(self.line('RECEIVE', fields, family='Y01Damage') +
                                     self.line('BORROW', fields + ' Slot=0 Serial=1'))
        self.assertEqual(self.m.delivery_audit(server, {'A': client})['deliveries'], 1)

    def test_frames_are_candidates_not_visual_pass(self):
        event = dict(t=10.0, Id=1, Epoch='abc', Serial=1)
        frames = [dict(path='a.png', start_ns=10_100_000_000, end_ns=10_110_000_000),
                  dict(path='b.png', start_ns=11_100_000_000, end_ns=11_110_000_000)]
        linked = self.m.frame_candidates(event, frames)
        self.assertEqual([f['path'] for f in linked], ['a.png'])
        self.assertAlmostEqual(linked[0]['since_borrow_ms'], 100)

    def test_hit_link_accepts_flush_delay_but_requires_same_transaction(self):
        self.assertTrue(callable(getattr(self.m, 'matching_hit', None)), '缺少按日志事务边界关联')
        text = self.line('COMMIT', 'Id=1 Actual=30', family='Y01Damage')
        text += self.line('HIT', 'BeforeHP=40 AfterHP=10 Actual=30', '00:011', 'Y01R34')
        rows = self.m.parse_damage(text)
        self.assertEqual(self.m.matching_hit(rows, rows[0])['AfterHP'], 10)
        with self.assertRaises(AssertionError):
            self.m.matching_hit(self.m.parse_damage(text.replace('AfterHP=10', 'AfterHP=9')), rows[0])
        blocked = self.m.parse_damage(text.splitlines(True)[0] +
            self.line('COMMIT', 'Id=2 Actual=30', family='Y01Damage') + text.splitlines(True)[1])
        with self.assertRaises(AssertionError): self.m.matching_hit(blocked, blocked[0])
        other_frame = self.m.parse_damage(text.replace('[  1]LogTemp: Display: Y01R34', '[  2]LogTemp: Display: Y01R34'))
        with self.assertRaises(AssertionError): self.m.matching_hit(other_frame, other_frame[0])

    def test_existing_evidence_load_does_not_launch_or_overwrite_result(self):
        self.assertTrue(callable(getattr(self.m, 'load_existing', None)), '缺少原始证据重审入口')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'result.json').write_text('{"scenario":"natural","code":1}', encoding='utf-8')
            for role in ('Server', 'A', 'B'): (root / (role + '.log')).write_text(role, encoding='utf-8')
            folder = root / 'sequence'; folder.mkdir()
            (folder / 'A-frames.jsonl').write_text(json.dumps(dict(path='original.png', start_ns=1, end_ns=2)) + '\n', encoding='utf-8')
            runner = self.m.load_existing(root)
            self.assertEqual(runner.text('Server'), 'Server')
            self.assertEqual(runner.args.scenario, 'natural')
            self.assertEqual(runner.records['sequence'].frames['A'][0]['path'], 'original.png')
            self.assertEqual(runner.records['sequence'].frames['B'], [])
            self.assertEqual(json.loads((root / 'result.json').read_text())['code'], 1)

    def test_filtered_diagnostics_remain_readonly(self):
        self.assertTrue(callable(getattr(self.m, 'slot_diagnostic', None)), '缺少活跃条目精确诊断')
        command = self.m.slot_diagnostic(0)
        self.m.validate_diagnostic(command)
        for field in ('RenderTarget', 'RelativeLocation', 'RelativeRotation', 'bVisible', 'Owner', 'ViewTarget', 'RenderOpacity', 'Text'):
            self.assertIn(field, command)
        for bad in ('getall Actor Owner NAME=Actor_0 | quit', 'set Actor_0 Owner None',
                    'getall Actor Owner NAME=Actor_0;quit', 'getall Actor Owner OUTER=../Other'):
            with self.assertRaises(AssertionError): self.m.validate_diagnostic(bad)

    def test_stress_capture_is_bounded_and_flush_does_not_throttle_burst(self):
        self.assertNotIn('-forcelogflush', self.m.launch_command('A', 18498, Path('A.log'), 'stress'))
        self.assertNotIn('-forcelogflush', self.m.launch_command('Server', 18498, Path('S.log'), 'stress'))
        captures = []
        fake = type('Run', (), {})()
        fake.record = lambda label, action, seconds: captures.append((label, seconds))
        fake.text = lambda role: 'Y01Damage NET_DONE' if len(captures) == 3 else ''
        fake.diagnostics = lambda role: None
        fake.emit = lambda *a, **k: None
        self.m.DamageRunner.stress(fake)
        self.assertEqual(len(captures), 3)
        self.assertTrue(all(seconds <= 2 for label, seconds in captures))

    def test_diagnose_leaves_training_and_drains_before_cleanup(self):
        self.assertTrue(callable(getattr(self.m.DamageRunner, 'finish_training', None)), '诊断缺少离房及借还收齐步骤')
        calls = []
        fake = type('Run', (), {})()
        fake.focus = lambda role: calls.append(('focus', role))
        fake.n = type('Native', (), {})()
        fake.n.click_training = lambda inside: calls.append(('training', inside))
        fake.n.hold = lambda seconds: calls.append(('hold', seconds))
        fake.telemetry = lambda role: {'Inside': 0}
        def wait(predicate, label, timeout):
            self.assertTrue(predicate(), label)
            calls.append(('wait', label))
        fake.wait = wait
        server = self.line('SEND', 'Player=7 Id=1 Epoch=abc Region=1 Actual=30 Display=30 Type=Damage', family='Y01Damage')
        client = self.line('RECEIVE', 'Player=7 Id=1 Epoch=abc Region=1 Actual=30 Display=30 Type=Damage', family='Y01Damage')
        client += self.line('BORROW', 'Id=1 Epoch=abc Region=1 Actual=30 Display=30 Type=Damage Serial=1')
        client += self.line('RETURN', 'Id=1 Serial=1 Age=0.8')
        texts = {'Server': server, 'A': client, 'B': ''}
        fake.text = lambda role: texts[role]
        fake.emit = lambda kind, **data: calls.append(('emit', kind))
        self.m.DamageRunner.finish_training(fake)
        self.assertIn(('training', 0), calls)
        self.assertIn(('hold', 1.2), calls)
        self.assertIn(('emit', 'TRAINING_DRAINED'), calls)
        texts['A'] = client.replace('Y01DamageNumber RETURN', 'Ignored RETURN')
        with self.assertRaises(AssertionError): self.m.DamageRunner.finish_training(fake)
        texts['A'] = client
        texts['Server'] += self.line('SEND', 'Player=7 Id=2 Epoch=abc Region=1 Actual=5 Display=5 Type=Damage', family='Y01Damage')
        with self.assertRaises(AssertionError): self.m.DamageRunner.finish_training(fake)
        fake.record = lambda label, action, seconds: calls.append(('record', label))
        fake.finish_training = lambda: calls.append(('finish', True))
        fake.keys = []
        self.m.DamageRunner.diagnose(fake)
        self.assertLess(calls.index(('record', 'Diagnose-Training-Clean')), calls.index(('finish', True)))

    def test_launch_natural_has_no_network_injection_or_manual_skill(self):
        for role in ('Server', 'A', 'B'):
            command = self.m.launch_command(role, 18497, Path('G:/temp/A.log'), 'natural')
            text = ' '.join(command)
            self.assertIn('-Y01DamageTrace', command)
            self.assertNotIn('-Y01DamageNetVerify', command)
            if role != 'Server':
                self.assertIn('-d3d12', command)
                self.assertNotIn('-nullrhi', command)
            self.assertNotIn('showhud', text)
        command = self.m.launch_command('Server', 18498, Path('G:/temp/S.log'), 'stress')
        self.assertIn('-Y01DamageNetVerify', command)
        self.assertIn('-Y01DamageBurstEvents=128', command)


if __name__ == '__main__':
    unittest.main(verbosity=2)
