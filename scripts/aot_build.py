#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The native CPU's library, built on this device (aarch64 Linux): the game's x86-64 code compiled ahead of time to AArch64
(libbbaot.so) by bb-recomp, the Bloodborne Android Rebuild Tool (ducvd89), from the game in the game folder. The launcher's
Game -> Native CPU runs it; so does `bbport_launcher.py --build-aot`.

    python3 scripts/aot_build.py --game <game folder> --data <data folder> [--tool <aot-tool folder>] [-O 1|2]
                                 [--host auto|armv8.2|armv8.4|armv9] [--jobs N] [--extra-entries auto|none|<file>] [--boot FILE]
    python3 scripts/aot_build.py --status --game ... --data ...      (the state as one JSON line)

Steps:
1. the game image as run.sh makes it: prepare.py, link_libc.py and link_modules.py into <data>/out (the mods view is left
   out: a mod that replaces eboot.bin or a module gives another image, and run.sh then finds no library for it). --boot
   uses a ready boot-linked.bin instead (link.json next to it);
2. its sha256: run.sh's BB_AOT=auto takes <data>/aot/<sha256>/libbbaot.so;
3. bb-recomp --boot ... --game-dir ... --so <data>/aot/<sha256>/work/libbbaot.so --progress --resume, with the tool's
   variant catalog. The work folder stays: a stop resumes there, and a later build (more entries, a new catalog) only
   recompiles the parts that changed;
4. libbbaot.so, libbbaot.json and compile.txt into <data>/aot/<sha256>/, with build.json (what it was made from and with);
   <data>/aot/current.json names it for the launcher. Only the newest two images' folders are kept.

The tool is a folder with bin/bb-recomp and share/bb-recomp (the link inputs for glibc, and variants/): the AppImage has
it as <port>/aot-tool (packaging/bb-recomp.nix, when it was built next to the Rebuild Tool's sources), or
`build_aot_docker.sh device-tool` makes one on a PC or Mac (no game files needed) to copy to <data>/aot-tool. It is looked
for in --tool, $BB_RECOMP_DIR, <port>/aot-tool and <data>/aot-tool.

--extra-entries auto (the default): the addresses of <data>/logs/aot-misses.log (the runtime writes it when the game
reaches code the library does not cover) join the image's entry list (<data>/aot/<sha256>/extra-entries.txt), which every
later build of that image uses.

Output: the tool's lines as they come (BBR-PROGRESS <stage> <done> <total> for a progress bar: the tool's
docs/NATIVE_CPU.md, "Progress"), lines of this script starting with "aot: ", and last "AOT-DONE <status> <library or the
reason>". Exit status: 0 built, 1 failed, 2 bad arguments, 3 no tool, 4 the game folder is not the supported game,
130/143 stopped (SIGINT/SIGTERM: the finished parts stay for the next run).

libbbaot.so is the game's own code, translated: it stays on this device. Never share it.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT / 'scripts'))

GAME_FILES = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx', 'sce_sys/param.sfo')
# What the tool's in-process link reads from --bundle-dir (its src/bundle.cpp; build_aot_docker.sh device-tool makes them
# for glibc under the Android names).
BUNDLE_FILES = ('helpers-aarch64.o', 'libbbcpu-stub.so', 'libm.so', 'libclang_rt.builtins-aarch64-android.a')
# The BBR-PROGRESS stages and their share of the time (the tool's docs/NATIVE_CPU.md, "Progress").
STAGE_WEIGHTS = {'scan': 1, 'discover': 2, 'lift': 2, 'compile': 93, 'variants': 1, 'link': 1}
# The misses log's entries. kind=patched (a patched function without a variant) and kind=unsupported (code the tool could
# not translate) are no missing entries: a build with more entries changes nothing there.
MISS_LINE = re.compile(r'^miss \+0x([0-9a-fA-F]+)\b(?!.*\bkind=(?:patched|unsupported)\b)')
KEEP_IMAGES = 2
# How the libraries are made (build.json 'recipe'). A library of an older recipe shows as to be rebuilt, and the launcher
# runs the game with FEX until then. 2: --host at most armv8.4 (recipe 1 took armv9 on CPUs with FRINTTS).
RECIPE = 2
TRAP_LINE = re.compile(r'units with bbr_trap \(unsupported ops\): (\d+)')


def say(text):
    print('aot: ' + text, flush=True)


