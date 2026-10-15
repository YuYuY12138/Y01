"""R34 可见验收纯测试；夹具不构成真实 UE 证据。"""
import copy
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).with_name('Test-Y01R34Visible.py')


def load():
    if not SCRIPT.exists(): return None
    spec = importlib.util.spec_from_file_location('r34_visible', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def attack(**values):
    return dict(dict(kind='ATTACK', Actor='Attack_1', Source='Player_1', Caster='Pawn_1', Target='Monster_1',
                     DS=10., Snapshot=30., SourceSession=1, TargetSession=2, Type=2,
                     Origin=(0, 0, 0), Direction=(1, 0, 0), Range=500, Angle=90, Radius=12,
                     Height=120, Speed=400, EndsAt=11.25), **values)


def hit(**values):
    return dict(dict(kind='HIT', Actor='Attack_1', Source='Player_1', Caster='Pawn_1', Target='Monster_1',
                     DS=10.5, Snapshot=30., Armor=0, BeforeHP=70, AfterHP=40, Actual=30,
                     SourceSession=1, TargetSession=2), **values)


def equipment(base=30, bonus=5, active=False, role='A', sample=1):
    return dict(owner='Controller_' + role, owner_guid='2' if role == 'A' else '8', role=role, run='fixture', bReady=True, bAutoAttackEnabled=True,
                ServerSampleTime=sample, AttackBase=base, AttackCurrent=base + bonus * active,
                Equipment=[dict(Row='EQ_0001', AttackBonus=bonus, Quality='Green', bBonusActive=True)]
                + [{} for _ in range(5)] if active else [{} for _ in range(6)])


class R34Regression(unittest.TestCase):
    def setUp(self): self.api = load()

    def function(self, name):
        function = getattr(self.api, name, None)
        self.assertTrue(callable(function), '缺少R34实现：' + name)
        return function

    def test_maps_are_explicit_persistent_copies(self):
        resolve = self.function('resolve_r34_map')
        self.assertEqual(resolve('main'), '/Game/Y01Playground/Maps/L_Y01Playground')
        self.assertIn('/R34Compare/', resolve('r34compare'))
        self.assertIn('/R34Ranged/', resolve('r34ranged'))
        for name in ('legacy', 'compare', '../main'):
            with self.assertRaises(ValueError): resolve(name)

    def test_parser_preserves_ds_and_vector_and_ignores_incomplete_tail(self):
        parse = self.function('parse_r34')
        line = '[2026.09.29-10.00.00:000][1]LogTemp: Display: Y01R34 ATTACK Actor=Attack_1 Source=P Caster=C Target=M DS=10.125 Snapshot=30 SourceSession=1 TargetSession=2 Type=1 Origin=X=1.000 Y=-2.000 Z=3.000 Direction=X=1.000 Y=0.000 Z=0.000 Range=200 Angle=90 Radius=200 Height=120 Speed=400 EndsAt=10.475\n'
        row = parse(line + line[:-10])[0]
        self.assertEqual(len(parse(line + line[:-10])), 1)
        self.assertEqual((row['DS'], row['Origin']), (10.125, (1., -2., 3.)))
        self.assertEqual(row['kind'], 'ATTACK')

    def test_parser_rejects_missing_or_nonfinite_ds(self):
        parse = self.function('parse_r34')
        for value in ('DS=nan', 'DS=inf', ''):
            with self.assertRaises(AssertionError): parse('Y01R34 EQ_REQUEST Source=P ' + value + ' Action=8\n')

    def test_actual_damage_armor_and_overkill(self):
        check = self.function('validate_hit')
        self.assertEqual(check(attack(), hit())['actual'], 30)
        self.assertEqual(check(attack(), hit(BeforeHP=10, AfterHP=0, Actual=10))['actual'], 10)
        self.assertEqual(check(attack(), hit(Armor=8, AfterHP=48, Actual=22))['actual'], 22)
        self.assertEqual(check(attack(), hit(Armor=40, AfterHP=70, Actual=0))['actual'], 0)

    def test_hit_rejects_wrong_snapshot_session_target_and_early_damage(self):
        check = self.function('validate_hit')
        for changes in (dict(Snapshot=31), dict(SourceSession=8), dict(Target='Other'), dict(DS=9),
                        dict(Actual=29), dict(AfterHP=39), dict(BeforeHP=float('nan'))):
            with self.assertRaises(AssertionError): check(attack(), hit(**changes))

    def test_melee_can_hit_two_targets_but_not_same_twice(self):
        check = self.function('validate_attack_hits')
        self.assertEqual(check(attack(Type=1), [hit(), hit(Target='Other')])['targets'], ['Monster_1', 'Other'])
        with self.assertRaises(AssertionError): check(attack(Type=1), [hit(), hit()])
        with self.assertRaises(AssertionError): check(attack(), [hit(), hit(Target='Other')])

    def test_intervals_use_ds_not_wall_clock(self):
        check = self.function('validate_intervals')
        rows = [attack(DS=t, t=100 + i * .1, Actor='Attack_' + str(i)) for i, t in enumerate((10, 11.02, 14.5))]
        result = check(rows, 1)
        self.assertAlmostEqual(result['minimum'], 1.02)
        self.assertAlmostEqual(result['maximum'], 3.48)
        with self.assertRaises(AssertionError): check([attack(DS=10), attack(DS=10.9)], 1)
        with self.assertRaises(AssertionError): check([attack()], 1)

    def test_delayed_group_is_not_stable_evidence(self):
        check = self.function('validate_intervals')
        rows = [attack(DS=40.859089441597462), attack(DS=42.991963442414999)]
        result = check(rows, 2)
        self.assertFalse(result['stable'])
        self.assertAlmostEqual(result['minimum'], 2.1328740008175373)

    def test_timing_categories_require_stable_evidence_and_check_every_group(self):
        check = self.function('validate_timing_groups')
        configs = {name: dict(Row=row, AttackInterval=interval) for name, row, interval in
                   [('P', 'Default', 1), ('T', 'TrainingDummy', 2), ('Short', 'TrainingDummy', 2), ('W', 'Wave1Minion', 1.5)]}
        rows = [attack(Source=name, DS=t) for name, times in
                [('P', (1, 2.02)), ('T', (1, 3.01)), ('Short', (1, 3.133)), ('W', (1, 2.52))] for t in times]
        expected = {'Default', 'TrainingDummy', 'Wave1Minion'}
        result = check(rows, configs, expected)
        self.assertEqual(len(result), 4)
        self.assertFalse(next(r for r in result if r['source'] == 'Short')['stable'])
        with self.assertRaises(AssertionError): check([r for r in rows if r['Source'] != 'T'], configs, expected)
        bad = copy.deepcopy(rows); bad[5]['DS'] = 2.99
        with self.assertRaises(AssertionError): check(bad, configs, expected)

    def test_equipment_chain_and_other_owner_unchanged(self):
        check = self.function('validate_equipment_chain')
        for base, bonus in ((30, 5), (36, 11)):
            states = [equipment(base, bonus, active, sample=i + 1) for i, active in enumerate((False, True, True, False))]
            peers = [equipment(base, bonus, role='B', sample=i + 1) for i in range(4)]
            self.assertEqual(check(states, peers, base, bonus)['current'], [base, base + bonus, base + bonus, base])
            states[2]['AttackCurrent'] += bonus
            with self.assertRaises(AssertionError): check(states, peers, base, bonus)

    def test_equipment_rejects_cross_owner_or_peer_contamination(self):
        check = self.function('validate_equipment_chain')
        states = [equipment(active=active, sample=i + 1) for i, active in enumerate((False, True, True, False))]
        peers = [equipment(role='B', sample=i + 1) for i in range(4)]
        for key, value in (('owner', 'Other'), ('run', 'Other'), ('AttackBase', 35)):
            bad = copy.deepcopy(states); bad[1][key] = value
            with self.assertRaises(AssertionError): check(bad, peers, 30, 5)
        peers[2]['AttackCurrent'] = 35
        with self.assertRaises(AssertionError): check(states, peers, 30, 5)

    def test_same_local_controller_names_require_distinct_netguids(self):
        check = self.function('validate_equipment_chain')
        states = [equipment(active=active, sample=i + 1) for i, active in enumerate((False, True, True, False))]
        peers = [equipment(role='B', sample=i + 1) for i in range(4)]
        for row in states + peers: row['owner'] = 'Controller_0'
        check(states, peers, 30, 5)
        for row in peers: row['owner_guid'] = '2'
        with self.assertRaises(AssertionError): check(states, peers, 30, 5)

    def test_equipment_export_omits_declared_green_default_only(self):
        check = self.function('validate_equipment_chain')
        states = [equipment(active=active, sample=i + 1) for i, active in enumerate((False, True, True, False))]
        peers = [equipment(role='B', sample=i + 1) for i in range(4)]
        for row in states[1:3]: del row['Equipment'][0]['Quality']
        check(states, peers, 30, 5)
        states[1]['Equipment'][0]['Quality'] = 'Blue'
        with self.assertRaises(AssertionError): check(states, peers, 30, 5)

    def test_batched_query_requires_terminal_sentinel(self):
        check = self.function('complete_batch')
        self.assertFalse(check('getall Y01GameState Snapshot\n.Health = (CurrentValue=70)\n'))
        self.assertFalse(check('.Snapshot = (Phase=Waiting)'))
        self.assertTrue(check('.Health = (CurrentValue=70)\n.Snapshot = (Phase=Waiting)\n'))

    def test_runner_reuses_native_safety_and_never_manual_attack(self):
        runner = self.function('R34Runner')
        self.assertEqual(runner.__bases__[0].__name__, 'Runner')
        for name in ('focus', 'record', 'click_point'): self.assertNotIn(name, runner.__dict__)
        check = self.function('validate_gameplay_keys')
        check([13, 87, 83, 65, 68])
        for keys in ([49], [50], [0x70]):
            with self.assertRaises(AssertionError): check(keys)

    def test_isolation_uses_both_caster_and_target_rooms(self):
        check = self.function('validate_rooms')
        positions = {'Pawn_1': (0, 10000, 0), 'Monster_1': (100, 10000, 0), 'Other': (100, 20000, 0)}
        self.assertEqual(check([hit()], positions)['hits'], 1)
        with self.assertRaises(AssertionError): check([hit(Target='Other')], positions)
        with self.assertRaises(AssertionError): check([hit(Target='Missing')], positions)

    def test_final_protection_rejects_changed_missing_and_added_files(self):
        check = self.function('validate_protected_snapshots')
        baseline = {'Content/Main.uasset': 'a' * 64, 'Source/Combat.cpp': 'b' * 64}
        self.assertEqual(check([baseline, dict(baseline)], dict(baseline)), 2)
        for changed in ({}, {**baseline, 'Source/Combat.cpp': 'c' * 64},
                        {**baseline, 'Content/Extra.uasset': 'd' * 64}):
            with self.assertRaises(AssertionError): check([baseline], changed)
            with self.assertRaises(AssertionError): check([baseline, changed], baseline)
        with self.assertRaises(AssertionError): check([], baseline)
        with self.assertRaises(AssertionError): check([{}], {})

    def test_ranged_speed_initial_same_tick_is_not_a_flight_segment(self):
        check = self.function('validate_r34_speed')
        commit = dict(kind='COMMIT', Actor='Attack_1', World=10., Origin=(0., 0., 0.))
        def step(previous, now, x):
            return dict(kind='STEP', Actor='Attack_1', World=now, PrevWorld=previous, SampleWorld=now,
                        PrevX=(previous - 10) * 400, PrevY=0., PrevZ=0., Position=(x, 0., 0.),
                        Mode=2, Phase=0, Terminal=0)
        events = [commit, step(10, 10, 0), step(10, 10.1, 40), step(10.1, 10.2, 80)]
        result = check(events, 'Attack_1')
        self.assertEqual(result['count'], 2)
        self.assertEqual(result['initial_zero_steps'], 1)
        self.assertAlmostEqual(result['mean_speed'], 400)
        for index, changes in ((1, dict(Position=(1., 0., 0.))), (2, dict(PrevWorld=10.1)),
                               (2, dict(PrevWorld=10.2)), (3, dict(Position=(90., 0., 0.)))):
            bad = copy.deepcopy(events); bad[index].update(changes)
            with self.assertRaises(AssertionError): check(bad, 'Attack_1')
        with self.assertRaises(AssertionError): check(events[:3], 'Attack_1')

    def test_projectile_rejects_instant_and_accepts_selected_single_hit(self):
        check = self.function('validate_ranged')
        events = [dict(kind='END', Actor='Attack_1', World=10.62, Phase=1),
                  dict(kind='STATE', Actor='Attack_1', World=10.5, Phase=1)]
        self.assertAlmostEqual(check(attack(), [hit()], events)['flight'], .5)
        with self.assertRaises(AssertionError): check(attack(), [hit(DS=10)], events)
        with self.assertRaises(AssertionError): check(attack(), [hit(Target='Other')], events)
        with self.assertRaises(AssertionError): check(attack(), [hit()], [])


if __name__ == '__main__': unittest.main(verbosity=2)
