"""R27 双D3D12验证；自然只读与显式隔离严格分开，完整PNG不加工。"""
import argparse
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import traceback

spec = importlib.util.spec_from_file_location('r27_damage_helpers', Path(__file__).with_name('Test-Y01DamageVisible.py'))
damage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(damage)
skill = damage.skill
ROOT = damage.ROOT


def parse_rows(text):
    rows = []
    for line in text.splitlines(keepends=True):
        if not line.endswith('\n') or 'Y01R27 {' not in line: continue
        rows.append(json.loads(line.split('Y01R27 ', 1)[1]))
    return rows


def canonical_snapshot(row, server_guids, client_guids):
    result = copy.deepcopy(row)
    server_names = {guid: name for name, guid in server_guids.items()}
    for c in result.get('candidates', []):
        c['local_name'] = c['name']
        c['netguid'] = client_guids.get(c['name'])
        c['name'] = server_names.get(c['netguid'], c['name'])
    for lease in result.get('leases', []):
        lease['local_target'] = lease['target']
        lease['target'] = server_names.get(client_guids.get(lease['target']), lease['target'])
    return result


def qualified(row):
    return [c for c in row['candidates'] if c['eligible'] and c['in_view'] and c['health'] > 0
            and not c['dead'] and all(c[k] == row[k] for k in ('epoch', 'region', 'session'))]


def audit_snapshot(row, competition=False):
    assert row['net'] == 3 and not row['null_rhi'], '必须真实GPU客户端'
    assert row['prewarm'] == 32 and row['cap'] == 128 and abs(row['hold'] - .3) < .00001
    assert 32 <= row['objects'] <= 128 and row.get('components') == row['objects'], '真实对象数越界或组件数未测量'
    assert row['active'] == len(row['leases']) == row['visible'], '活动/可见租约不一致'
    by_name = {c['name']: c for c in row['candidates']}
    assert len(by_name) == len(row['candidates'])
    q = qualified(row)
    assert all(math.isfinite(c['distance']) for c in q), '非有限距离'
    names = [l['target'] for l in row['leases']]
    assert len(names) == len(set(names)), '同目标重复租约'
    slots = [l.get('slot', -1) for l in row['leases']]
    assert len(slots) == len(set(slots)) and all(0 <= s < row['objects'] for s in slots), '真实槽位重复或越界'
    for lease in row['leases']:
        assert int(lease.get('version', 0)) > 0, '非活动租约版本'
        c = by_name[lease['target']]
        assert bool(lease.get('holding')) == bool(c['dead'] or c['health'] <= 0), '死亡短留与实际死亡状态不符'
        assert c['eligible'] and c['in_view'] and all(c[k] == row[k] for k in ('epoch', 'region', 'session')), '未授权/无资格目标'
        assert lease['visible'] and lease['render_target'] and lease['owner_view'], '组件不具备可见渲染条件'
        assert abs(lease['fraction'] - (0 if lease.get('holding') else c['health'] / c['maximum'])) < .0001, '复用旧血量'
        assert math.dist((lease['x'], lease['y'], lease['z']), (c['x'], c['y'], c.get('anchor_z', c['z'] + 108))) < .2, '没有跟随本任目标'
    if competition:
        assert len(q) == 129 and row['active'] == 128, '需要129个同区镜头内已伤存活候选和128真实活动租约'
        selected = set(names)
        assert selected <= {c['name'] for c in q}
        omitted = [c for c in q if c['name'] not in selected]
        assert len(omitted) == 1
        assert omitted[0]['distance'] >= max(by_name[n]['distance'] for n in names) - .001, '未选择最近128'
    return dict(qualified=len(q), active=row['active'], objects=row['objects'], visual_status='pending_original_png_review')


