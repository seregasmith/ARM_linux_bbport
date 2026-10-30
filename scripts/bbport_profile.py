#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""A performance profile of the running game (Linux): what each of its threads costs, the cores' and the GPU's load and
clocks, temperatures, battery power, memory, and the frame rate from the game's log. Made to compare the guest CPUs
(FEXCore and the native CPU, docs/AOT_LAUNCHER_PLAN.md) and settings at the same spot. The launcher's Developer ->
"Record a performance profile" runs it for every game start. It only reads /proc and /sys.

    python3 scripts/bbport_profile.py record (--session SID | --pid PID | --name bb-probe) --out DIR
                                             [--game-log FILE] [--interval 1] [--seconds N]
    python3 scripts/bbport_profile.py summary DIR [--skip SECONDS]       (summary.txt again, e.g. without the loading)
    python3 scripts/bbport_profile.py compare DIR_A DIR_B [--skip SECONDS] (side by side: FPS, CPU, GPU, power)

record: --session follows the game the launcher started (its process group's session: the bb-probe process in it), --pid
one process, --name the newest process of that name (a game started some other way). --game-log is the game's output
(the launcher writes it next to the profile; run.sh's BB_SAVE_LOG writes <data>/logs/<time>.log): it gives the frame rate
("Present rate:" with BB_FPS_PRINT=1, "Frame stats:" with BB_FRAME_STATS=1), the game's own thread names ("guest thread
'...' created": they tell the game's threads from the port's) and which guest CPU ran. It stops when the game exits, when
--seconds are up, or on SIGINT/SIGTERM, and then writes the summary.

DIR gets: samples.csv (one row per interval: the process's CPU, the game's and the port's threads, each core's load and
clock, the GPU's load and clock, temperatures, battery power, memory, FPS), threads.csv (per interval, each thread that
used CPU), facts.json (what the log said), summary.txt and summary.json.

CPU is in percent of one core (400 = four cores busy), as top shows it. GPU load is the game's GPU time from the DRM
driver's fdinfo (drm-engine-*: msm, amdgpu, i915/xe, panfrost), else kgsl's gpu_busy_percentage.
"""
import argparse
import csv
import json
import os
import re
import signal
import sys
import time
from pathlib import Path

CLK = os.sysconf('SC_CLK_TCK')
CORES = os.cpu_count() or 1
FPS_LINE = re.compile(r'^(?:Frame stats|Present rate): ([0-9.]+) FPS')
GUEST_THREAD = re.compile(r"guest thread '(.*)' created")
# BB_AOT_STATS=1 (the native CPU): "    <thread>  host 12.3%, 456 unit entries" per thread every BB_AOT_STATS_SEC.
AOT_HOST = re.compile(r'^(\S.{0,14}?)\s+host\s+([0-9.]+)%, \d+ unit entries$')
FACT_LINES = (
    ('guest_cpu', re.compile(r'^Guest CPU: (.*?)(?:, TSC.*)?$')),
    ('native_cpu', re.compile(r'^Native CPU: (.*)$')),
    ('gpu', re.compile(r'^GPU: (.*?\(\w+\)[^;]*)')),
    ('output', re.compile(r'^Output (\d+x\d+): scene (\d+x\d+)')),
    ('fps_preset', re.compile(r'^Patches: FPS preset (\S+);')),
    ('frame_limit', re.compile(r'^VideoOut: vblank .*?frame limit (\d+) FPS')),
)
ENV_FACTS = ('BB_FPS', 'BB_FPS_LIMIT', 'BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AOT', 'BB_DRAW_PIPE', 'BB_PRESENT_MODE')
SAMPLE_COLUMNS = ['t', 'pid', 'process_cpu', 'main_cpu', 'game_cpu', 'port_cpu', 'threads', 'busiest', 'busiest_cpu',
                  'cores_busy', 'core_busy_max', 'cpu_mhz_max', 'gpu_busy', 'gpu_mhz', 'temp_max', 'temp_cpu', 'temp_gpu',
                  'power_w', 'battery', 'mem_avail_mb', 'rss_mb', 'fps']


def read_text(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')
    except OSError:
        return None


def parse_stat(text):
    """(name, the fields after it: rest[0] is field 3, state) of a /proc stat line, or None."""
    if not text:
        return None
    lp, rp = text.find('('), text.rfind(')')
    if lp < 0 or rp < lp:
        return None
    return text[lp + 1:rp], text[rp + 2:].split()


def stat_ticks(rest):
    return int(rest[11]) + int(rest[12])  # utime, stime (fields 14, 15)


def processes():
    """(pid, name, session, start time) of every live process (not the zombies: a parent has yet to reap them)."""
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        parsed = parse_stat(read_text(f'/proc/{entry}/stat'))
        if parsed and len(parsed[1]) > 19 and parsed[1][0] not in 'ZX':
            name, rest = parsed
            yield int(entry), name, int(rest[3]), int(rest[19])


def alive(pid):
    parsed = parse_stat(read_text(f'/proc/{pid}/stat'))
    return bool(parsed) and parsed[1][0] not in 'ZX'


class Target:
    """The process to measure: --pid, the bb-probe process of --session, or the newest --name."""

    def __init__(self, pid=None, session=None, name=None):
        self.fixed, self.session, self.name = pid, session, name
        self.current = None

    def resolve(self):
        """The pid now; None while it has not started (a session whose game is not there yet); -1 once it is gone."""
        if self.current is not None:
            if alive(self.current):
                return self.current
            if self.fixed is not None or self.name:
                return -1
            self.current = None  # (a session: the game may come back, run.sh restarts it in place)
        if self.fixed is not None:
            self.current = self.fixed if alive(self.fixed) else None
            return self.current if self.current is not None else -1
        uid = os.getuid()
        found = []
        session_alive = False
        for pid, name, session, start in processes():
            if self.session is not None and session == self.session:
                session_alive = True
                if name == 'bb-probe':
                    found.append((start, pid))
            elif self.name and name == self.name:
                try:
                    if os.stat(f'/proc/{pid}').st_uid == uid:
                        found.append((start, pid))
                except OSError:
                    pass
        if found:
            self.current = max(found)[1]
            return self.current
        if self.session is not None and not session_alive:
            return -1
        return None


class GameLog:
    """The game's output, read as it grows: FPS lines, the game's thread names, facts."""

    def __init__(self, path):
        self.path = Path(path) if path else None
        self.pos = 0
        self.rest = ''
        self.fps = []          # (t, fps)
        self.guest = set()     # the game's thread names as the kernel shows them (15 bytes)
        self.facts = {}
        self.aot_host = {}     # thread name -> [host %] (BB_AOT_STATS)

    def poll(self, t):
        if not self.path:
            return
        try:
            with open(self.path, 'rb') as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return
        if not data:
            return
        self.pos += len(data)
        text = self.rest + data.decode('utf-8', 'replace')
        lines = text.split('\n')
        self.rest = lines.pop()
        for line in lines:
            self.line(line.strip(), t)

    def line(self, line, t):
        m = FPS_LINE.match(line)
        if m:
            self.fps.append((round(t, 1), float(m.group(1))))
            return
        m = AOT_HOST.match(line)
        if m:
            self.aot_host.setdefault(m.group(1).strip(), []).append(float(m.group(2)))
            return
        m = GUEST_THREAD.search(line)
        if m:
            self.guest.add(m.group(1).encode()[:15].decode('utf-8', 'ignore'))
            return
        for key, pattern in FACT_LINES:
            if key not in self.facts:
                m = pattern.match(line)
                if m:
                    self.facts[key] = ' '.join(m.groups())
        for key in ENV_FACTS:
            if key not in self.facts:
                m = re.search(r'(?:^|\s)' + key + r'=(\S+)', line)
                if m:
                    self.facts[key] = m.group(1)

    def fps_at(self, t):
        """The latest FPS reading of the last 6 s (the game prints one every 5 s)."""
        return self.fps[-1][1] if self.fps and t - self.fps[-1][0] <= 6.5 else None


class Gpu:
    """The game's GPU time (DRM fdinfo) and clock; kgsl's load where there is no fdinfo."""

    def __init__(self):
        self.fds, self.scanned, self.pid = [], 0.0, None
        self.devfreq = [d for d in Path('/sys/class/devfreq').glob('*')
                        if any(k in d.name.lower() for k in ('gpu', 'kgsl', 'mali', 'adreno'))] \
            if Path('/sys/class/devfreq').is_dir() else []
        kgsl = Path('/sys/class/kgsl/kgsl-3d0')
        self.kgsl = kgsl if kgsl.is_dir() else None

    def drm_fds(self, pid, now):
        if pid != self.pid or now - self.scanned > 10:
            self.pid, self.scanned, self.fds = pid, now, []
            try:
                for fd in os.listdir(f'/proc/{pid}/fd'):
                    try:
                        if os.readlink(f'/proc/{pid}/fd/{fd}').startswith('/dev/dri/'):
                            self.fds.append(fd)
                    except OSError:
                        pass
            except OSError:
                pass
        return self.fds

    def sample(self, pid, now):
        """{'engines': {name: ns}, 'mhz': clock or None, 'kgsl_busy': % or None}"""
        clients, mhz = {}, None
        for fd in self.drm_fds(pid, now):
            text = read_text(f'/proc/{pid}/fdinfo/{fd}')
            if not text or 'drm-driver:' not in text:
                continue
            client, engines = fd, {}
            for line in text.splitlines():
                key, _, value = line.partition(':')
                value = value.strip()
                if key == 'drm-client-id':
                    client = value
                elif key.startswith('drm-engine-') and value.endswith('ns'):
                    try:
                        engines[key[len('drm-engine-'):]] = int(value.split()[0])
                    except ValueError:
                        pass
                elif key.startswith('drm-curfreq-') and value.endswith('Hz'):
                    try:
                        mhz = int(value.split()[0]) / 1e6
                    except ValueError:
                        pass
            clients[client] = engines  # one client's fds share its counters
        engines = {}
        for per in clients.values():
            for name, ns in per.items():
                engines[name] = engines.get(name, 0) + ns
        if mhz is None:
            for d in self.devfreq:
                v = read_text(d / 'cur_freq')
                if v and v.strip().isdigit():
                    mhz = int(v) / 1e6
                    break
        kgsl_busy = None
        if self.kgsl:
            v = read_text(self.kgsl / 'gpu_busy_percentage')
            if v:
                try:
                    kgsl_busy = float(v.split()[0].rstrip('%'))
                except ValueError:
                    pass
            if mhz is None:
                v = read_text(self.kgsl / 'gpuclk')
                if v and v.strip().isdigit():
                    mhz = int(v) / 1e6
        return {'engines': engines, 'mhz': mhz, 'kgsl_busy': kgsl_busy}


def thermal():
    """(max of all zones, of the CPU's, of the GPU's) in °C."""
    every, cpu, gpu = [], [], []
    for zone in Path('/sys/class/thermal').glob('thermal_zone*'):
        kind, temp = read_text(zone / 'type'), read_text(zone / 'temp')
        try:
            c = int(temp) / 1000.0
        except (TypeError, ValueError):
            continue
        if not -40 < c < 150:
            continue
        kind = (kind or '').strip().lower()
        every.append(c)
        if 'cpu' in kind:
            cpu.append(c)
        if 'gpu' in kind:
            gpu.append(c)
    top = lambda v: round(max(v), 1) if v else None  # noqa: E731
    return top(every), top(cpu), top(gpu)


def battery():
    """(watts, status) of the first battery, or (None, '')."""
    for supply in Path('/sys/class/power_supply').glob('*'):
        if (read_text(supply / 'type') or '').strip() != 'Battery':
            continue
        status = (read_text(supply / 'status') or '').strip()
        try:
            power = read_text(supply / 'power_now')
            if power is not None and power.strip():
                return abs(int(power)) / 1e6, status
            current, voltage = read_text(supply / 'current_now'), read_text(supply / 'voltage_now')
            if current is not None and voltage is not None:
                return abs(int(current)) * int(voltage) / 1e12, status
        except ValueError:
            pass
        return None, status
    return None, ''


def battery_raw():
    """The first battery's raw counters (the units are the driver's: µWh, µAh, µV, µW, µA, %), to check power_now against
    the energy the session used."""
    for supply in Path('/sys/class/power_supply').glob('*'):
        if (read_text(supply / 'type') or '').strip() != 'Battery':
            continue
        out = {'name': supply.name, 'when': time.monotonic()}
        for key in ('energy_now', 'charge_now', 'voltage_now', 'power_now', 'current_now', 'capacity', 'energy_full',
                    'charge_full', 'status'):
            v = read_text(supply / key)
            if v is not None and v.strip():
                v = v.strip()
                out[key] = int(v) if v.lstrip('-').isdigit() else v
        return out
    return None


def session_power(start, end):
    """Watts from the battery's energy (or charge) drop over the session, or None."""
    if not start or not end or end['when'] - start['when'] < 30:
        return None
    hours = (end['when'] - start['when']) / 3600
    if isinstance(start.get('energy_now'), int) and isinstance(end.get('energy_now'), int):
        used = start['energy_now'] - end['energy_now']  # µWh
        return used / 1e6 / hours if used > 0 else None
    if isinstance(start.get('charge_now'), int) and isinstance(end.get('charge_now'), int):
        used = start['charge_now'] - end['charge_now']  # µAh
        volts = mean([v for v in (start.get('voltage_now'), end.get('voltage_now')) if isinstance(v, int)])
        return used * volts / 1e12 / hours if used > 0 and volts else None
    return None


def meminfo_available():
    for line in (read_text('/proc/meminfo') or '').splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1024
    return None


def rss(pid):
    for line in (read_text(f'/proc/{pid}/status') or '').splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1]) / 1024
    return None


