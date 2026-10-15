"""R27 验证规则纯回归；不把构造数据当作GPU或联网通过。"""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('r27_tested', Path(__file__).with_name('Test-Y01R27Visible.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def snapshot():
    candidates = [dict(name=f'M{i}', distance=i + 1, eligible=True, in_view=True,
                       health=75, maximum=100, session=0, region=0, epoch='abc', dead=False,
                       x=i, y=0, z=100) for i in range(129)]
    leases = [dict(target=f'M{i}', slot=i, version=i + 1, visible=True, render_target=True,
                   fraction=.75, x=i, y=0, z=208, owner_view=True) for i in range(128)]
    return dict(mode='controlled', net=3, null_rhi=False, objects=128, components=128, active=128, visible=128,
                candidates=candidates, leases=leases, prewarm=32, cap=128, hold=.3,
                region=0, session=0, epoch='abc', generation=1)


class R27Tests(unittest.TestCase):
    def rejects(self, change, competition=True):
        row = snapshot(); change(row)
        with self.assertRaises(AssertionError): m.audit_snapshot(row, competition)

    def test_real_128_and_129_accepted(self):
        result = m.audit_snapshot(snapshot(), True)
        self.assertEqual(result.get('qualified'), 129)
        self.assertEqual(result.get('active'), 128)

    def test_spawn_count_not_qualified_count(self):
        self.rejects(lambda r: r['candidates'][-1].update(eligible=False))

    def test_all_129_must_be_in_view(self):
        self.rejects(lambda r: r['candidates'][-1].update(in_view=False))

    def test_129_must_be_alive(self):
        self.rejects(lambda r: r['candidates'][-1].update(health=0))

    def test_129_must_share_scope(self):
        self.rejects(lambda r: r['candidates'][-1].update(session=2))

    def test_fewer_leases_not_gpu_stress(self):
        self.rejects(lambda r: r.update(active=8, leases=r['leases'][:8], visible=8))

    def test_farthest_cannot_displace_nearest(self):
        self.rejects(lambda r: r['leases'][-1].update(target='M128'))

    def test_visible_and_render_target_required(self):
        self.rejects(lambda r: r['leases'][0].update(render_target=False))
        self.rejects(lambda r: r['leases'][0].update(visible=False))

    def test_no_old_owner_first_frame(self):
        self.rejects(lambda r: r['leases'][0].update(fraction=.2))
        self.rejects(lambda r: r['leases'][0].update(x=999))
        self.rejects(lambda r: r['leases'][0].update(owner_view=False))

    def test_duplicate_or_overcapacity_lease_rejected(self):
        self.rejects(lambda r: r['leases'][1].update(target='M0'))
        self.rejects(lambda r: r.update(objects=129))

    def test_ds_and_nullrhi_not_gpu(self):
        self.rejects(lambda r: r.update(net=1))
        self.rejects(lambda r: r.update(null_rhi=True))

    def test_nonfinite_distance_rejected(self):
        self.rejects(lambda r: r['candidates'][0].update(distance=float('nan')))

    def test_full_health_keeps_eligibility(self):
        row = snapshot()
        for c in row['candidates']: c['health'] = 100
        for lease in row['leases']: lease['fraction'] = 1
        self.assertEqual(m.audit_snapshot(row, True).get('active'), 128)

    def test_real_slots_and_versions_required(self):
        self.rejects(lambda r: r['leases'][1].update(slot=0))
        self.rejects(lambda r: r['leases'][0].update(version=0))
        self.rejects(lambda r: r['leases'][0].update(slot=128))

    def test_dead_lease_requires_actual_hold(self):
        self.rejects(lambda r: (r['candidates'][0].update(dead=True, health=0),
                               r['leases'][0].update(fraction=0)), False)

    def test_alive_lease_cannot_claim_death_hold(self):
        self.rejects(lambda r: r['leases'][0].update(holding=True, fraction=0), False)

    def test_component_count_must_be_measured(self):
        self.rejects(lambda r: r.pop('components'))

    def test_replacement_proves_slot_and_version_reuse(self):
        before = snapshot(); after = copy.deepcopy(before)
        after['candidates'][-1]['distance'] = .5
        after['leases'][-1].update(target='M128', version=129, x=128)
        audit = getattr(m, 'audit_replacement', lambda *args: {})
        self.assertEqual(audit(before, after, 'M128').get('evicted'), 'M127')
        for change in (dict(version=128), dict(slot=0)):
            bad = copy.deepcopy(after); bad['leases'][-1].update(change)
            with self.assertRaises(AssertionError): audit(before, bad, 'M128')

    def death_rows(self):
        rows = []
        for i in range(10):
            row = snapshot(); row['world'] = i * .05
            if i:
                row['candidates'][0].update(dead=True, health=0)
                row['leases'][0].update(holding=True, fraction=0)
            if i >= 7:
                row['leases'] = row['leases'][1:]; row['active'] -= 1; row['visible'] -= 1
            rows.append(row)
        return rows

    def test_death_hold_needs_observed_interval_not_final_absence(self):
        audit = getattr(m, 'audit_death_hold', lambda *args: {})
        result = audit(self.death_rows(), 'M0')
        self.assertAlmostEqual(result.get('observed_seconds', -1), .3)
        with self.assertRaises(AssertionError): audit(self.death_rows()[7:], 'M0')
        bad = self.death_rows()
        for row in bad[2:]:
            row['leases'] = [x for x in row['leases'] if x['target'] != 'M0']
        with self.assertRaises(AssertionError): audit(bad, 'M0')

    def test_controlled_observation_never_sends_foreground_input(self):
        runner = object.__new__(m.R27Runner)
        runner.args = SimpleNamespace(mode='controlled'); runner.n = SimpleNamespace(hold=Mock())
        pause = getattr(runner, 'pause', lambda seconds: runner.n.hold(seconds))
        with patch.object(m.time, 'sleep') as sleep:
            pause(.2)
            runner.n.hold.assert_not_called(); sleep.assert_called_once_with(.2)
        runner.args.mode = 'natural'; pause(.2); runner.n.hold.assert_called_once_with(.2)

    def test_foreign_ue_is_never_taken_over(self):
        guard = getattr(m, 'assert_idle', lambda rows: None)
        guard([])
        with self.assertRaisesRegex(RuntimeError, '7788'):
            guard([dict(ProcessId=7788, Name='UnrealEditor.exe', CommandLine='user project')])

    def natural_rows(self):
        rows = []
        for region, eligible, health in ((1, False, 100), (1, True, 75), (0, False, 100), (0, True, 75)):
            row = snapshot(); row.update(region=region, session=region, active=int(eligible), visible=int(eligible))
            row['candidates'] = [dict(row['candidates'][0], region=region, session=region, eligible=eligible, health=health)]
            row['leases'] = row['leases'][:1] if eligible else []
            rows.append(row)
        return rows

    def test_natural_requires_first_damage_and_leave_reclamation(self):
        audit = getattr(m, 'audit_natural_states', lambda rows: {})
        result = audit(self.natural_rows())
        self.assertEqual(result.get('leave_count'), 1)
        self.assertEqual(set(result.get('first_damage_regions', [])), {0, 1})
        bad = self.natural_rows(); bad[2]['leases'] = bad[1]['leases']; bad[2].update(active=1, visible=1)
        with self.assertRaises(AssertionError): audit(bad)
        with self.assertRaises(AssertionError): audit(self.natural_rows()[1:])

    def test_original_frame_must_be_verified_and_time_aligned(self):
        pick = getattr(m, 'frame_for_snapshot', lambda *args: {})
        frame = dict(path='raw.png', start_ns=990000000, end_ns=1010000000, verified=True)
        self.assertEqual(pick(dict(wall_ns=1000000000), [frame]).get('path'), 'raw.png')
        with self.assertRaises(AssertionError): pick(dict(wall_ns=2000000000), [frame])
        with self.assertRaises(AssertionError): pick(dict(wall_ns=1000000000), [dict(frame, verified=False)])

    def test_late_join_after_damage_and_heal(self):
        server = dict(target='M0', damage_ns=10, heal_ns=20, actual=25, health=100, maximum=100, epoch='abc')
        client = dict(target='M0', first_seen_ns=40, health=100, maximum=100, eligible=True, visible=True, epoch='abc')
        self.assertTrue(m.audit_late_join(server, client, 30).get('snapshot_qualification'))
        for changed in (dict(heal_ns=35), dict(actual=0)):
            with self.assertRaises(AssertionError): m.audit_late_join(dict(server, **changed), client, 30)
        with self.assertRaises(AssertionError): m.audit_late_join(server, dict(client, epoch='other'), 30)

    def test_dynamic_names_must_use_real_netguid(self):
        row = snapshot()
        row['candidates'] = [dict(row['candidates'][0], name='Y01Monster_8')]
        row['leases'] = [dict(row['leases'][0], target='Y01Monster_8')]
        normalize = getattr(m, 'canonical_snapshot', lambda r, s, c: r)
        result = normalize(row, {'R27_Target_000': '42', 'WrongSameSuffix': '90'}, {'Y01Monster_8': '42'})
        self.assertEqual(result['candidates'][0]['name'], 'R27_Target_000')
        self.assertEqual(result['leases'][0]['target'], 'R27_Target_000')
        self.assertEqual(result['candidates'][0]['local_name'], 'Y01Monster_8')
        self.assertEqual(result['candidates'][0]['netguid'], '42')
        self.assertEqual(row['candidates'][0]['name'], 'Y01Monster_8')

    def test_parser_complete_json_only(self):
        row = dict(kind='snapshot', epoch='001234', generation=9007199254740993)
        line = '[2026.09.30-01.00.00:000][  1]LogTemp: Display: Y01R27 ' + json.dumps(row) + '\n'
        self.assertEqual(m.parse_rows(line + line[:-1]), [row])

    def test_natural_never_controls_gameplay(self):
        for role in ('Server', 'A', 'B'):
            args = m.launch_command(role, 18527, Path('A.log'), 'natural', Path('command.txt'))
            self.assertIn('-Y01R27Trace', args)
            self.assertNotIn('-Y01R27Verify', args)
            if role != 'Server':
                self.assertIn('-d3d12', args); self.assertNotIn('-nullrhi', args)

    def test_controlled_explicit_not_damage_stress(self):
        args = m.launch_command('Server', 18527, Path('S.log'), 'controlled', Path('command.txt'))
        self.assertIn('-Y01R27Verify', args)
        self.assertNotIn('-Y01DamageNetVerify', args)


if __name__ == '__main__': unittest.main(verbosity=2)
