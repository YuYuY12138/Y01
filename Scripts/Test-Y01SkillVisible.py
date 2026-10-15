"""R33U2双渲染端原生验收；只写Saved证据、只操作自建窗口，不生成或修改资产。"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import queue
import socket
import subprocess
import sys
import threading
import time
import traceback

ROOT = Path(__file__).resolve().parent.parent
ENGINE = Path(r'G:\UE_Engine\g01_engine\Engine\Binaries\Win64')
NUMBER = r'-?\d+(?:\.\d+)?'


def resolve_skill_map(selection):
    maps = {
        'u2baseline': '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Baseline/Maps/L_Y01Playground',
        'compare': '/Game/Developers/Y01PlaygroundTests/R33U2Compare/Maps/L_Y01Playground',
    }
    if selection not in maps:
        raise ValueError('固定25伤害验收禁止使用成长main；请显式 --map u2baseline（400速度）或 --map compare（200速度）。')
    return maps[selection]


def skill_map_speed(selection):
    resolve_skill_map(selection)
    return 400 if selection == 'u2baseline' else 200


def load_native():
    spec = importlib.util.spec_from_file_location('native_input', Path(__file__).with_name('Test-Y01PlaygroundInput.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_events(text):
    result = []
    for line in text.splitlines():
        match = re.search(r'^\[([\d.\-:]+)\].*Y01Skill (COMMIT|HIT|WALL|STATE|END|EXPIRE|STEP) (.+)', line)
        if not match:
            continue
        stamp = datetime.strptime(match[1], '%Y.%m.%d-%H.%M.%S:%f').replace(tzinfo=timezone.utc).timestamp()
        event = {'kind': match[2], 't': stamp, 'raw': line}
        for key, value in re.findall(r'(\w+)=([^\s]+)', match[3]):
            event[key] = float(value) if re.fullmatch(NUMBER, value) else value
        for key in ('Origin', 'Position'):
            vector = re.search(key + r'=X=(' + NUMBER + r') Y=(' + NUMBER + r') Z=(' + NUMBER + r')', match[3])
            if vector:
                event[key] = tuple(map(float, vector.groups()))
        result.append(event)
    return result


def parse_netguids(text):
    result = {}
    for name, path in re.findall(r'SerializeNewActor END: Finished Serializing\. Actor: (\w+), FullNetGUIDPath: (.+?), Channel:', text):
        guids = re.findall(r'\[(\d+)\]', path)
        if guids:
            guid = guids[-1]
            assert name not in result or result[name] == guid, 'Actor名复用，需按会话区分'
            result[name] = guid
    return result


def validate_readonly_command(value):
    for part in value.split('|'):
        assert re.fullmatch(r'\s*(?:getall\s+\w+\s+\w+|getalllocation\s+\w+)\s*', part), '仅允许只读getall/getalllocation'


def world(event, key='World'):
    value = event.get(key)
    assert isinstance(value, (int, float)) and math.isfinite(value), '缺少真实业务时钟：' + key
    return value


def validate_expiry(rows, commit, end, duration, phase):
    expires = [e for e in rows if e['kind'] == 'EXPIRE']
    assert len(expires) == 1 and rows.index(expires[0]) < rows.index(end), '自然结束必须先EXPIRE后END'
    expiry = expires[0]
    assert expiry['Phase'] == phase and expiry['Mode'] == commit['Mode'], '自然结束阶段不符'
    assert abs(world(commit) - world(commit, 'StartedAt')) < .00001
    assert abs(world(end, 'StartedAt') - world(commit, 'StartedAt')) < .00001
    assert abs(world(expiry, 'StartedAt') - world(commit, 'StartedAt')) < .00001
    assert abs(world(end, 'EndsAt') - world(expiry, 'EndsAt')) < .00001
    assert 0 <= world(expiry) - world(expiry, 'EndsAt') <= .12, '到期业务时钟超出单帧容差'
    assert abs(world(end) - world(expiry)) < .00001, '到期后没有立即销毁'
    assert abs(world(end, 'EndsAt') - duration) < .00001, '截止时间与配置窗期不符'


def validate_speed(events, actor, speed):
    rows = [e for e in events if e.get('Actor') == actor and e['kind'] == 'STEP' and e.get('Terminal') == 0]
    assert len(rows) >= 2, '缺少至少两个未碰撞未到期的完整STEP'
    samples = []
    for row in rows:
        delta = world(row, 'SampleWorld') - world(row, 'PrevWorld')
        assert delta > 0 and row['Mode'] == 2 and row['Phase'] == 0, '不是完整飞行步'
        assert abs(world(row) - world(row, 'SampleWorld')) < .00001
        distance = math.dist(tuple(row[k] for k in ('PrevX', 'PrevY', 'PrevZ')), row['Position'])
        measured = distance / delta
        assert abs(measured - speed) <= .1, f'实测速度{measured}不符{speed}'
        samples.append(dict(prev=row['PrevWorld'], sample=row['SampleWorld'], distance=distance, speed=measured))
    return dict(count=len(samples), mean_speed=sum(s['speed'] for s in samples) / len(samples),
                min_speed=min(s['speed'] for s in samples), max_speed=max(s['speed'] for s in samples), samples=samples)


def validate_cancellation(events, actor):
    rows = [e for e in events if e.get('Actor') == actor]
    commits = [e for e in rows if e['kind'] == 'COMMIT']
    ends = [e for e in rows if e['kind'] == 'END']
    assert len(commits) == len(ends) == 1, '取消必须有唯一提交和结束'
    commit, end = commits[0], ends[0]
    assert not any(e['kind'] in ('EXPIRE', 'HIT') for e in rows), '离区取消不得冒充自然到期或已经命中'
    assert world(commit) <= world(end) < world(commit, 'EndsAt'), '未在自然到期前取消'
    assert end['Phase'] == 0, '取消前已经进入命中反馈'
    return dict(actor=actor, seconds=world(end) - world(commit), commit_world=world(commit), end_world=world(end))


def validate_ended_attacks(events):
    commits = [e for e in events if e['kind'] == 'COMMIT']
    assert commits, '缺少曾经存在的真实攻击'
    for commit in commits:
        rows = [e for e in events if e.get('Actor') == commit['Actor']]
        ends = [e for e in rows if e['kind'] == 'END']
        assert len(ends) == 1, 'Ended后仍有未结束攻击'
        assert all(world(e) <= world(ends[0]) for e in rows if e['kind'] == 'HIT'), '结束后继续伤害'
    return dict(ended=len(commits))


def validate_projectile(events, actor, speed):
    rows = [e for e in events if e['Actor'] == actor]
    commits = [e for e in rows if e['kind'] == 'COMMIT']
    hits = [e for e in rows if e['kind'] == 'HIT']
    ends = [e for e in rows if e['kind'] == 'END']
    feedback = [e for e in rows if e['kind'] == 'STATE' and e['Phase'] == 1]
    assert len(commits) == len(hits) == len(ends) == 1, '球必须单提交、单命中、单结束'
    commit, hit, end = commits[0], hits[0], ends[0]
    assert commit['Mode'] == 2 and commit['Radius'] == 12 and commit['Speed'] == speed, '读盘球参数不符'
    assert world(hit) - world(commit) > .05, '此场景必须有真实飞行时间，不接受即时扣血'
    assert abs(world(commit, 'EndsAt') - world(commit, 'StartedAt') - 2.5) < .00001
    validate_expiry(rows, commit, end, world(hit) + .1, 1)
    assert len(feedback) == 1 and feedback[0]['Position'] == hit['Position'], '命中反馈必须停在命中点'
    assert .1 - .00001 <= world(end) - world(hit) <= .22, '反馈业务时长必须对应0.1秒（含一帧容差）'
    assert hit['Damage'] == commit['Damage'], '伤害必须等于Commit快照'
    return {'actor': actor, 'flight_seconds': world(hit) - world(commit), 'feedback_seconds': world(end) - world(hit),
            'commit_world': world(commit), 'hit_world': world(hit), 'end_world': world(end), 'deadline_world': world(end, 'EndsAt'),
            'target': hit['Target'], 'damage': hit['Damage'], 'hp_after': hit['HP'], 'commit': commit['t'], 'hit': hit['t'], 'end': end['t']}


def validate_area(events, actor):
    rows = [e for e in events if e['Actor'] == actor]
    commits = [e for e in rows if e['kind'] == 'COMMIT']
    ends = [e for e in rows if e['kind'] == 'END']
    hits = [e for e in rows if e['kind'] == 'HIT']
    assert len(commits) == len(ends) == 1, '范围必须提交并结束'
    commit, end = commits[0], ends[0]
    assert commit['Mode'] == 1 and commit['Radius'] == 250 and commit['Height'] == 120
    validate_expiry(rows, commit, end, world(commit, 'StartedAt') + .35, 0)
    assert abs(world(commit, 'EndsAt') - world(commit, 'StartedAt') - .35) < .00001
    assert .35 - .00001 <= world(end) - world(commit) <= .46, '完整范围业务窗口不是0.35秒'
    targets = [e['Target'] for e in hits]
    assert len(targets) == len(set(targets)), '同一范围重复扣血'
    assert all(world(commit) <= world(e) < world(commit, 'EndsAt') for e in hits), '业务窗口外伤害'
    assert all(e['Damage'] == commit['Damage'] for e in hits), '范围伤害必须等于Commit快照'
    return {'actor': actor, 'targets': targets, 'duration': world(end) - world(commit), 'commit': commit['t'], 'end': end['t'],
            'commit_world': world(commit), 'deadline_world': world(commit, 'EndsAt'), 'end_world': world(end)}


def validate_rejection(events, before, after):
    assert not any(e['kind'] in ('COMMIT', 'STATE', 'HIT') for e in events), '拒绝却产生假攻击/伤害'
    assert before == after, '拒绝不应消耗或刷新CD'


def validate_health(before, after, hits):
    changes = {}
    for hit in hits:
        name = hit['Target']
        assert name in before and name in after, '必须按同一怪物身份回读'
        changes[name] = changes.get(name, 0) + min(hit['Damage'], before[name])
        assert after[name] == hit['HP'], '命中日志与复制血量不符'
    for name, hp in before.items():
        assert name in after, '怪物缺少后血量，不能默认为死亡或伤害正确'
        assert abs(hp - after[name] - changes.get(name, 0)) < .001, '实际扣血与权威HIT不符或范围外目标受伤'


def validate_area_contrast(origin, before, after, hits):
    validate_health(before['health'], after['health'], hits)
    assert hits, '缺少范围内实际命中'
    inside, outside = [], []
    distances = {}
    for name, position in before['monsters'].items():
        assert name in after['monsters'], '缺少目标后坐标'
        following = after['monsters'][name]
        d0, d1 = math.dist(origin[:2], position[:2]), math.dist(origin[:2], following[:2])
        distances[name] = [d0, d1]
        if name in {hit['Target'] for hit in hits}:
            assert d0 <= 250 and abs(position[2] - origin[2]) <= 120, '前观测不能证实目标在范围内'
            inside.append(name)
        else:
            assert min(d0, d1) > 300, '范围外对照必须留50cm余量，不能把未命中的内部目标算外部'
            assert before['health'][name] == after['health'][name], '范围外目标受伤'
            outside.append(name)
    assert inside and outside, '本次必须同时有范围内/外目标'
    return dict(inside=inside, outside=outside, distances=distances, scope='前后位置及血量对照；连续移动路径由实际画面复核')


def validate_training_main_continuity(text, peer_text=None):
    states = []
    for log in ([text] if peer_text is None else [text, peer_text]):
        rows = re.findall(r'^\[([^\]]+)\].*Y01Flow Rep Phase=(\d+)[^\r\n]*', log, re.MULTILINE)
        telemetry = re.findall(r'Y01Training OBSERVE ([^\r\n]+)', log)
        assert rows and telemetry, '缺少最新复制Phase或真实玩家HP，不能沿用旧Snapshot'
        state = {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', telemetry[-1])}
        assert all(k in state for k in ('HP', 'Inside', 'Alive')), '遥测缺少死亡/清理依据'
        state.update(phase=int(rows[-1][1]), timestamp=rows[-1][0])
        states.append(state)
    dead = any(s['HP'] <= 0 for s in states)
    if dead:
        assert len(states) == 2, '全局死亡清理必须核对双端'
        assert all(s['phase'] == 3 and s['Inside'] == 0 and s['Alive'] == 0 for s in states), '死亡后必须Ended并清理双方训练'
    else:
        assert all(s['phase'] == 2 for s in states), '活着时必须Battle；旧Snapshot不能覆盖Ended'
    return dict(phase=states[0]['phase'], players=states, death=dead, scope='非Waiting任一玩家死亡沿用全局失败；不要求隔离或复活')


def validate_midflight_health(events, actor, text, target, hp_before):
    commits = [e for e in events if e.get('Actor') == actor and e['kind'] == 'COMMIT']
    hits = [e for e in events if e.get('Actor') == actor and e['kind'] == 'HIT']
    assert len(commits) == len(hits) == 1, '飞行中血量需要唯一提交/命中'
    pattern = r'^\[([\d.\-:]+)\].*PersistentLevel\.' + re.escape(target) + r'\.Attributes\.Health = .*CurrentValue=(' + NUMBER + r')'
    found = []
    for stamp, hp in re.findall(pattern, text, re.MULTILINE):
        timestamp = datetime.strptime(stamp, '%Y.%m.%d-%H.%M.%S:%f').replace(tzinfo=timezone.utc).timestamp()
        if commits[0]['t'] < timestamp < hits[0]['t']:
            found.append((timestamp, float(hp)))
    assert found and all(hp == hp_before for _, hp in found), '缺少生成后、首次命中前仍未扣血的同身份观测'
    return dict(target=target, t=found[-1][0], hp=found[-1][1], commit=commits[0]['t'], hit=hits[0]['t'])


def count_feedback_pixels(image):
    from PIL import ImageChops
    red, green, blue = image.convert('RGB').crop((458, 0, 920, 570)).split()
    high = lambda channel: channel.point([255 if value >= 180 else 0 for value in range(256)])
    low = lambda channel: channel.point([255 if value <= 100 else 0 for value in range(256)])
    def count(a, b, c):
        return ImageChops.multiply(ImageChops.multiply(a, b), c).histogram()[255]
    return dict(red=count(high(red), low(green), low(blue)), green=count(high(green), low(red), low(blue)))


def audit_frames(directory, pixels=False):
    native = load_native()
    count, series = 0, []
    for manifest in sorted(directory.rglob('*-frames.jsonl')):
        frames = [json.loads(line) for line in manifest.read_text(encoding='utf-8').splitlines() if line]
        assert frames, '空截图清单'
        pixel_frames = []
        for frame in frames:
            path = Path(frame['path'])
            assert path.resolve().is_relative_to(directory.resolve()), '截图不属于证据目录'
            assert path.is_file() and frame['verified'] and native.valid_png(path, frame['start_ns']), '截图缺失/非新鲜/损坏'
            stat = path.stat()
            assert stat.st_size == frame['size'] and stat.st_mtime_ns == frame['modified_ns'], '截图文件后续发生改变'
            assert stat.st_ctime_ns == frame['created_ns'] and frame['end_ns'] >= frame['start_ns'], '截图时间不一致'
            assert hashlib.sha256(path.read_bytes()).hexdigest() == frame['sha256'], '截图哈希不一致'
            count += 1
            if pixels:
                from PIL import Image
                with Image.open(path) as image:
                    pixel_frames.append(dict(path=str(path), start_ns=frame['start_ns'], end_ns=frame['end_ns'],
                                             sha256=frame['sha256'], **count_feedback_pixels(image)))
        gaps = [(b['start_ns'] - a['start_ns']) / 1e6 for a, b in zip(frames, frames[1:])]
        series.append(dict(manifest=str(manifest), count=len(frames), first=frames[0]['path'], last=frames[-1]['path'],
                           first_ns=frames[0]['start_ns'], last_ns=frames[-1]['end_ns'], max_gap_ms=max(gaps, default=0),
                           max_capture_ms=max((f['end_ns'] - f['start_ns']) / 1e6 for f in frames),
                           unique_hashes=len({f['sha256'] for f in frames})))
        if pixels:
            series[-1].update(pixel_frames=pixel_frames, red_frames=sum(f['red'] > 0 for f in pixel_frames),
                              green_frames=sum(f['green'] > 0 for f in pixel_frames),
                              best_red=max(pixel_frames, key=lambda f: f['red']), best_green=max(pixel_frames, key=lambda f: f['green']),
                              pixel_scope='原图中央世界ROI(458,0)-(920,570)阈值计数；排除HUD；不是全native或自动视觉通过')
    assert count, '没有连续截图证据'
    return dict(files=count, series=series, scope='只审计真实PNG完整性/哈希/时间，不自动宣称视觉通过')


def validate_peer(events, actor, feedback):
    rows = [e for e in events if e.get('Actor') == actor and e.get('NetMode') == 3]
    active = [e for e in rows if e['kind'] == 'STATE' and e['Phase'] == 0]
    hit = [e for e in rows if e['kind'] == 'STATE' and e['Phase'] == 1]
    end = [e for e in rows if e['kind'] == 'END']
    assert active and len(end) == 1 and active[0]['t'] <= end[0]['t'], '相关客户端没有完整生成/销毁复制'
    if feedback:
        assert hit and active[0]['t'] <= hit[0]['t'] <= end[0]['t'], '相关客户端缺少命中变色状态'


def validate_frame_series(frames, start_ns, end_ns):
    rows = sorted(frames, key=lambda f: f['start_ns'])
    assert rows and rows[0]['start_ns'] <= start_ns and rows[-1]['end_ns'] >= end_ns, '连续帧未覆盖攻击窗口'
    assert all(f['verified'] for f in rows), '存在损坏、非新鲜或未核验截图'
    assert len({f['sha256'] for f in rows}) > 1, '全程静止同一帧，不能证明瞬态'
    relevant = [f for f in rows if f['end_ns'] >= start_ns - 50_000_000 and f['start_ns'] <= end_ns + 50_000_000]
    assert len(relevant) >= 3, '攻击窗口采样不足'
    gaps = [b['start_ns'] - a['start_ns'] for a, b in zip(relevant, relevant[1:])]
    assert gaps and max(gaps) <= 80_000_000, '采样间隔超过80ms，不得声称捕获0.1秒反馈'
    assert all(f['end_ns'] - f['start_ns'] <= 80_000_000 for f in relevant), '截图调用本身跨越瞬态窗口'
    return {'count': len(relevant), 'max_gap_ms': max(gaps) / 1e6}


def boundary_ready(state):
    if state['phase'] != 'Battle' or not state['monsters'] or set(state['monsters']) != set(state['health']):
        return False
    distances = [math.dist(state['position'][:2], position[:2]) for position in state['monsters'].values()]
    return (any(distance <= 230 for distance in distances) and any(distance > 350 for distance in distances)
            and all(distance <= 230 or distance > 350 for distance in distances)
            and all(hp > 0 for hp in state['health'].values()))


def main_damage_ready(state):
    if (state['phase'] != 'Battle' or not state['ready'] or len(state['monsters']) != 3
            or set(state['monsters']) != set(state['health']) or any(hp != 60 for hp in state['health'].values())):
        return False
    target = state['monsters'].get(state['target'])
    return (target is not None and math.dist(state['position'][:2], target[:2]) <= 230
            and abs(target[2] - state['position'][2]) <= 120)


def main_approach_key(state):
    if (state['phase'] != 'Battle' or not state['ready'] or len(state['monsters']) != 3
            or set(state['monsters']) != set(state['health']) or any(hp != 60 for hp in state['health'].values())):
        return None
    target = state['monsters'].get(state['target'])
    if (target is None or abs(target[2] - state['position'][2]) > 120
            or math.dist(state['position'][:2], target[:2]) <= 230):
        return None
    dx, dy = (target[i] - state['position'][i] for i in (0, 1))
    return ('W' if dx > 0 else 'S') if abs(dx) >= abs(dy) else ('D' if dy > 0 else 'A')


def validate_main_damage_chain(before, area, projectile):
    a, b, c = before['health'], area['health'], projectile['health']
    assert len(a) == 3 and a.keys() == b.keys() == c.keys(), '主场景必须回读同一组三只怪，不能空放冒充通过'
    assert all(hp == 60 for hp in a.values()), '必须从固定60血开始'
    damaged = [name for name in a if b[name] != a[name]]
    assert damaged and all(b[name] == 35 for name in damaged), '范围必须实际造成固定25伤害'
    assert all(b[name] in (60, 35) for name in a), '范围血量异常'
    hit = [name for name in a if c[name] != b[name]]
    assert len(hit) == 1 and hit[0] in damaged and c[hit[0]] == 10, '球必须对同一范围受击目标形成60→35→10'
    return dict(target=hit[0], chain=[a[hit[0]], b[hit[0]], c[hit[0]]], damage=25, area_targets=damaged)


def parse_observer_clock(text, deadlines):
    observer = re.search(r'\.Observer = ([^\r\n]+)', text)
    assert observer, '缺少本次OwnerOnly Observer，不能沿用旧时间'
    row = observer[1]
    def number(name, default):
        match = re.search(r'\b' + name + '=('+ NUMBER + ')', row)
        return float(match[1]) if match else default
    sample = number('ServerSampleTime', -1.)
    ready = 'bReady=True' in row and math.isfinite(sample) and sample >= 0
    preparation = number('PreparationDeadline', 0.)
    training = number('NextBatchAt', 0.) if 'bInside=True' in row else 0.
    def remaining(deadline):
        return max(0., deadline - sample) if ready else None
    return dict(server_sample_time=sample, preparation_deadline=preparation,
                preparation_remaining=remaining(preparation), training_remaining=remaining(training),
                remaining=[remaining(deadline) for deadline in deadlines])


def validate_snapshot_cd(state, slot, commit):
    sample, deadline, remaining = state['server_sample_time'], state['deadlines'][slot], state['remaining'][slot]
    assert state['ready'] and math.isfinite(sample) and sample >= 0, '未就绪/未采样不能显示Ready或数值CD'
    assert sample >= world(commit), '新释放后的快照时间不能早于提交'
    assert remaining is not None and abs(remaining - max(0., deadline - sample)) < .00001, 'CD必须由同一快照Deadline-Sample计算'
    configured = (1.5, 2.)[slot]
    assert abs(deadline - world(commit) - configured) < .00001, '原参数CD或权威截止发生变化'
    assert 0 <= remaining <= configured + .00001, '本次新采样CD异常膨胀'
    return dict(sample=sample, deadline=deadline, remaining=remaining, configured=configured,
                source='OwnerOnly Observer；无新样本保持（非实时），不使用GS getter/本地墙钟')


def parse_observation(text):
    native = load_native()
    result = native.parse_snapshot(text)
    pawn = re.search(r'\.Pawn = [^\r\n]*\.(Y01PlaygroundCharacter_\d+)', text)
    assert pawn, '缺少本端Controller.Pawn，不能误用第一个远端角色坐标'
    locations = {}
    for name, x, y, z in re.findall(r'PersistentLevel\.((?:Y01PlaygroundCharacter|Y01Monster)_\d+) \((' + NUMBER + '), (' + NUMBER + '), (' + NUMBER + r')\)', text):
        locations[name] = (float(x), float(y), float(z))
    assert pawn[1] in locations, '本端Pawn坐标缺失'
    result.update(pawn=pawn[1], position=locations[pawn[1]], monsters={k: v for k, v in locations.items() if k.startswith('Y01Monster_')})
    result['x'], result['y'], result['z'] = result['position']
    result.update(parse_observer_clock(text, result['deadlines']))
    return result


class BitmapHeader(c.Structure):
    _fields_ = [('size', w.DWORD), ('width', w.LONG), ('height', w.LONG), ('planes', w.WORD),
                ('bits', w.WORD), ('compression', w.DWORD), ('image_size', w.DWORD), ('x', w.LONG),
                ('y', w.LONG), ('used', w.DWORD), ('important', w.DWORD)]


class Recorder:
    """PrintWindow真实渲染像素；采集时间与文件时间分开，不将日志帧号冒充渲染帧号。"""
    def __init__(self, runner, label):
        self.runner, self.label = runner, label
        self.stop_event = threading.Event()
        self.frames = {'A': [], 'B': []}
        self.errors = []
        self.sampled = {'A': 0, 'B': 0}
        self.directory = runner.output / label
        self.directory.mkdir(exist_ok=False)
        self.threads = [threading.Thread(target=self.work, args=(role,), daemon=True) for role in ('A', 'B')]

    def work(self, role):
        from PIL import Image
        n = self.runner.n
        user, gdi = n.user, c.WinDLL('gdi32', use_last_error=True)
        for name, arguments, restype in (
            ('GetDC', [w.HWND], w.HDC), ('ReleaseDC', [w.HWND, w.HDC], c.c_int),
            ('PrintWindow', [w.HWND, w.HDC, w.UINT], w.BOOL)):
            getattr(user, name).argtypes, getattr(user, name).restype = arguments, restype
        for name, arguments, restype in (
            ('CreateCompatibleDC', [w.HDC], w.HDC), ('CreateCompatibleBitmap', [w.HDC, c.c_int, c.c_int], w.HBITMAP),
            ('SelectObject', [w.HDC, w.HANDLE], w.HANDLE), ('DeleteObject', [w.HANDLE], w.BOOL),
            ('DeleteDC', [w.HDC], w.BOOL), ('GetDIBits', [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT], c.c_int)):
            getattr(gdi, name).argtypes, getattr(gdi, name).restype = arguments, restype
        hwnd = self.runner.windows[role]
        dc = user.GetDC(hwnd)
        memory = gdi.CreateCompatibleDC(dc)
        bitmap = gdi.CreateCompatibleBitmap(dc, 1280, 800)
        old = gdi.SelectObject(memory, bitmap)
        header = BitmapHeader(c.sizeof(BitmapHeader), 1280, -800, 1, 32, 0, 0, 0, 0, 0, 0)
        buffer = c.create_string_buffer(1280 * 800 * 4)
        pending = queue.Queue()
        def save():
            try:
                with (self.directory / (role + '-frames.jsonl')).open('x', encoding='utf-8', buffering=1) as manifest:
                    while True:
                        item = pending.get()
                        if item is None: break
                        index, begin, end, pixels = item
                        path = self.directory / f'{role}-{index:04d}.png'
                        image = Image.frombytes('RGB', (1280, 800), pixels, 'raw', 'BGRX')
                        with path.open('xb') as handle:
                            image.save(handle, format='PNG', compress_level=1)
                        stat = path.stat()
                        record = dict(path=str(path), role=role, hwnd=int(hwnd), pid=self.runner.owned[role].pid,
                                      start_ns=begin, end_ns=end, created_ns=stat.st_ctime_ns, modified_ns=stat.st_mtime_ns,
                                      size=stat.st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                      verified=n.valid_png(path, begin))
                        self.frames[role].append(record)
                        manifest.write(json.dumps(record, ensure_ascii=False) + '\n')
            except BaseException:
                self.errors.append(traceback.format_exc())
        saver = threading.Thread(target=save, daemon=True)
        saver.start()
        try:
                while not self.stop_event.is_set():
                    begin = time.time_ns()
                    self.runner.owned_identity(role)
                    if not user.PrintWindow(hwnd, memory, 3):
                        raise c.WinError(c.get_last_error())
                    gdi.SelectObject(memory, old)
                    if gdi.GetDIBits(memory, bitmap, 0, 800, buffer, c.byref(header), 0) != 800:
                        raise RuntimeError('窗体像素读取不完整')
                    gdi.SelectObject(memory, bitmap)
                    end = time.time_ns()
                    pending.put((self.sampled[role], begin, end, buffer.raw))
                    self.sampled[role] += 1
                    self.stop_event.wait(max(0, .025 - (time.time_ns() - begin) / 1e9))
        except BaseException:
            self.errors.append(traceback.format_exc())
        finally:
            pending.put(None)
            saver.join(timeout=30)
            if saver.is_alive(): self.errors.append('PNG写盘超时')
            gdi.SelectObject(memory, old)
            gdi.DeleteObject(bitmap)
            gdi.DeleteDC(memory)
            user.ReleaseDC(hwnd, dc)

    def start(self):
        for thread in self.threads: thread.start()
        self.runner.wait(lambda: all(self.frames.values()) or self.errors, '连续截图已开始')
        if self.errors: raise RuntimeError(str(self.errors))

    def stop(self):
        self.stop_event.set()
        for thread in self.threads: thread.join(timeout=35)
        if any(t.is_alive() for t in self.threads) or self.errors:
            raise RuntimeError('连续截图失败：' + str(self.errors))
        self.runner.emit('CAPTURE', label=self.label, counts={r: len(f) for r, f in self.frames.items()})


class Runner:
    def __init__(self, args):
        resolve_skill_map(args.map)
        self.args = args
        self.output = ROOT / 'Saved' / args.label
        self.output.mkdir(exist_ok=False)
        self.trace = (self.output / 'runner.jsonl').open('x', encoding='utf-8', buffering=1)
        self.n = load_native()
        self.n.configure_native()
        self.n.trace = (self.output / 'native.log').open('x', encoding='utf-8', buffering=1)
        self.owned, self.windows, self.logs = {}, {}, {}
        self.active = None
        self.concerns = []
        self.results = []
        self.records = {}

    def emit(self, kind, **data):
        record = dict(kind=kind, time_ns=time.time_ns(), **data)
        self.trace.write(json.dumps(record, ensure_ascii=False) + '\n')
        print(kind, json.dumps(data, ensure_ascii=False), flush=True)

    def text(self, role):
        path = self.logs[role]
        return path.read_text(encoding='utf-8', errors='replace') if path.exists() else ''

    def telemetry(self, role):
        rows = re.findall(r'Y01Training OBSERVE ([^\r\n]+)', self.text(role))
        return {k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', rows[-1])} if rows else {}

    def owned_identity(self, role):
        process, hwnd = self.owned[role], self.windows[role]
        if process.poll() is not None or not self.n.user.IsWindow(hwnd) or self.n.identity(hwnd)['pid'] != process.pid:
            raise RuntimeError('自建窗口HWND/PID已改变：' + role)

    def wait(self, predicate, description, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.active:
                self.n.focused('runner-wait:' + description)
            for role, process in self.owned.items():
                if process.poll() is not None:
                    raise RuntimeError(f'{role}提前退出 {process.returncode}')
            result = predicate()
            if result: return result
            time.sleep(.01)
        raise TimeoutError(description)

    def focus(self, role):
        if self.active:
            self.n.focused('before-explicit-owned-switch')
        self.owned_identity(role)
        self.n.p, self.n.window, self.n.log = self.owned[role], self.windows[role], self.logs[role]
        if self.active != role:
            self.n.activate()
            self.emit('OWNED_FOCUS_SWITCH', role=role, identity=self.n.identity(self.n.window))
        self.active = role

    def launch(self):
        selected = resolve_skill_map(self.args.map)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', self.args.port))
        assert (ROOT / 'Content' / (selected[6:] + '.umap')).is_file(), '持久地图不存在'
        for role in ('Server', 'A', 'B'):
            self.logs[role] = self.output / (role + '.log')
            if role == 'Server':
                binary, extra = 'UnrealEditor-Cmd.exe', [selected, '-server', '-nullrhi', '-Y01SkillTrace', f'-port={self.args.port}']
            else:
                binary, extra = 'UnrealEditor.exe', [f'127.0.0.1:{self.args.port}', '-game', '-windowed', '-ResX=1280', '-ResY=800',
                    '-ForceRes', '-d3d12', '-WinX=' + ('10' if role == 'A' else '1260'), '-WinY=40',
                    '-ExecCmds=t.MaxFPS 60,console.searchmode.legacy 1', '-Y01TrainingVerify']
            command = [str(ENGINE / binary), str(ROOT / 'Y01.uproject'), *extra, '-nop4', '-nosound', '-NoSplash',
                       '-NoScreenMessages', '-forcelogflush', '-LogCmds=LogNetPackageMap Log', '-abslog=' + str(self.logs[role])]
            self.owned[role] = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            self.emit('OWNED_START', role=role, pid=self.owned[role].pid, command=subprocess.list2cmdline(command))
            if role == 'Server': self.wait(lambda: 'listening on port' in self.text(role), 'DS监听', 70)
        def ready():
            for role in ('A', 'B'):
                self.n.p = self.owned[role]
                hwnd = self.n.locate()
                if not hwnd or self.telemetry(role).get('Inside') != 0 or self.telemetry(role).get('HP', 0) <= 0: return False
                self.windows[role] = hwnd
            return True
        self.wait(ready, '两渲染端窗口和遥测', 120)
        # 等两端异步首帧/窗口初始化结束，再进行一次显式激活；之后失焦仍立即停止。
        time.sleep(2)
        self.emit('PROVENANCE', input='玩法native OS keybd_event/mouse_event；只读控制台字符可posted', posted_input=self.args.fast_observe, injected=False,
                  read_only_console=True, training_verify='只读0.2秒遥测，不启用PlaygroundVerify',
                  client_console_search='console.searchmode.legacy=1仅本驱动自建客户端启动；不改游戏配置/普通启动器',
                  script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        self.focus('A')
        for role in ('A', 'B'):
            assert 'D3D12' in self.text(role) and '-nullrhi' not in self.text(role).lower(), '客户端必须真实渲染'

    def observe(self, role='A'):
        self.focus(role)
        offset = len(self.text(role))
        query = ('getall Y01PlaygroundController Pawn | getall Y01PlaygroundController Observer | '
                 'getall Y01AttributeSet Health | getalllocation Y01PlaygroundCharacter | '
                 'getalllocation Y01Monster | getall Y01GameState Snapshot')
        begin = time.time_ns()
        if self.args.fast_observe:
            validate_readonly_command(query)
            self.n.key(0xC0)
            # 首个原生g的Pressed解除Console字符捕获；余下只读字符定向投递，不发玩法RPC。
            self.n.type_text(query[0])
            self.n.hold(.03)
            for char in query[1:]:
                self.n.focused('posted-readonly-before')
                if not self.n.user.PostMessageW(self.n.window, 0x102, ord(char), 0):
                    raise c.WinError(c.get_last_error())
                self.n.focused('posted-readonly-after')
            self.n.key(13)
            self.emit('POSTED_READONLY', command=query, role=role)
        else:
            self.n.command(query)
        self.wait(lambda: '.Snapshot = ' in self.text(role)[offset:], '本次只读快照')
        fresh = self.text(role)[offset:]
        assert 'Command not recognized:' not in fresh, '控制台命令损坏'
        state = parse_observation(fresh)
        state.update(start_ns=begin, end_ns=time.time_ns(), telemetry=self.telemetry(role))
        self.emit('OBSERVE', role=role, state=state, offset=offset)
        return state

    def check(self, name, function):
        try:
            result = function()
            self.results.append(name)
            self.emit('ASSERT_PASS', name=name, result=result)
            return result
        except AssertionError as error:
            self.concerns.append(name + ': ' + str(error))
            self.emit('ASSERT_FAIL', name=name, error=str(error))
            return None

    def record(self, label, action, after=1):
        recorder = Recorder(self, label)
        self.records[label] = recorder
        recorder.start()
        try:
            self.n.hold(.12)
            self.emit('ACTION_BEGIN', label=label)
            action()
            self.n.hold(after)
            self.emit('ACTION_END', label=label)
        finally:
            recorder.stop()
        return recorder

    def moves(self):
        before = self.observe()
        for char, axis, sign in (('W', 'x', 1), ('S', 'x', -1), ('A', 'y', -1), ('D', 'y', 1)):
            self.record('Move-' + char, lambda ch=char: self.n.key(ord(ch), .3), .1)
            after = self.observe()
            def check():
                assert (after[axis] - before[axis]) * sign > 20, '没有对应方向实际位移'
                return {'before': before['position'], 'after': after['position']}
            self.check('native-' + char, check)
            before = after

    def click_point(self, x, y):
        self.n.focused('details-position-before')
        point = w.POINT(x, y)
        if not self.n.user.ClientToScreen(self.n.window, c.byref(point)) or not self.n.user.SetCursorPos(point.x, point.y):
            raise c.WinError(c.get_last_error())
        for flag in (0x0002, 0x0004):
            self.n.focused('details-click-before')
            self.n.user.mouse_event(flag, 0, 0, 0, 0)
            self.n.focused('details-click-after')
            if flag == 0x0002: self.n.hold(.07)

    def toggle_details(self):
        self.click_point(1092, 183)
        self.emit('NATIVE_DETAILS_TOGGLE', client_xy=[1092, 183])

    def hide_hud(self):
        for role in ('B', 'A'):
            self.focus(role)
            self.n.command('showhud')
        self.emit('AUXILIARY_VIEW', change='原生只读视图showhud隐藏HUD，未改变玩法/速度/资产；与普通HUD运行分列')

    def inspect_attack(self, label, before, after, mode):
        events = parse_events(self.text('Server'))
        rows = [e for e in events if e['kind'] == 'COMMIT' and e['t'] * 1e9 >= before['end_ns'] and e['t'] * 1e9 <= after['end_ns'] and e['Mode'] == mode]
        def one():
            assert len(rows) == 1, f'连续两次原生按键（第二次CD拒绝）却有{len(rows)}个实例'
        self.check(label + '-one-commit', one)
        if not rows: return
        actor = rows[0]['Actor']
        result = self.check(label + '-authority', lambda: validate_projectile(events, actor, skill_map_speed(self.args.map))
                            if mode == 2 else validate_area(events, actor))
        if mode == 2:
            self.check(label + '-actual-speed', lambda: validate_speed(events, actor, skill_map_speed(self.args.map)))
        hits = [e for e in events if e['kind'] == 'HIT' and e['Actor'] == actor]
        server_guids = parse_netguids(self.text('Server'))
        local_guids = parse_netguids(self.text('A'))
        def hp_check():
            translated = []
            for hit in hits:
                guid = server_guids.get(hit['Target'])
                names = [name for name, value in local_guids.items() if value == guid]
                assert guid and len(names) == 1, '缺少NetGUID身份对照，不得按动态Actor后缀猜测'
                translated.append(dict(hit, Target=names[0]))
            validate_health(before['health'], after['health'], translated)
            return translated
        translated = self.check(label + '-hp', hp_check)
        if self.args.scenario == 'boundary' and translated is not None:
            if mode == 1:
                self.check(label + '-inside-outside', lambda: validate_area_contrast(rows[0]['Origin'], before, after, translated))
            elif len(translated) == 1:
                target = translated[0]['Target']
                self.check(label + '-midflight-hp', lambda: validate_midflight_health(events, actor, self.text('A'), target, before['health'][target]))
        slot = mode - 1
        def cd():
            assert after['deadlines'][slot] > before['deadlines'][slot]
            assert after['deadlines'][1 - slot] == before['deadlines'][1 - slot]
            return {'before': before['deadlines'], 'after': after['deadlines']}
        self.check(label + '-cd', cd)
        self.check(label + '-snapshot-cd', lambda: validate_snapshot_cd(after, slot, rows[0]))
        for role in ('A', 'B'):
            peer_guids = parse_netguids(self.text(role))
            names = [name for name, guid in peer_guids.items() if guid == server_guids.get(actor)]
            peer_actor = names[0] if len(names) == 1 else 'MISSING-NETGUID'
            self.emit('NETGUID_EVIDENCE', label=label, role=role, server_actor=actor, client_actor=peer_actor, guid=server_guids.get(actor))
            self.check(label + '-peer-' + role, lambda r=role, a=peer_actor: validate_peer(parse_events(self.text(r)), a, mode == 2))
            if result:
                self.check(label + '-frames-' + role, lambda r=role: validate_frame_series(self.records[label].frames[r],
                           int(result['commit'] * 1e9) - 30_000_000, int(result['end'] * 1e9) + 100_000_000))
        self.emit('ATTACK_EVIDENCE', label=label, actor=actor, before=before, after=after, hits=hits, result=result)

    def attack_keys(self, mode):
        self.n.key(ord('0') + mode)
        if mode == 2 and self.args.scenario == 'boundary':
            self.n.command('getall Y01AttributeSet Health')
        self.n.hold(.04)
        self.n.key(ord('0') + mode)

    def attack_and_sample(self, mode):
        self.attack_keys(mode)
        self.n.hold(1.1 if mode == 2 else .5)
        return self.training_observe()

    def attack(self, label, mode, before=None):
        before = before if before is not None else self.observe()
        if mode == 2 and self.args.scenario == 'boundary':
            self.record('Projectile-Native-Retreat', lambda: self.n.key(ord('W'), .8), .05)
        sampled = []
        self.record(label, lambda: sampled.append(self.attack_and_sample(mode)), .3)
        after = self.observe()
        self.inspect_attack(label, before, after, mode)
        def live_cd():
            rows = [e for e in parse_events(self.text('Server')) if e['kind'] == 'COMMIT' and e['Mode'] == mode
                    and before['end_ns'] <= e['t'] * 1e9 <= after['end_ns']]
            assert len(rows) == 1
            result = validate_snapshot_cd(sampled[0], mode - 1, rows[0])
            assert result['remaining'] > 0, '本次应捕获非零快照CD，不用落盘后的Ready冒充'
            return result
        self.check(label + '-live-snapshot-cd', live_cd)
        return after

    def main_scenario(self):
        self.record('Initial-HUD', lambda: None, .3)
        self.record('HUD-Expand-A', self.toggle_details, .2)
        self.record('HUD-Collapse-A', self.toggle_details, .2)
        self.focus('B')
        self.record('HUD-Expand-B', self.toggle_details, .2)
        self.focus('A')
        self.emit('HUD_VIEWS', A='默认收起', B='原生按钮展开；两端同一攻击同时取证', hidden=False)
        before = self.observe()
        assert before['phase'] == 'Waiting' and before['ready']
        if self.args.scenario == 'main': self.moves()
        else:
            self.record('Native-Position-W', lambda: self.n.key(ord('W'), 1.6 if self.args.scenario == 'boundary' else .9), .1)
        before = self.observe()
        start = time.time()
        self.record('Waiting-Reject', lambda: (self.n.key(ord('1')), self.n.key(ord('2'))), .4)
        after = self.observe()
        self.check('waiting-rejection', lambda: validate_rejection([e for e in parse_events(self.text('Server')) if e['t'] >= start], before['deadlines'], after['deadlines']))
        if self.args.hide_hud: self.hide_hud()
        self.n.key(13)
        before = self.wait(lambda: (lambda s: s if s['phase'] == 'Battle' else None)(self.observe()), '原生Enter到Battle', 15)
        if self.args.scenario == 'boundary':
            before = self.wait(lambda: (lambda s: s if boundary_ready(s) else None)(self.observe()), '真实范围内外目标复制齐且就位', 6)
        elif self.args.scenario == 'main':
            steps = 0
            def ready():
                nonlocal steps
                state = self.observe()
                if main_damage_ready(state): return state
                key = main_approach_key(state)
                if key:
                    self.emit('MAIN_NATIVE_APPROACH', key=key, seconds=.2, before=state)
                    self.record('Main-Approach-' + str(steps), lambda: self.n.key(ord(key), .2), .1)
                    steps += 1
                return None
            before = self.wait(ready, '三只60血复制齐且原生短步靠近至范围内', 10)
        after = self.attack('Area', 1, before)
        projectile = self.attack('Projectile', 2, after)
        if self.args.scenario == 'main':
            self.check('main-fixed-25-60-35-10', lambda: validate_main_damage_chain(before, after, projectile))

    def phase(self):
        self.focus('A')
        offset = len(self.text('A'))
        self.n.command('getall Y01GameState Snapshot')
        self.wait(lambda: '.Snapshot = ' in self.text('A')[offset:], '单条Phase回读')
        row = self.text('A')[offset:]
        phase = re.search(r'\.Snapshot = \([^\r\n]*?Phase=(\w+)', row)
        assert phase, '缺少本次Phase'
        self.emit('PHASE', phase=phase[1], raw=row)
        return phase[1]

    def cast_and_leave(self, mode):
        self.n.key(ord('0') + mode)
        self.n.hold(.1)
        self.n.click_training(0)

    def training_observe(self, role='A'):
        self.focus(role)
        offset = len(self.text(role))
        begin = time.time_ns()
        self.n.command('getall Y01PlaygroundController Observer')
        self.wait(lambda: '.Observer = ' in self.text(role)[offset:], '训练本次CD回读')
        row = re.findall(r'\.Observer = ([^\r\n]+)', self.text(role)[offset:])[-1]
        deadlines = []
        for name in ('BasicAttack', 'HeavyAttack'):
            slot = re.search(r'\(Row="' + name + r'"([^)]*)\)', row)
            assert slot, '技能槽缺失'
            value = re.search(r'Deadline=([\d.]+)', slot[1])
            deadlines.append(float(value[1]) if value else 0.)
        result = dict(ready='bReady=True' in row, inside='bInside=True' in row, deadlines=deadlines,
                      telemetry=self.telemetry(role), start_ns=begin, end_ns=time.time_ns())
        result.update(parse_observer_clock('.Observer = ' + row, deadlines))
        self.emit('TRAINING_OBSERVE', role=role, state=result)
        return result

    def training_scenario(self):
        self.focus('A')
        self.n.key(13)
        self.emit('NATIVE_MAIN_STARTED', purpose='先开始准备倒计时，避免长查询等待中训练玩家先被怪击杀')
        self.focus('B')
        self.n.click_training(1)
        b = self.training_observe('B')
        self.focus('A')
        self.n.click_training(1)
        before = self.training_observe()
        def rooms():
            assert before['inside'] and b['inside'] and before['telemetry']['Room'] != b['telemetry']['Room']
        self.check('independent-rooms', rooms)
        for mode in (2, 1):
            if mode == 1:
                self.n.click_training(1)
                before = self.training_observe()
            start = time.time()
            def cast_leave():
                if mode == 1:
                    self.emit('NATIVE_RETREAT', key='S', seconds=.9, purpose='空放范围前拉开距离；连续采集期间不等待PNG保存')
                    self.n.key(ord('S'), .9)
                self.cast_and_leave(mode)
            self.record('Training-Cancel-' + str(mode), cast_leave, .35)
            after = self.training_observe()
            events = [e for e in parse_events(self.text('Server')) if e['t'] >= start]
            def cancellation():
                commits = [e for e in events if e['kind'] == 'COMMIT' and e['Mode'] == mode]
                assert len(commits) == 1, '没有实际释放实例，不能冒充离区清理'
                actor = commits[0]['Actor']
                timing = validate_cancellation(events, actor)
                assert not after['inside'] and after['telemetry']['Alive'] == 0
                assert after['deadlines'][mode - 1] > before['deadlines'][mode - 1], '清理不得退CD'
                for name in ('Gold', 'XP', 'Kills'):
                    assert before['telemetry'][name] == after['telemetry'][name], '清理不应发奖励'
                assert self.telemetry('B')['Inside'] == 1 and self.telemetry('B')['Alive'] >= b['telemetry']['Alive']
                server_guids = parse_netguids(self.text('Server'))
                for role in ('A', 'B'):
                    names = [name for name, guid in parse_netguids(self.text(role)).items() if guid == server_guids.get(actor)]
                    assert len(names) == 1, '清理前攻击没有复制到相关端'
                    validate_peer(parse_events(self.text(role)), names[0], False)
                return dict(timing, before=before, after=after, peer=self.telemetry('B'))
            self.check('training-cancel-' + str(mode), cancellation)
        self.wait(lambda: 'Y01Flow Rep Phase=2' in self.text('A'), '活着期间主线进入Battle', 15)
        def alive_battle():
            state = validate_training_main_continuity(self.text('A'), self.text('B'))
            assert not state['death'] and self.telemetry('B')['Inside'] == 1
            return state
        self.check('main-not-blocked-by-training', alive_battle)
        self.record('Main-Battle-B-Training', lambda: self.n.key(ord('1')), .5)
        self.wait(lambda: min(self.telemetry(r).get('HP', 100) for r in ('A', 'B')) <= 25, '自然受击到低HP，不注入死亡', 45)
        self.record('Natural-Death-Ended', lambda: self.wait(
            lambda: all(self.telemetry(r).get('Inside') == 0 and self.telemetry(r).get('Alive') == 0
                        and 'Y01Flow Rep Phase=3' in self.text(r) for r in ('A', 'B')), '自然死亡与双端Ended清理', 20), .4)
        self.check('training-main-continuity', lambda: validate_training_main_continuity(self.text('A'), self.text('B')))
        self.check('ended-no-attacks', lambda: validate_ended_attacks(parse_events(self.text('Server'))))
        before_end = self.training_observe()
        start = time.time()
        self.record('Ended-Reject', lambda: (self.n.key(ord('1')), self.n.key(ord('2'))), .3)
        after_end = self.training_observe()
        self.check('ended-rejection', lambda: validate_rejection([e for e in parse_events(self.text('Server')) if e['t'] >= start],
                   before_end['deadlines'], after_end['deadlines']))

    @staticmethod
    def require(condition):
        assert condition

    def cleanup(self):
        # 关闭消息仅发给身份仍匹配的自建窗口；失焦后不会发送任何玩法输入。
        for role in ('A', 'B'):
            if role in self.windows and self.owned[role].poll() is None:
                hwnd = self.windows[role]
                if self.n.user.IsWindow(hwnd) and self.n.identity(hwnd)['pid'] == self.owned[role].pid:
                    self.n.user.PostMessageW(hwnd, 0x10, 0, 0)
        for role, process in reversed(list(self.owned.items())):
            forced = False
            try: process.wait(timeout=10 if role != 'Server' else 1)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                process.wait(timeout=10)
            self.emit('OWNED_EXIT', role=role, pid=process.pid, code=process.returncode, external_stop=forced)
        self.n.trace.close()
        self.n.trace = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', choices=('main', 'compare', 'u2baseline'), default='main', help='固定25验收必须显式u2baseline或compare；main只保留离线证据审计')
    parser.add_argument('--scenario', choices=('main', 'skills', 'boundary', 'training'), default='main')
    parser.add_argument('--audit-only', action='store_true', help='仅重新核验label目录的连续PNG，绝不启动UE或发送输入')
    parser.add_argument('--audit-pixels', action='store_true', help='仅配合audit-only统计原图中央世界红/绿像素；不替代人工看图')
    parser.add_argument('--hide-hud', action='store_true', help='另列辅助观测：仅隐藏HUD防止遮挡技能')
    parser.add_argument('--fast-observe', action='store_true', help='仅只读控制台字符posted；玩法1/2/WASD/按钮仍原生OS')
    parser.add_argument('--port', type=int, default=18332)
    parser.add_argument('--label', default='R33-U2-Visible-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label): parser.error('label必须是ASCII安全名称')
    if not 1024 <= args.port <= 65535: parser.error('port不合法')
    if args.audit_pixels and not args.audit_only: parser.error('audit-pixels只允许离线审计')
    if args.audit_only:
        directory = ROOT / 'Saved' / args.label
        result = audit_frames(directory, pixels=args.audit_pixels)
        with (directory / ('audit-pixels.json' if args.audit_pixels else 'audit.json')).open('x', encoding='utf-8') as handle:
            json.dump(result, handle, ensure_ascii=False, indent=2)
        summary = dict(result, series=[{key: value for key, value in row.items() if key != 'pixel_frames'} for row in result['series']])
        print(json.dumps(summary, ensure_ascii=False))
        return 0
    try:
        resolve_skill_map(args.map)
    except ValueError as error:
        parser.error(str(error))
    runner = Runner(args)
    code = 1
    try:
        runner.launch()
        if args.scenario != 'training': runner.main_scenario()
        else: runner.training_scenario()
        code = 2 if runner.concerns else 0
    except BaseException:
        runner.emit('RUN_FAILED', error=traceback.format_exc())
    finally:
        try: runner.cleanup()
        except BaseException:
            code = 1
            runner.emit('CLEANUP_FAILED', error=traceback.format_exc())
        runner.emit('RUNNER_EXIT', code=code, concerns=runner.concerns, assertions=runner.results,
                    scope='仅所执行场景；未运行边界与未经人工看图的视觉结论不能自动PASS')
        runner.trace.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