def core_times():
    out = {}
    for line in (read_text('/proc/stat') or '').splitlines():
        if line.startswith('cpu') and line[3:4].isdigit():
            parts = line.split()
            values = [int(v) for v in parts[1:9]]
            idle = values[3] + values[4]
            out[int(parts[0][3:])] = (sum(values) - idle, sum(values))
    return out


def core_mhz(n):
    v = read_text(f'/sys/devices/system/cpu/cpu{n}/cpufreq/scaling_cur_freq')
    return int(v) / 1000 if v and v.strip().isdigit() else None


class Snapshot:
    def __init__(self, pid, gpu):
        self.when = time.monotonic()
        self.pid = pid
        parsed = parse_stat(read_text(f'/proc/{pid}/stat'))
        self.ticks = stat_ticks(parsed[1]) if parsed else 0
        self.threads = {}
        try:
            tids = os.listdir(f'/proc/{pid}/task')
        except OSError:
            tids = []
        for tid in tids:
            parsed = parse_stat(read_text(f'/proc/{pid}/task/{tid}/stat'))
            if parsed:
                self.threads[int(tid)] = (parsed[0], stat_ticks(parsed[1]))
        self.cores = core_times()
        self.mhz = {n: core_mhz(n) for n in self.cores}
        self.gpu = gpu.sample(pid, self.when)
        self.temps = thermal()
        self.power = battery()
        self.mem = meminfo_available()
        self.rss = rss(pid)