class Failure(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def tool_problem(folder):
    """None when `folder` is a usable aot-tool folder, else what is missing."""
    folder = Path(folder)
    exe = folder / 'bin' / 'bb-recomp'
    if not (exe.is_file() and os.access(exe, os.X_OK)):
        return 'no bin/bb-recomp'
    share = folder / 'share' / 'bb-recomp'
    missing = [name for name in BUNDLE_FILES if not (share / name).is_file()]
    if missing:
        return 'share/bb-recomp lacks ' + ', '.join(missing)
    if not any((share / 'variants').glob('*.txt')):
        return 'share/bb-recomp/variants has no catalog'
    return None


def find_tool(explicit, data):
    """The aot-tool folder, or None."""
    # The package's own first: a copy in the data folder may be from an older tool.
    for candidate in (explicit, os.environ.get('BB_RECOMP_DIR'), PORT / 'aot-tool', Path(data) / 'aot-tool'):
        if candidate and tool_problem(Path(candidate).expanduser()) is None:
            return Path(candidate).expanduser().resolve()
    return None


def tool_version(folder):
    try:
        return (Path(folder) / 'VERSION').read_text(encoding='utf-8').strip()
    except OSError:
        pass
    try:
        out = subprocess.run([str(Path(folder) / 'bin' / 'bb-recomp'), '--version'], capture_output=True, text=True,
                             timeout=20).stdout.split()
        return out[-1] if out else 'unknown'
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def cpu_features():
    try:
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('Features'):
                return set(line.split(':', 1)[1].split())
    except OSError:
        pass
    return set()


def host_profile(features):
    """bb-recomp's --host for this CPU (the tool's src/lifter.cpp, makeHost): armv8.4, the tool's default and the profile
    its builds are made with, when the CPU has FlagM and LRCPC2; else armv8.2. Never armv9 by itself: with FRINTTS FEX's
    front end turns cvttss2si/cvttsd2si into Vector_FToISized, which bb-recomp 1.3.0 does not lower. Bloodborne then had
    2431 units without native code (libc's sinf among them), and the game stopped in the first of them."""
    if {'flagm', 'ilrcpc'} <= features:
        return 'armv8.4'
    return 'armv8.2'


def mem_available_gb():
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) / (1024 * 1024)
    except (OSError, ValueError, IndexError):
        pass
    return 4.0


def default_jobs():
    """Worker threads: one core left for the rest of the system, and about 0.5 GB + 0.8 GB per worker (the tool's
    measurement on the AYN Thor)."""
    cores = os.cpu_count() or 4
    by_memory = int((mem_available_gb() - 1.5) / 0.8)
    return max(1, min(cores - 1, by_memory))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def run_step(command, what):
    """A preparation script, its output relayed; a failure (or a stop) raises."""
    code = subprocess.run(command, cwd=PORT, stdout=sys.stdout, stderr=subprocess.STDOUT).returncode
    if code < 0 or code in (130, 143):
        raise Failure(143, 'stopped')
    if code != 0:
        raise Failure(1, f'{what} failed (status {code})')


def miss_addresses(path):
    found = []
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            m = MISS_LINE.match(line.strip())
            if m:
                found.append(int(m.group(1), 16))
    except OSError:
        pass
    return found


def read_entries(path):
    """The addresses of an entry list: misses-log lines (`miss +0x...`) or one hex address per line."""
    entries = set(miss_addresses(path))
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if line and not line.startswith(('#', 'miss')):
                try:
                    entries.add(int(line, 16))
                except ValueError:
                    pass
    except OSError:
        pass
    return entries


def write_json(path, value):
    tmp = path.with_name(path.name + '.part')
    tmp.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def game_files_folder(game, aot):
    """The four files bb-recomp reads (--game-dir), as links in a folder whose path has no spaces: the tool splits some of
    its internal commands on spaces (its docs/NATIVE_CPU.md, "Command line")."""
    folder = aot / 'game-files'
    for name in GAME_FILES:
        source = (game / name).resolve()
        if not source.is_file():
            raise Failure(4, f'{game / name} is missing')
        link = folder / name
        link.parent.mkdir(parents=True, exist_ok=True)
        tmp = link.with_name(link.name + '.link')
        if tmp.is_symlink() or tmp.exists():
            tmp.unlink()
        tmp.symlink_to(source)
        os.replace(tmp, link)
    return folder


