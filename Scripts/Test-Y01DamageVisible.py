"""伤害数字薄验收驱动：复用旧安全输入及PrintWindow，原图不加工。"""
import argparse
from collections import Counter
import ctypes as c
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import traceback
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('damage_r34_helpers', Path(__file__).with_name('Test-Y01R34Visible.py'))
r34 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r34)
skill = r34.skill


def parse_damage(text):
    rows = []
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        if not line.endswith('\n'): continue
        match = re.search(r'^\[([\d.\-:]+)\].*?(Y01DamageNumber|Y01Damage|Y01R34) (\w+) (.+)', line)
        if not match: continue
        frame = re.search(r'^\[[^\]]+\]\[\s*(\d+)\]', line)
        row = dict(family=match[2], kind=match[3], line=number, raw=line.rstrip(), frame=int(frame[1]) if frame else None,
                   t=datetime.strptime(match[1], '%Y.%m.%d-%H.%M.%S:%f').replace(tzinfo=timezone.utc).timestamp())
        for key, value in re.findall(r'(\w+)=([^\s]+)', match[4]):
            if key != 'Epoch' and re.fullmatch(r'-?\d+', value): value = int(value)
            elif key != 'Epoch' and re.fullmatch(r'-?\d+(?:\.\d+)?(?:e[+-]?\d+)?', value, re.I): value = float(value)
            row[key] = value
        rows.append(row)
    return rows


def pool_audit(rows):
    rows = [r for r in rows if r['family'] == 'Y01DamageNumber']
    prewarm = [r for r in rows if r['kind'] == 'PREWARM']
    assert len(prewarm) == 1 and prewarm[0]['Count'] == 32 and prewarm[0]['Cap'] == 128
    active, slots, peak, reused, natural, previous = {}, Counter(), 0, 0, 0, None
    releases = []
    for row in rows:
        if row['kind'] == 'BORROW':
            slot = row['Slot']
            assert 0 <= slot < 128 and slot not in active, 'slot越界/没有先归还'
            if row['Oldest']:
                assert previous and previous['slot'] == slot and previous['count'] == 128
                assert previous['serial'] == previous['minimum'], '未复用最旧条目'
                reused += 1
            active[slot] = row
            slots[slot] += 1
            peak = max(peak, len(active))
        elif row['kind'] == 'RETURN':
            matched = [s for s, a in active.items() if a['Id'] == row['Id'] and a['Serial'] == row['Serial']]
            assert len(matched) == 1, '归还不匹配'
            previous = dict(slot=matched[0], count=len(active), serial=row['Serial'],
                            minimum=min(a['Serial'] for a in active.values()))
            active.pop(matched[0])
            natural += row['Age'] >= .8 - .000001
        elif row['kind'] == 'RELEASE': releases.append(row['Count'])
    assert not active and len(releases) == 1 and 32 <= releases[0] <= 128, '缺少退出清理'
    return dict(borrows=sum(slots.values()), max_slot=max(slots, default=-1), max_active=peak,
                repeated_slots=sorted(s for s, count in slots.items() if count > 1),
                oldest_reuses=reused, natural_returns=natural, release_counts=releases)


def delivery_audit(server, clients):
    key = lambda r: (r['Player'], r['Epoch'], r['Id'], r['Region'], r['Actual'], r['Display'], r['Type'])
    sent = Counter(key(r) for r in server if r['kind'] == 'SEND')
    received = Counter(key(r) for rows in clients.values() for r in rows if r['kind'] == 'RECEIVE')
    assert sent and sent == received and all(n == 1 for n in sent.values()), '发送/接收身份、载荷或顺序去重异常'
    evidence = []
    for role, rows in clients.items():
        recv = [r for r in rows if r['kind'] == 'RECEIVE']
        borrow = [r for r in rows if r['kind'] == 'BORROW']
        assert [(r['Epoch'], r['Id'], r['Region'], r['Actual'], r['Display'], r['Type']) for r in recv] == [
            (r['Epoch'], r['Id'], r['Region'], r['Actual'], r['Display'], r['Type']) for r in borrow], '接收与展示载荷不符'
        evidence.append(dict(role=role, received=len(recv), regions=sorted({r['Region'] for r in recv})))
    return dict(deliveries=sum(sent.values()), clients=evidence)