def kind_of(tid, name, pid, guest):
    if tid == pid:
        return 'main'
    return 'game' if name in guest else 'port'


def group_of(name):
    """Threads of one kind together: GXWorker:1-5, EzWorkPool_TypeA0-3, the five HavokWorkerThreads."""
    return re.sub(r'[:_#-]?\d+$', '', name) or name


def row(prev, cur, t, log):
    dt = cur.when - prev.when
    if dt <= 0:
        return None, []
    pct = lambda ticks: ticks / CLK / dt * 100.0  # noqa: E731
    guest = log.guest if log else set()
    per_thread = []
    sums = {'main': 0.0, 'game': 0.0, 'port': 0.0}
    for tid, (name, ticks) in cur.threads.items():
        if tid not in prev.threads:
            continue
        cpu = pct(ticks - prev.threads[tid][1])
        kind = kind_of(tid, name, cur.pid, guest)
        sums[kind] += cpu
        per_thread.append((tid, name, kind, cpu))
    busiest = max(per_thread, key=lambda r: r[3], default=(0, '', '', 0.0))
    busy = {}
    for n, (b, total) in cur.cores.items():
        if n in prev.cores and total > prev.cores[n][1]:
            busy[n] = 100.0 * (b - prev.cores[n][0]) / (total - prev.cores[n][1])
    gpu_busy = None
    if cur.gpu['engines'] and prev.gpu['engines']:
        deltas = [cur.gpu['engines'][e] - prev.gpu['engines'].get(e, cur.gpu['engines'][e])
                  for e in cur.gpu['engines']]
        gpu_busy = min(100.0, max(deltas) / (dt * 1e9) * 100.0) if deltas else None
    if gpu_busy is None:
        gpu_busy = cur.gpu['kgsl_busy']
    mhz = [m for m in cur.mhz.values() if m]
    fps = log.fps_at(t) if log else None
    r = {'t': round(t, 1), 'pid': cur.pid, 'process_cpu': pct(cur.ticks - prev.ticks), 'main_cpu': sums['main'],
         'game_cpu': sums['game'], 'port_cpu': sums['port'], 'threads': len(cur.threads), 'busiest': busiest[1],
         'busiest_cpu': busiest[3], 'cores_busy': sum(busy.values()) / len(busy) if busy else None,
         'core_busy_max': max(busy.values()) if busy else None, 'cpu_mhz_max': max(mhz) if mhz else None,
         'gpu_busy': gpu_busy, 'gpu_mhz': cur.gpu['mhz'], 'temp_max': cur.temps[0], 'temp_cpu': cur.temps[1],
         'temp_gpu': cur.temps[2], 'power_w': cur.power[0], 'battery': cur.power[1], 'mem_avail_mb': cur.mem,
         'rss_mb': cur.rss, 'fps': fps}
    for n in sorted(cur.cores):
        r[f'core{n}_busy'] = busy.get(n)
        r[f'core{n}_mhz'] = cur.mhz.get(n)
    return r, per_thread


