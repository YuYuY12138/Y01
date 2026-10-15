"""R33 U3真实双可见端成长验收；仅自建进程、原生玩法、只读控制台及Saved证据。"""
import argparse
import ctypes as c
from datetime import datetime
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import traceback

ROOT = Path(__file__).resolve().parent.parent


def resolve_growth_map(selection):
    maps = {'r33growth': '/Game/Developers/Y01PlaygroundTests/R33Growth/Maps/L_Y01Playground',
            'growthcompare': '/Game/Developers/Y01PlaygroundTests/R33U3GrowthCompare/Maps/L_Y01Playground'}
    if selection not in maps:
        raise ValueError('旧成长验收仅支持r33growth/growthcompare；main是真实R34主图，不能冒充历史配置')
    return maps[selection]


def split_fields(text):
    result, start, depth, quoted, escaped = [], 0, 0, False, False
    for i, char in enumerate(text):
        if quoted:
            if escaped: escaped = False
            elif char == '\\': escaped = True
            elif char == '"': quoted = False
        elif char == '"': quoted = True
        elif char == '(': depth += 1
        elif char == ')':
            depth -= 1
            assert depth >= 0, '结构括号损坏'
        elif char == ',' and depth == 0:
            result.append(text[start:i])
            start = i + 1
    assert not quoted and depth == 0, '结构文本截断'
    if start < len(text): result.append(text[start:])
    return result


def parse_struct(text):
    text = text.strip()
    if text.startswith('('):
        assert text.endswith(')'), '结构未闭合'
        parts = split_fields(text[1:-1])
        if not parts: return {}
        if parts[0].startswith('('): return [parse_struct(part) for part in parts]
        result = {}
        for part in parts:
            match = re.fullmatch(r'(\w+)=(.*)', part.strip(), re.S)
            assert match and match[1] not in result, '字段缺失或重复'
            result[match[1]] = parse_struct(match[2])
        return result
    if text.startswith('"'):
        assert text.endswith('"') and len(text) >= 2, '字符串未闭合'
        return text[1:-1].replace('\\"', '"').replace('\\n', '\n').replace('\\r', '\r')
    if text in ('True', 'False'): return text == 'True'
    if re.fullmatch(r'-?\d+', text): return int(text)
    if re.fullmatch(r'-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?', text):
        value = float(text)
        assert math.isfinite(value), '非有限数值'
        return value
    assert text.lower() not in ('nan', 'inf', '-inf') and not any(ch in text for ch in '()'), '非法数值或结构'
    return text


def parse_growth_observer(text):
    rows = re.findall(r'Y01PlaygroundController (\S+)\.Observer = ([^\r\n]+)', text)
    assert len(rows) == 1, '本次必须有唯一OwnerOnly Observer，禁止沿用旧样本'
    owner, raw = rows[0]
    state = parse_struct(raw)
    assert isinstance(state, dict) and state.get('bReady') is True, '本玩家未就绪'
    state['owner'] = owner
    return state


def number(row, name, integer=False):
    value = row.get(name)
    assert type(value) in (int, float) and math.isfinite(value), '缺少/非法数值：' + name
    assert not integer or type(value) is int, '必须保留整数账本：' + name
    return value


def validate_ledger(state, rate):
    assert state.get('bReady') is True and not state.get('Error'), '配置未就绪或错误'
    growth, training = state.get('Growth', {}), state.get('Training', {})
    assert not growth.get('Error'), '成长账本报错'
    config = growth.get('Config', {})
    assert config.get('bEnabled') is True
    assert (config.get('AttackPerEffectiveSecond'), config.get('AttackPerEffectiveKill'), config.get('GoldPerEffectiveSecond')) == (rate, 2, 1)
    seconds = number(growth, 'EffectiveSeconds', True)
    kills = number(growth, 'GrowthKills', True)
    rewards = number(training, 'Kills', True)
    assert min(seconds, kills, rewards) >= 0
    assert 0 <= number(growth, 'SecondRemainder') < 1
    assert number(growth, 'GoldRemainder') == 0
    assert number(growth, 'TimeAttackGranted') == seconds * rate
    assert number(growth, 'KillAttackGranted') == kills * 2
    assert number(growth, 'TimeGoldGranted', True) == seconds
    assert number(state, 'ServerSampleTime') >= 0
    assert number(state, 'AttackCurrent') - number(state, 'AttackBase') == 5, '被动必须仅+5'
    initial_base = state['AttackBase'] - growth['TimeAttackGranted'] - growth['KillAttackGranted']
    initial_gold = number(state, 'Gold', True) - growth['TimeGoldGranted'] - rewards * 10
    assert initial_base == 20 and initial_gold == 0, '从真实累计账本回构初值失败'
    assert number(state, 'XP', True) == rewards * 5, '训练XP不得与成长混账'
    assert number(state, 'MaxHealth') == 200 and 0 <= number(state, 'Health') <= 200
    return dict(initial_base=initial_base, initial_current=initial_base + 5, initial_gold=initial_gold,
                seconds=seconds, kills=kills, training_rewards=rewards)