def audit_replacement(before, after, entered):
    audit_snapshot(before, True); audit_snapshot(after, True)
    old = {l['target']: l for l in before['leases']}
    new = {l['target']: l for l in after['leases']}
    evicted = set(old) - set(new)
    assert set(new) - set(old) == {entered} and len(evicted) == 1, '并非一进一出'
    target = evicted.pop()
    assert new[entered]['slot'] == old[target]['slot'], '未复用被归还的真实槽位'
    assert int(new[entered]['version']) > int(old[target]['version']), '复用未更新租约版本'
    return dict(evicted=target, entered=entered, slot=new[entered]['slot'],
                old_version=old[target]['version'], new_version=new[entered]['version'])


def audit_death_hold(rows, target):
    observed = []
    for row in rows:
        candidate = next((c for c in row['candidates'] if c['name'] == target), None)
        lease = next((l for l in row['leases'] if l['target'] == target), None)
        if candidate: observed.append((row, candidate, lease))
    first = next((i for i, (_, c, _) in enumerate(observed) if c['dead'] or c['health'] <= 0), -1)
    assert first > 0 and observed[first - 1][2], '缺少死亡前已有租约'
    returned = next((i for i in range(first, len(observed)) if not observed[i][2]), -1)
    assert returned > first + 1, '缺少连续短留，不能以最终无条冒充'
    samples = observed[first:returned + 1]
    for row, candidate, lease in samples[:-1]:
        assert candidate['health'] == 0 and lease['holding'] and lease['fraction'] == 0 and lease['visible']
        assert lease['slot'] == observed[first - 1][2]['slot'] and lease['version'] == observed[first - 1][2]['version']
    elapsed = samples[-1][0]['world'] - samples[0][0]['world']
    gap = max(b[0]['world'] - a[0]['world'] for a, b in zip(samples, samples[1:]))
    assert gap <= .15 and .22 <= elapsed <= .45, '死亡短留时长不符或采样不足'
    return dict(observed_seconds=elapsed, max_sample_gap=gap, held_samples=len(samples)-1,
                first=samples[0][0], returned=samples[-1][0])


def audit_natural_states(rows):
    untouched, damaged, leaves = {}, {}, []
    previous = None
    for row in rows:
        if row['objects'] < 32: continue
        scope = tuple(row[k] for k in ('epoch', 'region', 'session'))
        candidates = {c['name']: c for c in row['candidates']}
        for c in candidates.values():
            key = (*scope, c['name'])
            if not c['eligible'] and c['health'] == c['maximum'] and c['maximum'] > 0:
                untouched.setdefault(key, row)
        for lease in row['leases']:
            c = candidates[lease['target']]
            assert c['eligible'] and all(c[k] == row[k] for k in ('epoch', 'region', 'session')), '自然场跨区域租约'
            key = (*scope, c['name'])
            if key in untouched and 0 < c['health'] < c['maximum'] and row['region'] not in damaged:
                damaged[row['region']] = dict(before=untouched[key], after=row, target=c['name'])
        if previous and previous['region'] > 0 and row['region'] == 0:
            assert row['active'] == len(row['leases']) == 0, '离区后仍保留旧租约'
            leaves.append(dict(before=previous, after=row))
        previous = row
    assert 0 in damaged and any(region > 0 for region in damaged), '缺少自然主线/私房首次实际受伤资格链'
    assert leaves, '缺少自然离房回收'
    return dict(first_damage_regions=list(damaged), first_damage=damaged, leave_count=len(leaves), leaves=leaves)


def frame_for_snapshot(row, frames):
    candidates = [f for f in frames if f.get('verified')]
    assert candidates, '缺少经完整性校验的原图'
    frame = min(candidates, key=lambda f: abs((f['start_ns'] + f['end_ns']) / 2 - row['wall_ns']))
    delta = ((frame['start_ns'] + frame['end_ns']) / 2 - row['wall_ns']) / 1e6
    assert abs(delta) <= 150, '原图与租约时间相差超过150ms'
    return dict(frame, snapshot_delta_ms=delta)