def fmt(v, digits=1):
    if v is None or v == '':
        return ''
    if isinstance(v, float):
        return f'{v:.{digits}f}'
    return str(v)


def record(args):
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    stop = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.append(1))
    target = Target(args.pid, args.session, args.name)
    log = GameLog(args.game_log)
    gpu = Gpu()
    columns = SAMPLE_COLUMNS + [f'core{n}_{k}' for n in sorted(core_times()) for k in ('busy', 'mhz')]
    start = time.monotonic()
    battery_start = battery_raw()
    prev = None
    waited = False
    with open(out / 'samples.csv', 'w', newline='') as sf, open(out / 'threads.csv', 'w', newline='') as tf:
        samples = csv.DictWriter(sf, columns, extrasaction='ignore')
        samples.writeheader()
        threads = csv.writer(tf)
        threads.writerow(['t', 'tid', 'name', 'kind', 'cpu'])
        while not stop:
            now = time.monotonic()
            t = now - start
            log.poll(t)
            pid = target.resolve()
            if pid == -1:
                break
            if pid is None:
                if not waited:
                    print('profile: waiting for the game to start', flush=True)
                    waited = True
                if t > 900:
                    break
                prev = None
            else:
                cur = Snapshot(pid, gpu)
                if prev is not None and prev.pid == pid:
                    r, per_thread = row(prev, cur, t, log)
                    if r:
                        samples.writerow({k: fmt(v, 2) if isinstance(v, float) else v for k, v in r.items()})
                        for tid, name, kind, cpu in per_thread:
                            if cpu >= 0.1:
                                threads.writerow([r['t'], tid, name, kind, f'{cpu:.2f}'])
                        sf.flush()
                        tf.flush()
                prev = cur
            if args.seconds and t >= args.seconds:
                break
            time.sleep(max(0.05, args.interval - (time.monotonic() - now)))
    log.poll(time.monotonic() - start)
    facts = dict(log.facts)
    facts.update({'fps': log.fps, 'guest_threads': sorted(log.guest), 'cores': CORES, 'aot_host': log.aot_host,
                  'battery_start': battery_start, 'battery_end': battery_raw(),
                  'started': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(time.time() - (time.monotonic() - start)))})
    (out / 'facts.json').write_text(json.dumps(facts, indent=1) + '\n', encoding='utf-8')
    summary = summarize(out, args.skip)
    print(summary['text'], end='', flush=True)
    print(f'profile: {out}', flush=True)
    return 0


