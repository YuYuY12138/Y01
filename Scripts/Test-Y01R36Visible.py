"""R36 S1隔离运行器；controlled不是自然购买验收。"""
from pathlib import Path
import json
import re
import uuid
import traceback
import subprocess
import time
import os
import hashlib
import math
import importlib.util
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
ENGINE = Path(r'G:\UE_Engine\g01_engine\Engine\Binaries\Win64')


def map_path(run):
    if not isinstance(run, str) or (run and not re.fullmatch(r'[A-Za-z0-9_]+', run)):
        raise ValueError('地图只接受R36隔离Run名称')
    root = '/Game/Developers/Y01PlaygroundTests/R36'
    return root + (f'/Runs/{run}' if run else '') + '/Maps/L_Y01R36'


def commands(label, port, run, count, profile='content', scenario='controlled'):
    if not isinstance(label, str) or not re.fullmatch(r'R36-(?:S1-UI|S2-Visible|S2-Runtime|S3-Visible|S3-Presentation|S4)-[A-Za-z0-9_-]+', label):
        raise ValueError('需要唯一R36隔离运行标签')
    if scenario not in ('controlled', 'natural'): raise ValueError('场景必须明确')
    natural = scenario == 'natural'
    s3 = label.startswith('R36-S3-')
    s4 = label.startswith('R36-S4-')
    if s4 and (not isinstance(run, str) or not re.fullmatch(r'S4_[A-Za-z0-9_]+', run)):
        raise ValueError('S4必须使用独立S4_Run，不能复用历史Run')
    natural_profile = profile in ('content', 'evidence', 'compare') if s4 else profile in ('evidence', 'compare') if s3 else profile == 'evidence'
    if natural and (not (label.startswith('R36-S2-') or s3 or s4) or not run or not natural_profile or type(count) is not int or count != 0):
        raise ValueError('自然购买需S2新Run、Baseline及显式空包，不能混用预置')
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('端口必须为1024至65535整数')
    if s3 and not run: raise ValueError('S3必须使用新隔离Run')
    if not natural and (type(count) is not int or not (0 if s3 or s4 else 1) <= count <= 2147483647):
        raise ValueError('controlled数量必须为int32正整数；不截断')
    if profile not in ('content', 'evidence', 'compare'):
        raise ValueError('未知R36Profile')
    project = str(ROOT / 'Y01.uproject')
    result = {'Server': [str(ENGINE / 'UnrealEditor-Cmd.exe'), project, map_path(run),
                         '-server', '-nullrhi', f'-port={port}', f'-R36Profile={profile}']}
    result['Server'].extend(['-R36Scenario=natural'] if natural else ['-R36Fixture=controlled', f'-R36Items={count}'])
    for role in ('A', 'B'):
        result[role] = [str(ENGINE / 'UnrealEditor.exe'), project, f'127.0.0.1:{port}',
                        '-game', '-d3d12', '-windowed', '-ResX=1280', '-ResY=800',
                        '-WinX=20', '-WinY=40', '-NoSplash']
        if s4: result[role].append(f'-R36Profile={profile}')
    for role, args in result.items():
        if s3 or s4: args.append('-R36Run=' + label)
        args.extend(['-unattended', '-nosound', '-R36Trace', '-forcelogflush', '-ForceRes',
                     f'-abslog={ROOT / "Saved" / (label + "-" + role + ".log")}'])
    return result


def load_content_check(label, run):
    if not isinstance(label, str) or not re.fullmatch(r'R36-S4-[A-Za-z0-9_-]+', label):
        raise ValueError('内容验收清单仅接受固定Saved下的安全标签')
    path = ROOT / 'Saved' / (label + '.json')
    if path.resolve().parent != (ROOT / 'Saved').resolve():
        raise ValueError('内容验收清单不能跳出Saved目录')
    plan = json.loads(path.read_text(encoding='utf-8'))
    names = ('equipment_offer', 'book_offer', 'direct_offer', 'random_offer', 'equipment', 'skill')
    if not isinstance(plan, dict) or plan.get('run') != run:
        raise ValueError('内容验收清单必须与实际Run一致')
    if any(not isinstance(plan.get(k), str) or not plan[k] or plan[k] == 'None' for k in names):
        raise ValueError('内容验收清单缺少商品或目标引用')
    candidates = plan.get('random_candidates')
    if (not isinstance(candidates, list) or len(candidates) < 2
            or any(not isinstance(v, str) or not v or v == 'None' for v in candidates)
            or len(set(candidates)) != len(candidates) or plan['equipment'] not in candidates):
        raise ValueError('随机商品必须至少两个不同候选并包含目标装备')
    attack = plan.get('equipment_attack')
    if type(attack) not in (int, float) or not math.isfinite(attack):
        raise ValueError('装备效果必须为有限数值')
    if type(plan.get('quality')) is not int or not 0 <= plan['quality'] <= 4:
        raise ValueError('验收品质必须为原生有效品质')
    return plan


def parse_trace(text):
    records = []
    for line in text.splitlines(keepends=True):
        if not line.endswith(('\n', '\r')) or 'Y01R36_TRACE ' not in line:
            continue
        record = json.loads(line.split('Y01R36_TRACE ', 1)[1])
        if not isinstance(record, dict):
            raise ValueError('Trace记录必须为对象')
        records.append(record)
    return records


def confirmation_open(ui):
    """兼容旧布尔记录，以及实际序列化后仅保留的确认弹窗几何。"""
    if type(ui.get('confirmation')) is bool:
        return ui['confirmation']
    geometry = ui.get('Confirmation', {})
    return geometry.get('visible') is True and geometry.get('laidOut') is True


def clock_alignment(rows):
    """用本机计数器夹取求实测偏移；缺失、冲突或截断时拒绝跨进程计时。"""
    assert len(rows) >= 2, '时钟校准至少两个样本'
    assert len({r['pid'] for r in rows}) == 1, '混合进程时钟'
    lower, upper = -math.inf, math.inf
    for r in rows:
        begin, end, scale = (r[k] for k in ('time', 'clockEnd', 'secondsPerCycle'))
        assert all(type(v) in (int, float) and math.isfinite(v) for v in (begin, end, scale))
        assert begin <= end and scale > 0 and 0 < r['event'] < 16384, '时钟倒序或日志达到上限'
        counter = int(r['qpc']); assert counter >= 0
        qpc = counter * scale
        lower, upper = max(lower, begin - qpc - 1.e-8), min(upper, end - qpc + 1.e-8)
    assert lower <= upper, '夹取偏移不相交，禁止跨进程直接相减'
    return dict(offset=(lower + upper) / 2, uncertainty=(upper - lower) / 2,
                lower=lower, upper=upper, samples=len(rows), pid=rows[0]['pid'])


def python_clock_samples():
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    frequency, counter = ctypes.c_longlong(), ctypes.c_longlong()
    assert kernel.QueryPerformanceFrequency(ctypes.byref(frequency)) and frequency.value > 0
    rows = []
    for i in range(8):
        begin = time.perf_counter()
        assert kernel.QueryPerformanceCounter(ctypes.byref(counter))
        end = time.perf_counter()
        rows.append(dict(time=begin, clockEnd=end, qpc=str(counter.value), secondsPerCycle=1 / frequency.value,
                         event=i + 1, pid=os.getpid()))
    return rows


def timing_ui_complete(ui, view):
    try:
        if any(ui[k] != view[k] for k in ('connection', 'revision', 'response')): return False
        if ui['pending'] is not False or ui['total'] != view['bag']: return False
        cells = ui['cells']; own = {i['item'] for i in view['itemDetails'] if i['location'] == 0}
        if len(own) != view['bag']: return False
        if any(c.get('item') not in own for c in cells if c.get('zone') == 0): return False
        for zone, count in ((1, 6), (2, 4)):
            slots = [c for c in cells if c.get('zone') == zone and c.get('laidOut')]
            if len(slots) != count or {c['slot'] for c in slots} != set(range(count)): return False
            if zone == 1 and any(c['item'] != view['equipment'][c['slot']] for c in slots): return False
        return bool(visible_bag_cells(ui)) if view['bag'] else bool(ui['bagGeometry']['laidOut'])
    except (KeyError, TypeError, ValueError):
        return False


def timing_authority_map(records):
    commits = {(v['connection'], v['revision']): v for v in records['Server'] if v.get('stage') == 'COMMIT'}
    authority = {}
    for role in ('Server', 'A', 'B'):
        for view in records[role]:
            if view.get('kind') != 'VIEW': continue
            key = (view['connection'], view['revision']); event = commits.get(key)
            assert event and all(view[k] == event[k] for k in ('match', 'join', 'connection', 'revision', 'digest', 'items')), '完整快照无匹配提交事件摘要'
            authority[key] = view
    return authority


def timing_authority_view(view, first, authority):
    assert all(view[k] == first[k] for k in ('connection', 'match', 'join')), '客户端会话或Owner错配'
    expected = authority.get((view['connection'], view['revision']))
    if view['revision'] < first['revision'] and not expected:
        assert view['revision'] == 1 and all(view[k] == 0 for k in ('items', 'bag', 'seq', 'response'))
        assert not view['itemDetails'] and all(not set(v) - {'0', '-'} for v in view['equipment'])
        assert all(not set(v['id']) - {'0', '-'} for v in view['skills'])
        return 'bootstrap_not_sampled'
    assert expected and view['digest'] == expected['digest'], '缺少同版本完整权威摘要或出现半快照'
    return 'full_digest'


def timing_segments(records, role, view, clocks, seq=None):
    scope = ('match', 'join', 'connection')
    def events(who, stage, revision=True, **filters):
        return [r for r in records[who] if r.get('kind') == 'TIMING' and r.get('stage') == stage
                and all(r.get(k) == view[k] for k in scope)
                and (not revision or r.get('revision') == view['revision'])
                and all(r.get(k) == v for k, v in filters.items())]
    def one(rows, description):
        assert len(rows) == 1, '计时边界缺失或重复：' + description
        return rows[0]
    done = one(events('Server', 'REQUEST_DONE', revision=False, seq=seq), 'REQUEST_DONE') if seq is not None else None
    commit_revision = done['revision'] if done else view['revision']
    commit = one([r for r in events('Server', 'COMMIT', revision=False) if r['revision'] == commit_revision], 'COMMIT')
    encode = one(events('Server', 'ENCODE'), 'ENCODE')
    transfer = encode['transfer']
    sends = events('Server', 'SEND_WINDOW', transfer=transfer)
    receives = events(role, 'RECEIVE_WINDOW', transfer=transfer)
    assembled = one(events(role, 'ASSEMBLED', transfer=transfer), 'ASSEMBLED')
    published = one(events(role, 'PUBLISHED', transfer=transfer), 'PUBLISHED')
    present = one(events(role, 'UI_PRESENT'), 'UI_PRESENT')
    assert sends and receives and {r['window'] for r in sends} == {r['window'] for r in receives}, '窗口覆盖不完整'
    assert all(r['bytes'] == encode['bytes'] for r in sends + receives + [assembled, published]), '编码字节不一致'
    layouts = [r for r in records[role] if r.get('kind') == 'UI' and r.get('time', 0) >= present['time']
               and timing_ui_complete(r, view)]
    assert layouts, '缺少同版本完整UI布局观测'
    layout = min(layouts, key=lambda r: r['time'])
    seconds = {}
    def interval(name, left, right, lr='Server', rr='Server', field='time'):
        value = (right['time'] - clocks[rr]['offset']) - (left[field] - clocks[lr]['offset'])
        assert value >= -(clocks[lr]['uncertainty'] + clocks[rr]['uncertainty'] + 1.e-7), name + '时钟倒序'
        seconds[name] = value
    interval('server_view_build', commit, commit, field='start')
    interval('commit_to_encode_complete', commit, encode)
    interval('encode', encode, encode, field='start')
    interval('encode_to_first_window', encode, sends[0])
    interval('send_to_assembled_scheduled', sends[0], assembled, rr=role)
    interval('receive_first_to_assembled', receives[0], assembled, lr=role, rr=role)
    interval('decode_validate_publish', published, published, lr=role, rr=role, field='start')
    interval('publish_to_ui_present', published, present, lr=role, rr=role)
    interval('ui_model_refresh', present, present, lr=role, rr=role, field='start')
    interval('ui_present_to_layout_observed', present, layout, lr=role, rr=role)
    interval('commit_to_layout_observed', commit, layout, rr=role)
    acks = events('Server', 'ACK_WINDOW', transfer=transfer)
    rtts = [a['time'] - s['time'] for s in sends for a in acks if a['window'] == s['window']]
    boundary = dict(commit=commit, encode=encode, first_send=sends[0], assembled=assembled,
                    published=published, ui_present=present, layout_time=layout['time'])
    if seq is not None:
        sent = one(events(role, 'REQUEST_SENT', revision=False, seq=seq), 'REQUEST_SENT')
        enter = one(events('Server', 'REQUEST_ENTER', revision=False, seq=seq), 'REQUEST_ENTER')
        assert done['revision'] <= view['revision'] and commit['seq'] == published['seq'] == seq, '请求版本错配'
        interval('request_to_server_enter', sent, enter, lr=role)
        interval('server_enter_to_commit', enter, commit)
        interval('server_request_through_done', enter, done)
        interval('request_to_publish', sent, published, lr=role, rr=role)
        interval('request_to_layout_observed', sent, layout, lr=role, rr=role)
        boundary.update(request_sent=sent, request_enter=enter, request_done=done)
    return dict(role=role, connection=view['connection'], revision=view['revision'], transfer=transfer, seq=seq,
                encoded_bytes=encode['bytes'], windows=len(sends), ack_roundtrips_seconds=rtts,
                cross_clock_uncertainty=clocks['Server']['uncertainty'] + clocks[role]['uncertainty'],
                seconds=seconds, boundaries=boundary)


def drag_points(source, target):
    for point in (source, target):
        if len(point) != 2 or any(type(v) is not int for v in point) or not (0 <= point[0] < 1280 and 0 <= point[1] < 800):
            raise ValueError('拖动必须位于自有1280×800客户区内')
    return [tuple(round(a + (b - a) * step / 15) for a, b in zip(source, target)) for step in range(16)]


def validate_views(server, a, b, count):
    keys = ('match', 'join', 'connection', 'revision', 'digest', 'items')
    if len(server) != 2 or len(a) != 1 or len(b) != 1:
        raise AssertionError('必须两份权威视图且每客户端仅一份私有视图')
    def identity(view):
        if any(key not in view for key in keys):
            raise AssertionError('完整视图Trace字段缺失')
        if not view['match'] or not view['connection'] or not view['digest'] or view['items'] != count:
            raise AssertionError('会话、摘要或物品数量无效')
        return tuple(view[key] for key in keys)
    authority = [identity(view) for view in server]
    clients = [identity(a[0]), identity(b[0])]
    if clients[0][:3] == clients[1][:3] or len({v[1] for v in clients}) != 2 or len({v[2] for v in clients}) != 2:
        raise AssertionError('两客户端Owner身份未隔离')
    if clients[0][0] != clients[1][0] or set(authority) != set(clients):
        raise AssertionError('客户端与服务器完整提交不一致')
    return True