def status(data, game, tool=None):
    """What the launcher shows: the tool, and the library of the last build when it was made for this game folder (run.sh
    makes the final check at every start: the hash of the image it has just made)."""
    data = Path(data).expanduser()
    found = find_tool(tool, data)
    result = {'tool': str(found) if found else None, 'tool_version': tool_version(found) if found else None,
              'built': False, 'interrupted': False, 'misses_new': 0}
    try:
        current = json.loads((data / 'aot' / 'current.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        current = None
    if current and Path(current.get('game', '')) == Path(game).expanduser().resolve():
        folder = data / 'aot' / str(current.get('sha256', '-'))
        result['built'] = (folder / 'libbbaot.so').is_file()
        result.update({k: current.get(k) for k in ('sha256', 'created', 'opt', 'host', 'extra_entries', 'trap_units')})
        result['built_with'] = current.get('tool_version')
        result['outdated'] = result['built'] and not recipe_current(current)
        result['library'] = str(folder / 'libbbaot.so')
        known = read_entries(folder / 'extra-entries.txt')
        result['misses_new'] = len(set(miss_addresses(data / 'logs' / 'aot-misses.log')) - known)
    if not result['built']:
        result['interrupted'] = any((data / 'aot').glob('*/work/*.stat'))
    return result


def recipe_current(info):
    try:
        return int(info.get('recipe') or 1) >= RECIPE
    except (TypeError, ValueError):
        return False


def library_usable(data):
    """False when the last build (aot/current.json) is of an older recipe: the launcher then leaves BB_AOT unset and FEX
    runs the game until it is rebuilt. True without a build (run.sh finds no library and says so)."""
    try:
        current = json.loads((Path(data).expanduser() / 'aot' / 'current.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return True
    return recipe_current(current)


def build(args):
    data = Path(args.data).expanduser().resolve()
    game = Path(args.game).expanduser().resolve()
    aot = data / 'aot'
    if os.uname().machine != 'aarch64':
        raise Failure(1, 'the native CPU is for aarch64 devices: on x86-64 the game runs on the CPU itself')
    tool = find_tool(args.tool, data)
    if tool is None:
        where = Path(args.tool or os.environ.get('BB_RECOMP_DIR') or data / 'aot-tool').expanduser()
        why = tool_problem(where) if where.exists() else 'not found'
        raise Failure(3, f'no bb-recomp for this device in {where} ({why}): an AppImage built next to the Rebuild Tool\'s '
                         f'sources has it; or make it with ./build_aot_docker.sh device-tool on a PC or Mac and copy the '
                         f'folder to {data / "aot-tool"}')
    if ' ' in str(data) or ' ' in str(tool):
        raise Failure(1, f'the data folder and the tool folder must have no spaces in their paths ({data}, {tool})')
    import game_check
    problem = game_check.problem(game)
    if problem:
        raise Failure(4, f'the game folder is not Bloodborne 1.09 with a clean eboot.bin ({problem[0]})')
    version = tool_version(tool)
    say(f'bb-recomp {version} from {tool}')

    if args.boot:
        boot = Path(args.boot).expanduser().resolve()
        say(f'game image: {boot}')
    else:
        out = data / 'out'
        out.mkdir(parents=True, exist_ok=True)
        say('making the game image as run.sh does (prepare, link_libc, link_modules)')
        for script in ('prepare', 'link_libc', 'link_modules'):
            run_step([sys.executable, str(PORT / 'scripts' / f'{script}.py'), str(game), '--out', str(out)],
                     f'scripts/{script}.py')
        boot = out / 'boot-linked.bin'
    try:
        with open(boot, 'rb') as f:
            magic = f.read(8)
    except OSError:
        magic = b''
    if magic != b'BBPROBE5':
        raise Failure(1, f'{boot} is not a boot-linked.bin (BBPROBE5)')
    sha = sha256(boot)
    folder = aot / sha
    work = folder / 'work'
    image = work / 'image'
    image.mkdir(parents=True, exist_ok=True)
    say(f'game image sha256 {sha}')
    # The image stays with the objects made from it: a resumed or a delta build sees the same bytes.
    shutil.copyfile(boot, image / 'boot-linked.bin')
    if boot.with_name('link.json').is_file():
        shutil.copyfile(boot.with_name('link.json'), image / 'link.json')
    game_dir = game_files_folder(game, aot)

    entries_file = folder / 'extra-entries.txt'
    entries = read_entries(entries_file)
    if args.extra_entries == 'auto':
        new = set(miss_addresses(data / 'logs' / 'aot-misses.log')) - entries
        if new:
            say(f'{len(new)} missing native entries from logs/aot-misses.log join the build')
        entries |= new
    elif args.extra_entries != 'none':
        entries |= read_entries(args.extra_entries)
    if entries:
        # In the misses log's own format, which bb-recomp --extra-entries reads as it is.
        entries_file.write_text('# addresses the game reached that the library did not cover (logs/aot-misses.log)\n' +
                                ''.join(f'miss +{a:#x} kind=kept\n' for a in sorted(entries)), encoding='utf-8')

    host = args.host if args.host != 'auto' else host_profile(cpu_features())
    jobs = args.jobs or default_jobs()
    share = tool / 'share' / 'bb-recomp'
    command = [str(tool / 'bin' / 'bb-recomp'), '--boot', str(image / 'boot-linked.bin'), '--game-dir', str(game_dir),
               '--bundle-dir', str(share), '--variants', str(share / 'variants'), '--so', str(work / 'libbbaot.so'),
               '--stats', str(work / 'compile.txt'), '--shards', '64', f'-O{args.opt}', '--host', host,
               '--jobs', str(jobs), '--no-nice', '--progress', '--resume']
    if entries:
        command += ['--extra-entries', str(entries_file)]
    say(f'compiling: -O{args.opt} --host {host}, {jobs} workers, {len(entries)} extra entries '
        '(minutes; keep the charger connected, the device gets warm)')
    started = time.time()
    proc = subprocess.Popen(command, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                            errors='replace')
    error = done = None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        if line.startswith('BBR-ERROR '):
            error = line[len('BBR-ERROR '):].strip()
        elif line.startswith('BBR-DONE '):
            done = line.split()[1:2]
    code = proc.wait()
    if code in (130, 143) or code < 0:
        raise Failure(130 if code == 130 else 143, 'stopped: the finished parts stay for the next build')
    if code != 0 or done != ['0']:
        raise Failure(1, error or f'bb-recomp failed (status {code})')
    try:
        meta = json.loads((work / 'libbbaot.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        raise Failure(1, 'bb-recomp wrote no libbbaot.json')
    if meta.get('abi_version') != 2:
        raise Failure(1, f'the library has ABI version {meta.get("abi_version")}; cpu/aot runs 2 (another tool release)')
    for name in ('libbbaot.so', 'libbbaot.json', 'compile.txt'):
        if (work / name).is_file():
            os.replace(work / name, folder / name)
    try:
        trap = TRAP_LINE.search((folder / 'compile.txt').read_text(encoding='utf-8', errors='replace'))
    except OSError:
        trap = None
    trap_units = int(trap.group(1)) if trap else None
    if trap_units:
        say(f'warning: {trap_units} units hold an instruction bb-recomp does not translate (compile.txt, "unsupported '
            'ops"): they have no native code, and the game stops if it reaches one')
    info = {'sha256': sha, 'game': str(game), 'created': time.strftime('%Y-%m-%dT%H:%M:%S'),
            'seconds': round(time.time() - started), 'tool': str(tool), 'tool_version': version, 'recipe': RECIPE,
            'opt': args.opt, 'host': host, 'jobs': jobs, 'extra_entries': len(entries), 'trap_units': trap_units,
            'so_size': (folder / 'libbbaot.so').stat().st_size, 'unit_count': meta.get('unit_count'),
            'image_hash': meta.get('image_hash')}
    write_json(folder / 'build.json', info)
    write_json(aot / 'current.json', info)
    # Older images' folders (another game version, older scripts): only the newest KEEP_IMAGES stay.
    images = sorted((p for p in aot.iterdir() if p.is_dir() and re.fullmatch(r'[0-9a-f]{64}', p.name)),
                    key=lambda p: p.stat().st_mtime, reverse=True)
    for old in images[KEEP_IMAGES:]:
        if old != folder:
            shutil.rmtree(old, ignore_errors=True)
    say(f'done in {info["seconds"]} s: {folder / "libbbaot.so"} ({info["so_size"] >> 20} MB)')
    return folder / 'libbbaot.so'


def main(argv=None):
    p = argparse.ArgumentParser(description='Builds the native CPU library on this device (see the module docstring).')
    p.add_argument('--game', required=True, help='the game folder (eboot.bin, sce_module, sce_sys)')
    p.add_argument('--data', required=True, help="the port's data folder (out/, aot/, logs/)")
    p.add_argument('--tool', help='the aot-tool folder (bin/bb-recomp, share/bb-recomp)')
    p.add_argument('-O', dest='opt', type=int, choices=(0, 1, 2), default=1, help="optimisation level (default 1, the tool's)")
    p.add_argument('--host', default='auto', choices=('auto', 'armv8.2', 'armv8.4', 'armv9'),
                   help='the target profile (auto: armv8.4 or armv8.2 from /proc/cpuinfo; armv9 leaves code without '
                        'native code with bb-recomp 1.3.0, see host_profile)')
    p.add_argument('--jobs', type=int, default=0)
    p.add_argument('--extra-entries', default='auto', help='auto (logs/aot-misses.log), none, or a file')
    p.add_argument('--boot', help='a ready boot-linked.bin instead of making one')
    p.add_argument('--status', action='store_true', help='print the state as one JSON line and exit')
    args = p.parse_args(argv)
    if args.status:
        print(json.dumps(status(args.data, args.game, args.tool)))
        return 0
    # The launcher stops a build with SIGTERM to the process group: bb-recomp gets it as well and stops at its next
    # function; this script waits for it and says so.
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: None)
    try:
        library = build(args)
    except Failure as failure:
        print(f'AOT-DONE {failure.status} {failure}', flush=True)
        return failure.status
    except Exception as error:  # an unexpected one: the launcher still gets the last line
        print(f'AOT-DONE 1 {type(error).__name__}: {error}', flush=True)
        return 1
    print(f'AOT-DONE 0 {library}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