def number(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def mode_of(facts):
    cpu = facts.get('guest_cpu', '')
    if cpu.startswith('ahead-of-time'):
        return 'native CPU'
    if cpu.startswith('FEXCore'):
        return 'FEXCore'
    return cpu or 'unknown CPU'


def fps_cap(facts):
    caps = []
    if facts.get('fps_preset') == '30' or facts.get('BB_FPS') == '30':
        caps.append(30.0)
    for key in ('BB_FPS_LIMIT', 'frame_limit'):
        v = number(facts.get(key))
        if v and v > 0:
            caps.append(v)
    return min(caps) if caps else None


def summarize(out, skip=0.0, write=True):
    out = Path(out)
    try:
        facts = json.loads((out / 'facts.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        facts = {}
    with open(out / 'samples.csv', newline='') as f:
        samples = [r for r in csv.DictReader(f) if (number(r['t']) or 0) >= skip]
    with open(out / 'threads.csv', newline='') as f:
        thread_rows = [r for r in csv.DictReader(f) if (number(r['t']) or 0) >= skip]
    guest = set(facts.get('guest_threads', []))
    n = len(samples)
    col = lambda name: mean(number(r.get(name)) for r in samples)  # noqa: E731
    span = (number(samples[-1]['t']) - number(samples[0]['t'])) if n > 1 else 0.0
    fps = [v for t, v in facts.get('fps', []) if t >= skip]
    # Threads: CPU per thread and per group, averaged over the samples (0 where a thread did not run).
    per_tid, per_group, group_tids, kinds = {}, {}, {}, {'main': 0.0, 'game': 0.0, 'port': 0.0}
    for r in thread_rows:
        cpu = number(r['cpu']) or 0.0
        tid = int(r['tid'])
        kind = 'main' if r['kind'] == 'main' else ('game' if r['name'] in guest or r['kind'] == 'game' else 'port')
        name = r['name'] if kind != 'main' else f"{r['name']} (main thread)"
        per_tid.setdefault(tid, [name, kind, 0.0, 0.0])
        per_tid[tid][2] += cpu
        per_tid[tid][3] = max(per_tid[tid][3], cpu)
        group = (group_of(r['name']) if kind != 'main' else name, kind)
        per_group[group] = per_group.get(group, 0.0) + cpu
        group_tids.setdefault(group, set()).add(tid)
        kinds[kind] += cpu
    div = max(n, 1)
    groups = sorted(((g, k, total / div, len(group_tids[(g, k)])) for (g, k), total in per_group.items()),
                    key=lambda x: -x[2])
    busiest = sorted(((v[0], v[1], v[2] / div, v[3]) for v in per_tid.values()), key=lambda x: -x[2])
    s = {
        'dir': str(out), 'mode': mode_of(facts), 'samples': n, 'seconds': round(span, 1), 'skip': skip,
        'fps_mean': mean(fps), 'fps_min': min(fps) if fps else None, 'fps_readings': len(fps), 'fps_cap': fps_cap(facts),
        'process_cpu': col('process_cpu'), 'main_cpu': kinds['main'] / div, 'game_cpu': kinds['game'] / div,
        'port_cpu': kinds['port'] / div, 'cores_busy': col('cores_busy'), 'cpu_mhz_max': col('cpu_mhz_max'),
        'gpu_busy': col('gpu_busy'), 'gpu_mhz': col('gpu_mhz'),
        'temp_max': max((number(r['temp_max']) for r in samples if number(r['temp_max']) is not None), default=None),
        'temp_cpu': col('temp_cpu'), 'temp_gpu': col('temp_gpu'), 'power_w': col('power_w'),
        'battery': next((r['battery'] for r in reversed(samples) if r.get('battery')), ''),
        'session_power_w': session_power(facts.get('battery_start'), facts.get('battery_end')),
        'aot_host': {name: mean(v) for name, v in facts.get('aot_host', {}).items()},
        'rss_mb': col('rss_mb'), 'mem_avail_min_mb': min((number(r['mem_avail_mb']) for r in samples
                                                         if number(r['mem_avail_mb']) is not None), default=None),
        'groups': [{'group': g, 'kind': k, 'cpu': c, 'threads': t} for g, k, c, t in groups[:30]],
        'threads': [{'name': nm, 'kind': k, 'cpu': c, 'peak': p} for nm, k, c, p in busiest[:15]],
        'facts': {k: v for k, v in facts.items() if k not in ('fps', 'guest_threads', 'aot_host')},
    }
    s['verdict'] = verdict(s)
    s['minutes'] = minutes(samples, facts.get('fps', []), skip)
    s['text'] = text_of(s)
    if write:
        (out / 'summary.txt').write_text(s['text'], encoding='utf-8')
        (out / 'summary.json').write_text(json.dumps({k: v for k, v in s.items() if k != 'text'}, indent=1) + '\n',
                                          encoding='utf-8')
    return s


def verdict(s):
    """What limits the frame rate, as far as the numbers tell."""
    lines = []
    if not s['samples']:
        return ['no samples: the game did not run long enough']
    cap, fps = s['fps_cap'], s['fps_mean']
    capped = bool(cap and fps and fps >= cap * 0.95)
    if capped:
        lines.append(f'the frame rate is at its cap ({cap:.0f} FPS): compare CPU, temperature and power, not FPS')
    gpu = s['gpu_busy']
    top = s['threads'][0] if s['threads'] else None
    limit = False
    if gpu is not None and gpu >= 85:
        limit = True
        lines.append(f'GPU-bound: the GPU is busy {gpu:.0f} % of the time; a faster CPU path (native CPU) gives no FPS here')
    if top and top['cpu'] >= 85 and capped:
        lines.append(f"even at the cap {top['name']} ({top['kind']}) is busy {top['cpu']:.0f} % of a core: it may wait by "
                     'spinning; compare the same cap on the other CPU')
    elif top and top['cpu'] >= 85:
        limit = True
        lines.append(f"one thread is at its limit: {top['name']} ({top['kind']}, {top['cpu']:.0f} % of a core): "
                     'it sets the frame rate' + (" (the game's code: the native CPU can help)" if top['kind'] != 'port'
                                                  else " (the port's own code, native already: the native CPU cannot help)"))
    if not limit and not capped:
        lines.append('neither the GPU nor a single thread is at its limit: the threads wait for each other (or the '
                     'display); see the busiest threads')
    total = s['process_cpu'] or 0.0
    game = s['main_cpu'] + s['game_cpu']
    if total > 0:
        lines.append(f"the game's threads and the main thread: {game:.0f} of {total:.0f} % CPU "
                     f"({100 * game / total:.0f} %), the most the native CPU can save (they also run the port's functions "
                     f"the game calls: an upper bound); the port's own threads {s['port_cpu']:.0f} %")
    main_host = next((v for k, v in s['aot_host'].items() if k == 'bb-probe'), None)
    if main_host is not None:
        lines.append(f"native CPU: the main thread spends {main_host:.0f} % of its time in the port's functions "
                     '(BB_AOT_STATS), the rest in the game\'s translated code')
    w, ws = s['power_w'], s['session_power_w']
    if w and ws and (w > 2 * ws or ws > 2 * w):
        lines.append(f"the battery's power reading ({w:.0f} W) and its energy drop ({ws:.1f} W) disagree: trust the "
                     'energy drop')
    elif w and w > 40 and not ws:
        lines.append(f'the battery reports {w:.0f} W, more than a handheld can draw: its driver uses another unit')
    if gpu is None:
        lines.append('GPU load unknown here (no DRM fdinfo, no kgsl)')
    return lines


def minutes(samples, fps, skip):
    rows = {}
    for r in samples:
        m = int((number(r['t']) or 0) // 60)
        rows.setdefault(m, []).append(r)
    out = []
    for m in sorted(rows):
        rs = rows[m]
        c = lambda name: mean(number(r.get(name)) for r in rs)  # noqa: E731
        f = [v for t, v in fps if t >= skip and m * 60 <= t < (m + 1) * 60]
        out.append({'minute': m, 'fps': mean(f), 'process_cpu': c('process_cpu'),
                    'game_cpu': (c('game_cpu') or 0) + (c('main_cpu') or 0), 'port_cpu': c('port_cpu'),
                    'gpu_busy': c('gpu_busy'), 'gpu_mhz': c('gpu_mhz'), 'cpu_mhz_max': c('cpu_mhz_max'),
                    'temp_max': c('temp_max'), 'power_w': c('power_w')})
    return out


def text_of(s):
    f = lambda v, unit='', d=1: '-' if v is None else f'{v:.{d}f}{unit}'  # noqa: E731
    facts = s['facts']
    lines = [f"bbport performance profile: {s['dir']}",
             f"{s['mode']}; {facts.get('gpu', 'GPU ?')}; output/scene {facts.get('output', '?')}; FPS preset "
             f"{facts.get('fps_preset', facts.get('BB_FPS', '?'))}, limit {facts.get('BB_FPS_LIMIT', '0')}",
             f"{s['seconds']:.0f} s measured ({s['samples']} samples" + (f", the first {s['skip']:.0f} s left out" if s['skip'] else '') + ')',
             '',
             f"FPS            {f(s['fps_mean'])} (lowest 5 s window {f(s['fps_min'])}; {s['fps_readings']} readings)",
             f"process CPU    {f(s['process_cpu'], ' %', 0)} of one core ({facts.get('cores', CORES)} cores: {f(s['cores_busy'], ' %', 0)} of the machine busy)",
             f"  game         {f(s['game_cpu'] + s['main_cpu'], ' %', 0)} (its threads {f(s['game_cpu'], ' %', 0)}, main thread {f(s['main_cpu'], ' %', 0)})",
             f"  port         {f(s['port_cpu'], ' %', 0)} (GPU emulation, Vulkan, audio, ...)",
             f"GPU            {f(s['gpu_busy'], ' % busy', 0)}, {f(s['gpu_mhz'], ' MHz', 0)}; CPU clock (fastest core) {f(s['cpu_mhz_max'], ' MHz', 0)}",
             f"temperature    max {f(s['temp_max'], ' °C')}, CPU {f(s['temp_cpu'], ' °C')}, GPU {f(s['temp_gpu'], ' °C')}",
             f"power          {f(s['power_w'], ' W')} reported" + (f" (battery {s['battery'].lower()})" if s['battery'] else '')
             + (f"; {s['session_power_w']:.1f} W from the battery's energy drop" if s['session_power_w'] else ''),
             f"memory         game {f(s['rss_mb'], ' MB', 0)}, lowest free {f(s['mem_avail_min_mb'], ' MB', 0)}",
             '', 'What limits it:'] + [f'  - {v}' for v in s['verdict']]
    lines += ['', 'Busiest threads (mean % of a core, peak):']
    lines += [f"  {t['name'][:34]:34} {t['kind']:5} {t['cpu']:6.1f} {t['peak']:6.1f}" for t in s['threads']]
    lines += ['', 'Thread groups (mean % of a core, threads):']
    lines += [f"  {g['group'][:34]:34} {g['kind']:5} {g['cpu']:6.1f} {g['threads']:3}" for g in s['groups'][:20]]
    if len(s['minutes']) > 1:
        lines += ['', 'Per minute:   FPS   CPU  game  port  GPU%   GPU MHz  CPU MHz  °C max  W']
        for m in s['minutes']:
            lines.append(f"  {m['minute']:4}   {f(m['fps']):>6} {f(m['process_cpu'], '', 0):>5} {f(m['game_cpu'], '', 0):>5} "
                         f"{f(m['port_cpu'], '', 0):>5} {f(m['gpu_busy'], '', 0):>5} {f(m['gpu_mhz'], '', 0):>9} "
                         f"{f(m['cpu_mhz_max'], '', 0):>8} {f(m['temp_max']):>7} {f(m['power_w']):>4}")
    return '\n'.join(lines) + '\n'


def compare(a, b, skip=0.0):
    sa, sb = summarize(a, skip, write=False), summarize(b, skip, write=False)
    f = lambda v, d=1: '-' if v is None else f'{v:.{d}f}'  # noqa: E731

    def change(x, y):
        if x is None or y is None or not x:
            return ''
        return f'{100.0 * (y - x) / abs(x):+.0f} %'
    name_a, name_b = f"A: {Path(a).name} ({sa['mode']})", f"B: {Path(b).name} ({sb['mode']})"
    rows = [('FPS', 'fps_mean', 1), ('FPS, lowest 5 s', 'fps_min', 1), ('process CPU, % of a core', 'process_cpu', 0),
            ("  the game's threads + main", None, 0), ("  the port's threads", 'port_cpu', 0),
            ('GPU busy, %', 'gpu_busy', 0), ('GPU clock, MHz', 'gpu_mhz', 0), ('CPU clock, fastest core', 'cpu_mhz_max', 0),
            ('temperature max, °C', 'temp_max', 1), ('power reported, W', 'power_w', 1),
            ("power from the battery's drop, W", 'session_power_w', 1), ('game memory, MB', 'rss_mb', 0)]
    lines = [name_a, name_b, '', f"{'':34} {'A':>9} {'B':>9} {'B vs A':>8}"]
    for label, key, d in rows:
        x = (sa['game_cpu'] + sa['main_cpu']) if key is None else sa[key]
        y = (sb['game_cpu'] + sb['main_cpu']) if key is None else sb[key]
        lines.append(f'{label:34} {f(x, d):>9} {f(y, d):>9} {change(x, y):>8}')
    groups = {}
    for s, i in ((sa, 0), (sb, 1)):
        for g in s['groups']:
            groups.setdefault((g['group'], g['kind']), [None, None])[i] = g['cpu']
    top = sorted(groups.items(), key=lambda kv: -max(v or 0 for v in kv[1]))[:16]
    lines += ['', 'Thread groups, % of a core:']
    for (g, k), (x, y) in top:
        lines.append(f'  {g[:26]:26} {k:5} {f(x):>9} {f(y):>9} {change(x, y):>8}')
    lines += ['', 'A: ' + '; '.join(sa['verdict']), 'B: ' + '; '.join(sb['verdict'])]
    return '\n'.join(lines) + '\n'


def main(argv=None):
    p = argparse.ArgumentParser(description='Performance profile of the running game (see the module docstring).')
    sub = p.add_subparsers(dest='command', required=True)
    r = sub.add_parser('record', help='measure until the game exits')
    who = r.add_mutually_exclusive_group(required=True)
    who.add_argument('--session', type=int, help="the bb-probe process of this session (the launcher's)")
    who.add_argument('--pid', type=int, help='one process')
    who.add_argument('--name', help='the newest process of this name (bb-probe)')
    r.add_argument('--out', required=True, help='the profile folder')
    r.add_argument('--game-log', help="the game's output, as it is written (FPS, thread names)")
    r.add_argument('--interval', type=float, default=1.0, help='seconds between samples (default 1)')
    r.add_argument('--seconds', type=float, default=0, help='stop after this long (default: when the game exits)')
    r.add_argument('--skip', type=float, default=0, help="the summary leaves out the first seconds (loading)")
    s = sub.add_parser('summary', help='the summary of a profile again')
    s.add_argument('dir')
    s.add_argument('--skip', type=float, default=0)
    c = sub.add_parser('compare', help='two profiles side by side')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--skip', type=float, default=0)
    args = p.parse_args(argv)
    if args.command == 'record':
        return record(args)
    if args.command == 'summary':
        print(summarize(args.dir, args.skip)['text'], end='')
        return 0
    print(compare(args.a, args.b, args.skip), end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
