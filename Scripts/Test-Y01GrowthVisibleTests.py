"""成长验收器纯回归；夹具仅在测试内，不启动UE或操作桌面。"""
import copy
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).with_name('Test-Y01GrowthVisible.py')


def load():
    if not SCRIPT.exists():
        return None
    spec = importlib.util.spec_from_file_location('growth_visible', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sample(seconds=3, kills=0, rate=1, world=None):
    return dict(owner='Controller_A', role='A', run='fixture', bReady=True,
                ServerSampleTime=float(seconds + 10 if world is None else world),
                Health=200, MaxHealth=200, AttackBase=20 + seconds * rate + kills * 2,
                AttackCurrent=25 + seconds * rate + kills * 2, Gold=seconds + kills * 10, XP=kills * 5,
                Error='', ProfilePath='fixture', MoveSpeed=600,
                Training=dict(bInside=False, Room=0, Session=0, Kills=kills, Alive=0),
                Growth=dict(Config=dict(bEnabled=True, AttackPerEffectiveSecond=rate,
                                        AttackPerEffectiveKill=2, GoldPerEffectiveSecond=1),
                            StopReason='运行中', Error='', EffectiveSeconds=seconds, SecondRemainder=.25,
                            GrowthKills=kills, TimeAttackGranted=seconds * rate,
                            KillAttackGranted=kills * 2, TimeGoldGranted=seconds, GoldRemainder=0))


class GrowthRegression(unittest.TestCase):
    def setUp(self):
        self.api = load()

    def function(self, name):
        value = getattr(self.api, name, None)
        self.assertTrue(callable(value), '缺少成长验收实现：' + name)
        return value

    def test_main_compare_maps_only(self):
        resolve = self.function('resolve_growth_map')
        self.assertEqual(resolve('r33growth'), '/Game/Developers/Y01PlaygroundTests/R33Growth/Maps/L_Y01Playground')
        self.assertIn('/R33U3GrowthCompare/', resolve('growthcompare'))
        for name in ('main', '/Game/Y01Playground/Maps/L_Y01Playground', 'compare', 'legacy', 'u2baseline', '../main'):
            with self.assertRaises(ValueError): resolve(name)

    def test_default_is_explicit_r33growth(self):
        import ast
        tree = ast.parse(SCRIPT.read_text(encoding='utf-8-sig'))
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call) and node.args
                 and isinstance(node.args[0], ast.Constant) and node.args[0].value == '--map']
        self.assertEqual(len(calls), 1)
        self.assertEqual(next(ast.literal_eval(k.value) for k in calls[0].keywords if k.arg == 'default'), 'r33growth')

    def test_nested_struct_keeps_gold_and_error_separate(self):
        parse = self.function('parse_struct')
        row = parse('(Gold=8,Error="",Growth=(Error="x,y(停)",Config=(bEnabled=True),EffectiveSeconds=8),Slots=((Row="A",Deadline=0),(Row="B",Deadline=2)))')
        self.assertEqual(row['Gold'], 8)
        self.assertEqual(row['Error'], '')
        self.assertEqual(row['Growth']['Error'], 'x,y(停)')
        self.assertIs(row['Growth']['Config']['bEnabled'], True)
        self.assertEqual(row['Slots'][1]['Deadline'], 2)

    def test_struct_rejects_truncation_and_duplicate(self):
        parse = self.function('parse_struct')
        for text in ('(Gold=1', '(Gold=1,Gold=2)', '(Error="broken)', '(N=nan)'):
            with self.assertRaises((ValueError, AssertionError)): parse(text)

    def test_observer_requires_unique_owner_fresh_row(self):
        parse = self.function('parse_growth_observer')
        text = '0) Y01PlaygroundController /Game/M.M:PersistentLevel.Controller_A.Observer = (bReady=True,ServerSampleTime=4,Growth=(EffectiveSeconds=3))'
        self.assertEqual(parse(text)['owner'], '/Game/M.M:PersistentLevel.Controller_A')
        with self.assertRaises(AssertionError): parse(text + '\n' + text)
        with self.assertRaises(AssertionError): parse('Y01Training OBSERVE HP=200')

    def test_ledger_reconstructs_initial_not_exactly_zero(self):
        check = self.function('validate_ledger')
        self.assertEqual(check(sample(7, 2), 1)['initial_base'], 20)
        self.assertEqual(check(sample(7, 0, 2), 2)['initial_gold'], 0)

    def test_ledger_rejects_passive_double_and_missing_counter(self):
        check = self.function('validate_ledger')
        row = sample()
        row['AttackBase'] += 5
        with self.assertRaises(AssertionError): check(row, 1)
        row = sample()
        del row['Growth']['TimeAttackGranted']
        with self.assertRaises(AssertionError): check(row, 1)

    def test_ledger_rejects_nonfinite_and_growth_error(self):
        check = self.function('validate_ledger')
        for key, value in (('SecondRemainder', float('nan')), ('Error', '拒绝'), ('EffectiveSeconds', 3.5)):
            row = sample()
            row['Growth'][key] = value
            with self.assertRaises(AssertionError): check(row, 1)

    def test_equal_integer_seconds_not_wall_sleep(self):
        check = self.function('validate_delta')
        result = check(sample(3), sample(8), 1, expected_seconds=5, expected_kills=0)
        self.assertEqual((result['seconds'], result['base'], result['gold']), (5, 5, 5))
        with self.assertRaises(AssertionError): check(sample(3), sample(9), 1, expected_seconds=5)

    def test_compare_same_n_double_attack_only(self):
        check = self.function('validate_delta')
        result = check(sample(3, rate=2), sample(8, rate=2), 2, expected_seconds=5)
        self.assertEqual((result['base'], result['gold']), (10, 5))

    def test_training_rewards_separate_from_time_gold(self):
        check = self.function('validate_delta')
        result = check(sample(3), sample(5, 2), 1, expected_kills=2)
        self.assertEqual((result['time_gold'], result['reward_gold'], result['xp'], result['kill_attack']), (2, 20, 10, 4))
        bad = sample(5, 2)
        bad['Gold'] -= 2
        with self.assertRaises(AssertionError): check(sample(3), bad, 1)

    def test_cleanup_allows_natural_gold_but_no_rewards(self):
        check = self.function('validate_delta')
        result = check(sample(3, 2), sample(5, 2), 1, expected_kills=0)
        self.assertEqual((result['gold'], result['reward_gold'], result['kill_attack']), (2, 0, 0))
        with self.assertRaises(AssertionError): check(sample(3, 2), sample(5, 3), 1, expected_kills=0)

    def test_cross_owner_or_stale_world_rejected(self):
        check = self.function('validate_delta')
        for key, value in (('owner', 'Controller_B'), ('role', 'B'), ('run', 'other'), ('ServerSampleTime', 1)):
            after = sample(8)
            after[key] = value
            with self.assertRaises(AssertionError): check(sample(3), after, 1)

    def test_stop_uses_advanced_world_and_frozen_ledger(self):
        check = self.function('validate_stop')
        before = sample(9)
        before['Health'] = 0
        before['Growth']['StopReason'] = '死亡'
        after = copy.deepcopy(before)
        after['ServerSampleTime'] += 3
        self.assertEqual(check(before, after)['world_span'], 3)
        after['Growth']['SecondRemainder'] += .1
        with self.assertRaises(AssertionError): check(before, after)

    def test_damage_uses_commit_snapshot_and_explicit_clamp(self):
        check = self.function('validate_damage')
        commit = dict(Actor='Attack_1', Damage=38, World=10.)
        hit = dict(Actor='Attack_1', Damage=38, HP=22, World=10.5)
        self.assertEqual(check(commit, hit, 60)['actual_damage'], 38)
        hit['HP'] = 0
        result = check(commit, hit, 20)
        self.assertEqual((result['actual_damage'], result['snapshot_damage'], result['overkill']), (20, 38, True))
        hit['Damage'] = 39
        with self.assertRaises(AssertionError): check(commit, hit, 20)

    def test_readonly_query_rejects_injection(self):
        check = self.function('validate_query')
        check('getall Y01PlaygroundController Observer')
        check('getall Y01PlaygroundController Observer.Growth')
        for query in ('getall X Y | god', 'set X Y 1', 'getall X Y\nquit', 'getall X Y;quit'):
            with self.assertRaises(AssertionError): check(query)

    def test_health_query_waits_for_terminal_readonly_sentinel(self):
        runner_type = self.function('GrowthRunner')
        runner = runner_type.__new__(runner_type)
        lines = ['']
        calls = []
        first = '0) Y01AttributeSet /Game/M:PersistentLevel.Y01Monster_0.Attributes.Health = (CurrentValue=21.000000)\n'
        second = '1) Y01AttributeSet /Game/M:PersistentLevel.Y01Monster_1.Attributes.Health = (CurrentValue=24.000000)\n'
        runner.text = lambda role: lines[0]
        runner.emit = lambda *args, **kwargs: None
        def query(role, command, marker):
            calls.append(command)
            lines[0] = first if len(calls) == 1 else first + second + '.Pawn = local'
            return lines[0]
        runner.query = query
        self.assertEqual(runner.healths(), {'Y01Monster_0': 21, 'Y01Monster_1': 24})
        self.assertEqual(calls[-1], 'getall Y01PlaygroundController Pawn')

    def test_exported_slot_default_deadline_is_declared_zero(self):
        deadlines = self.function('slot_deadlines')
        self.assertEqual(deadlines({'Slots': [{'Row': 'BasicAttack'}, {'Row': 'HeavyAttack', 'Deadline': 7}, {}, {}]}), [0, 7])
        with self.assertRaises(AssertionError): deadlines({'Slots': []})

    def test_restart_checks_waiting_reset_and_cycles_visible_panel(self):
        runner_type = self.function('GrowthRunner')
        self.assertTrue(callable(getattr(runner_type, 'restart', None)), '缺少Waiting重开可见验收')
        for rate in (1, 2):
            runner = runner_type.__new__(runner_type)
            runner.rate = rate
            events = []
            row = sample(4, rate=rate)
            runner.query = lambda *args: 'Phase=Waiting'
            runner.growth = lambda role: dict(row, role=role)
            runner.emit = lambda kind, **data: events.append((kind, data))
            runner.picture = lambda label: events.append(('picture', label))
            runner.hud_cycle = lambda: events.append(('cycle', None))
            runner.restart()
            checks = [data for kind, data in events if kind == 'RESTART_INITIAL_PASS']
            self.assertEqual([data['role'] for data in checks], ['A', 'B'])
            self.assertTrue(all(data['result']['initial_gold'] == 0 for data in checks))
            self.assertIn(('picture', 'Restart-Early-Ledger'), events)
            self.assertIn(('cycle', None), events)
            row = sample(4, 1, rate=rate)
            with self.assertRaises(AssertionError): runner.restart()

    def test_runner_reuses_existing_safety_and_capture(self):
        runner = self.function('GrowthRunner')
        self.assertEqual(runner.__bases__[0].__name__, 'Runner')
        for name in ('focus', 'owned_identity', 'cleanup', 'record'):
            self.assertNotIn(name, runner.__dict__)


if __name__ == '__main__':
    unittest.main(verbosity=2)