def validate_delivery_transition(before, after):
    """只读验证单件权威ETA前后提交；不能替代真实双端或PNG验收。"""
    try:
        scope = ('match', 'join', 'connection')
        if any(not before[k] or before[k] != after[k] for k in scope): return False
        if any(v['deliveryEnabled'] is not True or v['deliveryEnded'] is not False
               or v['deliveryFault'] != 0 for v in (before, after)): return False
        if not (after['revision'] > before['revision'] and before['items'] == 0 and after['items'] == 1): return False
        if len(before['deliveries']) != 1 or len(after['deliveries']) != 1: return False
        pending, delivered = before['deliveries'][0], after['deliveries'][0]
        frozen = ('id', 'kill', 'item', 'match', 'join', 'definition', 'type', 'quality', 'origin', 'start', 'eta')
        if any(pending[k] != delivered[k] for k in frozen): return False
        if any(not pending[k] for k in ('id', 'kill', 'item', 'definition')): return False
        if pending['match'] != before['match'] or pending['join'] != before['join']: return False
        if pending['state'] != 0 or delivered['state'] != 1: return False
        if any(r['fault'] != 0 or r['cancel'] != 0 for r in (pending, delivered)): return False
        times = (pending['start'], pending['eta'], pending['deliveredTime'], delivered['deliveredTime'],
                 before['sampleTime'], after['sampleTime'], pending['delay'], delivered['delay'])
        if any(type(t) not in (int, float) or not math.isfinite(t) or t < 0 for t in times): return False
        if not (pending['start'] <= before['sampleTime'] < pending['eta'] <= delivered['deliveredTime'] <= after['sampleTime']): return False
        if pending['deliveredTime'] != 0 or not math.isclose(delivered['delay'], delivered['deliveredTime'] - pending['eta'], abs_tol=1.e-9): return False
        return (before['arrivals'] == [] and after['arrivals'] == [dict(item=pending['item'], delivery=pending['id'])])
    except (KeyError, TypeError, ValueError):
        return False


def s3_delivery_evidence(records):
    clients = latest_views(records['A']); others = latest_views(records['B'])
    if len(clients) != 1 or len(others) != 1: return None
    owner, other = clients[0]['connection'], others[0]['connection']
    assert owner != other, '双端Owner必须不同'
    for role in ('Server', 'B'):
        for row in records[role]:
            if row.get('kind') != 'VIEW': continue
            if role == 'B': assert row['connection'] == other, 'B收到其他Owner视图'
            if row['connection'] == other:
                assert row['items'] == row['bag'] == row['strength'] == 0 and not row['deliveries'], 'B库存或成长受污染'
    rows = [r for r in records['Server'] if r.get('kind') == 'VIEW' and r['connection'] == owner]
    before = next((r for r in rows if r['items'] == 0 and len(r['deliveries']) == 1 and r['deliveries'][0]['state'] == 0), None)
    if not before: return None
    after = next((r for r in rows if validate_delivery_transition(before, r)), None)
    if not after: return None
    item = after['deliveries'][0]['item']; delivery = after['deliveries'][0]['id']
    assert all(r['items'] == 1 and r['arrivals'] == after['arrivals'] for r in rows if r['revision'] >= after['revision']), '同Record重复发物'
    client = clients[0]
    if client['items'] != 1 or client['arrivals'] != after['arrivals']: return None
    ui = [r for r in records['A'] if r.get('kind') == 'UI']
    return dict(before=before, after=after, item=item, delivery=delivery,
                flight_observed=any(g.get('laidOut') for r in ui for g in r.get('flightGlyphs', [])),
                flash_observed=any(c.get('item') == item and c.get('zone') == 0 and c.get('laidOut')
                    and 0 < c.get('flashRemaining', 0) <= .6 for r in ui for c in r.get('cells', [])))


def same_process(expected, actual):
    return (type(expected.get('pid')) is int and expected['pid'] > 0
            and expected.get('created') is not None
            and expected['pid'] == actual.get('pid')
            and expected['created'] == actual.get('created'))


def require_idle(processes, owned):
    for process in processes:
        if process.get('ProcessId') in owned:
            continue
        name, command = process.get('Name', '').lower(), (process.get('CommandLine') or '').lower()
        if (any(part in name for part in ('unrealeditor', 'unrealbuildtool', 'y01server', 'msbuild'))
                or 'unrealbuildtool' in command
                or ('msbuild.dll' in command and '/nodemode:1' not in command)):
            raise RuntimeError('共享UE或构建占用；不关闭他人进程：' + str(process))
    return True


def close_owned(process, identity, created, request=None):
    if not same_process(identity, {'pid': process.pid, 'created': created}):
        raise RuntimeError('退出前PID或创建时间不符，拒绝发送任何信号')
    code = process.poll()
    if code is not None:
        return code
    if request is None: raise RuntimeError('缺少正常退出适配，不发送控制台终止信号')
    request()
    return process.wait(timeout=45)


def launch_all(plan, owned, scan, start, created, ready, emit):
    for role, command in plan.items():
        require_idle(scan(), {p.pid for p in owned.values()})
        owned[role] = start(command)
        emit(role, {'pid': owned[role].pid, 'created': created(owned[role])})
        ready(role)


def perform_drag(source, target, move, down, up, guard, pause, observe=None):
    points = drag_points(source, target)
    guard(); move(points[0]); guard(); down(); pause()
    for step, point in enumerate(points[1:], 1):
        guard(); move(point); pause()
        if observe is not None and step == len(points) // 2:
            guard(); observe(); guard()
    guard(); up()


def run_session(runner):
    code = 1
    try:
        runner.launch()
        runner.verify()
        code = 0
    except BaseException:
        runner.emit('RUN_FAILED', error=traceback.format_exc())
    finally:
        try: runner.close()
        except BaseException:
            code = 1
            runner.emit('CLOSE_FAILED', error=traceback.format_exc())
        runner.finish(code)
    return code


def latest_views(rows):
    result = {}
    for row in rows:
        if row.get('kind') == 'VIEW': result[row['connection']] = row
    return list(result.values())


def wire_progress(rows):
    groups = {}
    for row in rows:
        if row.get('kind') == 'WIRE': groups.setdefault(row['transfer'], []).append(row)
    return {key: dict(connection=values[-1]['connection'], observed_windows=len({v['window'] for v in values}),
                      last_window=max(v['window'] for v in values), end=max(v['end'] for v in values),
                      bytes=values[-1]['bytes'], seconds=values[-1]['time'] - values[0]['time'])
            for key, values in groups.items()}