def validate_delta(before, after, rate, expected_seconds=None, expected_kills=None):
    for row in (before, after): validate_ledger(row, rate)
    assert all(before.get(k) == after.get(k) for k in ('owner', 'role', 'run')), '跨玩家或跨进程样本'
    assert after['ServerSampleTime'] > before['ServerSampleTime'], '服务器样本时间未推进'
    a, b = before['Growth'], after['Growth']
    delta = dict(seconds=b['EffectiveSeconds'] - a['EffectiveSeconds'],
                 kills=b['GrowthKills'] - a['GrowthKills'], time_attack=b['TimeAttackGranted'] - a['TimeAttackGranted'],
                 kill_attack=b['KillAttackGranted'] - a['KillAttackGranted'], time_gold=b['TimeGoldGranted'] - a['TimeGoldGranted'],
                 base=after['AttackBase'] - before['AttackBase'], gold=after['Gold'] - before['Gold'], xp=after['XP'] - before['XP'],
                 reward_kills=after['Training']['Kills'] - before['Training']['Kills'],
                 world_span=after['ServerSampleTime'] - before['ServerSampleTime'])
    delta['reward_gold'] = delta['gold'] - delta['time_gold']
    assert min(delta[k] for k in ('seconds', 'kills', 'reward_kills')) >= 0, '成长被清零/倒退'
    assert delta['base'] == delta['time_attack'] + delta['kill_attack']
    assert delta['reward_gold'] == delta['reward_kills'] * 10 and delta['xp'] == delta['reward_kills'] * 5
    if expected_seconds is not None: assert delta['seconds'] == expected_seconds, '不是要求的整数有效秒跨度'
    if expected_kills is not None:
        assert delta['kills'] == delta['reward_kills'] == expected_kills, '有效击杀/训练奖励计数不符'
    return delta


def validate_stop(before, after):
    assert all(before.get(k) == after.get(k) for k in ('owner', 'role', 'run'))
    span = after['ServerSampleTime'] - before['ServerSampleTime']
    assert span >= 2, '停账需要真实World时间继续至少2秒'
    assert before['Health'] == after['Health'] == 0 or before['Growth']['StopReason'] == 'Ended'
    assert before['Growth'] == after['Growth'], '死亡/Ended后账本仍变动'
    for key in ('AttackBase', 'AttackCurrent', 'Gold', 'XP'):
        assert before[key] == after[key], '停止后仍授予：' + key
    return dict(world_span=span, reason=after['Growth']['StopReason'], seconds=after['Growth']['EffectiveSeconds'])


def validate_damage(commit, hit, hp_before):
    assert commit['Actor'] == hit['Actor'] and hit['World'] >= commit['World']
    assert commit['Damage'] == hit['Damage'] > 0, 'HIT不等于Commit快照'
    assert hp_before > 0 and hit['HP'] == max(0, hp_before - commit['Damage']), 'HP实际扣减不符'
    return dict(snapshot_damage=commit['Damage'], actual_damage=hp_before - hit['HP'],
                hp_before=hp_before, hp_after=hit['HP'], overkill=commit['Damage'] > hp_before,
                commit_world=commit['World'], hit_world=hit['World'])


def slot_deadlines(state):
    slots = state.get('Slots', [])
    assert len(slots) == 4 and [s.get('Row') for s in slots[:2]] == ['BasicAttack', 'HeavyAttack']
    return [number({'Deadline': s.get('Deadline', 0)}, 'Deadline') for s in slots[:2]]


def validate_query(query):
    assert re.fullmatch(r'(?:getall \w+ \w+(?:\.\w+)*|getalllocation \w+)', query), '仅允许单条只读查询'