def frame_candidates(event, frames):
    return [dict(f, since_borrow_ms=(f['start_ns'] / 1e9 - event['t']) * 1000)
            for f in frames if f['end_ns'] / 1e9 >= event['t'] and f['start_ns'] / 1e9 <= event['t'] + .95]


def launch_command(role, port, log, scenario):
    if role == 'Server':
        binary = 'UnrealEditor-Cmd.exe'
        extra = [r34.resolve_r34_map('main'), '-server', '-nullrhi', '-Y01SkillTrace', f'-port={port}',
                 '-ExecCmds=obj list class=Y01DamageNumberWidget,obj list class=Y01DamageWorldWidgetComponent,obj list class=Y01DamageNumberPoolComponent']
        if scenario == 'stress': extra += ['-Y01DamageNetVerify', '-Y01DamageBurstEvents=128']
    else:
        binary = 'UnrealEditor.exe'
        extra = [f'127.0.0.1:{port}', '-game', '-windowed', '-ResX=1280', '-ResY=800', '-ForceRes', '-d3d12',
                 '-WinX=' + ('10' if role == 'A' else '1260'), '-WinY=40',
                 '-ExecCmds=t.MaxFPS 60,console.searchmode.legacy 1', '-Y01TrainingVerify']
    flush = [] if scenario == 'stress' else ['-forcelogflush']
    return [str(skill.ENGINE / binary), str(ROOT / 'Y01.uproject'), *extra, '-nop4', '-nosound', '-NoSplash',
            '-NoScreenMessages', *flush, '-Y01DamageTrace', '-Y01DamageNumberTrace',
            '-LogCmds=LogNetPackageMap Log', '-abslog=' + str(log)]


def protected_hashes():
    roots = ('Source', 'Content', 'Config', 'Docs', 'UI_Deliverables', 'ArtSource', 'Scripts')
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for root in roots for p in (ROOT / root).rglob('*')
            if p.is_file() and '__pycache__' not in p.parts}


def validate_diagnostic(command):
    for part in command.split('|'):
        assert re.fullmatch(r'\s*(?:getall\s+\w+\s+\w+(?:\s+(?:NAME|OUTER)=\w+)*|getalllocation\s+\w+)\s*', part), '仅允许筛选后的只读属性'


def slot_diagnostic(slot):
    assert isinstance(slot, int) and 0 <= slot < 128
    component = f'getall Y01DamageWorldWidgetComponent {{}} OUTER=Actor_{slot}'
    commands = [component.format(field) for field in ('Widget', 'RenderTarget', 'RelativeLocation', 'RelativeRotation', 'bVisible', 'bOnlyOwnerSee')]
    commands += [f'getall Actor Owner NAME=Actor_{slot}', f'getall Actor bHidden NAME=Actor_{slot}',
                 f'getall Y01DamageNumberWidget RenderOpacity NAME=Y01DamageNumberWidget_{slot}',
                 f'getall TextBlock Text OUTER=Y01DamageNumberWidget_{slot}', 'getall PlayerCameraManager ViewTarget']
    return ' | '.join(commands)