def scan_processes():
    command = '@(Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,Name,CommandLine) | ConvertTo-Json -Compress'
    result = subprocess.run([r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe', '-NoProfile', '-Command', command],
                            capture_output=True, check=True)
    text = result.stdout
    if isinstance(text, bytes):
        try:
            text = text.decode('utf-8-sig')
        except UnicodeDecodeError:
            text = text.decode('oem')
    try:
        rows = json.loads(text)
    except json.JSONDecodeError as error:
        fragment = ascii(text[max(0, error.pos - 160):error.pos + 160])
        raise RuntimeError(f'进程清单JSON损坏，位置{error.pos}，原片段{fragment}') from error
    return rows if isinstance(rows, list) else [rows]


def start_process(command):
    return subprocess.Popen(command, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def validate_packages(before, after):
    assert len(before) == 70 and before == after, '固定三Profile及地图70包新增/缺失/哈希改变'
    return True


def economy_delta(before, after, cost, allow_growth=False):
    assert before['strength'] == after['strength'], '购买期间不能隐式注入成长'
    if not allow_growth: assert before['strength'] == 0, 'S2不得触发力量成长'
    natural = after['gold'] - before['gold'] + cost
    assert natural >= 0, '支出之外出现扣款或收入被回退'
    return natural


def validate_income_interval(before, after, stopped=False):
    assert before['connection'] == after['connection'] and before['fault'] == after['fault'] == 0
    assert before['totalSpending'] == after['totalSpending'], '观察收入期间不得有隐式支出'
    income = int(after['totalIncome']) - int(before['totalIncome'])
    assert income >= 0 and after['balance'] - before['balance'] == income, '钱包与收入账本不同'
    assert before['phase'] == after['phase'] == (3 if stopped else 2)
    assert before['rate'] == after['rate'] and after['lastTime'] >= before['lastTime']
    if stopped:
        assert all(before[k] == after[k] for k in ('totalIncome', 'balance', 'lastTime', 'remainder')), '结束后收入仍推进'
    else:
        earned = before['remainder'] + (after['lastTime'] - before['lastTime']) * before['rate']
        assert abs(earned - income - after['remainder']) < 1e-6, '收入、小数或时间段不守恒'
    return dict(income=income, seconds=after['lastTime'] - before['lastTime'], remainder=after['remainder'])


def validate_wave_wallet(view, ledger, initial, reward, experience=0):
    assert view['connection'] == ledger['connection'] and view['revision'] == ledger['revision'], '波次账本Owner或版本错配'
    assert ledger['fault'] == 0 and ledger['balance'] == view['gold'] and ledger['rate'] == view['incomeRate']
    actual = view['gold'] - initial - int(ledger['totalIncome']) + int(ledger['totalSpending'])
    assert actual == reward and view['experience'] == experience, '必须逐人全额奖励，不能均分或隐式注入'
    return actual


def validate_wave_samples(rows, configs):
    assert rows and configs, '缺少真实波次证据'
    for index, config in enumerate(configs):
        samples = [r for r in rows if r['index'] == index]
        battle = [r for r in samples if r['phase'] == 2]
        assert battle, '缺少真实Battle'
        start = battle[0]['phaseTime']; seen = {}; total = sum(s['count'] for s in config['spawns'])
        for row in samples:
            assert row['time'] >= row['phaseTime'] and row['pending'] >= 0
            if row['pending']:
                assert not row['cleared'] and row['completed'] == index, '待生成时提前清波'
            if row['phase'] not in (2, 3): continue
            assert len(row['monsters']) + row['pending'] == total, '实际生成数不等于表配置'
            for monster in row['monsters']:
                old = seen.setdefault(monster['actor'], (monster['definition'], row['time']))
                assert old[0] == monster['definition'], '怪物身份复用'
            if row['cleared']:
                assert row['pending'] == 0 and row['completed'] == index + 1
                assert all(m['health'] == 0 for m in row['monsters']), '存活怪物不能清波'
        expected = {}
        for spawn in config['spawns']:
            expected.setdefault(spawn['definition'], []).extend([spawn['delay']] * spawn['count'])
        assert set(expected) == {value[0] for value in seen.values()}, '怪物定义不等于表配置'
        for definition, delays in expected.items():
            times = sorted(t for name, t in seen.values() if name == definition)
            assert len(times) == len(delays), '自然怪物数量错误'
            for stamp, delay in zip(times, sorted(delays)):
                assert stamp + 1e-4 >= start + delay, '延迟怪物提前生成'
        if any(s['delay'] > 0 for s in config['spawns']):
            assert any(r['pending'] > 0 for r in battle), '缺少真实延迟待生成窗口'
        assert any(r['cleared'] for r in samples), '没有正常清波'
    assert rows[-1]['phase'] == 3 and rows[-1]['result'] == 1 and rows[-1]['completed'] == len(configs), '末波未正常成功结束'
    return True


def hud_record(ui, viewport=(1280, 800)):
    """按PanelRecord原字段归一化；legacy显隐只代表背包，不推断人物或商店。"""
    names = ('characterExpanded', 'backpackExpanded', 'shopExpanded', 'panelsExpanded',
             'canToggleCharacter', 'canToggleBackpack', 'canToggleShop', 'canDragBackpack')
    assert all(type(ui.get(k)) is bool for k in names), '缺少独立面板状态或操作门禁'
    assert ui['panelsExpanded'] == ui['backpackExpanded'], 'legacy显隐只能代表背包'
    assert ui.get('panelsExpandedMeaning') == 'backpack-only-compatibility', 'legacy字段含义不明确'
    assert 'TogglePanels' not in ui and 'Advance' not in ui, '日常HUD不能包含总展开或Start'
    def bounds(g):
        rect = g.get('rect', [])
        assert g.get('visible') and g.get('laidOut') and len(rect) == 4, '控件未实际布局'
        assert all(type(v) in (int, float) and math.isfinite(v) for v in rect), '像素矩形必须有限'
        assert 0 <= rect[0] < rect[2] <= viewport[0] + .01 and 0 <= rect[1] < rect[3] <= viewport[1] + .01, '控件不在完整客户区'
        return rect
    geometry = {}
    for zone, name, count in ((1, 'equipment', 6), (2, 'skills', 4)):
        cells = [c for c in ui.get('cells', []) if c.get('zone') == zone]
        assert len(cells) == count and {c['slot'] for c in cells} == set(range(count)), '常驻槽数不完整'
        for cell in cells:
            a = bounds(cell)
            for panel in ('CharacterPanel', 'BackpackPanel', 'ShopPanel', 'Confirmation'):
                g = ui.get(panel, {})
                if g.get('visible') and g.get('laidOut'):
                    b = g['rect']
                    assert min(a[2], b[2]) <= max(a[0], b[0]) or min(a[3], b[3]) <= max(a[1], b[1]), '常驻槽被面板覆盖'
        geometry[name] = cells
    controls = {k: v for k, v in ui.items() if isinstance(v, dict) and 'rect' in v}
    for name in ('TopVitals', 'WaveCountdown', 'ToggleCharacter', 'ToggleBackpack', 'ToggleShop'):
        bounds(controls.get(name, {}))
    return dict(panel={k: ui[k] for k in names + ('panelsExpandedMeaning',)}, geometry=geometry,
                controls=controls, time=ui.get('time'), revision=ui.get('revision'), viewport=viewport)


def validate_automatic_wave_samples(rows, configs, require_overlap=False):
    """自然逐波只读验证；不要求死前通关，不把全局index当作旧波已清。"""
    assert rows and configs, '缺少自动波次证据'
    starts, deadlines, previous, overlap = {}, {}, {}, None
    for row in rows:
        assert row.get('automaticWaves') is True and row['totalWaves'] == len(configs)
        states = row['waveStates']; now = row['time']
        assert len(states) == len(configs) and {s['key'] for s in states} == set(range(len(configs)))
        assert row['totalAliveMonsters'] == sum(s['alive'] for s in states)
        assert row['totalPendingSpawns'] == sum(s['pending'] for s in states)
        assert row['completed'] == sum(s['rewarded'] for s in states)
        for s in states:
            key = int(s['key']); config = configs[key]; total = sum(e['count'] for e in config['spawns'])
            assert s['id'] == config['id'] and s['total'] == total
            assert 0 <= s['alive'] <= s['spawned'] <= total and s['pending'] >= 0
            if not s['started']:
                assert s['alive'] == s['spawned'] == s['pending'] == 0 and not s['rewarded']
                continue
            start = s['startTime']
            assert 0 <= start <= now and starts.setdefault(key, start) == start, '逐波开始时间改变'
            if key in deadlines: assert start + 1.e-4 >= deadlines[key], '下一波提前开始'
            if key > 0:
                assert key - 1 in starts and start + 1.e-4 >= starts[key - 1] + config['prepare'], '准备间隔不符'
            due = sum(e['count'] for e in config['spawns'] if now + 1.e-4 >= start + e['delay'])
            assert s['spawned'] <= due, '延迟怪提前生成'
            if row['phase'] != 3:
                assert s['pending'] + s['spawned'] == total, '旧波延迟批次丢失'
            if s['rewarded']: assert s['pending'] == s['alive'] == 0, '有存活或待生成时提前奖励'
            if key in previous:
                old = previous[key]
                assert s['spawned'] >= old['spawned'] and (not old['rewarded'] or s['rewarded'])
            previous[key] = s
        if row['nextWaveIndex'] >= 0:
            key = int(row['nextWaveIndex']); deadline = row['nextWaveDeadline']
            assert deadlines.setdefault(key, deadline) == deadline, '未出波截止时间漂移'
        if sum(s['alive'] > 0 for s in states) >= 2 and overlap is None: overlap = row
    if require_overlap: assert overlap, '没有至少两波同时存活的真实样本'
    return dict(started_waves=len(starts), starts=starts, overlap=overlap, samples=len(rows))


def visible_control(ui, name, container):
    control, area = ui.get(name, {}), ui.get(container, {})
    if not control.get('laidOut') or not area.get('laidOut'): return False
    rect, bounds = control.get('rect', []), area.get('rect', [])
    if len(rect) != 4 or len(bounds) != 4: return False
    if any(type(v) not in (int, float) or not math.isfinite(v) for v in rect + bounds): return False
    return (max(0, bounds[0]) <= rect[0] < rect[2] <= min(1280, bounds[2])
            and max(0, bounds[1]) <= rect[1] < rect[3] <= min(800, bounds[3]))


def ui_point(geometry):
    if not geometry.get('laidOut'):
        raise ValueError('控件尚未实际布局，不能猜测屏幕位置')
    values = [geometry.get(k) for k in ('x', 'y')]
    if any(type(v) not in (int, float) for v in values): raise ValueError('控件像素坐标缺失')
    point = tuple(round(v) for v in values)
    drag_points(point, point)
    return point


def agreed_view(records, role, seq=None):
    clients = latest_views(records[role])
    if len(clients) != 1: return None
    view = clients[0]
    if seq is not None and view.get('seq') != seq: return None
    keys = ('connection', 'revision', 'digest')
    authority = next((server for server in latest_views(records['Server'])
                      if server.get('connection') == view.get('connection')), None)
    if not authority or not all(view.get(k) is not None for k in keys): return None
    if all(authority.get(k) == view[k] for k in keys): return view
    # 收入持续发布时，三端0.5秒采样不必同时停在最新版本；仍须精确匹配已记录的完整权威摘要。
    # 只允许随后发生自然金币与采样时间推进，任何身份、回执、存活、槽位或来源变化都继续等待。
    transient = {'revision', 'digest', 'gold', 'sampleTime'}
    if (set(authority) != set(view) or any(authority[k] != view[k] for k in view.keys() - transient)
            or authority['revision'] <= view['revision'] or authority.get('gold', -1) < view.get('gold', 0)
            or authority.get('sampleTime', -1) < view.get('sampleTime', 0)):
        return None
    if any(server.get('kind') == 'VIEW' and all(server.get(k) == view[k] for k in keys)
           for server in records['Server']):
        return view
    return None


def settled_ui(records, role, seq=None):
    view = agreed_view(records, role, seq)
    rows = [r for r in records[role] if r.get('kind') == 'UI']
    if not view or not rows: return None
    ui = rows[-1]
    if ui.get('pending') is not False or ui.get('revision') != view['revision']: return None
    return ui, view


def combat_scope(records, role):
    """只读服务器相邻VIEW/SCOPE配对，不能把另一Owner的合法目标当成本人的。"""
    view = agreed_view(records, role)
    if not view or view.get('health', 0) <= 0: return None
    owner = None; scope = None
    for row in records['Server']:
        if row.get('kind') == 'VIEW': owner = row.get('connection')
        elif row.get('kind') == 'SCOPE' and owner == view['connection']: scope = row
    if (not scope or scope.get('authority') is not True or scope.get('ownerLinked') is not True
            or scope.get('phase') != 2 or scope.get('inside') is not False): return None
    for field in ('pawnX', 'pawnY', 'time', 'targetDistance'):
        value = scope.get(field)
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError('战斗位置观测缺少有限数值：' + field)
    if type(scope.get('targetValid')) is not bool: raise ValueError('缺少真实目标合法性')
    return scope


def training_scope(records, role):
    clients = latest_views(records.get(role, []))
    if len(clients) != 1: return None
    owner, result = None, None
    for row in records['Server']:
        if row.get('kind') == 'VIEW': owner = row.get('connection')
        elif row.get('kind') == 'SCOPE' and owner == clients[0]['connection']: result = row
    if not result or not result.get('authority') or not result.get('ownerLinked'): return None
    for key in ('trainingKills', 'trainingAlive'):
        assert type(result.get(key)) in (int, float) and result[key] >= 0 and result[key] == int(result[key]), '缺少真实训练计数'
    return result


def validate_s3_growth(view, item, duplicate, count, equipped):
    assert view['attack'] == (40 + count if equipped else 30) and view['strength'] == count
    assert (view['equipment'][0] == item) is equipped
    ledger = view['growth']; assert ledger['fault'] == 0 and ledger['strength'] == count
    details = {row['item']: row for row in view['itemDetails']}
    assert item != duplicate and details[item]['definition'] == details[duplicate]['definition'] == 'S3Blade'
    rules = [r for r in details[item]['growth'] if r['target'] == 0 and r['attribute'] == 0]
    assert len(rules) == 1 and rules[0]['accumulated'] == count and int(rules[0]['appliedCount']) == count
    assert all(r['accumulated'] == 0 and int(r['appliedCount']) == 0 for r in details[duplicate]['growth'])
    assert sum(r['accumulated'] for r in ledger['playerRules']) == count
    guid = lambda value: str(uuid.UUID(value)).upper() if re.fullmatch(r'[0-9A-Fa-f]{32}', value) else value
    source = f"R36/{guid(view['match'])}/P{int(view['join'])}/Equipment/{guid(item)}"
    assert all(r['source'] == source and r['accumulated'] == count and int(r['appliedCount']) == count for r in ledger['playerRules'])
    if count: assert len(ledger['playerRules']) == 1
    return True


def natural_skill_activation(records, role, slot, before, after_time):
    clients = latest_views(records[role])
    if len(clients) != 1: return None
    view = clients[0]
    ui = next((r for r in reversed(records[role]) if r.get('kind') == 'UI'), None)
    old, current = before['skills'][slot], view['skills'][slot]
    if (not ui or ui.get('pending') is not False or ui.get('revision') != view['revision']
            or ui.get('time', 0) <= after_time or view['connection'] != before['connection']
            or view['revision'] <= before['revision'] or any(current[k] != old[k] for k in ('id', 'skill'))):
        return None
    now, deadline = ui.get('serverTime'), current.get('deadline')
    if any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in (now, deadline)):
        raise ValueError('自然技能提交需要有限非负同步时间及CD')
    if deadline <= max(now, old['deadline']): return None
    authority = [r for r in records['Server'] if r.get('kind') == 'VIEW' and r['connection'] == view['connection']]
    if not any(r == view for r in authority): return None
    if any(r['skills'][slot] != current for r in authority if r['revision'] > view['revision']): return None
    return ui, view


def cooldown_activation(records, role, slot, before, after_time):
    state = settled_ui(records, role)
    if not state: return None
    ui, view = state
    if ui.get('time', 0) <= after_time: return None
    old, current = before['skills'][slot], view['skills'][slot]
    assert view['connection'] == before['connection'] and all(current[k] == old[k] for k in ('id', 'skill')), '激活期间Owner或已学技能身份改变'
    now = ui.get('serverTime')
    if type(now) not in (int, float) or not math.isfinite(now) or now < 0:
        raise ValueError('技能CD需要有限非负的服务器同步时间')
    deadline = current.get('deadline')
    if (view['revision'] <= before['revision'] or type(deadline) not in (int, float)
            or not math.isfinite(deadline) or deadline <= max(now, old['deadline'])):
        return None
    return state


def choose_cell(ui, zone, **criteria):
    cells = [c for c in ui.get('cells', []) if c.get('zone') == zone and c.get('laidOut')
             and all(c.get(k) == v for k, v in criteria.items())]
    assert cells, f'没有实际布局的目标格：zone={zone}, criteria={criteria}'
    cell = min(cells, key=lambda c: c.get('order', 0))
    ui_point(cell)
    return cell


def choose_book(ui, skill=None, exclude=()):
    cells = [c for c in ui.get('cells', []) if c.get('definition')
             and (skill is None or c['definition'] == skill) and c['definition'] not in exclude]
    return choose_cell(dict(cells=cells), 0, type=5)


def visible_bag_cells(ui):
    def rect(geometry):
        value = geometry.get('rect')
        if (not isinstance(value, (list, tuple)) or len(value) != 4
                or any(type(v) not in (int, float) or not math.isfinite(v) for v in value)
                or value[0] >= value[2] or value[1] >= value[3]):
            raise ValueError('缺少有限正面积像素矩形，不能以中心点冒充完整可见')
        return value
    bag = ui.get('bagGeometry', {})
    if not bag.get('laidOut'): raise ValueError('背包尚未实际布局')
    left, top, right, bottom = rect(bag)
    left, top, right, bottom = max(0, left), max(0, top), min(1280, right), min(800, bottom)
    visible = []
    for cell in ui.get('cells', []):
        if cell.get('zone') != 0 or not cell.get('laidOut'): continue
        x1, y1, x2, y2 = rect(cell)
        if left <= x1 and top <= y1 and x2 <= right and y2 <= bottom:
            visible.append(cell)
    return visible


class ScrollCoverage:
    """仅核验controlled初始包；记录逐实例完整可见与真实条目重绑。"""
    def __init__(self, count, revision):
        self.count, self.revision = count, revision
        self.items, self.orders, self.widgets = {}, {}, {}
        self.peak = 0

    def observe(self, ui):
        assert ui.get('revision') == self.revision and ui.get('total') == self.count and ui.get('pending') is False, '滚动途中完整视图变化'
        cells = visible_bag_cells(ui)
        self.peak = max(self.peak, sum(c.get('zone') == 0 for c in ui.get('cells', [])))
        for cell in cells:
            item, order, widget = cell.get('item'), cell.get('order'), cell.get('widget')
            assert isinstance(item, str) and item and set(item) - {'0', '-'} and widget, '条目缺少实例或控件身份'
            assert type(order) in (int, float) and order == int(order) and 1 <= order <= self.count, 'controlled初始取得顺序无效'
            assert self.items.get(item, order) == order and self.orders.get(order, item) == item, '实例与取得顺序不是一一对应'
            self.items[item] = order; self.orders[order] = item
            self.widgets.setdefault(widget, set()).add(item)
        return cells

    def finish(self):
        assert len(self.items) == self.count and len(self.orders) == self.count, '没有逐件证明全量可见，重复页或末页不能替代'
        reused = sum(len(items) > 1 for items in self.widgets.values())
        if self.count > self.peak:
            assert reused and len(self.widgets) < self.count, '未证明滚动条目复用'
        return dict(items=len(self.items), orders=len(self.orders), widgets=len(self.widgets),
                    reused_widgets=reused, peak_entries=self.peak)


def network_ready(records, count):
    views = {role: latest_views(rows) for role, rows in records.items()}
    for role in ('A', 'B'):
        scopes = [r for r in records[role] if r.get('kind') == 'SCOPE']
        assert len(views[role]) <= 1 and all(s.get('privateViews', 0) <= 1 for s in scopes), '非Owner收到私有视图'
        if (not scopes or scopes[-1].get('authority') or not scopes[-1].get('ownerLinked')
                or scopes[-1].get('states', 0) < 2 or scopes[-1].get('pawn') in (None, '', 'None')):
            return False
    if len(views['Server']) != 2 or not views['A'] or not views['B']: return False
    if any(v['items'] != count for rows in views.values() for v in rows): return False
    if {(v['connection'], v['revision'], v['digest']) for v in views['Server']} != {
            (v['connection'], v['revision'], v['digest']) for role in ('A', 'B') for v in views[role]}:
        return False
    return validate_views(views['Server'], views['A'], views['B'], count)


def burst_result(records, count):
    """controlled快速RPC专用观测；不替代真实鼠标验收。"""
    plans = [r for r in records['A'] if r.get('kind') == 'BURST_SENT']
    if not plans: return None
    assert len(plans) == 1, '快速意图只能发送一次'
    plan = plans[0]; base = plan['base']; requests = plan['requests']
    assert plan['connection'] == base['connection'] and base['items'] == base['bag'] == count, '快速场必须从自身完整初始包开始'
    empty = lambda value: isinstance(value, str) and bool(value) and not set(value) - {'0', '-'}
    assert len(base['equipment']) == 6 and all(empty(v) for v in base['equipment']), '快速场需要六个空装备槽'
    assert len(base['skills']) == 4 and all(empty(v['id']) for v in base['skills']), '快速场需要四个空技能槽'
    assert len(requests) == 8 and len({r['item'] for r in requests}) == 8, '需要八个不同实例意图'
    assert len({r['definition'] for r in requests[6:]}) == 2, '两本书必须不同SkillID'
    for i, request in enumerate(requests):
        assert request['seq'] == base['expectedSeq'] + i, '快速请求Seq必须连续'
        assert request['operation'] == (0 if i < 6 else 2) and request['slot'] == (i if i < 6 else i - 6), '只测六槽装配及两空槽学习'
        assert request['item'] and not empty(request['item']) and request['definition'], '意图身份缺失'
    owner = base['connection']
    for role in ('A', 'B'):
        scopes = [r for r in records[role] if r.get('kind') == 'SCOPE']
        assert all(s.get('privateViews', 0) <= 1 for s in scopes), '非Owner收到私有视图'
        if not scopes or not scopes[-1].get('ownerLinked') or scopes[-1].get('authority') or scopes[-1].get('states', 0) < 2:
            return None
    others = [r for r in records['B'] if r.get('kind') == 'VIEW' and r.get('items') == count]
    if not others: return None
    other = others[0]
    assert other['connection'] != owner and other['join'] != base['join'] and other['match'] == base['match'], 'Owner会话未隔离'
    for role in ('Server', 'B'):
        for row in records[role]:
            if row.get('kind') != 'VIEW': continue
            if role == 'B': assert row['connection'] == other['connection'], 'B收到其他Owner视图'
            if row['connection'] == other['connection'] and row['revision'] >= other['revision']:
                assert all(row.get(k) == other.get(k) for k in other), '未操作Owner的完整提交改变'
    commits = [r for r in records['Server'] if r.get('kind') == 'REQUEST_COMMIT' and r['view']['connection'] == owner]
    by_seq = {}
    for row in commits:
        seq = row['view']['seq']
        assert seq not in by_seq, '服务器请求回执重复'
        by_seq[seq] = row
    if any(r['seq'] not in by_seq for r in requests): return None
    known = {base['revision']: base}; previous = base; merged = False; frames = set()
    for i, request in enumerate(requests):
        row = by_seq[request['seq']]; view = row['view']; frames.add(row['frame'])
        assert all(view[k] == base[k] for k in ('match', 'join', 'connection')), '服务器回执Owner改变'
        assert view['code'] == 0 and view['requestItem'] == request['item'] and view['expectedSeq'] == request['seq'] + 1, '快速请求拒绝或回执错配'
        assert view['revision'] > previous['revision'] and view['response'] == previous['response'] + 1, '每次请求必须有新完整响应'
        assert view['bag'] == count - i - 1 and view['items'] == count - max(0, i - 5), '快速装配或耗书数量错误'
        equipment = list(previous['equipment']); skills = list(previous['skills'])
        if i < 6:
            equipment[i] = request['item']
            assert row['requestPresent'] is True, '装备不能从注册表消失'
        else:
            learned = view['skills'][i - 6]
            assert learned['skill'] == request['definition'] and not empty(learned['id']) and learned['deadline'] == 0, '学习身份或初始CD错误'
            assert row['requestPresent'] is False, '学习必须删除原书'
            skills[i - 6] = learned
        assert view['equipment'] == equipment and view['skills'] == skills, '完整提交包含错误槽位变化'
        in_flight = row['inFlightRevision']
        assert 0 < in_flight <= view['revision'], '缺少实际在途版本'
        merged |= in_flight < view['revision']
        known[view['revision']] = view; previous = view
    assert merged and len(frames) < len(requests), '没有在途期间的同帧快速合并证据'
    seen = set()
    for row in records['A']:
        if row.get('kind') not in ('VIEW', 'UI') or row.get('revision', 0) < base['revision']: continue
        expected = known.get(row['revision'])
        assert expected, '客户端发布了不属于权威完整提交的版本'
        if row['kind'] == 'VIEW':
            assert all(row.get(k) == expected[k] for k in expected), '客户端出现半交易或摘要不一致'
            seen.add(row['revision'])
        else:
            assert row.get('total') == expected['bag'], 'UI同版本混入其他交易数量'
    state = settled_ui(records, 'A', requests[-1]['seq'])
    if not state: return None
    ui, final = state
    assert final['revision'] == previous['revision'] and final['digest'] == previous['digest'], '最终完整提交不一致'
    skipped = sum(v['view']['revision'] not in seen for v in by_seq.values())
    assert skipped > 0, '没有实际跳过中间提交，不能称快速合并通过'
    return dict(requests=len(requests), deleted_books=2, skipped_commits=skipped,
                final_items=final['items'], final_bag=final['bag'], final_revision=final['revision'])


class Runner:
    def emit(self, kind, **data):
        row = dict(kind=kind, time_ns=time.time_ns(), perf=time.perf_counter(), **data)
        with (self.output / 'runner.jsonl').open('a', encoding='utf-8') as out:
            out.write(json.dumps(row, ensure_ascii=False) + '\n')
        print(kind, json.dumps(data, ensure_ascii=False), flush=True)

    def created(self, process):
        return self.helpers.process_created(process)

    def text(self, role):
        path = ROOT / 'Saved' / (self.args.label + '-' + role + '.log')
        return path.read_text(encoding='utf-8', errors='replace') if path.exists() else ''

    def records(self):
        if not hasattr(self, '_trace_logs'): self._trace_logs = {}
        result = {}
        for role in ('Server', 'A', 'B'):
            path = ROOT / 'Saved' / (self.args.label + '-' + role + '.log')
            previous = self._trace_logs.get(role)
            try:
                stat = path.stat()
            except FileNotFoundError:
                if previous is not None: raise RuntimeError(role + '日志消失，不能丢弃已观测证据')
                result[role] = []; continue
            identity = (stat.st_dev, stat.st_ino)
            old_identity, offset, pending, rows = previous or (identity, 0, b'', [])
            if identity != old_identity or stat.st_size < offset:
                raise RuntimeError(role + '日志被替换或截断，拒绝混合运行证据')
            if stat.st_size > offset:
                with path.open('rb') as source:
                    source.seek(offset); added = source.read(stat.st_size - offset)
                if len(added) != stat.st_size - offset:
                    raise RuntimeError(role + '日志读取期间缩短，不能跳过证据')
                data = pending + added; boundary = data.rfind(b'\n') + 1
                # 按字节保留未写完的UTF-8/CRLF；解析失败不推进游标。完整历史不去重、不采样。
                parsed = parse_trace(data[:boundary].decode('utf-8', errors='replace'))
                rows.extend(parsed); pending = data[boundary:]; offset = stat.st_size
            self._trace_logs[role] = (identity, offset, pending, rows)
            result[role] = rows
        return result

    def wait(self, predicate, description, seconds=None):
        until = time.monotonic() + (seconds or self.args.timeout)
        while time.monotonic() < until:
            for role, process in self.owned.items():
                if process.poll() is not None: raise RuntimeError(f'{role}提前退出：{process.returncode}')
            value = predicate()
            if value: return value
            time.sleep(.1)
        self.emit('TIMEOUT', description=description, progress={r: wire_progress(v) for r, v in self.records().items()})
        raise TimeoutError(description)

    def ready(self, role):
        if role == 'Assets': return
        if role == 'Server':
            self.wait(lambda: 'listening on port' in self.text(role), 'DS监听')
        else:
            self.wait(lambda: any(r.get('kind') == 'SCOPE' for r in parse_trace(self.text(role))), role + '真实Controller出生')

    def launch(self):
        def emit(role, identity):
            self.identities[role] = identity
            self.emit('OWNED_START', role=role, identity=identity, command=self.plan[role])
            if hasattr(self, 'output'):
                (self.output / 'processes.json').write_text(json.dumps(dict(runner_pid=os.getpid(), owned=self.identities), indent=2), encoding='utf-8')
        self.plan = commands(self.args.label, self.args.port, self.args.run, self.args.items, self.args.profile, getattr(self.args, 'scenario', 'controlled'))
        if getattr(self.args, 'timing', False):
            self.clock_samples = python_clock_samples()
            self.emit('PYTHON_CLOCK', samples=self.clock_samples)
            for command in self.plan.values(): command.append('-R36Timing')
        if getattr(self.args, 'burst', False): self.plan['A'].append('-R36Burst=controlled')
        if getattr(self.args, 'lifecycle', False): self.plan['Server'].append('-R36Lifecycle=controlled')
        self.stop_token = uuid.uuid4().hex
        if '-R36Run=' + self.args.label not in self.plan['Server']: self.plan['Server'].append('-R36Run=' + self.args.label)
        self.plan['Server'].append('-R36StopToken=' + self.stop_token)
        if self.args.validate_assets:
            self.before = self.package_hashes()
            self.plan = {'Assets': [str(ENGINE / 'UnrealEditor-Cmd.exe'), str(ROOT / 'Y01.uproject'),
                '-run=Y01R36Assets', '-Root=' + map_path(self.args.run).rsplit('/Maps/', 1)[0], '-ValidateOnly',
                '-unattended', '-nullrhi', '-nosound', '-abslog=' + str(ROOT / 'Saved' / (self.args.label + '-Assets.log'))]}
            (self.output / 'packages-before.json').write_text(json.dumps(self.before, indent=2), encoding='utf-8')
        def start(command):
            if getattr(self.args, 'timing', False):
                self.emit('PROCESS_LAUNCH_REQUEST', role=next(r for r, c in self.plan.items() if c is command))
            return start_process(command)
        launch_all(self.plan, self.owned, scan_processes, start, self.created, self.ready, emit)

    def package_hashes(self):
        base = ROOT / 'Content' / map_path(self.args.run)[6:].rsplit('/Maps/', 1)[0]
        return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for part in ('Baseline', 'Content', 'Compare', 'Maps') for p in (base / part).rglob('*') if p.is_file()}

    def verify(self):
        if self.args.validate_assets:
            code = self.owned['Assets'].wait(timeout=self.args.timeout)
            after = self.package_hashes()
            (self.output / 'packages-after.json').write_text(json.dumps(after, indent=2), encoding='utf-8')
            assert code == 0 and 'Result=1 Created=0 Preserved=70' in self.text('Assets'), '冷读未成功或发生创建'
            validate_packages(self.before, after)
            self.emit('COLD_PACKAGES_PASS', count=len(after), child_exit=code)
            return
        begin = time.monotonic()
        natural = getattr(self.args, 'scenario', 'controlled') == 'natural'
        self.wait(lambda: network_ready(self.records(), self.args.items), '两端完整视图与DS一致及Owner隔离')
        self.emit('NETWORK_INITIAL_PASS', seconds_since_wait=time.monotonic() - begin,
                  views={r: latest_views(v) for r, v in self.records().items()},
                  progress={r: wire_progress(v) for r, v in self.records().items()}, controlled=not natural, natural_purchase=natural)
        self.locate()
        if natural: self.snapshot('00-Natural-Default')
        for role in ('A', 'B'): self.expand_panels(role)
        self.snapshot('01-Initial')
        if getattr(self.args, 'scroll_all', False):
            for role in ('A', 'B'): self.scroll_bag(role)
        if getattr(self.args, 'timing', False):
            self.timing_actions()
            self.timing_report()
        elif getattr(self.args, 'presentation', None):
            self.presentation_actions()
        elif getattr(self.args, 'burst', False):
            self.burst_actions()
        elif getattr(self.args, 'lifecycle', False):
            self.lifecycle_actions()
        elif not self.args.observe_only and natural:
            if self.args.label.startswith('R36-S4-'):
                if self.args.profile == 'content': self.content_actions()
                else: self.source_actions()
            else: self.natural_actions()
        elif not self.args.observe_only:
            self.controlled_actions()
            if not getattr(self.args, 'incremental', False): self.skill_actions()

    def locate(self):
        self.windows = {}
        for role in ('A', 'B'):
            self.n.p = self.owned[role]
            self.windows[role] = self.wait(self.n.locate, role + '自有1280×800窗口')
            assert 'LogD3D12RHI:' in self.text(role), '没有真实D3D12 RHI日志'
        self.emit('DUAL_D3D12', windows={r: int(w) for r, w in self.windows.items()})

    def set_panel(self, role, panel, expanded):
        assert panel in ('character', 'backpack', 'shop') and type(expanded) is bool
        def latest():
            return next((r for r in reversed(self.records()[role]) if r.get('kind') == 'UI'), None)
        before = self.wait(latest, role + '独立面板初始观测')
        key = panel + 'Expanded'; title = panel.capitalize(); area = title + 'Panel'
        assert type(before.get(key)) is bool, '缺少独立面板显隐'
        if before[key] is expanded: return before
        button = before.get('Toggle' + title, {})
        assert before.get('canToggle' + title) is True and button.get('enabled'), '独立面板门禁拒绝'
        ui_point(button); self.pointer(role, button)
        def changed():
            current = latest()
            if not current or current['time'] <= before['time'] or current.get(key) is not expanded: return None
            for other in ('character', 'backpack', 'shop'):
                if other != panel: assert current[other + 'Expanded'] == before[other + 'Expanded'], '其它面板被联动切换'
            if current.get(area, {}).get('laidOut') is not expanded: return None
            return current
        after = self.wait(changed, role + '独立点击后新鲜布局')
        self.emit('PANEL_TOGGLED', role=role, panel=panel, expanded=expanded, before=before, after=after)
        return after

    def advance_manual(self):
        def reliable(ui):
            sample = ui.get('waveServerTime')
            return (ui.get('automaticWaves') is True or
                    (ui.get('automaticWaves') is False and ui.get('waveClockSource') in ('AuthoritySample', 'ObserverHeld')
                     and type(sample) in (int, float) and math.isfinite(sample) and sample >= 0))
        ui, _ = self.interaction_state()
        if not reliable(ui):
            def policy():
                current, _ = self.interaction_state()
                return current if reliable(current) else None
            ui = self.wait(policy, '等待可靠波次策略，禁止缺样本人工推进')
        if ui.get('automaticWaves') is True: return False
        before = self.wait(self.wave_state, '手动推进前权威阶段样本')
        if ui.get('Advance', {}).get('laidOut'):
            assert ui['Advance'].get('enabled'), '手动推进按钮不可用'
            self.pointer('A', ui['Advance'])
        else:
            viewport = ui.get('TopVitals', {})
            assert viewport.get('laidOut'), '缺少安全视口点击几何，拒绝Enter'
            # 此文本及其祖先不拦截点击，原生鼠标将Slate焦点交回游戏视口。
            self.pointer('A', viewport)
            self.owned_identity('A')
            self.n.p = self.owned['A']; self.n.window = self.windows['A']
            self.n.focused('R36-manual-advance')
            assert not self.n.user.GetAsyncKeyState(0x0D) & 0x8000, '用户正按住Enter'
            self.n.key(0x0D)
            self.emit('NATIVE_MANUAL_ADVANCE', scenario=getattr(self.args, 'scenario', 'controlled'), key='Enter')
        def advanced():
            current = self.wave_state()
            return current if (current and current['time'] > before['time'] and current['phase'] in (1, 2)
                               and (current['phase'], current['index']) != (before['phase'], before['index'])) else None
        after = self.wait(advanced, '手动推进后等待权威阶段改变')
        self.emit('MANUAL_ADVANCE_CONFIRMED', before=before, after=after)
        return True

    def expand_panels(self, role):
        def latest():
            rows = [row for row in self.records()[role] if row.get('kind') == 'UI']
            return rows[-1] if rows else None
        before = self.wait(latest, role + '展开前真实UI观测')
        assert isinstance(before, dict), '缺少真实UI观测，不能发送展开输入'
        if 'backpackExpanded' in before:
            for panel in ('character', 'backpack', 'shop'): self.set_panel(role, panel, True)
            return
        assert type(before.get('panelsExpanded')) is bool, '缺少面板显隐观测，不能假定旧版本默认可见'
        if before['panelsExpanded']:
            self.wait(lambda: (latest() or {}).get('bagGeometry', {}).get('laidOut'), role + '已展开背包实际布局')
            return
        button = before.get('TogglePanels', {})
        assert button.get('enabled'), '展开按钮不可用'
        ui_point(button)
        self.pointer(role, button)
        def expanded():
            current = latest()
            return (current and current['time'] > before['time'] and current.get('panelsExpanded') is True
                    and current.get('bagGeometry', {}).get('laidOut') is True)
        self.wait(expanded, role + '真实点击后新鲜展开布局')
        self.emit('PANELS_EXPANDED', role=role)

    def owned_identity(self, role):
        process = self.owned[role]
        assert same_process(self.identities[role], dict(pid=process.pid, created=self.created(process))), '原生句柄创建时间不符'
        assert process.poll() is None and self.n.identity(self.windows[role])['pid'] == process.pid, '窗口不属于本次活进程'

    def snapshot(self, label):
        recorder = self.helpers.skill.Recorder(self, label)
        recorder.start()
        recorder.stop()
        self.emit('PNG_CAPTURE', frames=recorder.frames)
        records = self.records()
        for role in ('A', 'B'):
            ui = next((r for r in reversed(records.get(role, [])) if r.get('kind') == 'UI'), None)
            if ui and 'backpackExpanded' in ui:
                self.emit('HUD_SNAPSHOT', label=label, role=role, hud=hud_record(ui))

    def pointer(self, role, source, target=None, button='left'):
        start = ui_point(source); end = ui_point(target) if target is not None else None
        if button not in ('left', 'right', 'hover') or (button == 'hover' and target is not None):
            raise ValueError('只允许明确鼠标键或无拖动悬停')
        self.owned_identity(role)
        n = self.n; n.p = self.owned[role]; n.window = self.windows[role]
        n.activate()
        def guard():
            self.owned_identity(role); n.focused('R36-mouse')
        def move(point):
            p = n.w.POINT(*point)
            if not n.user.ClientToScreen(n.window, n.c.byref(p)) or not n.user.SetCursorPos(p.x, p.y):
                raise RuntimeError('自有客户区鼠标定位失败')
        if button == 'hover':
            guard(); move(start); time.sleep(.8); guard()
            self.emit('NATIVE_HOVER', role=role, point=start)
            return
        down, up = (2, 4) if button == 'left' else (8, 16)
        def event(flags):
            begin = time.perf_counter()
            n.user.mouse_event(flags, 0, 0, 0, 0)
            end_time = time.perf_counter()
            if flags == up and getattr(self.args, 'timing', False):
                self.emit('MOUSE_RELEASE', role=role, begin=begin, end=end_time)
        if end is not None:
            perform_drag(start, end, move, lambda: event(down), lambda: event(up), guard, lambda: time.sleep(.035),
                         observe=lambda: self.snapshot('Drag-' + role + '-' + uuid.uuid4().hex))
        else:
            guard(); move(start); guard(); event(down); time.sleep(.08); guard(); event(up)
        self.emit('NATIVE_POINTER', role=role, source=start, target=end, button=button)

    def skill_key(self, role, slot):
        if type(slot) is not int or not 0 <= slot < 4:
            raise ValueError('技能键只接受四个合法槽索引')
        self.owned_identity(role)
        n = self.n; n.p = self.owned[role]; n.window = self.windows[role]
        n.activate()
        self.owned_identity(role); n.focused('R36-skill-before')
        vk = 0x31 + slot
        if any(n.user.GetAsyncKeyState(key) & 0x8000 for key in (0x10, 0x11, 0x12, 0x5B, 0x5C, vk)):
            raise RuntimeError('用户仍按住技能键或修饰键，拒绝覆盖输入或释放用户按键')
        n.key(vk)
        self.owned_identity(role); n.focused('R36-skill-after')
        self.emit('NATIVE_SKILL_KEY', role=role, slot=slot, vk=vk)

    def move_key(self, role, key, seconds):
        if key not in ('W', 'A', 'S', 'D') or type(seconds) not in (int, float) or not 0 < seconds <= .2:
            raise ValueError('只接受WASD及不超过0.2秒的有限物理输入')
        self.owned_identity(role)
        n = self.n; n.p = self.owned[role]; n.window = self.windows[role]
        n.activate(); self.owned_identity(role); n.focused('R36-move-before')
        if any(n.user.GetAsyncKeyState(vk) & 0x8000 for vk in (0x10, 0x11, 0x12, 0x5B, 0x5C, 0x57, 0x41, 0x53, 0x44)):
            raise RuntimeError('用户仍按住移动键或修饰键，拒绝覆盖输入')
        n.key(ord(key), seconds)
        self.owned_identity(role); n.focused('R36-move-after')
        self.emit('NATIVE_MOVE_KEY', role=role, key=key, seconds=seconds)

    def ensure_cd_target(self):
        scope = self.wait(lambda: combat_scope(self.records(), 'A'), 'CD前本人真实Battle观测', seconds=15)
        for step in range(9):
            if scope['targetValid']:
                self.emit('COMBAT_TARGET_READY', scope=scope, moves=step, controlled=True, natural_purchase=False)
                return
            if step == 8: raise RuntimeError('真实移动后仍无合法目标，不伪造CD前置条件')
            x, y = scope['pawnX'], scope['pawnY']
            if max(abs(x), abs(y)) <= 20:
                raise RuntimeError('已到最小证据场中心仍无合法目标，停止而非猜测成功')
            key = ('W' if x < 0 else 'S') if abs(x) >= abs(y) else ('D' if y < 0 else 'A')
            self.move_key('A', key, .2)
            before = scope
            def moved():
                current = combat_scope(self.records(), 'A')
                if not current or current['time'] <= before['time'] + .5: return None
                if not current['targetValid'] and max(abs(current[k] - before[k]) for k in ('pawnX', 'pawnY')) < 1: return None
                return current
            scope = self.wait(moved, '原生移动后的新鲜权威位置或合法目标', seconds=15)

    def scroll_bag(self, role):
        ui, view = self.interaction_state(role)
        coverage = ScrollCoverage(self.args.items, view['revision'])
        begin = time.monotonic(); pages = 0
        while True:
            cells = coverage.observe(ui); pages += 1
            self.emit('SCROLL_PAGE', role=role, page=pages, time=ui['time'], offset=ui['offset'],
                      revision=view['revision'], covered=len(coverage.items),
                      cells=[{k: c[k] for k in ('item', 'order', 'widget', 'rect')} for c in cells])
            if len(coverage.items) == self.args.items:
                result = coverage.finish()
                self.snapshot('Scroll-' + role + '-Last')
                self.emit('SCROLL_PASS', role=role, seconds=time.monotonic() - begin, pages=pages,
                          coverage=result, controlled=True, natural_purchase=False)
                return result
            if time.monotonic() - begin >= self.args.timeout:
                raise TimeoutError(f'{role}逐件滚动超时：{len(coverage.items)}/{self.args.items}')
            previous = ui
            self.wheel(role, ui)
            def changed():
                state = settled_ui(self.records(), role)
                if not state: return None
                current, authority = state
                assert all(authority[k] == view[k] for k in ('connection', 'revision', 'digest')), '滚动期间Owner或完整提交改变'
                if current.get('time', 0) <= previous['time'] or current.get('offset', 0) <= previous['offset']:
                    return None
                return current
            ui = self.wait(changed, role + '真实滚轮后新鲜布局且向下推进', seconds=15)

    def wheel(self, role, ui, ticks=-6):
        if type(ticks) is not int or not 1 <= abs(ticks) <= 8:
            raise ValueError('滚轮只接受非零且不超过八格的整数步长')
        visible_bag_cells(ui)
        point = ui_point(ui['bagGeometry'])
        self.owned_identity(role)
        n = self.n; n.p = self.owned[role]; n.window = self.windows[role]
        n.activate()
        self.owned_identity(role); n.focused('R36-wheel-position')
        p = n.w.POINT(*point)
        if not n.user.ClientToScreen(n.window, n.c.byref(p)) or not n.user.SetCursorPos(p.x, p.y):
            raise RuntimeError('自有背包滚轮定位失败')
        self.owned_identity(role); n.focused('R36-wheel-input')
        n.user.mouse_event(0x0800, 0, 0, (ticks * 120) & 0xffffffff, 0)
        self.emit('NATIVE_WHEEL', role=role, point=point, ticks=ticks)

    def interaction_state(self, role='A', seq=None):
        state = self.wait(lambda: settled_ui(self.records(), role, seq), role + '完整权威回执及同版本UI')
        assert isinstance(state, tuple) and len(state) == 2, '没有完整UI和权威视图，禁止原生输入'
        if getattr(self.args, 'timing', False):
            assert timing_ui_complete(*state), '专项UI不是本人同版本完整布局'
        return state

    def action(self, label, source, target=None, button='left', code=0, check=None):
        ui, before = self.interaction_state()
        start = source(ui)
        assert (ui.get('canDrag') or confirmation_open(ui) or
                (target is None and ui.get('selectingMaterials') and start.get('enabled'))), '当前面板不允许操作'
        _, other = self.interaction_state('B')
        begin = time.monotonic()
        self.pointer('A', start, target(ui) if target else None, button=button)
        _, after = self.interaction_state('A', before['expectedSeq'])
        assert after['code'] == code, f'{label}服务器回执不符合预期：{after}'
        assert check is None or check(before, after), f'{label}完整提交业务变化不符合预期'
        _, other_after = self.interaction_state('B')
        assert all(other[k] == other_after[k] for k in ('items', 'bag', 'equipment', 'skills')), '另一Owner私有库存意外改变'
        natural = getattr(self.args, 'scenario', 'controlled') == 'natural'
        self.emit('ACTION_PASS', label=label, before=before, after=after, other=other_after,
                  input_to_agreed_ui_seconds=time.monotonic() - begin, controlled=not natural, natural_purchase=natural)
        self.snapshot(label)
        return after

    def button_in_list(self, name, container):
        begin = time.monotonic()
        while True:
            ui, _ = self.interaction_state()
            if visible_control(ui, name, container): return ui[name]
            assert time.monotonic() - begin < self.args.timeout, '列表按钮未能滚动到完整可见'
            control = ui.get(name, {}); area = ui.get(container, {})
            assert control and area.get('laidOut'), '列表尚未布局或目标不存在'
            if control.get('laidOut'):
                ticks = 2 if control['rect'][1] < area['rect'][1] else -2
            else:
                # Slate尚未绘制的远端子项没有缓存几何；只按真实可见兄弟的顺序滚动列表。
                prefix, index = name.rsplit('_', 1)
                visible = [int(key.rsplit('_', 1)[1]) for key in ui
                           if key.startswith(prefix + '_') and key.rsplit('_', 1)[1].isdigit()
                           and visible_control(ui, key, container)]
                assert visible and (int(index) < min(visible) or int(index) > max(visible)), '无法从可见项确定滚动方向'
                ticks = 2 if int(index) < min(visible) else -2
            self.wheel('A', dict(ui, bagGeometry=area, cells=[]), ticks)
            self.wait(lambda: (settled_ui(self.records(), 'A') or ({}, {}))[0].get('time', 0) > ui['time'], '商品滚动后新鲜布局')

    def purchase(self, index, label):
        _, view = self.interaction_state(); offer = view['offers'][index]
        self.button_in_list('Purchase_' + str(index), 'ShopList')
        cost = offer['price']
        def enough():
            state = settled_ui(self.records(), 'A')
            return state and state[1]['gold'] >= cost
        if view['gold'] < cost: self.wait(enough, '等待合法自然收入足够支付商品')
        def check(b, a):
            economy_delta(b, a, cost, allow_growth=bool(getattr(self.args, 'presentation', None)))
            if offer['kind'] == 1:
                return (all(a[k] == b[k] for k in ('items', 'bag', 'equipment', 'skills'))
                        and a['attack'] == b['attack'] + offer['attackEffect'] and a['incomeRate'] == b['incomeRate'] + offer['incomeEffect'])
            return (a['items'] == b['items'] + 1 and a['bag'] == b['bag'] + 1
                    and all(a[k] == b[k] for k in ('equipment', 'skills', 'attack', 'incomeRate'))
                    and a['requestOffer'] == offer['id'] and bool(set(a['productItem']) - {'0', '-'}))
        before = view
        after = self.action(label, lambda ui: ui['Purchase_' + str(index)], check=check)
        self.emit('PURCHASE_ACCOUNTING', label=label, price=cost,
                  inferred_natural_since_sample=economy_delta(before, after, cost, allow_growth=bool(getattr(self.args, 'presentation', None))), product=after.get('productItem'),
                  definition=after.get('productDefinition'), quality=after.get('productQuality'), seed_override=False)
        return after

    def s3_capture(self, label, action):
        recorder = self.helpers.skill.Recorder(self, label)
        try:
            recorder.start()
            return action()
        finally:
            recorder.stop()
            self.emit('PNG_CAPTURE', label=label, frames=recorder.frames)

    def s3_region(self, inside):
        before = self.wait(lambda: training_scope(self.records(), 'A'), '本人训练状态')
        if before['inside'] is inside: return before
        ui, _ = self.interaction_state(); self.pointer('A', ui['Training'])
        def changed():
            current = training_scope(self.records(), 'A')
            return current if current and current['time'] > before['time'] and current['inside'] is inside else None
        result = self.wait(changed, '正常鼠标切换本人训练房')
        self.emit('S3_REGION', inside=inside, before=before, after=result)
        return result

    def s3_train_kills(self, count):
        before = self.wait(lambda: training_scope(self.records(), 'A'), '击杀前本人计数')
        target = before['trainingKills'] + count
        self.s3_region(True)
        def reached():
            scope = training_scope(self.records(), 'A')
            if not scope: return None
            assert scope['trainingKills'] <= target, '多杀不可冒充指定成长节点'
            return scope if scope['trainingKills'] == target else None
        after = self.wait(reached, '等待正常普攻有效致死，不注入伤害或累计', seconds=40)
        self.s3_region(False)
        final = self.wait(reached, '离房清理不新增击杀')
        self.emit('S3_NORMAL_KILLS', count=count, before=before, after=after, final=final)
        return final

    def s3_toggle_flights(self, enabled):
        ui, before = self.interaction_state()
        if ui['flightsEnabled'] is enabled: return
        self.pointer('A', ui['ToggleFlights'])
        def changed():
            state = settled_ui(self.records(), 'A')
            if not state or state[0]['time'] <= ui['time'] or state[0]['flightsEnabled'] is not enabled: return None
            assert all(state[1][k] == before[k] for k in ('expectedSeq', 'response')), '视觉开关不能发送交易'
            return state
        _, after = self.wait(changed, '本地视觉开关，不发领奖RPC')
        self.emit('S3_FLIGHTS_TOGGLE', enabled=enabled, before=before, after=after)

    def s3_growth_node(self, label, item, duplicate, count, equipped):
        _, view = self.interaction_state(); _, other = self.interaction_state('B')
        validate_s3_growth(view, item, duplicate, count, equipped)
        assert other['items'] == other['bag'] == other['strength'] == 0 and other['attack'] == 30
        assert not other['growth']['playerRules'] and not other['deliveries']
        self.emit('S3_GROWTH_NODE', label=label, view=view, other=other, normal_purchase=True)
        self.snapshot(label)
        for identity, zone, suffix in ((item, 1 if equipped else 0, '-ItemDetails'), (duplicate, 0, '-DuplicateDetails')):
            ui, _ = self.interaction_state()
            self.pointer('A', choose_cell(ui, zone, item=identity), button='hover')
            self.snapshot(label + suffix)
        return view

    def s3_growth(self):
        item = self.purchase(0, 'S3-Buy-First')['productItem']
        duplicate = self.purchase(0, 'S3-Buy-Duplicate')['productItem']
        self.action('S3-Equip40', lambda u: choose_cell(u, 0, item=item), lambda u: choose_cell(u, 1, slot=0))
        self.s3_growth_node('S3-Growth40', item, duplicate, 0, True)
        self.s3_train_kills(3)
        self.s3_growth_node('S3-Growth43', item, duplicate, 3, True)
        self.action('S3-Unequip30', lambda u: choose_cell(u, 1, slot=0), lambda u: u['bagGeometry'])
        self.s3_growth_node('S3-GrowthUnequipped', item, duplicate, 3, False)
        self.s3_train_kills(2)
        self.s3_growth_node('S3-GrowthStopped', item, duplicate, 3, False)
        self.action('S3-Reequip43', lambda u: choose_cell(u, 0, item=item), lambda u: choose_cell(u, 1, slot=0))
        self.s3_growth_node('S3-GrowthReequipped', item, duplicate, 3, True)

    def s3_drop(self):
        mode = self.args.presentation
        if mode == 'disabled': self.s3_toggle_flights(False)
        deadline = time.monotonic() + min(self.args.timeout, 120)
        attempt = 0
        while time.monotonic() < deadline:
            attempt += 1
            def sample():
                before = self.wait(lambda: training_scope(self.records(), 'A'), '正常击杀前计数')
                self.s3_region(True)
                def killed():
                    scope = training_scope(self.records(), 'A')
                    return scope if scope and scope['trainingKills'] > before['trainingKills'] else None
                killed_scope = self.wait(killed, '正常鼠标进入后自动攻击真实致死', seconds=15)
                assert killed_scope['trainingKills'] == before['trainingKills'] + 1
                views = latest_views(self.records()['A'])
                owner = views[0]['connection']
                authority = next(v for v in latest_views(self.records()['Server']) if v['connection'] == owner)
                self.emit('S3_DROP_SAMPLE', attempt=attempt, scenario=self.args.scenario, scope=killed_scope, view=authority, seed_override=False)
                if not authority['deliveries']:
                    self.s3_region(False)
                    return None
                assert len(authority['deliveries']) == 1
                pending = authority['deliveries'][0]
                if mode == 'region':
                    self.s3_region(False)
                elif mode == 'death':
                    assert pending['state'] == 0, '死亡场必须在原ETA前触发'
                    self.request_lifecycle('death')
                    self.wait(lambda: (settled_ui(self.records(), 'A') or ({}, {}))[1].get('health') == 0, '本人死亡提交')
                else:
                    # 正常后退避开下一靶，保留当前交付及镜头，不清怪或修改权威ETA。
                    for _ in range(4): self.move_key('A', 'S', .2)
                    if mode == 'drop' and self.args.scenario == 'controlled':
                        self.s3_toggle_flights(False); self.s3_toggle_flights(True)
                result = self.wait(lambda: s3_delivery_evidence(self.records()), '权威0到1及A到货B0', seconds=15)
                self.wait(lambda: (settled_ui(self.records(), 'A') or ({}, {}))[0].get('serverTime', 0) > pending['eta'] + 1,
                          '到货闪格后继续观测重复Publish', seconds=10)
                self.s3_region(False)
                return s3_delivery_evidence(self.records()) or result
            result = self.s3_capture('S3-Drop-' + str(attempt), sample)
            if not result:
                self.emit('S3_NATURAL_NO_DROP', attempt=attempt)
                assert self.args.scenario == 'natural', 'controlled掉率1却未判定交付'
                continue
            self.s3_toggle_flights(False); self.s3_toggle_flights(True)
            ui, view = self.interaction_state()
            assert view['items'] == view['bag'] == 1
            assert not any(c.get('flashRemaining', 0) > 0 for c in ui.get('cells', [])), '旧物不能重闪'
            final = s3_delivery_evidence(self.records()); assert final
            if mode == 'disabled': assert not result['flight_observed'], '禁播期间出现真实图元'
            self.emit('S3_DELIVERY_PASS', scenario=self.args.scenario, mode=mode, result=final, png_review_required=True)
            self.snapshot('S3-Final-Bags')
            return
        raise TimeoutError('自然随机在有界正常击杀采样内未落；保留事实，不注入结果')

    def presentation_actions(self):
        ui, initial = self.interaction_state()
        assert initial['items'] == 0, 'S3只允许从真实空包开始'
        assert all(initial[k] == v for k, v in dict(gold=0, bag=0, attack=30, incomeRate=1, strength=0).items()), 'S3初始账本不符'
        _, other = self.interaction_state('B')
        assert other['items'] == other['bag'] == other['strength'] == 0 and other['attack'] == 30
        self.emit('S3_INITIAL', initial=initial, other=other, scenario=self.args.scenario)
        if not ui.get('canDrag'):
            self.advance_manual()
            self.wait(lambda: (settled_ui(self.records(), 'A') or ({}, {}))[0].get('canDrag'), '正常开始准备及收入')
        if self.args.presentation == 'growth': self.s3_growth()
        else: self.s3_drop()

    def wave_state(self):
        return next((r for r in reversed(self.records()['Server']) if r.get('kind') == 'WAVE'), None)

    def offer_index(self, item_type=None, kind=None):
        wave = self.wait(self.wave_state, '读取真实Profile商品配置')
        _, view = self.interaction_state()
        candidates = [o for o in wave['shopConfig'] if (item_type is None or o.get('itemType') == item_type)
                      and (kind is None or o['kind'] == kind)]
        assert candidates, '实际Profile没有所需种类商品'
        offer = min(candidates, key=lambda o: o['price'])
        return next(i for i, o in enumerate(view['offers']) if o['id'] == offer['id'])

    def source_actions(self):
        ui, initial = self.interaction_state(); _, other = self.interaction_state('B')
        for view in (initial, other):
            assert all(view[k] == v for k, v in dict(gold=200, items=0, bag=0, attack=30, incomeRate=1, strength=0).items())
        self.emit('S4_SOURCE_INITIAL', initial=initial, other=other)
        if not ui.get('canDrag'):
            self.advance_manual()
            self.wait(lambda: (settled_ui(self.records(), 'A') or ({}, {}))[0].get('canDrag'), '真实开始准备')
        equipment = self.purchase(self.offer_index(item_type=7), 'S4-BuyEquipment')['productItem']
        self.action('S4-Equip', lambda u: choose_cell(u, 0, item=equipment), lambda u: choose_cell(u, 1, slot=0),
                    check=lambda b, a: a['equipment'][0] == equipment and a['bag'] == b['bag'] - 1)
        book = self.purchase(self.offer_index(item_type=5), 'S4-BuyBook')['productItem']
        self.action('S4-Learn', lambda u: choose_cell(u, 0, item=book), lambda u: choose_cell(u, 2, slot=0),
                    check=lambda b, a: a['items'] == b['items'] - 1 and a['skills'][0]['id'] != b['skills'][0]['id'])
        self.button_in_list('Talent_0', 'TalentList')
        self.action('S4-Talent', lambda u: u['Talent_0'], check=lambda b, a: a['selectedTalent'] != 'None')
        self.purchase(self.offer_index(kind=1), 'S4-Direct')
        _, full = self.interaction_state(); _, other = self.interaction_state('B')
        expected = (52, 6) if self.args.profile == 'compare' else (50, 5)
        assert (full['attack'], full['incomeRate']) == expected and full['strength'] == 0
        assert other['items'] == other['bag'] == 0 and other['attack'] == 30 and other['incomeRate'] == 1
        self.emit('S4_FOUR_SOURCES', profile=self.args.profile, view=full, other=other)
        self.snapshot('S4-FourSources')
        self.action('S4-Unequip', lambda u: choose_cell(u, 1, slot=0), lambda u: u['bagGeometry'],
                    check=lambda b, a: a['attack'] == 45 and a['incomeRate'] == 4 and a['bag'] == b['bag'] + 1)
        _, off = self.interaction_state(); assert off['attack'] == 45 and off['incomeRate'] == 4
        self.emit('S4_SOURCE_PASS', equipped=full, unequipped=off, other=other)

    def wave_wallets(self, wave):
        result = {}
        reward = sum(w['rewardGold'] for w in wave['orderedWaves'][:int(wave['completed'])])
        xp = sum(w['rewardXP'] for w in wave['orderedWaves'][:int(wave['completed'])])
        for role in ('A', 'B'):
            def account():
                records = self.records(); state = settled_ui(records, role)
                if not state: return None
                view = state[1]
                # 同一Capture内VIEW先于WAVE；先锚定本Owner已清波的权威版本，再等待客户端。
                committed = None; cleared_revision = None
                for row in records['Server']:
                    if row.get('kind') == 'VIEW': committed = row
                    elif (row.get('kind') == 'WAVE' and row.get('cleared') and row['completed'] >= wave['completed']
                          and committed and committed['connection'] == view['connection']):
                        cleared_revision = committed['revision']; break
                if cleared_revision is None or view['revision'] < cleared_revision: return None
                ledger = next((r for r in reversed(records['Server']) if r.get('kind') == 'ECONOMY'
                    and r['connection'] == view['connection'] and r['revision'] == view['revision']), None)
                if not ledger: return None
                validate_wave_wallet(view, ledger, wave['initialGold'], reward, xp)
                return dict(view=view, ledger=ledger, full_wave_reward=reward)
            result[role] = self.wait(account, role + '逐人清波钱包与收入支出对账')
        self.emit('S4_WAVE_WALLETS', completed=wave['completed'], accounts=result)
        return result

    def play_wave(self, index):
        self.advance_manual()
        def preparing():
            wave = self.wave_state()
            return wave if wave and wave['index'] == index and wave['phase'] == 1 else None
        prepare = self.wait(preparing, '正常开始/下一波准备')
        config = prepare['orderedWaves'][index]
        assert math.isclose(prepare['prepareDeadline'] - prepare['phaseTime'], config['prepare'], abs_tol=.001)
        self.emit('S4_PREPARING', wave=prepare); self.snapshot(f'S4-Wave{index + 1}-Preparing')
        if index == 0:
            for role in ('A', 'B'): self.move_key(role, 'W', .2)
        seen = set(); last_move = 0.; cast = set()
        def combat():
            nonlocal last_move
            wave = self.wave_state()
            if not wave or wave['index'] != index: return None
            assert wave['phase'] != 3 or wave['result'] == 1, '自然玩家死亡/失败，不能注入复活'
            if wave['cleared']: return wave
            if wave['phase'] != 2: return None
            state = (wave['pending'], len(wave['monsters']))
            if state not in seen:
                seen.add(state); self.emit('S4_SPAWN_STAGE', wave=wave)
                self.snapshot(f'S4-Wave{index + 1}-Pending{int(wave["pending"])}')
            if time.monotonic() - last_move < .5: return None
            last_move = time.monotonic()
            for role in ('A', 'B'):
                scope = combat_scope(self.records(), role)
                if not scope: continue
                if scope['targetValid']:
                    views = latest_views(self.records()[role])
                    if role == 'A' and views:
                        for slot, skill in enumerate(views[0]['skills']):
                            key = (index, slot)
                            if key not in cast and skill['skill'] != 'None':
                                cast.add(key)
                                if getattr(self.args, 'content_check', None): self.content_skill(slot)
                                else: self.skill_key(role, slot)
                    continue
                live = [m for m in wave['monsters'] if m['health'] > 0]
                if not live: continue
                target = min(live, key=lambda m: math.hypot(m['x'] - scope['pawnX'], m['y'] - scope['pawnY']))
                dx, dy = target['x'] - scope['pawnX'], target['y'] - scope['pawnY']
                key = ('W' if dx > 0 else 'S') if abs(dx) >= abs(dy) else ('D' if dy > 0 else 'A')
                self.move_key(role, key, .15)
            return None
        final = self.wait(combat, '真实移动/自动普攻完成当前波，不注入伤害')
        self.emit('S4_WAVE_CLEARED', wave=final)
        self.wave_wallets(final); self.snapshot(f'S4-Wave{index + 1}-Cleared')
        return final

    def purchase_until(self, index, target, candidates, quality):
        assert len(set(candidates)) >= 2 and target in candidates
        deadline = time.monotonic() + self.args.timeout
        attempt = 0
        while time.monotonic() < deadline:
            attempt += 1
            result = self.purchase(index, 'S4-Added-Random-' + str(attempt))
            assert result['productDefinition'] in candidates and result['productQuality'] == quality, '真实随机产物不在配置候选或品质不符'
            self.emit('S4_RANDOM_SAMPLE', attempt=attempt, result=result, candidates=candidates, seed_override=False)
            if result['productDefinition'] == target: return result
        raise TimeoutError('自然随机购买尚未获得目标，保留逐笔结果，不指定seed或赠送')

    def configured_purchase(self, plan):
        _, view = self.interaction_state()
        config = self.wave_state()['shopConfig']
        indices = {}
        for key, kind, item_type in (('equipment_offer', 0, 7), ('book_offer', 0, 5), ('direct_offer', 1, None), ('random_offer', 0, 7)):
            matches = [i for i, offer in enumerate(view['offers']) if offer['id'] == plan[key]]
            assert len(matches) == 1, '选择商品必须存在于真实Profile公开列表且唯一'
            actual = [o for o in config if o['id'] == plan[key]]
            assert len(actual) == 1 and actual[0]['kind'] == kind and (item_type is None or actual[0]['itemType'] == item_type)
            indices[key] = matches[0]
        equipment = self.purchase(indices['equipment_offer'], 'S4-Added-BuyEquipment')
        assert equipment['productDefinition'] == plan['equipment'] and equipment['productQuality'] == plan['quality']
        item = equipment['productItem']
        self.action('S4-Added-Equip', lambda u: choose_cell(u, 0, item=item), lambda u: choose_cell(u, 1, slot=0),
                    check=lambda b, a: a['equipment'][0] == item and a['bag'] == b['bag'] - 1
                    and a['attack'] == b['attack'] + plan['equipment_attack'] and a['incomeRate'] == b['incomeRate'])
        book = self.purchase(indices['book_offer'], 'S4-Added-BuyBook')
        assert book['productDefinition'] == plan['skill'] and book['productQuality'] == plan['quality']
        self.action('S4-Added-Learn', lambda u: choose_cell(u, 0, item=book['productItem']), lambda u: choose_cell(u, 2, slot=0),
                    check=lambda b, a: a['items'] == b['items'] - 1 and a['bag'] == b['bag'] - 1
                    and a['skills'][0]['skill'] == plan['skill'] and a['skills'][0]['id'] != b['skills'][0]['id'])
        self.purchase(indices['direct_offer'], 'S4-Added-Direct')
        self.disabled_click('Purchase_' + str(indices['direct_offer']), 'ShopList', 'S4-Added-DirectLimit')
        random = self.purchase_until(indices['random_offer'], plan['equipment'], plan['random_candidates'], plan['quality'])
        self.emit('S4_ADDED_PURCHASE_PASS', plan=plan, equipment=equipment, book=book, random=random, seed_override=False)
        self.snapshot('S4-Added-Purchases')

    def content_skill(self, slot):
        ui, before = self.interaction_state()
        self.skill_key('A', slot)
        active_ui, active = self.wait(lambda: natural_skill_activation(self.records(), 'A', slot, before, ui['time']),
                                      '自然战斗按键后新鲜权威CD提交')
        assert all(active[k] == before[k] for k in ('items', 'bag')), '自然技能释放不能消耗书或生成物品'
        self.content_skill_confirmed = True
        self.emit('S4_ACTIVE_SKILL_PASS', slot=slot, before=before, active=active, ui=active_ui,
                  controlled=False, natural_purchase=True)
        self.snapshot('S4-Added-ActiveSkill-' + str(active['skills'][slot]['deadline']))

    def content_purchase(self):
        # 只在已经正常清波的间隙购物，避免把仍在飞行的自然掉落当作购买产物。
        def delivered():
            views = [agreed_view(self.records(), r) for r in ('A', 'B')]
            return all(v and all(d['state'] != 0 for d in v['deliveries']) for v in views)
        self.wait(delivered, '正常掉落飞行完成后购物')
        if getattr(self.args, 'content_check', None):
            return self.configured_purchase(load_content_check(self.args.content_check, self.args.run))
        equipment = self.purchase(self.offer_index(item_type=7), 'S4-Content-BuyEquipment')['productItem']
        self.action('S4-Content-Equip', lambda u: choose_cell(u, 0, item=equipment), lambda u: choose_cell(u, 1, slot=0),
                    check=lambda b, a: a['equipment'][0] == equipment and a['bag'] == b['bag'] - 1)
        book = self.purchase(self.offer_index(item_type=5), 'S4-Content-BuyBook')['productItem']
        self.action('S4-Content-Learn', lambda u: choose_cell(u, 0, item=book), lambda u: choose_cell(u, 2, slot=0),
                    check=lambda b, a: a['items'] == b['items'] - 1 and a['skills'][0]['id'] != b['skills'][0]['id'])
        self.emit('S4_CONTENT_PURCHASE_PASS', equipment=equipment, consumed_book=book, seed_override=False)

    def automatic_actions(self):
        assert getattr(self.args, 'scenario', 'controlled') == 'natural', '自动自然验收不能混受控夹具'
        initial = self.wait(self.wave_state, '自然自动配置')
        self.emit('AUTOMATIC_INITIAL', wave=initial, manual_advance=False)
        def overlap():
            wave = self.wave_state()
            if not wave: return None
            if sum(s['alive'] > 0 for s in wave['waveStates']) >= 2: return wave
            assert wave['phase'] != 3, '自然对局已结束，未观察到重叠，不注入通过'
            return None
        wave = self.wait(overlap, '自然等待前波存活时下一波出怪')
        rows = [r for r in self.records()['Server'] if r.get('kind') == 'WAVE']
        result = validate_automatic_wave_samples(rows, initial['orderedWaves'], require_overlap=True)
        self.snapshot('Automatic-Overlap')
        self.emit('AUTOMATIC_OVERLAP_PASS', result=result, wave=wave, full_clear_verified=False, manual_advance=False)

    def content_actions(self):
        config = self.wait(self.wave_state, '实际波次模式')
        if config.get('automaticWaves') is True: return self.automatic_actions()
        for role in ('A', 'B'):
            _, initial = self.interaction_state(role)
            assert all(initial[k] == v for k, v in dict(gold=0, items=0, bag=0, attack=30, incomeRate=1, strength=0, health=300).items())
            self.emit('S4_CONTENT_INITIAL', role=role, view=initial)
        config = self.wait(self.wave_state, '通用Profile与波次配置')
        for index in range(len(config['orderedWaves'])):
            self.play_wave(index)
            if index == 0: self.content_purchase()
        rows = [r for r in self.records()['Server'] if r.get('kind') == 'WAVE']
        validate_wave_samples(rows, config['orderedWaves'])
        if getattr(self.args, 'content_check', None):
            assert getattr(self, 'content_skill_confirmed', False), '新增主动技能必须在真实战斗中成功释放'
        self.emit('S4_CONTENT_PASS', waves=len(config['orderedWaves']), natural=True, seed_override=False)
        self.snapshot('S4-Ended')

    def natural_actions(self):
        ui, initial = self.interaction_state()
        assert all(initial[k] == v for k, v in dict(gold=200, items=0, bag=0, attack=30, incomeRate=1, strength=0).items()), '自然场必须明确Baseline空包200金币'
        if not ui.get('canDrag'):
            self.advance_manual()
            self.wait(lambda: self.interaction_state()[0].get('canDrag'), '首次Preparing启动自然收入')
        equipment = self.purchase(0, 'Natural-BuyEquipment')['productItem']
        self.action('Natural-Equip', lambda u: choose_cell(u, 0, item=equipment), lambda u: choose_cell(u, 1, slot=0),
                    check=lambda b, a: a['attack'] == 35 and a['incomeRate'] == 2 and a['bag'] == b['bag'] - 1 and a['equipment'][0] == equipment)
        book = self.purchase(1, 'Natural-BuyBook')['productItem']
        self.action('Natural-Learn', lambda u: choose_cell(u, 0, item=book), lambda u: choose_cell(u, 2, slot=0),
                    check=lambda b, a: a['attack'] == 40 and a['incomeRate'] == 3 and a['items'] == b['items'] - 1 and a['bag'] == b['bag'] - 1)
        self.button_in_list('Talent_0', 'TalentList')
        self.action('Natural-Talent', lambda u: u['Talent_0'], check=lambda b, a:
                    a['attack'] == 45 and a['incomeRate'] == 4 and a['selectedTalent'] != 'None'
                    and all(a[k] == b[k] for k in ('items', 'bag', 'equipment', 'skills')))
        self.purchase(2, 'Natural-Direct')
        self.disabled_click('Purchase_2', 'ShopList', 'Natural-Limit')
        self.action('Natural-Unequip', lambda u: choose_cell(u, 1, slot=0), lambda u: u['bagGeometry'], check=lambda b, a:
                    a['attack'] == 45 and a['incomeRate'] == 4 and a['items'] == b['items'] and a['bag'] == b['bag'] + 1)
        self.craft_actions()
        self.purchase(3, 'Natural-RandomTier')
        self.full_slots_purchase()

    def disabled_click(self, name, container, label):
        self.button_in_list(name, container)
        ui, before = self.interaction_state()
        assert ui[name].get('enabled') is False, '限购按钮必须权威禁用'
        self.pointer('A', ui[name])
        def unchanged():
            state = settled_ui(self.records(), 'A')
            if not state or state[0].get('time', 0) <= ui['time'] + 1: return None
            after = state[1]
            assert all(after[k] == before[k] for k in ('expectedSeq', 'response', 'items', 'bag', 'equipment', 'skills', 'attack', 'incomeRate')), '禁用点击意外发出交易'
            economy_delta(before, after, 0)
            return state
        _, after = self.wait(unchanged, '限购已购禁用点击无请求且无扣款')
        self.emit('DISABLED_CLICK_PASS', label=label, before=before, after=after, server_rejection=False, ui_rejection=True)
        self.snapshot(label)

    def set_material_mode(self, enabled):
        ui, before = self.interaction_state()
        if ui.get('selectingMaterials') is enabled: return
        self.pointer('A', ui['MaterialMode'])
        def changed():
            state = settled_ui(self.records(), 'A')
            return state and state[0].get('time', 0) > ui['time'] and state[0].get('selectingMaterials') is enabled
        self.wait(changed, '真实材料选择模式切换')

    def select_material(self, item):
        ui, before = self.interaction_state()
        cell = choose_cell(ui, 0, item=item)
        assert ui.get('selectingMaterials') and not cell.get('selected'), '只能选择未选中的包内材料'
        self.pointer('A', cell)
        def changed():
            state = settled_ui(self.records(), 'A')
            if not state or state[0].get('time', 0) <= ui['time']: return None
            current, view = state
            assert all(view[k] == before[k] for k in ('expectedSeq', 'items', 'bag')), '选材不能提前消耗或发请求'
            return current.get('materials') == ui['materials'] + 1 and any(c.get('item') == item and c.get('selected') for c in current.get('cells', []))
        self.wait(changed, '鼠标选择材料且未消费')
        self.emit('MATERIAL_SELECTED', item=item)

    def craft_actions(self):
        for i in range(4): self.purchase(0, 'Natural-Material-' + str(i + 2))
        self.set_material_mode(True)
        ui, _ = self.interaction_state()
        materials = [c for c in visible_bag_cells(ui) if c.get('type') == 7 and c.get('quality') == 0]
        assert len(materials) == 5, '五件自然购买同类同品质材料必须可见'
        for cell in materials: self.select_material(cell['item'])
        def check(b, a):
            economy_delta(b, a, 0)
            return (a['items'] == b['items'] - 4 and a['bag'] == b['bag'] - 4 and a['productQuality'] == 1
                    and all(a[k] == b[k] for k in ('equipment', 'skills', 'attack', 'incomeRate')))
        self.action('Natural-FreeCraft', lambda u: u['Synthesize'], check=check)
        self.set_material_mode(False)

    def full_slots_purchase(self):
        for slot in range(6):
            item = self.purchase(0, 'Natural-FullSlotBuy-' + str(slot))['productItem']
            self.action('Natural-FullSlotEquip-' + str(slot), lambda u: choose_cell(u, 0, item=item),
                        lambda u: choose_cell(u, 1, slot=slot), check=lambda b, a:
                        a['equipment'][slot] == item and a['bag'] == b['bag'] - 1 and a['attack'] == b['attack'] + 5 and a['incomeRate'] == b['incomeRate'] + 1)
        self.purchase(0, 'Natural-FullSlotsOnlyBag')

    def burst_actions(self):
        ui, _ = self.interaction_state()
        if not ui.get('canDrag'): self.advance_manual()
        result = self.wait(lambda: burst_result(self.records(), self.args.items), 'controlled快速RPC完整提交及删除合并')
        self.emit('BURST_PASS', **result, controlled=True, natural_purchase=False, mouse_drag=False)
        self.snapshot('Burst-Final')

    def timing_book(self):
        begin = time.monotonic()
        while True:
            ui, _ = self.interaction_state()
            cells = visible_bag_cells(ui)
            if any(c.get('type') == 5 for c in cells): return choose_book(dict(ui, cells=cells))
            assert time.monotonic() - begin < self.args.timeout, '真实滚动未找到已有技能书'
            self.wheel('A', ui)
            self.wait(lambda: (settled_ui(self.records(), 'A') or ({},))[0].get('offset', 0) > ui['offset'], '查书后新鲜布局')

    def timing_actions(self):
        for role in ('A', 'B'):
            _, view = self.interaction_state(role)
            assert view['items'] == view['bag'] == 500 and view['gold'] == 200
        # 初始覆盖结束后保留末页PNG；仅用物理滚轮回到可操作的首页。
        begin = time.monotonic()
        while True:
            ui, _ = self.interaction_state()
            if ui['offset'] == 0: break
            assert time.monotonic() - begin < self.args.timeout, '原生回顶未完成'
            self.wheel('A', ui, 8)
            self.wait(lambda: (settled_ui(self.records(), 'A') or ({},))[0].get('offset', math.inf) < ui['offset'], '回顶新鲜布局')
        self.snapshot('Timing-Before-Actions')
        self.controlled_actions()
        self.purchase(self.offer_index(item_type=7), 'Timing-Purchase-PlusOne')
        book = self.timing_book()
        self.snapshot('Timing-Before-Learn')
        item, definition = book['item'], book['definition']
        self.action('Timing-Learn-DeleteOne', lambda u: choose_cell(u, 0, item=item), lambda u: choose_cell(u, 2, slot=0),
                    check=lambda b, a: a['items'] == b['items'] - 1 and a['bag'] == b['bag'] - 1
                    and item not in {i['item'] for i in a['itemDetails']} and a['skills'][0]['skill'] == definition
                    and bool(set(a['skills'][0]['id']) - {'0', '-'}) and a['equipment'] == b['equipment'])
        self.snapshot('Timing-Final')

    def timing_report(self):
        records = self.records()
        self.clock_samples.extend(python_clock_samples())
        clocks = {r: clock_alignment([v for v in rows if v.get('kind') == 'TIMING']) for r, rows in records.items()}
        clocks['Python'] = clock_alignment(self.clock_samples)
        runner = [json.loads(line) for line in (self.output / 'runner.jsonl').read_text(encoding='utf-8').splitlines()]
        initial, operations, cold, audit = {}, [], {}, {}
        authority = timing_authority_map(records)
        identities = {}
        for role in ('A', 'B'):
            views = [v for v in records[role] if v.get('kind') == 'VIEW']
            first = next(v for v in views if v['items'] == 500)
            owner = first['connection']; identities[role] = owner
            ids = {i['item'] for i in first['itemDetails']}
            bootstrap = 0
            for v in views:
                bootstrap += timing_authority_view(v, first, authority) == 'bootstrap_not_sampled'
                assert len({i['item'] for i in v['itemDetails']}) == v['items'], '实例重复或数量不完整'
                if role == 'B' and v['revision'] >= first['revision']:
                    assert all(v[k] == first[k] for k in ('itemDetails', 'equipment', 'skills', 'response', 'seq', 'items', 'bag')), 'B混入A操作'
            for u in records[role]:
                if u.get('kind') != 'UI' or u.get('revision', 0) < first['revision']: continue
                expected = authority.get((owner, u['revision']))
                assert expected and u.get('connection') == owner and u['total'] == expected['bag'], 'UI数量或Owner混版'
                own = {i['item'] for i in expected['itemDetails']}
                assert all(c['item'] in own for c in u.get('cells', []) if c['zone'] == 0), 'UI串入外部实例'
            assert bootstrap == 0, '当前专项不允许缺失启动版本摘要'
            initial[role] = timing_segments(records, role, first, clocks)
            launch = next(r for r in runner if r['kind'] == 'PROCESS_LAUNCH_REQUEST' and r['role'] == role)
            cold[role] = initial[role]['boundaries']['layout_time'] - clocks[role]['offset'] - (launch['perf'] - clocks['Python']['offset'])
            audit[role] = dict(views=len(views), bootstrap_samples_without_authority_digest=bootstrap,
                               verified_full_samples=len(views) - bootstrap, initial_items=len(ids), connection=owner, final=views[-1])
        assert identities['A'] != identities['B']
        all_ids = {r: {i['item'] for v in records[r] if v.get('kind') == 'VIEW' for i in v['itemDetails']} for r in ('A', 'B')}
        assert all_ids['A'].isdisjoint(all_ids['B']), '双端实例交叉'
        for row in runner:
            if row['kind'] != 'ACTION_PASS': continue
            after = row['after']; seq = after['seq']
            done = [e for e in records['Server'] if e.get('stage') == 'REQUEST_DONE' and e['connection'] == identities['A'] and e['seq'] == seq]
            assert len(done) == 1
            committed = authority.get((identities['A'], done[0]['revision']))
            assert committed, '未采到请求commit完整快照，不能冒称已分段'
            transient = {'revision', 'digest', 'gold', 'sampleTime'}
            assert all(after[k] == committed[k] for k in committed.keys() - transient), '后续发布不是相同请求完整业务快照'
            assert after['gold'] >= committed['gold'], '合并期间余额异常回退'
            result = timing_segments(records, 'A', after, clocks, seq)
            result.update(label=row['label'], before_items=row['before']['items'], after_items=after['items'])
            sent = result['boundaries']['request_sent']['time'] - clocks['A']['offset']
            releases = [e for e in runner if e['kind'] == 'MOUSE_RELEASE' and e['role'] == 'A'
                        and e['begin'] - clocks['Python']['offset'] <= sent]
            assert releases
            release = releases[-1]
            result['mouse_release'] = release
            result['seconds']['mouse_release_end_to_request'] = sent - (release['end'] - clocks['Python']['offset'])
            operations.append(result)
        assert len(operations) == 5 and operations[-2]['after_items'] == 501 and operations[-1]['after_items'] == 500
        report = dict(clocks=clocks, python_clock_samples=self.clock_samples, initial=initial, operations=operations,
                      cold_launch_to_layout_seconds=cold, audit=audit,
                      limits=['编码载荷字节不是抓包量；SEND_WINDOW是复制排队，不是网卡发送',
                              '跨端传输区间包含复制调度、ACK和游戏线程调度及遥测扰动，不是纯网络延迟',
                              'UI_PRESENT是模型和刷新结束；layout是0.1秒轮询首次完整几何观测，不是GPU Present',
                              '技能图文由同版本模型和原PNG核对，几何遥测没有独立已学ID字段',
                              '每份完整VIEW均匹配服务器COMMIT事件摘要，包括启动空包v1；不依赖定时采样命中',
                              'COMMIT摘要耗时单列digestSeconds；逐事件强制落盘扰动仍包含在后续调度区间'])
        with (self.output / 'timing.json').open('x', encoding='utf-8') as out:
            json.dump(report, out, ensure_ascii=False, indent=2)
        self.emit('TIMING_PASS', operations=len(operations), cold=cold, clocks=clocks)

    def controlled_actions(self):
        ui, _ = self.interaction_state()
        if not ui.get('canDrag'):
            self.advance_manual()
            self.wait(lambda: self.interaction_state()[0].get('canDrag'), '真实开始后面板允许操作')
        selected = {}
        def equipment(ui):
            cell = choose_cell(ui, 0, type=7); selected['new'] = cell['attackEffect']; return cell
        slot = lambda ui: choose_cell(ui, 1, slot=0)
        self.action('02-Equip', equipment, slot, check=lambda b, a:
                    a['items'] == b['items'] and a['bag'] == b['bag'] - 1
                    and a['equipment'][0] == a['requestItem'] and a['attack'] == b['attack'] + selected['new'])
        selected['old'] = selected.get('new', 0)
        self.action('03-Replace', equipment, slot, check=lambda b, a:
                    a['items'] == b['items'] and a['bag'] == b['bag'] and a['attack'] == b['attack'] - selected['old'] + selected['new']
                    and a['equipment'][0] == a['requestItem'] and a['equipment'][0] != b['equipment'][0])
        self.action('04-Unequip', slot, lambda ui: ui['bagGeometry'], check=lambda b, a:
                    a['items'] == b['items'] and a['bag'] == b['bag'] + 1 and a['attack'] == b['attack'] - selected['new']
                    and a['requestItem'] == b['equipment'][0] and set(a['equipment'][0]) <= {'0', '-'})

    def activate_skill(self, label, slot):
        self.ensure_cd_target()
        ui, before = self.interaction_state()
        self.skill_key('A', slot)
        active_ui, active = self.wait(lambda: cooldown_activation(self.records(), 'A', slot, before, ui['time']),
                                      label + '真实技能键后的权威CD提交', seconds=15)
        assert all(active[k] == before[k] for k in ('items', 'bag')), '释放技能不能消耗书'
        def decreased():
            state = settled_ui(self.records(), 'A')
            if not state: return None
            current_ui, current = state
            assert current['connection'] == active['connection'] and all(current[k] == active[k] for k in ('skills', 'items', 'bag')), '观察CD时身份/截止/库存改变'
            now = current_ui.get('serverTime')
            if type(now) not in (int, float) or not math.isfinite(now) or now < 0:
                raise ValueError('CD递减观测缺少有效服务器时间')
            if current_ui['time'] <= active_ui['time'] or now <= active_ui['serverTime'] + .25: return None
            assert active['skills'][slot]['deadline'] > now, '必须在非零CD期间观察递减'
            return state
        later_ui, later = self.wait(decreased, label + '服务器同步时间下CD递减', seconds=15)
        self.emit('COOLDOWN_PASS', label=label, before=before, active=active, later=later,
                  first_server_time=active_ui['serverTime'], later_server_time=later_ui['serverTime'],
                  controlled=True, natural_purchase=False)
        self.snapshot(label)
        return later

    def skill_actions(self):
        ui, _ = self.interaction_state()
        first = choose_book(ui)['definition']
        second = choose_book(ui, exclude=(first,))['definition']
        book = lambda definition: lambda ui: choose_book(ui, skill=definition)
        slot = lambda ui: choose_cell(ui, 2, slot=0)
        empty = lambda value: bool(value) and set(value) <= {'0', '-'}
        unchanged = lambda b, a: all(b[k] == a[k] for k in ('items', 'bag', 'equipment', 'skills'))
        def learned(definition):
            return lambda b, a: (a['items'] == b['items'] - 1 and a['bag'] == b['bag'] - 1
                and a['equipment'] == b['equipment'] and a['skills'][1:] == b['skills'][1:]
                and bool(a['skills'][0]['id']) and not empty(a['skills'][0]['id'])
                and a['skills'][0]['id'] != b['skills'][0]['id']
                and a['skills'][0]['skill'] == definition and a['skills'][0]['deadline'] == 0
                and empty(a['ticket']))
        initial = self.action('05-Learn', book(first), slot, check=learned(first))
        cooldown = getattr(self.args, 'cooldown', False)
        if cooldown:
            self.advance_manual()
            self.activate_skill('CD-First', 0)
        self.action('06-Duplicate', book(first), lambda ui: choose_cell(ui, 2, slot=1), code=26, check=unchanged)
        prepared = lambda b, a: unchanged(b, a) and bool(a['ticket']) and not empty(a['ticket'])
        self.action('07-PrepareCancel', book(second), slot, check=prepared)
        self.action('08-Cancel', lambda ui: ui['Cancel'], check=lambda b, a: unchanged(b, a) and empty(a['ticket']))
        self.action('09-PrepareConfirm', book(second), slot, check=prepared)
        self.action('10-Confirm', lambda ui: ui['Confirm'], check=learned(second))
        if cooldown: self.activate_skill('CD-Replacement', 0)
        self.action('11-Remove', slot, button='right', check=lambda b, a:
                    all(a[k] == b[k] for k in ('items', 'bag', 'equipment'))
                    and a['skills'][1:] == b['skills'][1:] and empty(a['skills'][0]['id'])
                    and a['skills'][0]['skill'] == 'None' and a['skills'][0]['deadline'] == 0)
        self.action('12-Relearn', book(first), slot, check=lambda b, a:
                    learned(first)(b, a) and a['skills'][0]['id'] != initial['skills'][0]['id'])
        if cooldown: self.activate_skill('CD-Relearn', 0)

    def request_lifecycle(self, name):
        if (name not in ('death', 'ended') or not getattr(self.args, 'lifecycle', False)
                or self.args.scenario != 'controlled' or not self.args.run
                or not re.fullmatch(r'R36-(?:S2-(?:Visible|Runtime)|S3-(?:Visible|Presentation))-[A-Za-z0-9_-]+', self.args.label)
                or self.output != ROOT / 'Saved' / self.args.label
                or not re.fullmatch(r'[0-9a-f]{32}', self.stop_token)):
            raise ValueError('仅允许本次显式controlled生命周期令牌')
        with (self.output / (name + '.request')).open('x', encoding='ascii') as out: out.write(self.stop_token)
        self.emit('LIFECYCLE_REQUEST', action=name, controlled=True, natural_purchase=False)

    def lifecycle_state(self, role='A'):
        records = self.records(); state = settled_ui(records, role)
        if not state: return None
        ui, view = state
        scope = next((r for r in reversed(records[role]) if r.get('kind') == 'SCOPE'), None)
        ledger = next((r for r in reversed(records['Server']) if r.get('kind') == 'ECONOMY'
                       and r.get('connection') == view['connection'] and r.get('revision') == view['revision']), None)
        if not scope or not ledger or not all(k in scope for k in ('phase', 'inside', 'room')): return None
        assert ledger['balance'] == view['gold'] and ledger['rate'] == view['incomeRate']
        assert ledger['balance'] == 200 + int(ledger['totalIncome']) - int(ledger['totalSpending'])
        return ui, view, scope, ledger

    def lifecycle_observe(self, label, stopped=False):
        def ready(role):
            state = self.lifecycle_state(role)
            return state if state and state[3]['phase'] == (3 if stopped else 2) else None
        before = {r: self.wait(lambda role=r: ready(role), '完整生命周期账本') for r in ('A', 'B')}
        def progressed():
            current = {r: self.lifecycle_state(r) for r in ('A', 'B')}
            if not all(current.values()): return None
            return current if all(current[r][0]['time'] >= before[r][0]['time'] + 3 for r in current) else None
        after = self.wait(progressed, label + '双端在线时间推进三秒')
        for role in ('A', 'B'):
            result = validate_income_interval(before[role][3], after[role][3], stopped)
            assert stopped or result['income'] > 0, '在线期间必须实际入账'
            self.emit('LIFECYCLE_INCOME_PASS', label=label, role=role, before=before[role][3], after=after[role][3],
                      result=result, health=after[role][1]['health'], inside=after[role][2]['inside'], controlled=True, natural_purchase=False)
        self.snapshot(label)
        return after

    def lifecycle_rejected(self, label):
        ui, before = self.interaction_state(); assert not ui['canDrag']
        self.disabled_click('Purchase_0', 'ShopList', label + '-Purchase')
        ui, before = self.interaction_state()
        self.pointer('A', choose_cell(ui, 0, type=7), choose_cell(ui, 1, slot=0))
        def unchanged():
            state = settled_ui(self.records(), 'A')
            if not state or state[0]['time'] <= ui['time'] + 1: return None
            assert all(state[1][k] == before[k] for k in ('expectedSeq', 'response', 'equipment', 'skills', 'items', 'bag'))
            return state
        self.wait(unchanged, label + '禁操作没有发请求')
        self.emit('LIFECYCLE_GATE_PASS', label=label, ui_rejection=True, server_rejection=False, controlled=True)
        self.snapshot(label)

    def lifecycle_actions(self):
        self.lifecycle_rejected('Life-Waiting')
        self.advance_manual()
        self.wait(lambda: (self.lifecycle_state() or ({},))[0].get('canDrag'), '首次准备可操作')
        self.lifecycle_observe('Life-Preparing')
        for inside, label in ((True, 'Life-Enter'), (False, 'Life-Leave'), (True, 'Life-Reenter')):
            ui, before = self.interaction_state(); self.pointer('A', ui['Training'])
            def region():
                state = self.lifecycle_state()
                if not state or state[0]['time'] <= ui['time'] or state[2]['inside'] is not inside: return None
                assert state[1]['health'] == before['health'] and state[1]['skills'] == before['skills']
                return state
            self.wait(region, '真实鼠标切换个人房状态'); self.lifecycle_observe(label)
        self.request_lifecycle('death')
        self.wait(lambda: (self.lifecycle_state() or ({}, {}))[1].get('health') == 0, 'controlled首位玩家死亡提交')
        self.lifecycle_rejected('Life-Dead')
        after = self.lifecycle_observe('Life-DeadIncome')
        assert after['A'][1]['health'] == 0 and after['B'][1]['health'] > 0
        self.request_lifecycle('ended')
        def ended():
            states = [self.lifecycle_state(r) for r in ('A', 'B')]
            return all(s and s[3]['phase'] == 3 and not s[0]['canDrag'] for s in states)
        self.wait(ended, '真实Ended双方停账')
        self.lifecycle_rejected('Life-Ended')
        self.lifecycle_observe('Life-EndedFrozen', stopped=True)

    def close_server(self):
        if (self.output != ROOT / 'Saved' / self.args.label
                or not re.fullmatch(r'R36-(?:S1-UI|S2-Visible|S2-Runtime|S3-Visible|S3-Presentation|S4)-[A-Za-z0-9_-]+', self.args.label)
                or not re.fullmatch(r'[0-9a-f]{32}', self.stop_token) or not int(self.stop_token, 16)):
            raise RuntimeError('停止请求路径或令牌不属于本次受控运行')
        with (self.output / 'stop.request').open('x', encoding='ascii') as out:
            out.write(self.stop_token)
        self.emit('SERVER_STOP_REQUESTED')

    def close_window(self, role):
        process = self.owned[role]
        self.n.p = process
        window = self.n.locate()
        if (not window or self.n.identity(window)['pid'] != process.pid
                or not same_process(self.identities[role], dict(pid=process.pid, created=self.created(process)))):
            raise RuntimeError('退出窗口不属于本次进程，拒绝发送WM_CLOSE')
        if not self.n.user.PostMessageW(window, 0x0010, 0, 0):
            raise RuntimeError('自有窗口拒绝WM_CLOSE')

    def close(self):
        errors = []
        for role in reversed(self.owned):
            try:
                process = self.owned[role]
                request = (lambda: self.close_window(role)) if role in ('A', 'B') else self.close_server if role == 'Server' else None
                code = close_owned(process, self.identities[role], self.created(process), request)
                self.exits.append(dict(role=role, pid=process.pid, code=code))
                self.emit('OWNED_EXIT', role=role, pid=process.pid, code=code)
                if code != 0: raise RuntimeError(f'{role}退出码非零：{code}')
            except BaseException:
                errors.append(traceback.format_exc())
        if errors: raise RuntimeError('\n'.join(errors))

    def finish(self, code):
        result = dict(Started=self.started, Finished=datetime.now(timezone.utc).isoformat(), ExitCode=code,
                      owned_remaining=[p.pid for p in self.owned.values() if p.poll() is None], exits=self.exits,
                      scope=getattr(self.args, 'scenario', 'controlled') + '：仅已执行断言；原PNG须另读，不代表S3/S4完成')
        (self.output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        self.emit('FINISHED', **result)


def parse_args(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--run', default='')
    parser.add_argument('--profile', choices=('content', 'evidence', 'compare'), default='content')
    parser.add_argument('--scenario', choices=('controlled', 'natural'), default='controlled')
    parser.add_argument('--port', type=int, default=18436)
    parser.add_argument('--items', type=int, default=17)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--observe-only', action='store_true')
    parser.add_argument('--scroll-all', action='store_true')
    parser.add_argument('--incremental', action='store_true')
    parser.add_argument('--timing', action='store_true')
    parser.add_argument('--lifecycle', action='store_true')
    parser.add_argument('--presentation', choices=('drop', 'disabled', 'region', 'death', 'growth'))
    parser.add_argument('--cooldown', action='store_true')
    parser.add_argument('--burst', action='store_true')
    parser.add_argument('--validate-assets', action='store_true')
    parser.add_argument('--content-check')
    args = parser.parse_args(argv)
    try:
        commands(args.label, args.port, args.run, args.items, args.profile, args.scenario)
        if args.timing and (not args.label.startswith('R36-S4-') or args.items != 500 or args.profile != 'evidence'
                or args.scenario != 'controlled' or not args.scroll_all or args.observe_only or args.validate_assets
                or args.incremental or args.burst or args.lifecycle or args.presentation or args.cooldown or args.content_check):
            raise ValueError('分段专项只接受独立S4、controlled500、evidence及全滚动；不限制普通容量')
        if args.content_check and (not args.label.startswith('R36-S4-') or args.scenario != 'natural'
                or args.profile != 'content' or args.observe_only or args.validate_assets
                or not re.fullmatch(r'R36-S4-[A-Za-z0-9_-]+', args.content_check)):
            raise ValueError('新增内容验收只用于S4自然Content和固定Saved安全清单')
        if args.presentation:
            if (not args.label.startswith('R36-S3-') or not args.run or args.items != 0 or args.observe_only
                    or args.validate_assets or args.burst or args.cooldown or args.scroll_all or args.incremental):
                raise ValueError('S3可见场只允许新Run、空包和正常输入，不混入旧驱动')
            if (args.presentation == 'growth' and args.profile != 'compare') or (args.presentation != 'growth' and args.profile == 'compare'):
                raise ValueError('成长使用无掉落Compare，掉落使用明确Baseline或controlled Content')
            if args.lifecycle != (args.presentation == 'death') or (args.presentation in ('death', 'disabled', 'region') and args.scenario != 'controlled'):
                raise ValueError('死亡需要显式生命周期授权；禁播及切区为controlled专场')
        if args.lifecycle and not args.presentation and (args.scenario != 'controlled' or args.profile != 'evidence' or not args.run
                or not args.label.startswith('R36-S2-') or args.items != 17 or args.observe_only or args.validate_assets
                or args.burst or args.cooldown or args.scroll_all or args.incremental):
            raise ValueError('生命周期需独立S2 Baseline、17件controlled夹具，不混自然购买或压力/CD场')
        if args.scenario == 'natural' and (args.burst or args.scroll_all or args.cooldown or args.incremental):
            raise ValueError('自然购买不能混入受控预置、压力或CD场')
        if args.incremental and (not args.label.startswith('R36-S2-') or not args.scroll_all or args.observe_only or args.cooldown or args.burst or not args.run):
            raise ValueError('增量验收需独立S2 Run、全滚动及真实交互')
        if args.timeout <= 0: raise ValueError('超时必须为正整数')
        if args.burst and (not args.observe_only or args.scroll_all or args.validate_assets or args.cooldown):
            raise ValueError('快速RPC验证需显式observe-only，与鼠标拖动、全量滚动、冷读和CD场分开')
        if args.cooldown and (not args.run or args.observe_only or args.validate_assets):
            raise ValueError('CD验证需要显式隔离Run和真实交互，不能使用固定入口或只读模式')
        if args.scroll_all and ((not args.observe_only and not args.incremental and not args.timing) or args.validate_assets):
            raise ValueError('全量滚动仅用于controlled初始包，需observe-only且不能是资产冷读')
    except ValueError as error: parser.error(str(error))
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.content_check: load_content_check(args.content_check, args.run)
    runner = Runner(); runner.args = args
    runner.output = ROOT / 'Saved' / args.label
    if runner.output.exists() or any((ROOT / 'Saved').glob(args.label + '-*.log')):
        raise RuntimeError('Label已存在，拒绝覆盖')
    runner.output.mkdir()
    runner.started = datetime.now(timezone.utc).isoformat()
    runner.owned, runner.identities, runner.exits = {}, {}, []
    spec = importlib.util.spec_from_file_location('r36_existing_helpers', Path(__file__).with_name('Test-Y01R34Visible.py'))
    runner.helpers = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner.helpers)
    runner.n = runner.helpers.skill.load_native(); runner.n.configure_native()
    runner.n.trace = (runner.output / 'native.log').open('x', encoding='utf-8', buffering=1)
    try: return run_session(runner)
    finally: runner.n.trace.close()


if __name__ == '__main__':
    raise SystemExit(main())