def audit_existing(directory, label):
    directory = directory.resolve()
    assert directory.parent == (ROOT / 'Saved').resolve(), '仅审核Saved直接子目录'
    original = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    trace = [json.loads(line) for line in (directory / 'runner.jsonl').read_text(encoding='utf-8').splitlines() if line]
    text = {r: (directory / (r + '.log')).read_text(encoding='utf-8', errors='replace') for r in ('Server', 'A', 'B')}
    rows = {r: [s for s in parse_rows(t) if s['kind'] == 'snapshot'] for r, t in text.items()}
    starts = [r for r in trace if r['kind'] == 'OWNED_START']
    assert len(starts) == 3 and len({s['pid'] for s in starts}) == 3
    for r in ('A', 'B'):
        start = next(s for s in starts if s['role'] == r)
        assert '-d3d12' in start['command'] and '-nullrhi' not in start['command'] and 'D3D12' in text[r]
    assert rows['Server'] and all(s['net'] == 1 and s['active'] == s['objects'] == s['components'] == 0 for s in rows['Server'])
    audit = skill.audit_frames(directory)
    frames, keyframes = {}, []
    for path in directory.glob('*/*-frames.jsonl'):
        role = path.name.split('-')[0]
        series = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line]
        frames.setdefault(role, []).extend(series)
        sampled = [s for s in rows[role] if series[0]['start_ns'] <= s['wall_ns'] <= series[-1]['end_ns']]
        if sampled:
            row = next((s for s in reversed(sampled) if s['active']), sampled[-1])
            keyframes.append(dict(label=path.parent.name, role=role, frame=frame_for_snapshot(row, series), snapshot=row))
    before = json.loads((directory / 'protected-before.json').read_text(encoding='utf-8'))
    after = json.loads((directory / 'protected-after.json').read_text(encoding='utf-8'))
    assert before == after == protected_hashes() and not original['owned_remaining']
    result = dict(original=original, processes=starts, protected_files=len(before), protected_unchanged=True,
                  png_count=audit['files'], ds_snapshots=len(rows['Server']), ds_ui_instances=0,
                  keyframes=keyframes, visual_status='pending_original_png_review', source=str(directory))
    if original['mode'] == 'natural':
        result['natural'] = {r: audit_natural_states(rows[r]) for r in ('A', 'B')}
        for role, evidence in result['natural'].items():
            for region, chain in evidence['first_damage'].items():
                chain['before_frame'] = frame_for_snapshot(chain['before'], frames[role])
                chain['after_frame'] = frame_for_snapshot(chain['after'], frames[role])
        event_report = json.loads((directory / 'event-frame-report.json').read_text(encoding='utf-8'))
        result['damage_delivery'] = event_report['delivery']; result['damage_pools'] = event_report['pools']
        guids = {r: skill.parse_netguids(t) for r, t in text.items()}
        coexist = []
        for role in ('A', 'B'):
            for region in result['natural'][role]['first_damage_regions']:
                for link in event_report['links']:
                    event = link['event']
                    if link['role'] != role or event['Region'] != region or not link.get('hit'): continue
                    guid = guids['Server'].get(link['hit']['Target'])
                    names = {n for n, g in guids[role].items() if g == guid}
                    matching = [s for s in rows[role] if event['t'] + .08 <= s['wall_ns']/1e9 <= event['t'] + .6
                                and any(l['target'] in names for l in s['leases'])]
                    if matching and link['frames']:
                        s = matching[len(matching)//2]
                        try: f = frame_for_snapshot(s, link['frames'])
                        except AssertionError: continue
                        coexist.append(dict(role=role, region=region, target_guid=guid, event=event,
                                            hit=link['hit'], snapshot=s, frame=f))
                        break
        assert len(coexist) == sum(len(r['first_damage_regions']) for r in result['natural'].values()), '缺少各端各区数字与血条同帧证据'
        result['coexist'] = coexist
    else:
        result['competition'] = [r for r in trace if r['kind'] == 'COMPETITION_PASS']
        result['replacement'] = [r for r in trace if r['kind'] == 'REPLACEMENT_PASS']
        result['recovery'] = [r for r in trace if r['kind'] == 'RECOVERY_PASS']
        result['death_hold'] = [r for r in trace if r['kind'] == 'DEATH_HOLD_PASS']
        result['late_join'] = [r for r in trace if r['kind'] == 'LATE_JOIN_PASS']
        for row in result['competition']: audit_snapshot(row['snapshot'], True)
        assert all(len(result[k]) == 2 for k in ('competition', 'replacement', 'recovery', 'death_hold'))
        for row in result['death_hold']:
            row['frame'] = frame_for_snapshot(row['result']['first'], frames[row['role']])
    result['code'] = original['code']
    path = ROOT / 'Saved' / (label + '-report.json')
    with path.open('x', encoding='utf-8') as out: json.dump(result, out, ensure_ascii=False, indent=2)
    print(json.dumps(dict(code=result['code'], report=str(path), png=result['png_count'], keyframes=len(keyframes)), ensure_ascii=False))
    return result['code']


def assert_idle(processes):
    if processes:
        raise RuntimeError('检测到非本次UE/构建占用，不关闭、不接管：' + json.dumps(processes, ensure_ascii=False))


def existing_ue():
    command = ("[Console]::OutputEncoding=[Text.UTF8Encoding]::new(); "
               "@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'UnrealEditor|UnrealBuildTool|Y01Server' } | "
               "Select-Object ProcessId,ParentProcessId,Name,CommandLine) | ConvertTo-Json -Compress")
    output = subprocess.check_output([str(Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'),
                                     '-NoProfile', '-NonInteractive', '-Command', command], encoding='utf-8')
    value = json.loads(output) if output.strip() else []
    return value if isinstance(value, list) else [value]


def audit_late_join(server, client, started_ns):
    assert server['actual'] > 0 and server['damage_ns'] < server['heal_ns'] < started_ns < client['first_seen_ns'], '第二进程必须在服务器受伤并回满后才启动'
    assert server['target'] == client['target'] and server['epoch'] == client['epoch']
    assert client['eligible'] and client['visible'] and client['health'] == client['maximum'] == server['health'] == server['maximum']
    return dict(snapshot_qualification=True, visual_status='pending_original_png_review')


def launch_command(role, port, log, mode, command_file):
    assert mode in ('natural', 'controlled')
    args = damage.launch_command(role, port, log, 'natural')
    args += ['-Y01R27Trace']
    if mode == 'controlled': args += ['-Y01R27Verify', '-Y01R27CommandFile=' + str(command_file).replace('\\', '/')]
    return args


def protected_hashes():
    # Docs由主代理独占，不作为本轮运行中禁止变化的断言。
    roots = ('Config', 'Content/Y01Playground', 'Content/Y01EquipmentLab', 'UI_Deliverables', 'ArtSource')
    return {str(p.relative_to(ROOT)): damage.hashlib.sha256(p.read_bytes()).hexdigest()
            for root in roots for p in (ROOT / root).rglob('*') if p.is_file()}


class R27Runner(damage.DamageRunner):
    def __init__(self, args):
        super().__init__(args)
        self.command_file = self.output / 'command.txt'
        self.sequence = 0
        self.started = {}
        self.checks = []

    def rows(self, role):
        return parse_rows(self.text(role))

    def snap(self, role):
        row = next((r for r in reversed(self.rows(role)) if r['kind'] == 'snapshot'), {})
        if role == 'Server': return row
        return canonical_snapshot(row, skill.parse_netguids(self.text('Server')), skill.parse_netguids(self.text(role)))

    def start_role(self, role):
        self.logs[role] = self.output / (role + '.log')
        args = launch_command(role, self.args.port, self.logs[role], self.args.mode, self.command_file)
        self.active = None
        self.started[role] = time.time_ns()
        self.owned[role] = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        self.emit('OWNED_START', role=role, pid=self.owned[role].pid, command=subprocess.list2cmdline(args), start_ns=self.started[role])
        if role == 'Server':
            self.wait(lambda: 'listening on port' in self.text(role), 'DS监听', 70)
            return
        def ready():
            self.n.p = self.owned[role]
            hwnd = self.n.locate()
            if not hwnd or not self.snap(role).get('objects'): return False
            self.windows[role] = hwnd
            return True
        self.wait(ready, role + '真实D3D12窗口及授权池', 120)
        assert 'D3D12' in self.text(role) and '-nullrhi' not in self.text(role).lower()
        if self.args.mode == 'natural': self.focus(role)

    def launch(self):
        processes = existing_ue()
        self.emit('PREFLIGHT', existing_ue=processes)
        assert_idle(processes)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe: probe.bind(('127.0.0.1', self.args.port))
        self.start_role('Server'); self.start_role('A')
        if self.args.mode == 'natural':
            self.start_role('B')
            self.focus('A')

    def pause(self, seconds):
        if self.args.mode == 'controlled': time.sleep(seconds)
        else: self.n.hold(seconds)

    def record(self, label, action, after=1):
        recorder = skill.Recorder(self, label)
        if 'B' not in self.windows:
            recorder.threads = recorder.threads[:1]
            recorder.frames.pop('B')
        self.records[label] = recorder
        recorder.start()
        try:
            self.emit('ACTION_BEGIN', label=label)
            action(); self.pause(after)
            self.emit('ACTION_END', label=label)
        finally: recorder.stop()
        return recorder

    def command(self, action):
        self.sequence += 1
        self.command_file.write_text(f'{self.sequence}|{action}\n', encoding='utf-8')
        row = self.wait(lambda: next((r for r in reversed(self.rows('Server')) if r['kind'] == 'action'
            and r['sequence'] == self.sequence), None), '服务器动作确认 ' + action, 10)
        assert row['ok'], 'GAS/动作拒绝：' + action
        self.emit('CONTROLLED_ACTION', action=action, acknowledgement=row, mode='controlled_not_natural')

    def step(self, label, action, predicate=None, seconds=1):
        def run():
            self.command(action)
            if predicate: self.wait(predicate, '复制及调度 ' + label, 15)
        self.record(label, run, seconds)
        for role in self.windows:
            row = self.snap(role)
            self.emit('STEP_SNAPSHOT', label=label, role=role, snapshot=row)
        self.checks.append(label)

    def target(self, role, name='R27_Target_000'):
        row = self.snap(role)
        c = next((c for c in row.get('candidates', []) if c['name'] == name), None)
        l = next((l for l in row.get('leases', []) if l['target'] == name), None)
        return c, l

    def both(self, predicate):
        return all(predicate(role) for role in self.windows)

    def controlled(self):
        self.record('01-Prewarm32', lambda: None, .3)
        assert self.snap('A')['objects'] == 32 and self.snap('A')['active'] == 0
        self.step('02-Undamaged-Boss', 'create', lambda: self.target('A')[0] is not None)
        assert self.snap('A')['active'] == 0
        self.step('03-First-Actual-Damage', 'damage', lambda: self.target('A')[1] is not None)
        self.step('04-Healed-Still-Visible', 'heal', lambda: self.target('A')[0]['health'] == 100 and self.target('A')[1]['fraction'] == 1)
        self.start_role('B')
        self.record('05-Real-Late-Join', lambda: self.wait(lambda: self.target('B')[1] is not None, '晚加入快照资格', 20), 1)
        d = next(r for r in self.rows('Server') if r['kind'] == 'damage' and r['target'] == 'R27_Target_000')
        h = next(r for r in self.rows('Server') if r['kind'] == 'heal')
        c, l = self.target('B')
        late = audit_late_join(dict(target=h['target'], epoch=h['epoch'], damage_ns=d['wall_ns'], heal_ns=h['wall_ns'],
                                   actual=d['actual'], health=h['health'], maximum=h['maximum']),
                              dict(target=c['name'], epoch=c['epoch'], first_seen_ns=self.snap('B')['wall_ns'], health=c['health'],
                                   maximum=c['maximum'], eligible=c['eligible'], visible=l['visible']), self.started['B'])
        self.emit('LATE_JOIN_PASS', result=late, server_damage=d, server_heal=h, second_process_started_ns=self.started['B'])
        self.step('06-Follow', 'follow', lambda: self.both(lambda r: self.target(r)[1] and abs(self.target(r)[1]['x']-250) < .2))
        self.step('07-Offscreen', 'offscreen', lambda: self.both(lambda r: self.snap(r)['active'] == 0))
        self.step('08-Backscreen', 'backscreen', lambda: self.both(lambda r: self.target(r)[1] is not None))
        self.step('09-Occlusion', 'occlude')
        self.step('10-Unoccluded', 'unocclude')
        self.step('11-Grid129-Nearest128', 'grid', lambda: self.both(lambda r: self.snap(r)['active'] == 128), 2)
        before = {r: self.snap(r) for r in self.windows}
        for r, row in before.items(): self.emit('COMPETITION_PASS', role=r, result=audit_snapshot(row, True), snapshot=row)
        omitted = next(c for c in qualified(before['A']) if c['name'] not in {l['target'] for l in before['A']['leases']})
        index = int(omitted['name'].rsplit('_', 1)[1])
        self.step('12-Nearest-Replaces-Farthest', f'near:{index}', lambda: self.both(lambda r: self.target(r, omitted['name'])[1] is not None), 2)
        after = {r: self.snap(r) for r in self.windows}
        replacements = {}
        for r, row in after.items():
            replacements[r] = audit_replacement(before[r], row, omitted['name'])
            self.emit('REPLACEMENT_PASS', role=r, result=replacements[r], snapshot=row)
        self.step('13-Destroy-Recover-Free-Slot', f'destroy:{index}', lambda: self.both(lambda r: len(qualified(self.snap(r))) == 128 and self.snap(r)['active'] == 128), 1)
        for role in self.windows:
            recovered = self.snap(role); audit_snapshot(recovered)
            _, lease = self.target(role, replacements[role]['evicted'])
            assert lease and int(lease['version']) > int(replacements[role]['new_version'])
            self.emit('RECOVERY_PASS', role=role, recovered=replacements[role]['evicted'], snapshot=recovered)
        death_before = {r: self.snap(r) for r in self.windows}
        self.step('14-Death-Hold-Then-Return', 'death', lambda: self.both(lambda r: self.target(r)[0]['dead'] and self.target(r)[1] is None), 1)
        for role in self.windows:
            local = next(c['local_name'] for c in death_before[role]['candidates'] if c['name'] == 'R27_Target_000')
            samples = [row for row in self.rows(role) if row['kind'] == 'snapshot' and row['world'] >= death_before[role]['world']]
            self.emit('DEATH_HOLD_PASS', role=role, target=local, result=audit_death_hold(samples, local))
        self.step('15-Destroy-Dead-Immediate', 'destroy:0', lambda: self.both(lambda r: self.target(r)[0] is None), .5)
        self.step('16-Private-Room-A', 'enter', lambda: self.snap('A')['region'] > 0)
        old_session = self.snap('A')['session']
        self.step('17-Private-Actual-Damage', 'roomdamage', lambda: self.snap('A')['active'] > 0)
        private = self.snap('A'); assert private['region'] != self.snap('B')['region']
        assert not ({l['target'] for l in private['leases']} & {l['target'] for l in self.snap('B')['leases']})
        self.step('18-Leave-Clear-Session', 'leave', lambda: self.snap('A')['region'] == 0)
        self.step('19-Reenter-New-Session', 'reenter', lambda: self.snap('A')['session'] > old_session)
        assert self.snap('A')['active'] == 0
        self.step('20-New-Session-Damage', 'roomdamage', lambda: self.snap('A')['active'] > 0)
        self.step('21-Leave-Again', 'leave', lambda: self.snap('A')['region'] == 0)
        self.step('22-Clear-All', 'clear', lambda: self.both(lambda r: self.snap(r)['active'] == 0))
        assert all(row['objects'] == row['components'] == row['active'] == 0 for row in self.rows('Server') if row['kind'] == 'snapshot'), 'DS出现UI'
        self.emit('CONTROLLED_PASS', checks=self.checks, visual_status='pending_original_png_review')

    def natural(self):
        for role in ('A', 'B'): self.sample(role)
        self.focus('A')
        self.record('01-Training-A-Only', lambda: self.n.click_training(1), 4)
        assert self.snap('A')['region'] > 0 and self.snap('B')['region'] == 0 and self.snap('B')['active'] == 0
        self.focus('B')
        self.record('02-Training-Separate', lambda: self.n.click_training(1), 4)
        assert self.snap('A')['region'] != self.snap('B')['region'] and self.snap('B')['region'] > 0
        for role in ('A', 'B'):
            self.focus(role)
            self.record('Leave-' + role, lambda: (self.n.click_training(0), self.wait(
                lambda: self.snap(role)['region'] == 0 and self.snap(role)['active'] == 0, '离房全部归还')), .6)
            self.emit('NATURAL_LEAVE_PASS', role=role, snapshot=self.snap(role))
        self.focus('A')
        self.record('03-Main-Natural-Auto', lambda: self.gameplay_key(13), 17)
        self.record('04-Main-Continued', lambda: None, 2)
        self.emit('NO_MANUAL_SKILLS', keys=self.keys, rule='仅原生训练按钮及Enter；不改相机、不停普攻')
        rows = {r: self.rows(r) for r in ('A', 'B')}
        for role, snapshots in rows.items():
            snapshots = [s for s in snapshots if s['kind'] == 'snapshot']
            assert any(s['active'] > 0 and s['region'] == 0 for s in snapshots), '自然主线没有活动血条'
            assert any(s['active'] > 0 and s['region'] > 0 for s in snapshots), '自然私房没有活动血条'
            assert all(s['objects'] <= 128 for s in snapshots)
        self.emit('NATURAL_STATE_PASS', scope='自然普攻/按钮；视觉需原图复核')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=('natural', 'controlled'), default='controlled')
    p.add_argument('--audit-existing', type=Path)
    p.add_argument('--label', required=True); p.add_argument('--port', type=int, default=18527)
    args = p.parse_args()
    if not re.fullmatch(r'R27-U3-[A-Za-z0-9_-]+', args.label) or not 1024 <= args.port <= 65535: p.error('不安全Label或端口')
    if args.audit_existing: return audit_existing(args.audit_existing, args.label)
    args.map='main'; args.fast_observe=True; args.scenario='natural'
    before = protected_hashes()
    runner = R27Runner(args)
    (runner.output / 'protected-before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    code, audit = 1, {}
    try:
        runner.emit('PROVENANCE', mode=args.mode, pid=os.getpid(), assets='瞬态副本，不保存main；自然不修改配置',
                    screenshots='既有PrintWindow完整原图，无加工', visual_status='pending_original_png_review')
        runner.launch()
        if args.mode == 'controlled': runner.controlled()
        else: runner.natural()
        code=0
    except BaseException: runner.emit('RUN_FAILED', error=traceback.format_exc())
    finally:
        try: runner.cleanup()
        except BaseException:
            code=1; runner.emit('CLEANUP_FAILED', error=traceback.format_exc())
        after=protected_hashes()
        (runner.output / 'protected-after.json').write_text(json.dumps(after, indent=2), encoding='utf-8')
        if before != after: code=1
        try: audit=skill.audit_frames(runner.output)
        except BaseException:
            code=1; runner.emit('PNG_AUDIT_FAILED', error=traceback.format_exc())
        if args.mode == 'natural' and code == 0:
            try: damage.audit_run(runner)
            except BaseException:
                code=1; runner.emit('DAMAGE_REGRESSION_FAILED', error=traceback.format_exc())
        result=dict(code=code, mode=args.mode, exits=runner.exits, protected_unchanged=before==after,
                    png_count=audit.get('files',0), checks=runner.checks, visual_status='pending_original_png_review',
                    owned_remaining=[x.pid for x in runner.owned.values() if x.poll() is None])
        (runner.output / 'audit.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
        (runner.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        runner.emit('RUNNER_EXIT', **result); runner.trace.close()
    return code


if __name__ == '__main__': sys.exit(main())
