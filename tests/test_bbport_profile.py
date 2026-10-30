# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/bbport_profile.py: the fdinfo and log parsing, the summary and the comparison (no game needed)."""
from paths import ROOT  # noqa: F401  (scripts/ on sys.path)
import csv
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import bbport_profile as bp

MSM_FDINFO = """pos:\t0
flags:\t02100002
drm-driver:\tmsm
drm-client-id:\t7
drm-pdev:\t3d00000.gpu
drm-engine-gpu:\t{ns} ns
drm-cycles-gpu:\t123
drm-maxfreq-gpu:\t1100000000 Hz
drm-curfreq-gpu:\t680000000 Hz
"""


def write_profile(folder, mode, fps, threads):
    """A profile folder as record writes it: samples.csv, threads.csv, facts.json."""
    folder.mkdir()
    with open(folder / 'samples.csv', 'w', newline='') as f:
        w = csv.DictWriter(f, bp.SAMPLE_COLUMNS)
        w.writeheader()
        for t in range(1, 11):
            total = sum(cpu for _, _, cpu in threads)
            w.writerow({'t': t, 'pid': 100, 'process_cpu': total, 'gpu_busy': 95.0, 'gpu_mhz': 680,
                        'temp_max': 60 + t, 'power_w': 9.5, 'mem_avail_mb': 4000, 'rss_mb': 7000, 'fps': fps})
    with open(folder / 'threads.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['t', 'tid', 'name', 'kind', 'cpu'])
        for t in range(1, 11):
            for tid, (name, kind, cpu) in enumerate(threads, 100):
                w.writerow([t, tid, name, kind, cpu])
    (folder / 'facts.json').write_text(json.dumps({
        'guest_cpu': mode, 'fps': [[float(t), fps] for t in range(1, 11, 5)],
        'guest_threads': ['GXRenderThread', 'HavokWorkerThre'], 'fps_preset': 'uncap'}))


class ProfileTests(unittest.TestCase):
    def test_fdinfo_gives_the_gpu_time_and_clock(self):
        gpu = bp.Gpu()
        gpu.kgsl = None
        texts = {'/proc/1/fdinfo/5': MSM_FDINFO.format(ns=2_000_000_000),
                 '/proc/1/fdinfo/6': MSM_FDINFO.format(ns=2_000_000_000)}  # the same client twice: counted once
        with mock.patch.object(gpu, 'drm_fds', return_value=['5', '6']), \
                mock.patch.object(bp, 'read_text', side_effect=lambda p: texts.get(str(p))):
            s = gpu.sample(1, 0.0)
        self.assertEqual(s['engines'], {'gpu': 2_000_000_000})
        self.assertEqual(s['mhz'], 680.0)

    def test_log_facts_threads_and_fps(self):
        log = bp.GameLog(None)
        for line in ('Guest CPU: ahead-of-time units only, TSC x64',
                     "Runtime: guest thread 'Core.Logging.BackgroundStrategy' created (stack=65536, prio=700)",
                     'Patches: FPS preset 30; 85 writes from []', ' BB_RENDER_RES=1064x600 BB_FPS=30',
                     'Present rate: 29.9 FPS over 5.0 s', 'Frame stats: 31.5 FPS, worst frame 40.1 ms'):
            log.line(line, 5.0)
        self.assertEqual(log.facts['guest_cpu'], 'ahead-of-time units only')
        self.assertIn('Core.Logging.Ba', log.guest)  # the kernel keeps 15 bytes of a thread's name
        self.assertEqual([v for _, v in log.fps], [29.9, 31.5])
        self.assertEqual(bp.mode_of(log.facts), 'native CPU')
        self.assertEqual(bp.fps_cap(log.facts), 30.0)

    def test_summary_and_compare(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / 'fex', Path(tmp) / 'native'
            write_profile(a, 'FEXCore JIT', 37.0, [('GXRenderThread', 'port', 60.0), ('HavokWorkerThre', 'port', 40.0),
                                                     ('bb:DrawRec', 'port', 90.0)])
            write_profile(b, 'ahead-of-time units only', 37.5, [('GXRenderThread', 'game', 45.0),
                                                                 ('HavokWorkerThre', 'game', 30.0), ('bb:DrawRec', 'port', 90.0)])
            s = bp.summarize(a)
            self.assertTrue((a / 'summary.txt').is_file())
            self.assertEqual(s['mode'], 'FEXCore')
            self.assertAlmostEqual(s['game_cpu'], 100.0)  # named in the log: the game's, whatever record thought
            self.assertAlmostEqual(s['port_cpu'], 90.0)
            self.assertTrue(any(v.startswith('GPU-bound') for v in s['verdict']))
            self.assertTrue(any('bb:DrawRec' in v and 'cannot help' in v for v in s['verdict']))
            text = bp.compare(a, b)
            self.assertIn('native CPU', text)
            self.assertIn('-25 %', text)  # the game's threads: 100 -> 75


if __name__ == '__main__':
    unittest.main()