class DamageRunner(r34.R34Runner):
    def launch(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', self.args.port))
        for role in ('Server', 'A', 'B'):
            self.logs[role] = self.output / (role + '.log')
            command = launch_command(role, self.args.port, self.logs[role], self.args.scenario)
            self.owned[role] = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            self.emit('OWNED_START', role=role, pid=self.owned[role].pid, command=subprocess.list2cmdline(command))
            if role == 'Server': self.wait(lambda: 'listening on port' in self.text(role), 'DS监听', 70)
        def ready():
            for role in ('A', 'B'):
                self.n.p = self.owned[role]
                hwnd = self.n.locate()
                if not hwnd or self.telemetry(role).get('HP', 0) <= 0: return False
                self.windows[role] = hwnd
            return True
        self.wait(ready, '两个D3D12窗口及复制遥测', 120)
        time.sleep(2)
        self.focus('A')
        for role in ('A', 'B'):
            assert 'D3D12' in self.text(role) and '-nullrhi' not in self.text(role).lower()
        self.emit('VISIBLE_READY', windows=self.windows, scenario=self.args.scenario)

    def query(self, role, command):
        validate_diagnostic(command)
        self.focus(role)
        offset = len(self.text(role))
        command += ' | getall Y01GameState Snapshot'
        self.n.key(0xC0); self.n.type_text(command[0]); self.n.hold(.03)
        for char in command[1:]:
            self.n.focused('damage-readonly')
            if not self.n.user.PostMessageW(self.n.window, 0x102, ord(char), 0): raise c.WinError(c.get_last_error())
        self.n.key(13)
        self.wait(lambda: r34.complete_batch(self.text(role)[offset:]), '诊断整组查询哨兵', 8)
        result = self.text(role)[offset:]
        self.emit('READONLY_DIAGNOSTIC', role=role, command=command, offset=offset, raw=result)
        return result

    def diagnostics(self, role):
        return self.query(role, 'getall Y01DamageNumberPoolComponent Entries | '
            'getall Y01DamageWorldWidgetComponent RenderTarget | getall Y01DamageWorldWidgetComponent Widget | '
            'getall Y01DamageWorldWidgetComponent bVisible | getall Y01DamageWorldWidgetComponent bOnlyOwnerSee | '
            'getall Y01DamageWorldWidgetComponent DrawSize | getall Y01DamageWorldWidgetComponent Space | '
            'getall Y01DamageNumberWidget RenderOpacity | getall TextBlock Text | '
            'getalllocation Y01DamageWorldWidgetComponent | getall Y01PlaygroundController ViewTarget')

    def natural(self):
        for role in ('A', 'B'): self.sample(role)
        self.focus('A')
        self.record('01-Training-A-Only', lambda: self.n.click_training(1), 6)
        a = self.sample('A'); b = self.sample('B')
        assert a['Training']['bInside'] and not b['Training']['bInside']
        self.focus('B')
        self.record('02-Training-Separate', lambda: self.n.click_training(1), 6)
        a = self.sample('A'); b = self.sample('B')
        assert a['Training']['bInside'] and b['Training']['bInside'] and a['Training']['Room'] != b['Training']['Room']
        self.emit('ROOMS_DISTINCT', A=a['Training'], B=b['Training'])
        for role in ('A', 'B'):
            self.focus(role); self.n.click_training(0)
        self.n.hold(1)
        for role in ('A', 'B'): self.sample(role)
        self.focus('A')
        self.record('03-Main-Natural-Auto', lambda: self.gameplay_key(13), 17)
        for role in ('A', 'B'): self.sample(role)
        self.record('04-Main-Continued', lambda: None, 5)
        for role in ('A', 'B'): self.diagnostics(role)
        self.emit('NO_MANUAL_SKILLS', keys=self.keys, rule='玩法键仅Enter；训练使用原生按钮')

    def diagnose(self):
        self.focus('A')
        def observe_active():
            self.n.click_training(1)
            last = 0
            for _ in range(4):
                event = self.wait(lambda: next((r for r in reversed(parse_damage(self.text('A')))
                    if r['kind'] == 'BORROW' and r['Slot'] == 0 and r['Id'] > last), None), '等待自然普攻活跃slot0', 6)
                last = event['Id']
                self.emit('ACTIVE_DIAGNOSTIC_EVENT', event=event)
                self.query('A', slot_diagnostic(0))
        self.record('Diagnose-Training-Active', observe_active, 1)
        self.record('Diagnose-Training-Clean', lambda: None, 4)
        self.finish_training()
        self.emit('NO_MANUAL_SKILLS', keys=self.keys, rule='仅原生训练按钮及只读属性诊断')

    def finish_training(self):
        self.focus('A')
        self.n.click_training(0)
        self.wait(lambda: self.telemetry('A').get('Inside') == 0, '诊断离房遥测', 8)
        self.n.hold(1.2)
        def drained():
            for role in ('A', 'B'):
                rows = parse_damage(self.text(role))
                borrowed = Counter((r['Id'], r['Serial']) for r in rows if r['kind'] == 'BORROW')
                returned = Counter((r['Id'], r['Serial']) for r in rows if r['kind'] == 'RETURN')
                if borrowed != returned: return False
            return True
        self.wait(drained, '诊断借还全部收齐', 8)
        delivery = delivery_audit(parse_damage(self.text('Server')),
                                  {role: parse_damage(self.text(role)) for role in ('A', 'B')})
        self.emit('TRAINING_DRAINED', delivery=delivery)

    def stress(self):
        for index in range(30):
            self.record(f'Stress-Network-128-{index:02d}', lambda: None, 2)
            assert 'Y01Damage NET_FAIL' not in self.text('Server')
            if 'Y01Damage NET_DONE' in self.text('Server'): break
        else: raise TimeoutError('既有网络场景未完成')
        for role in ('A', 'B'): self.diagnostics(role)
        self.emit('STRESS_SCOPE', natural=False, scenario='既有网络场景；临时内存配置/直接真实扣血，非自然普攻')


def matching_hit(rows, commit):
    following = next((r for r in rows if r['line'] > commit['line'] and
        ((r['family'] == 'Y01Damage' and r['kind'] == 'COMMIT') or
         (r['family'] == 'Y01R34' and r['kind'] == 'HIT'))), None)
    assert following and following['family'] == 'Y01R34' and following['kind'] == 'HIT', '扣血事务间插入其他COMMIT/缺少HIT'
    assert commit['frame'] is not None and following['frame'] == commit['frame'], '扣血事务跨游戏帧'
    assert following['Actual'] == commit['Actual']
    assert abs(following['BeforeHP'] - following['AfterHP'] - commit['Actual']) < .001
    return following


def load_existing(output):
    output = Path(output)
    original = json.loads((output / 'result.json').read_text(encoding='utf-8'))
    records = {}
    for manifest in output.glob('*/*-frames.jsonl'):
        record = records.setdefault(manifest.parent.name, SimpleNamespace(frames={'A': [], 'B': []}))
        role = manifest.name.split('-')[0]
        record.frames[role] = [json.loads(line) for line in manifest.read_text(encoding='utf-8').splitlines() if line]
    return SimpleNamespace(output=output, args=SimpleNamespace(scenario=original['scenario']), records=records,
        text=lambda role: (output / (role + '.log')).read_text(encoding='utf-8', errors='replace'))


def audit_run(runner):
    server = parse_damage(runner.text('Server'))
    clients = {role: parse_damage(runner.text(role)) for role in ('A', 'B')}
    assert not any(r['family'] == 'Y01DamageNumber' or r['kind'] == 'RECEIVE' for r in server), 'DS出现展示实例/消费'
    delivery = delivery_audit(server, clients)
    pools = {role: pool_audit(rows) for role, rows in clients.items()}
    if runner.args.scenario == 'natural':
        assert all(p['release_counts'] == [32] and p['repeated_slots'] and p['natural_returns'] > 0 for p in pools.values())
        main = [{(r['Epoch'], r['Id']) for r in rows if r['kind'] == 'BORROW' and r['Region'] == 0} for rows in clients.values()]
        rooms = [{r['Region'] for r in rows if r['kind'] == 'BORROW' and r['Region'] > 0} for rows in clients.values()]
        assert main[0] and main[0] == main[1] and all(rooms) and rooms[0].isdisjoint(rooms[1])
    links = []
    for role, rows in clients.items():
        frames = [f for recorder in runner.records.values() for f in recorder.frames[role]]
        for event in (r for r in rows if r['kind'] == 'BORROW'):
            returns = [r for r in rows if r['kind'] == 'RETURN' and r['Id'] == event['Id'] and r['Serial'] == event['Serial']]
            commits = [r for r in server if r['kind'] == 'COMMIT' and r.get('Epoch') == event['Epoch'] and r.get('Id') == event['Id']]
            assert len(commits) == len(returns) == 1
            commit = commits[0]
            assert (commit['Actual'], commit['Display'], commit['Type']) == (
                event['Actual'], event['Display'], event['Type']), '提交与展示载荷不符'
            assert abs(event['X'] - commit['X']) < .001 and abs(event['Y'] - commit['Y']) < .001
            assert abs(event['Z'] - commit['Z'] - 100) < .001, '受击位置未冻结或初始高度不符'
            following = matching_hit(server, commit) if runner.args.scenario != 'stress' else None
            links.append(dict(role=role, event=event, returned=returns[0], commit=commit,
                              hit=following,
                              frames=frame_candidates(event, frames)))
    result = dict(delivery=delivery, pools=pools, ds_display_trace_count=0,
                  links=links, visual_pass=None, scope='事件与原图候选时窗对照，不以日志或像素阈值替代人工看图')
    (runner.output / 'event-frame-report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return dict(delivery=delivery, pools=pools, ds_display_trace_count=0, linked_events=len(links))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit-existing', type=Path)
    parser.add_argument('--scenario', choices=('natural', 'stress', 'diagnose'), default='natural')
    parser.add_argument('--port', type=int, default=18497)
    parser.add_argument('--label', default='Damage-Visible-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    args = parser.parse_args()
    if args.audit_existing:
        runner = load_existing(args.audit_existing)
        evidence = audit_run(runner)
        evidence['png_audit'] = skill.audit_frames(runner.output)
        (runner.output / 'reaudit.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: v for k, v in evidence.items() if k != 'png_audit'}, ensure_ascii=False))
        return 0
    if not re.fullmatch(r'Damage-Visible-[A-Za-z0-9_-]+', args.label): parser.error('唯一安全Label不合法')
    if not 1024 <= args.port <= 65535: parser.error('端口不合法')
    args.map = 'main'; args.fast_observe = True
    before = protected_hashes()
    runner = DamageRunner(args)
    (runner.output / 'protected-before.json').write_text(json.dumps(before, ensure_ascii=False, indent=2), encoding='utf-8')
    code, audit, evidence = 1, {}, {}
    try:
        runner.emit('DAMAGE_PROVENANCE', pid=os.getpid(), scenario=args.scenario, map='main',
                    screenshots='旧PrintWindow完整原图；无合成/裁剪/标注', gameplay='natural仅Enter/训练按钮')
        runner.launch()
        if args.scenario == 'natural': runner.natural()
        elif args.scenario == 'diagnose': runner.diagnose()
        else: runner.stress()
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
            evidence = audit_run(runner)
        except BaseException:
            code = 1; runner.emit('AUDIT_FAILED', error=traceback.format_exc())
        result = dict(code=code, scenario=args.scenario, exits=runner.exits, protected_unchanged=unchanged,
                      protected_files=len(before), png_count=audit.get('files', 0), evidence=evidence,
                      owned_remaining=[p.pid for p in runner.owned.values() if p.poll() is None],
                      visual_pass=None, scope='视觉须实际打开PNG；DS被清理结束不等于自然退出0')
        (runner.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        runner.emit('RUNNER_EXIT', **result)
        runner.trace.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
