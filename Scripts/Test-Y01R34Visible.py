"""R34 薄验收驱动：复用旧窗口/DS/PNG/NetGUID工具，不写资产、不发送手动技能键。"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('r34_growth_helpers', Path(__file__).with_name('Test-Y01GrowthVisible.py'))
growth = importlib.util.module_from_spec(spec)
spec.loader.exec_module(growth)
skill = growth.skill


def resolve_r34_map(selection):
    roots = {'main': '/Game/Y01Playground', 'r34compare': '/Game/Developers/Y01PlaygroundTests/R34Compare',
             'r34ranged': '/Game/Developers/Y01PlaygroundTests/R34Ranged'}
    if selection not in roots: raise ValueError('R34仅接受main/r34compare/r34ranged持久地图')
    return roots[selection] + '/Maps/L_Y01Playground'


# 私有导入适配，不改变旧入口与旧验收常量。
skill.resolve_skill_map = resolve_r34_map
skill.Recorder = growth.GrowthRecorder.__bases__[0]


def parse_r34(text):
    result = []
    for line in text.splitlines(keepends=True):
        if not line.endswith('\n'): continue
        match = re.search(r'Y01R34 (CONFIG|ATTACK|HIT|CANCEL|REGEN|EQ_\w+) (.+)', line)
        if not match: continue
        row = dict(kind=match[1], raw=line.rstrip())
        for key, value in re.findall(r'(\w+)=([^\s]+)', match[2]):
            row[key] = float(value) if re.fullmatch(skill.NUMBER, value) else value
        growth.number(row, 'DS')
        for key in ('Origin', 'Direction', 'Position'):
            vector = re.search(key + r'=X=(' + skill.NUMBER + r') Y=(' + skill.NUMBER + r') Z=(' + skill.NUMBER + r')', line)
            if vector: row[key] = tuple(map(float, vector.groups()))
        stamp = re.search(r'^\[([\d.\-:]+)\]', line)
        if stamp: row['t'] = datetime.strptime(stamp[1], '%Y.%m.%d-%H.%M.%S:%f').replace(tzinfo=timezone.utc).timestamp()
        result.append(row)
    return result


def validate_hit(attack, hit):
    for key in ('Actor', 'Source', 'Caster', 'Snapshot', 'SourceSession'):
        assert attack[key] == hit[key], '命中身份/提交快照不符：' + key
    if attack['Type'] == 2:
        assert attack['Target'] == hit['Target'] and attack['TargetSession'] == hit['TargetSession'], '球只能伤害原指定目标会话'
    assert growth.number(hit, 'DS') >= growth.number(attack, 'DS'), '提交前扣血'
    before, after = growth.number(hit, 'BeforeHP'), growth.number(hit, 'AfterHP')
    damage = max(0, growth.number(hit, 'Snapshot') - growth.number(hit, 'Armor'))
    actual = min(before, damage)
    assert before > 0 and after >= 0 and abs(before - after - actual) < .001
    assert abs(growth.number(hit, 'Actual') - actual) < .001, '真实HP差与Actual不符'
    return dict(target=hit['Target'], before=before, after=after, actual=actual, snapshot=attack['Snapshot'])


def validate_attack_hits(attack, hits):
    assert hits, '没有真实命中'
    targets = [hit['Target'] for hit in hits]
    assert len(targets) == len(set(targets)), '同一攻击重复伤害同目标'
    if attack['Type'] == 2: assert len(hits) == 1
    return dict(targets=targets, damage=[validate_hit(attack, hit) for hit in hits])


def validate_intervals(rows, interval):
    assert len(rows) >= 2 and interval > 0, '缺少同源连续攻击'
    assert len({(row['Source'], row['SourceSession']) for row in rows}) == 1, '混用来源/区域会话'
    deltas = [growth.number(b, 'DS') - growth.number(a, 'DS') for a, b in zip(rows, rows[1:])]
    assert min(deltas) >= interval - .00001, '攻击早于权威间隔'
    return dict(count=len(rows), configured=interval, minimum=min(deltas), maximum=max(deltas),
                stable=min(deltas) <= interval + .12, ds=[r['DS'] for r in rows])


def validate_timing_groups(attacks, configs, expected):
    groups = {}
    for attack in attacks: groups.setdefault((attack['Source'], attack['SourceSession']), []).append(attack)
    timings, categories = [], set()
    for (source, session), rows in groups.items():
        config = configs[source]
        if len(rows) >= 2:
            result = validate_intervals(rows, config['AttackInterval'])
            timings.append(dict(source=source, session=session, row=config['Row'], **result))
            if result['stable']: categories.add(config['Row'])
    assert expected <= categories, '缺少各类来源稳定自动攻击间隔：' + str(expected - categories)
    return timings


def validate_equipment_chain(states, peers, base, bonus):
    assert len(states) == len(peers) == 4
    for rows in (states, peers):
        assert len({(r['owner'], r['owner_guid'], r['role'], r['run']) for r in rows}) == 1, '跨拥有者/运行样本'
        assert all(b['ServerSampleTime'] > a['ServerSampleTime'] for a, b in zip(rows, rows[1:]))
    assert states[0]['owner_guid'] and peers[0]['owner_guid'] and states[0]['owner_guid'] != peers[0]['owner_guid']
    assert states[0]['run'] == peers[0]['run'], '双方不是同一DS会话'
    for i, (a, b) in enumerate(zip(states, peers)):
        for row in (a, b):
            assert row['bReady'] and row['bAutoAttackEnabled'] and row['AttackBase'] == base
            assert len(row['Equipment']) == 6, '不是六个真实槽'
        active = i in (1, 2)
        assert a['AttackCurrent'] == base + bonus * active and b['AttackCurrent'] == base
        assert all(item.get('Row', 'None') == 'None' for item in b['Equipment'])
        assert all(item.get('Row', 'None') == 'None' for item in a['Equipment'][1:])
        slot = a['Equipment'][0]
        if active:
            assert slot.get('Row') == 'EQ_0001' and slot.get('AttackBonus') == bonus
            assert slot.get('Quality', 'Green') == 'Green' and slot.get('bBonusActive') is True
        else: assert slot.get('Row', 'None') == 'None'
    return dict(base=base, bonus=bonus, current=[r['AttackCurrent'] for r in states], peer=[r['AttackCurrent'] for r in peers])


def complete_batch(text):
    return bool(re.search(r'\.Snapshot = [^\r\n]+\r?\n', text))


def validate_gameplay_keys(keys):
    assert all(key in (13, 87, 83, 65, 68) for key in keys), 'R34禁止手动技能键/未批准玩法输入'


def validate_rooms(hits, positions):
    assert hits
    for hit in hits:
        assert hit['Caster'] in positions and hit['Target'] in positions, '缺少真实双方坐标'
        assert math.dist(positions[hit['Caster']], positions[hit['Target']]) < 1500, '跨房命中'
    return dict(hits=len(hits), scope='真实命中时双方坐标；房间间隔远大于1500，非按Actor后缀推断')


def validate_r34_speed(events, actor):
    commits = [e for e in events if e.get('Actor') == actor and e['kind'] == 'COMMIT']
    assert len(commits) == 1, '速度证据必须有唯一提交'
    commit, complete, initial = commits[0], [], []
    for row in events:
        if row.get('Actor') != actor or row['kind'] != 'STEP' or row.get('Terminal') != 0: continue
        delta = skill.world(row, 'SampleWorld') - skill.world(row, 'PrevWorld')
        if delta == 0:
            previous = tuple(row[k] for k in ('PrevX', 'PrevY', 'PrevZ'))
            assert not complete and not initial and skill.world(row) == skill.world(commit), '非提交首帧零时长STEP'
            assert skill.world(row, 'SampleWorld') == skill.world(commit) and row['Mode'] == 2 and row['Phase'] == 0
            assert previous == row['Position'] and math.dist(previous, commit['Origin']) < .002, '零时长STEP发生位移'
            initial.append(row)
        else: complete.append(row)
    result = skill.validate_speed(complete, actor, 400)
    return dict(result, initial_zero_steps=len(initial))


def validate_ranged(attack, hits, events):
    assert attack['Type'] == 2 and attack['Range'] == 500 and attack['Speed'] == 400 and attack['Radius'] == 12
    result = validate_attack_hits(attack, hits)
    flight = hits[0]['DS'] - attack['DS']
    assert flight > .05, '本次必须存在真实飞行段'
    assert abs(attack['EndsAt'] - attack['DS'] - 1.25) < .00001
    rows = [e for e in events if e.get('Actor') == attack['Actor']]
    ends = [e for e in rows if e['kind'] == 'END']
    feedback = [e for e in rows if e['kind'] == 'STATE' and e['Phase'] == 1]
    assert len(ends) == 1 and feedback, '缺少命中反馈或最终销毁'
    span = ends[0]['World'] - hits[0]['DS']
    assert .1 - .00001 <= span <= .22, '真实DS反馈时长不符'
    return dict(result, flight=flight, feedback=span, actor=attack['Actor'])


def protected_hashes():
    roots = ['Source', 'Config', 'UI_Deliverables', 'Content/Y01Playground', 'Content/Y01EquipmentLab',
             'Docs', 'ArtSource', 'Content/Developers/Y01PlaygroundTests/R34Compare',
             'Content/Developers/Y01PlaygroundTests/R34Ranged', 'Content/Developers/Y01PlaygroundTests/R33Growth',
             'Content/Developers/Y01PlaygroundTests/R33U2Legacy']
    files = [p for root in roots for p in (ROOT / root).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    files += list(ROOT.glob('*.md')) + [ROOT / 'Y01.uproject']
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}


def validate_protected_snapshots(snapshots, current):
    assert snapshots and current, '保护清单不得为空'
    assert all(snapshot == current for snapshot in snapshots), '受保护文件新增/缺失/内容变化'
    return len(current)


def audit_final(label):
    current, runs = protected_hashes(), []
    for path in sorted((ROOT / 'Saved').glob('R34-Visible-*/result.json')):
        directory = path.parent
        result = json.loads(path.read_text(encoding='utf-8'))
        snapshots = [json.loads((directory / name).read_text(encoding='utf-8'))
                     for name in ('protected-before.json', 'protected-after.json')]
        count = validate_protected_snapshots(snapshots, current)
        audit = skill.audit_frames(directory)
        assert len(list(directory.rglob('*.png'))) == audit['files'] == result['png_count'], 'PNG总数/清单/原结果不符'
        assert not result['owned_remaining'] and result['protected_unchanged']
        rows = [json.loads(line) for line in (directory / 'runner.jsonl').read_text(encoding='utf-8').splitlines() if line]
        authority = next((r for r in rows if r['kind'] == 'AUTHORITY_PASS'), {})
        record = dict(directory=str(directory), **result, protected_files=count,
                      max_capture_gap_ms=max(s['max_gap_ms'] for s in audit['series']),
                      equipment=[r['result'] for r in rows if r['kind'] == 'EQUIPMENT_CHAIN_PASS'],
                      failure=[r['error'] for r in rows if r['kind'] == 'RUN_FAILED'],
                      attacks=authority.get('attacks'), hits=authority.get('hits'),
                      stationary=authority.get('stationary_observations'),
                      png_integrity_sha256_freshness=True)
        runs.append(record)
        print(json.dumps(dict(label=directory.name, code=result['code'], png=audit['files'], protected_files=count)), flush=True)
    assert runs, '没有实际运行目录'
    report = dict(runs=runs, png_total=sum(r['png_count'] for r in runs), protected_files=len(current),
                  verified_ns=time.time_ns(), scope='复核既有证据，不改变原轮成功/失败；进程实时残留另核对')
    with (ROOT / 'Saved' / (label + '.json')).open('x', encoding='utf-8') as out:
        json.dump(report, out, ensure_ascii=False, indent=2)
    print(json.dumps(dict(runs=len(runs), png=report['png_total'], protected_files=len(current), code=0)), flush=True)
    return 0


def process_created(process):
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
    times = [w.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(w.HANDLE(int(process._handle)), *(c.byref(t) for t in times)):
        raise c.WinError(c.get_last_error())
    return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime


class R34Runner(skill.Runner):
    def __init__(self, args):
        self.created, self.exits = {}, []
        super().__init__(args)
        self.samples = {'A': [], 'B': []}
        self.base, self.bonus, self.monster_attack = (36, 11, 8) if args.map == 'r34compare' else (30, 5, 5)
        self.keys = []

    def emit(self, kind, **data):
        if kind == 'OWNED_START':
            role = data['role']
            self.created[role] = process_created(self.owned[role])
            data['creation_filetime'] = self.created[role]
            (self.output / 'processes.json').write_text(json.dumps(dict(runner_pid=os.getpid(),
                owned={r: dict(pid=p.pid, creation_filetime=self.created[r]) for r, p in self.owned.items()}), indent=2), encoding='utf-8')
        if kind == 'OWNED_EXIT': self.exits.append(data)
        super().emit(kind, **data)

    def owned_identity(self, role):
        assert process_created(self.owned[role]) == self.created[role], '进程创建时间不符'
        super().owned_identity(role)

    def cleanup(self):
        for role, process in self.owned.items():
            assert process_created(process) == self.created[role], '清理拒绝：原生进程句柄/创建时间不符'
        super().cleanup()

    def sample(self, role='A'):
        self.focus(role)
        offset = len(self.text(role))
        command = ('getall Y01PlaygroundController Observer | getall Y01AttributeSet Health | '
                   'getall Y01PlaygroundController Pawn | getalllocation Y01PlaygroundCharacter | '
                   'getalllocation Y01Monster | getall Y01GameState Snapshot')
        skill.validate_readonly_command(command)
        begin = time.time_ns()
        self.n.key(0xC0); self.n.type_text(command[0]); self.n.hold(.03)
        for char in command[1:]:
            self.n.focused('r34-posted-readonly')
            if not self.n.user.PostMessageW(self.n.window, 0x102, ord(char), 0): raise c.WinError(c.get_last_error())
        self.n.key(13)
        self.wait(lambda: complete_batch(self.text(role)[offset:]), '整组只读查询末尾哨兵', 6)
        text = self.text(role)[offset:]
        assert 'Command not recognized:' not in text
        state = growth.parse_growth_observer(text)
        observation = skill.parse_observation(text)
        owner_guid = skill.parse_netguids(self.text(role)).get(state['owner'].rsplit('.', 1)[-1])
        assert owner_guid, '必须以真实NetGUID识别本端Controller，不按相同动态后缀猜测'
        state.update(role=role, run=self.args.label, owner_guid=owner_guid, start_ns=begin, end_ns=time.time_ns(), observation=observation)
        assert state['bAutoAttackEnabled'] and not state.get('Error')
        assert not state['Growth'].get('Config', {}).get('bEnabled', False), 'R34主配置不得混入旧成长'
        assert growth.slot_deadlines(state) == [0, 0], '没有发送1/2，手动技能CD不应变化'
        assert state['AttackBase'] == self.base
        self.samples[role].append(state)
        self.emit('R34_SAMPLE', role=role, offset=offset, command=command, state=state)
        return state

    def gameplay_key(self, key, seconds=.07):
        validate_gameplay_keys([key]); self.keys.append(key)
        self.emit('NATIVE_GAMEPLAY_KEY', key=key, seconds=seconds)
        self.n.key(key, seconds)

    def panel(self, role):
        self.focus(role); self.click_point(1092, 141); self.n.hold(.1)
        self.emit('NATIVE_ATTRIBUTE_PANEL', role=role, xy=[1092, 141])

    def equipment(self):
        self.record('01-Naked-Default-HUD', lambda: None, .2)
        for role in ('A', 'B'): self.panel(role)
        a, b = [self.sample('A')], [self.sample('B')]
        self.record('02-Naked-Attributes', lambda: None, .2)
        for label, action, x in (('03-Equip', 8, 129), ('04-Repeat-Equip', 8, 129), ('05-Unequip', 9, 339)):
            self.focus('A')
            offset = len(self.text('Server'))
            self.record(label, lambda xx=x: self.click_point(xx, 286), .25)
            self.wait(lambda: any(e['kind'] == 'EQ_REQUEST' and e.get('Action') == action
                                 for e in parse_r34(self.text('Server')[offset:])), '装备原生命中及权威请求')
            a.append(self.sample('A')); b.append(self.sample('B'))
        self.emit('EQUIPMENT_CHAIN_PASS', result=validate_equipment_chain(a, b, self.base, self.bonus))
        rows = parse_r34(self.text('Server'))
        configs = [e for e in rows if e['kind'] == 'CONFIG' and e['Row'] == 'Default']
        assert len(configs) == 2 and all(e['AttackDamage'] == self.base and e['Health'] == 300 for e in configs)
        eq = [e for e in rows if e['kind'] == 'EQ_CONFIG']
        assert len(eq) == 1 and eq[0]['AttackBonus'] == self.bonus and eq[0]['Effect'] == 'None' and eq[0]['Quality'] == 'Green'
        self.emit('NEW_DS_CONFIG_PASS', players=configs, equipment=eq,
                  map=resolve_r34_map(self.args.map), isolated_persistent_copy=self.args.map != 'main')

    def monitor(self, seconds, roles=('A', 'B')):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for role in roles: self.sample(role)

    def training(self):
        self.focus('A')
        self.record('06-Training-A-Auto', lambda: (self.n.click_training(1), self.monitor(9, ('A',))), .2)
        a = self.sample('A'); b = self.sample('B')
        assert a['Training']['bInside'] and not b['Training']['bInside']
        self.focus('B')
        self.record('07-Two-Independent-Rooms', lambda: (self.n.click_training(1), self.monitor(4)), .2)
        a = self.sample('A'); b = self.sample('B')
        assert a['Training']['Room'] != b['Training']['Room'] and b['Training']['bInside']
        self.focus('A')
        self.record('08-A-Leave-B-Continues', lambda: (self.n.click_training(0), self.monitor(3)), .2)
        left = self.sample('A'); peer = self.sample('B')
        assert not left['Training']['bInside'] and left['Training']['Alive'] == 0 and peer['Training']['bInside']
        # 自动击杀可发生在离区前，只有离区后的安全区样本才用于停伤/停奖励断言。
        self.monitor(2, ('A',))
        safe = self.samples['A'][-1]
        for key in ('Health', 'Gold', 'XP', 'AttackBase', 'AttackCurrent'): assert safe[key] == left[key]
        assert safe['Training']['Kills'] == left['Training']['Kills']
        self.emit('LEAVE_SAFE_PASS', before=left, after=safe, peer=peer)
        self.focus('B'); self.n.click_training(0)
        self.record('09-Both-Returned', lambda: None, .2)

    def main_wave(self):
        self.focus('A')
        self.record('10-Main-Auto-No-Movement', lambda: (self.gameplay_key(13), self.monitor(15)), .2)
        self.emit('NO_MANUAL_ATTACK_KEYS', keys=self.keys, scope='所有玩法键仅Enter；只读控制台Enter与玩法输入分列')

    def authority(self):
        events = parse_r34(self.text('Server'))
        attacks = [e for e in events if e['kind'] == 'ATTACK']
        hits = [e for e in events if e['kind'] == 'HIT']
        configs = {e['Source']: e for e in events if e['kind'] == 'CONFIG'}
        assert attacks and hits
        damage = []
        for attack in attacks:
            selected = [h for h in hits if h['Actor'] == attack['Actor']]
            if selected: damage.append(dict(actor=attack['Actor'], **validate_attack_hits(attack, selected)))
        expected = {'Default', 'Wave1Minion'} | ({'TrainingDummy'} if self.args.scenario == 'full' else set())
        timings = validate_timing_groups(attacks, configs, expected)
        for config in configs.values():
            if config['Row'] == 'TrainingDummy': assert config['AttackDamage'] == self.monster_attack and config['Health'] == 70
            if config['Row'] == 'Wave1Minion': assert config['AttackDamage'] == 10 and config['Health'] == 100
        skill_events = skill.parse_events(self.text('Server'))
        geometry = []
        for h in (e for e in skill_events if e['kind'] == 'HIT'):
            positions = {h['Caster']: tuple(h[k] for k in ('SourceX', 'SourceY', 'SourceZ')),
                         h['Target']: tuple(h[k] for k in ('TargetX', 'TargetY', 'TargetZ'))}
            geometry.append(validate_rooms([h], positions))
        positions = []
        for role, samples in self.samples.items():
            for inside in (False, True):
                rows = [s for s in samples if bool(s['Training'].get('bInside')) == inside]
                if len(rows) >= 2:
                    points = [s['observation']['position'] for s in rows]
                    positions.append(dict(role=role, inside=inside, samples=len(rows), first=points[0], last=points[-1],
                                          maximum_displacement=max(math.dist(points[0], p) for p in points)))
        multi = [d for d in damage if len(d['targets']) >= 2]
        self.emit('AUTHORITY_PASS', attacks=len(attacks), hits=len(hits), timings=timings, damage=damage,
                  geometric_room_checks=len(geometry), stationary_observations=positions, multi_target_melee=multi)
        if self.args.map == 'r34ranged': self.ranged(attacks, hits, skill_events)
        else:
            assert multi, '尚未取得一次扇形命中多敌的真实HP证据'
            self.peers(next(a for a in attacks if a['Actor'] == multi[0]['actor']), False)

    def peers(self, attack, feedback):
        server = skill.parse_netguids(self.text('Server'))
        result = []
        guid = server.get(attack['Actor'])
        assert guid, 'DS缺少攻击NetGUID'
        for role in ('A', 'B'):
            names = [name for name, value in skill.parse_netguids(self.text(role)).items() if value == guid]
            assert len(names) == 1, '相关客户端没有唯一NetGUID映射'
            skill.validate_peer(skill.parse_events(self.text(role)), names[0], feedback)
            result.append(dict(role=role, actor=names[0], guid=guid))
        self.emit('DUAL_PEER_PASS', server_actor=attack['Actor'], peers=result)
        return result

    def ranged(self, attacks, hits, skill_events):
        candidates, rejected = [], []
        server_guids = skill.parse_netguids(self.text('Server'))
        for attack in attacks:
            if attack['Type'] != 2 or not attack['Caster'].startswith('Y01PlaygroundCharacter'): continue
            selected = [h for h in hits if h['Actor'] == attack['Actor']]
            if len(selected) != 1: continue
            try:
                result = validate_ranged(attack, selected, skill_events)
                speed = validate_r34_speed(skill_events, attack['Actor'])
                peers = self.peers(attack, True)
                midflight = []
                target_guid = server_guids.get(attack['Target'])
                assert target_guid
                for role in ('A', 'B'):
                    names = [n for n, g in skill.parse_netguids(self.text(role)).items() if g == target_guid]
                    assert len(names) == 1
                    try:
                        midflight.append(dict(role=role, **skill.validate_midflight_health(skill_events, attack['Actor'],
                                         self.text(role), names[0], selected[0]['BeforeHP'])))
                    except AssertionError: pass
                assert midflight, '没有批量哨兵收齐的在途目标HP观测'
                candidates.append(dict(result=result, speed=speed, peers=peers, midflight=midflight))
                if len(candidates) >= 2: break
            except AssertionError as error: rejected.append(dict(actor=attack['Actor'], reason=str(error)))
        self.emit('RANGED_CANDIDATES', passed=candidates, rejected=rejected)
        assert candidates, '没有完整飞行/在途HP/速度/双端NetGUID/消失证据链'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', choices=('main', 'r34compare', 'r34ranged'), default='main')
    parser.add_argument('--scenario', choices=('full', 'wave'), default='full')
    parser.add_argument('--port', type=int, default=18434)
    parser.add_argument('--label', default='R34-Visible-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--audit-only', action='store_true')
    parser.add_argument('--audit-final', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'R34-Visible-[A-Za-z0-9_-]+', args.label): parser.error('必须使用唯一安全Label')
    if not 1024 <= args.port <= 65535: parser.error('端口不合法')
    args.fast_observe = True
    if args.audit_final: return audit_final(args.label)
    if args.audit_only:
        result = skill.audit_frames(ROOT / 'Saved' / args.label, pixels=True)
        target = ROOT / 'Saved' / args.label / 'audit-pixels.json'
        with target.open('x', encoding='utf-8') as out: json.dump(result, out, ensure_ascii=False, indent=2)
        print(json.dumps(dict(files=result['files'], series=len(result['series'])))); return 0
    before = protected_hashes()
    runner = R34Runner(args)
    (runner.output / 'protected-before.json').write_text(json.dumps(before, ensure_ascii=False, indent=2), encoding='utf-8')
    code = 1
    try:
        runner.emit('R34_PROVENANCE', pid=os.getpid(), map=args.map, scenario=args.scenario,
                    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    gameplay='原生OS按钮/Enter；禁止1/2；不改Gameplay', console='posted只读批量并等待末尾完整哨兵',
                    screenshots='真实PrintWindow原图，按实测采样间隔判断瞬态，不由目标帧率推断')
        runner.launch(); runner.equipment()
        if args.scenario == 'full': runner.training()
        runner.main_wave(); runner.authority()
        code = 0
    except BaseException: runner.emit('RUN_FAILED', error=traceback.format_exc())
    finally:
        try: runner.cleanup()
        except BaseException:
            code = 1; runner.emit('CLEANUP_FAILED', error=traceback.format_exc())
        after = protected_hashes()
        unchanged = before == after
        if not unchanged: code = 1
        (runner.output / 'protected-after.json').write_text(json.dumps(after, ensure_ascii=False, indent=2), encoding='utf-8')
        try:
            audit = skill.audit_frames(runner.output)
            (runner.output / 'audit.json').write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding='utf-8')
        except BaseException:
            audit = {}; code = 1; runner.emit('AUDIT_FAILED', error=traceback.format_exc())
        result = dict(code=code, pid=os.getpid(), map=args.map, scenario=args.scenario, exits=runner.exits,
                      owned_remaining=[p.pid for p in runner.owned.values() if p.poll() is None],
                      protected_unchanged=unchanged, png_count=audit.get('files', 0),
                      scope='仅实际执行的断言；视觉须另读关键原图。DS清理退出不等于自然退出0。')
        runner.emit('RUNNER_EXIT', **result)
        (runner.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        runner.trace.close()
    return code


if __name__ == '__main__': sys.exit(main())