spec = importlib.util.spec_from_file_location('u3_skill_driver', Path(__file__).with_name('Test-Y01SkillVisible.py'))
skill = importlib.util.module_from_spec(spec)
spec.loader.exec_module(skill)
# 仅本Python进程的私有导入适配；不改旧文件、旧命令入口或用户INI。
skill.resolve_skill_map = resolve_growth_map


class PacedCaptureEvent(threading.Event):
    def wait(self, timeout=None):
        return super().wait(max(.2, timeout) if timeout is not None else None)


class GrowthRecorder(skill.Recorder):
    def __init__(self, runner, label):
        super().__init__(runner, label)
        self.stop_event = PacedCaptureEvent()


skill.Recorder = GrowthRecorder


def protected_hashes():
    roots = ['Source', 'Config', 'Scripts', 'UI_Deliverables', 'Content/Y01Playground', 'Content/Y01EquipmentLab',
             'Content/Developers/Y01PlaygroundTests/R33U3GrowthCompare',
             'Content/Developers/Y01PlaygroundTests/R33U2Compare',
             'Content/Developers/Y01PlaygroundTests/R33U2Legacy',
             'Content/Developers/Y01PlaygroundTests/R33Growth',
             'Content/Developers/Y01PlaygroundTests/R33TrainingCompare']
    result = {}
    for name in roots:
        for path in (ROOT / name).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts:
                result[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def audit_configuration(output, selections):
    result = []
    roots = {'r33growth': '/Game/Developers/Y01PlaygroundTests/R33Growth', 'growthcompare': '/Game/Developers/Y01PlaygroundTests/R33U3GrowthCompare',
             'u2baseline': '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline',
             'compare': '/Game/Developers/Y01PlaygroundTests/R33U2Compare',
             'trainingcompare': '/Game/Developers/Y01PlaygroundTests/R33TrainingCompare'}
    for name in selections:
        for mode in ('GrowthAudit', 'TrainingAudit', 'SkillAudit'):
            path = output / f'{name}-{mode}.log'
            command = [str(skill.ENGINE / 'UnrealEditor-Cmd.exe'), str(ROOT / 'Y01.uproject'), '-run=Y01CreatePlayground',
                       '-Root=' + roots[name], '-ValidateOnly', '-' + mode, '-nullrhi', '-unattended', '-nop4',
                       '-nosound', '-NoSplash', '-UTF8Output', '-stdout', '-abslog=' + str(path)]
            completed = subprocess.run(command, capture_output=True, timeout=120)
            assert completed.returncode == 0, f'只读审计失败 {name}/{mode} Exit={completed.returncode}'
            text = path.read_text(encoding='utf-8', errors='replace')
            rows = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in text.splitlines() if 'GROWTH_AUDIT ' in line]
            if mode == 'GrowthAudit':
                target = [row for row in rows if row.get('Row') == row.get('Character')]
                assert len(target) == 1, '目标Character行不唯一'
                row = target[0]
                assert row['Profile'] == roots[name] + '/DA_PlaygroundProfile.DA_PlaygroundProfile'
                expected = 2 if name == 'growthcompare' else 1
                if name in ('r33growth', 'growthcompare'):
                    assert (row['Enabled'], float(row['Seconds']), float(row['Kills']), float(row['Gold'])) == ('1', expected, 2, 1)
                    assert (float(row['HP']), float(row['Attack'])) == (200, 20)
                elif name != 'trainingcompare': assert row['Enabled'] == '0', '旧U2资产成长污染'
                result.append(dict(map=name, **row, old_training_rejected=name == 'trainingcompare' and row['Enabled'] != '0'))
    return result


class GrowthRunner(skill.Runner):
    def __init__(self, args):
        super().__init__(args)
        self.rate = 2 if args.map == 'growthcompare' else 1
        self.samples = {'A': [], 'B': []}
        self.damage_rows = []

    def query(self, role, command, marker):
        validate_query(command)
        self.focus(role)
        offset = len(self.text(role))
        begin = time.time_ns()
        self.n.key(0xC0)
        self.n.type_text(command[0])
        self.n.hold(.03)
        for char in command[1:]:
            self.n.focused('growth-posted-readonly-before')
            if not self.n.user.PostMessageW(self.n.window, 0x102, ord(char), 0): raise c.WinError(c.get_last_error())
            self.n.focused('growth-posted-readonly-after')
        self.n.key(13)
        self.wait(lambda: marker in self.text(role)[offset:], '成长只读：' + marker, 5)
        text = self.text(role)[offset:]
        assert 'Command not recognized:' not in text
        self.emit('POSTED_READONLY', role=role, command=command, offset=offset, start_ns=begin, end_ns=time.time_ns())
        return text

    def growth(self, role='A'):
        text = self.query(role, 'getall Y01PlaygroundController Observer', '.Observer = ')
        state = parse_growth_observer(text)
        state.update(role=role, run=self.args.label, wall_ns=time.time_ns())
        validate_ledger(state, self.rate)
        self.samples[role].append(state)
        self.emit('GROWTH_SAMPLE', role=role, state=state)
        return state

    def healths(self, role='A'):
        offset = len(self.text(role))
        self.query(role, 'getall Y01AttributeSet Health', '.Health = ')
        self.query(role, 'getall Y01PlaygroundController Pawn', '.Pawn = ')
        text = self.text(role)[offset:]
        result = {}
        for line in text.splitlines():
            match = re.search(r'PersistentLevel\.(Y01Monster_\d+)\.Attributes\.Health = .*CurrentValue=(-?[\d.]+)', line)
            if match: result[match[1]] = float(match[2])
        self.emit('MONSTER_HP', role=role, health=result)
        return result

    def panel(self, role, y=141):
        self.focus(role)
        self.click_point(1092, y)
        self.n.hold(.06)
        self.emit('NATIVE_PANEL', role=role, x=1092, y=y)

    def picture(self, label, action=lambda: None, after=.12):
        return self.record(label, action, after)

    def same_seconds(self):
        self.picture('01-Default-HUD')
        self.panel('B')
        self.panel('A')
        first = {r: self.growth(r) for r in ('A', 'B')}
        self.picture('02-Waiting-Start')
        found = {}
        deadline = time.monotonic() + 15
        recorder = GrowthRecorder(self, '03-Waiting-EqualSeconds')
        recorder.start()
        try:
            while len(found) < 2 and time.monotonic() < deadline:
                for role in ('A', 'B'):
                    if role in found: continue
                    state = self.growth(role)
                    delta = state['Growth']['EffectiveSeconds'] - first[role]['Growth']['EffectiveSeconds']
                    assert delta <= self.args.seconds, '轮询错过目标整数秒，保留失败且不得按墙钟改判'
                    if delta == self.args.seconds: found[role] = state
            assert len(found) == 2, '双端等秒采样超时'
        finally: recorder.stop()
        for role in ('A', 'B'):
            delta = validate_delta(first[role], found[role], self.rate, self.args.seconds, 0)
            self.emit('EQUAL_SECONDS_PASS', role=role, before=first[role], after=found[role], delta=delta)
        self.picture('04-Waiting-End')
        return found

    def fire(self, label, mode=2):
        before = self.growth()
        hp = self.healths()
        start = len(skill.parse_events(self.text('Server')))
        self.picture(label, lambda: self.n.key(ord('0') + mode), 1.0 if mode == 2 else .4)
        after = self.growth()
        hp_after = self.healths()
        events = skill.parse_events(self.text('Server'))[start:]
        commits = [e for e in events if e['kind'] == 'COMMIT']
        assert len(commits) == 1 and commits[0]['Mode'] == mode, '未取得唯一真实技能Commit'
        commit = commits[0]
        assert before['ServerSampleTime'] <= commit['World'] <= after['ServerSampleTime']
        # 释放前后没有其它攻击；整秒跳变可发生于只读查询间，不能伪称与旧快照同刻。
        assert before['AttackCurrent'] <= commit['Damage'] <= after['AttackCurrent']
        assert commit['Damage'] - before['AttackCurrent'] <= self.rate * (after['Growth']['EffectiveSeconds'] - before['Growth']['EffectiveSeconds'])
        hits = [e for e in events if e['kind'] == 'HIT' and e['Actor'] == commit['Actor']]
        assert hits, '真实释放未命中，不算伤害验收通过'
        server_guids, client_guids = skill.parse_netguids(self.text('Server')), skill.parse_netguids(self.text('A'))
        for hit in hits:
            guid = server_guids.get(hit['Target'])
            names = [name for name, value in client_guids.items() if value == guid]
            assert guid and len(names) == 1 and names[0] in hp, '缺少真实NetGUID前血量映射'
            damage = validate_damage(commit, hit, hp[names[0]])
            if hit['HP'] > 0: assert hp_after.get(names[0]) == hit['HP'], '客户端实际HP与权威HIT不同'
            damage.update(guid=guid, target=hit['Target'], client_target=names[0], commit=commit, hit=hit)
            self.damage_rows.append(damage)
            self.emit('ACTUAL_DAMAGE_PASS', label=label, **damage)
        delta = validate_delta(before, after, self.rate)
        assert delta['kills'] == sum(hit['HP'] == 0 for hit in hits) == delta['reward_kills']
        self.emit('KILL_LEDGER_PASS', label=label, before=before, after=after, delta=delta)
        self.picture(label + '-AfterHP')
        return after

    def hud_cycle(self):
        before = self.growth()
        for label, y in (('Skill-Exclusive', 183), ('Attribute-Exclusive', 141), ('Attribute-Closed', 141), ('Attribute-Reopen', 141)):
            self.picture('HUD-' + label, lambda yy=y: self.panel('A', yy))
        after = self.growth()
        self.emit('HUD_CYCLE_LEDGER', delta=validate_delta(before, after, self.rate, expected_kills=0),
                  scope='互斥和默认HUD由原图人工复核；数值以OwnerOnly样本为准')

    def restart(self):
        phase = self.query('A', 'getall Y01GameState Snapshot', '.Snapshot = ')
        assert 'Phase=Waiting' in phase, '重开必须处于Waiting安全阶段'
        for role in ('A', 'B'):
            row = self.growth(role)
            assert row['Health'] == 200 and not row['Training']['bInside']
            assert row['Growth']['GrowthKills'] == 0 and row['Training']['Kills'] == 0
            self.emit('RESTART_INITIAL_PASS', role=role, state=row, result=validate_ledger(row, self.rate))
        self.picture('Restart-Early-Ledger')
        self.hud_cycle()

    def training(self):
        self.focus('A')
        self.n.click_training(1)
        self.picture('05-Training-InitialHP')
        # 第一球对初生怪，第二球/范围在不同真实时刻；不改表、不等待攻击超过60才取样。
        self.fire('06-Projectile-First')
        self.fire('07-Area-Second', 1)
        if self.samples['A'][-1]['Growth']['GrowthKills'] == 0:
            self.n.hold(.3)
            self.fire('08-Projectile-Kill')
        assert self.samples['A'][-1]['Growth']['GrowthKills'] > 0, '未真实击杀'
        assert len({row['snapshot_damage'] for row in self.damage_rows}) >= 2, '后续释放快照未提高'
        self.hud_cycle()
        if self.args.scenario == 'compare': return
        self.focus('B')
        self.n.click_training(1)
        b = self.growth('B')
        self.focus('A')
        old_x = self.telemetry('A')['X']
        self.picture('Native-Retreat-Before-Cleanup', lambda: self.n.key(ord('S'), .7))
        self.wait(lambda: self.telemetry('A')['X'] < old_x - 50, '真实S移动后拉开清理前受击距离')
        a = self.growth('A')
        assert a['Training']['Room'] != b['Training']['Room'] and a['Training']['bInside'] and b['Training']['bInside']
        self.picture('09-Separate-Rooms')
        self.n.click_training(0)
        left = self.growth()
        assert not left['Training']['bInside'] and left['Training']['Alive'] == 0
        assert left['Health'] == a['Health'], '离区HP变化，需要检查自然伤害时间，不能当重置通过'
        assert slot_deadlines(left) == slot_deadlines(a), '离区CD被重置'
        self.emit('CLEANUP_PASS', before=a, after=left, delta=validate_delta(a, left, self.rate, expected_kills=0))
        self.picture('10-A-Leave-B-Continues')
        peer = self.wait(lambda: (lambda s: s if s['Training']['Alive'] > b['Training']['Alive'] else None)(self.growth('B')), 'B独立继续刷怪', 6)
        self.emit('PEER_BATCH_PASS', before=b, after=peer, delta=validate_delta(b, peer, self.rate, expected_kills=0))
        self.focus('A')
        self.n.click_training(1)
        entered = self.growth()
        assert entered['Training']['bInside'] and entered['Training']['Alive'] == 2
        assert entered['Health'] == left['Health']
        assert slot_deadlines(entered) == slot_deadlines(left)
        self.emit('REENTER_PASS', before=left, after=entered, delta=validate_delta(left, entered, self.rate, expected_kills=0))
        self.picture('11-A-Reenter-HP-CD')
        # B回Waiting安全区，A自然死亡；不让两端同时死亡掩盖个人停账。
        self.focus('B')
        self.n.click_training(0)
        self.focus('A')
        self.wait(lambda: self.telemetry('A').get('HP', 200) <= 30, 'A自然受伤低HP', 55)
        self.picture('12-A-LowHP-B-Alive')
        self.wait(lambda: self.telemetry('A').get('HP', 200) <= 0, 'A训练Waiting自然死亡', 15)
        dead = self.growth()
        live = self.growth('B')
        self.picture('13-A-Dead-B-Alive')
        self.wait(lambda: (lambda s: s if s['ServerSampleTime'] >= dead['ServerSampleTime'] + 3 else None)(self.growth('A')), '死亡后World仍推进', 8)
        stopped = self.samples['A'][-1]
        peer_after = self.growth('B')
        self.emit('DEATH_STOP_PASS', before=dead, after=stopped, result=validate_stop(dead, stopped))
        assert peer_after['Health'] > 0
        self.emit('PEER_ALIVE_GROWTH_PASS', before=live, after=peer_after, delta=validate_delta(live, peer_after, self.rate, expected_kills=0))
        phase_text = self.query('A', 'getall Y01GameState Snapshot', '.Snapshot = ')
        assert 'Phase=Waiting' in phase_text, 'Waiting死亡不能误报全局Ended'
        self.picture('14-Waiting-Death-Frozen')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', choices=('r33growth', 'growthcompare'), default='r33growth')
    parser.add_argument('--scenario', choices=('main', 'compare', 'restart'), default='main')
    parser.add_argument('--seconds', type=int, default=5)
    parser.add_argument('--port', type=int, default=18343)
    parser.add_argument('--label', default='R33-U3-Visible-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    parser.add_argument('--preflight-only', action='store_true')
    parser.add_argument('--audit-only', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'R33-U3-Visible-[A-Za-z0-9_-]+', args.label): parser.error('必须使用唯一R33-U3-Visible安全Label')
    if not 1024 <= args.port <= 65535 or not 2 <= args.seconds <= 10: parser.error('端口或有效秒跨度不合法')
    args.fast_observe = True
    output = ROOT / 'Saved' / args.label
    if args.audit_only:
        result = skill.audit_frames(output)
        (output / 'audit.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False))
        return 0
    before = protected_hashes()
    if args.preflight_only:
        output.mkdir(exist_ok=False)
        result = audit_configuration(output, ('r33growth', 'growthcompare', 'u2baseline', 'compare', 'trainingcompare'))
        assert protected_hashes() == before, '只读审计改写受保护文件'
        (output / 'preflight.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False))
        return 0
    runner = GrowthRunner(args)
    code = 1
    try:
        runner.emit('PROTECTED_START', files=before)
        runner.emit('GROWTH_PROVENANCE', sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    screenshots='完整1280x800默认HUD；连续采样约5FPS，不声称捕获0.1秒瞬态',
                    gameplay='native OS，复用每事件前后前台HWND/PID校验；失焦立即停止', console='posted只读单命令，非native文字')
        runner.launch()
        runner.same_seconds()
        if args.scenario != 'restart': runner.training()
        else: runner.restart()
        code = 0
    except BaseException:
        runner.emit('RUN_FAILED', error=traceback.format_exc())
    finally:
        try: runner.cleanup()
        except BaseException:
            code = 1
            runner.emit('CLEANUP_FAILED', error=traceback.format_exc())
        unchanged = protected_hashes() == before
        if not unchanged: code = 1
        runner.emit('PROTECTED_END', unchanged=unchanged)
        runner.emit('OWNED_REMAINING', pids=[p.pid for p in runner.owned.values() if p.poll() is None])
        summary = dict(code=code, map=args.map, scenario=args.scenario, samples=runner.samples, damage=runner.damage_rows,
                       owned_remaining=[p.pid for p in runner.owned.values() if p.poll() is None], protected_unchanged=unchanged,
                       scope='场景断言结果，视觉必须另行人工读原图；Ended/登出/在途快照未在此场景重复')
        (runner.output / 'result.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
        runner.emit('RUNNER_EXIT', code=code)
        runner.trace.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
