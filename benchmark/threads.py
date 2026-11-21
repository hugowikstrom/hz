#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Thread scaling: every packer at one core and at as many as it can use.

    python3 benchmark/threads.py kod                 # both configs, every packer
    python3 benchmark/threads.py kod 'tar.xz -9e'    # one packer
    python3 benchmark/threads.py kod --only 1        # just the single-thread half
    python3 benchmark/threads.py kod --dry-run       # print commands and estimates
    python3 benchmark/threads.py --audit kod         # re-check invariants, measure nothing

Why this exists. Every number in the repo's other tables is a max-thread
number, and the thread models are silently mixed: pigz is parallel where
gzip is not, zstd's default is four workers, and `xz -9e -T0` is not 24
threads but about five, because its memory limiter reduces them. This
harness states the thread count instead of assuming it.

Three facts shape the design.

  SIZE CHANGES WITH THREADS, not just time. xz -T>1 starts a new block
  with a fresh dictionary; zstd splits into jobs; hz sizes its xz
  dictionary from the number of concurrent encoders. Only pigz is
  verified thread-invariant. So every configuration records its own
  bytes, and a speedup is never printed without the size delta beside
  it.

  "AS MANY AS IT ALLOWS" IS FOUR DIFFERENT CEILINGS: the flag, the
  memory limiter, the structural limit (xz -9e codes 192 MiB blocks, so
  a 74 MiB corpus can only ever use one thread), and the data itself.
  Hence three columns — requested, granted, and `cpu/wall`, which is
  the only one that cannot lie.

  THE MACHINE IS NOT IDLE. Agent timers fire every minute here, so each
  measurement records free memory, load average and the foreign CPU it
  saw, and flags itself disturbed rather than quietly reporting noise.

Results go to results/threads-<corpus>.json — a separate family, because
experiment/dominance.py splits results/<corpus>.json on the row-key
prefix and would otherwise treat a single-thread row as an opponent.
"""
import argparse, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'benchmark'))
import measure as m                      # fingerprint(), corpus_bytes()

# Scratch must not sit on tmpfs: archives written there are RAM, and RAM
# is what hz reads to size its dictionaries (codec.cpp free_memory()).
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', ROOT / '.scratch')) / 'threads'
HZ = str(ROOT / 'src' / 'hz')

# Each packer: the 1-thread command pair, the max-thread pair, and what
# to do when one of them does not exist. Templates are measure.py's:
# {c} corpus dir, {cd} its parent, {cn} its basename, {a} archive,
# {u} unpack dir. Row keys are measure.py's frozen identifiers so the
# two files join; new rows get new keys.
#
# Verbosity is identical in both halves of a pair, so neither is charged
# for the other's logging.
TAR = "tar -C {cd} -cf - {cn}"
UNTAR = "tar -C {u} -xf -"

CONFIGS = {
 'tar (okomprimerad)': {
   '1':   ("tar -C {cd} -cf {a} {cn}", "tar -C {u} -xf {a}"),
   'max': ('enkeltradad', 'tar has no threads; this row measures the pipeline overhead '
                          'the other rows are charged for'),
 },
 'tar.gz -6': {
   '1':   (TAR + " | gzip -6 > {a}", "gzip -dc {a} | " + UNTAR),
   'max': ('enkeltradad', 'gzip has no parallel mode; the parallel gzip is the pigz row'),
 },
 'tar.gz -9 (pigz)': {
   '1':   (TAR + " | pigz -9 -p 1 > {a}",  "pigz -dc -p 1 {a} | " + UNTAR),
   'max': (TAR + " | pigz -9 -p 24 > {a}", "pigz -dc -p 24 {a} | " + UNTAR),
 },
 'tar.bz2 -9': {
   '1':   (TAR + " | bzip2 -9 > {a}", "bzip2 -dc {a} | " + UNTAR),
   'max': ('saknas', 'bzip2 1.0.8 is single-threaded and lbzip2/pbzip2 are not installed; '
                     'a different program would be a different row'),
 },
 'tar.zst -3 (dagens)': {
   '1':   (TAR + " | zstd -3 -T1 -v -o {a}",  "zstd -dq -c {a} | " + UNTAR),
   'max': (TAR + " | zstd -3 -T24 -v -o {a}", "zstd -dq -c {a} | " + UNTAR),
 },
 'tar.zst -19 --long': {
   '1':   (TAR + " | zstd -19 --long=27 -T1 -v -o {a}",  "zstd -dq --long=27 -c {a} | " + UNTAR),
   'max': (TAR + " | zstd -19 --long=27 -T24 -v -o {a}", "zstd -dq --long=27 -c {a} | " + UNTAR),
 },
 'tar.zst -22 --ultra': {
   '1':   (TAR + " | zstd -22 --ultra --long=31 -T1 -v -o {a}",
           "zstd -dq --long=31 -c {a} | " + UNTAR),
   'max': (TAR + " | zstd -22 --ultra --long=31 -T24 -v -o {a}",
           "zstd -dq --long=31 -c {a} | " + UNTAR),
 },
 'tar.xz -9e': {
   '1':   (TAR + " | xz -9e -T1 -vv -c > {a}",  "xz -dc -T1 {a} | " + UNTAR),
   'max': (TAR + " | xz -9e -T24 -vv -c > {a}", "xz -dc -T24 {a} | " + UNTAR),
 },
 'tar.br -q 11': {
   '1':   (TAR + " | brotli -q 11 -c > {a}", "brotli -dc {a} | " + UNTAR),
   'max': ('enkeltradad', 'brotli 1.2.0 has no threading at any quality'),
 },
 'nvz snabb': {
   '1':   (HZ + " pack {c} {a} -q --mode fast -j 1",  HZ + " unpack {a} {u} -q -j 1"),
   'max': (HZ + " pack {c} {a} -q --mode fast -j 24", HZ + " unpack {a} {u} -q -j 24"),
 },
 'nvz normal': {
   '1':   (HZ + " pack {c} {a} -q --mode normal -j 1",  HZ + " unpack {a} {u} -q -j 1"),
   'max': (HZ + " pack {c} {a} -q --mode normal -j 24", HZ + " unpack {a} {u} -q -j 24"),
 },
 'nvz liten': {
   '1':   (HZ + " pack {c} {a} -q --mode small -j 1",  HZ + " unpack {a} {u} -q -j 1"),
   'max': (HZ + " pack {c} {a} -q --mode small -j 24", HZ + " unpack {a} {u} -q -j 24"),
 },
}
ORDER = list(CONFIGS)

# What the tool says about its own parallelism, parsed from its log.
# Everything else reports nothing and is judged by cpu/wall alone.
TOOL_THREADS = [
    (re.compile(r'Reduced the number of threads from (\d+) to (\d+)'), 2),   # xz
    (re.compile(r'Using up to (\d+) threads'), 1),                           # xz
    (re.compile(r'Compressing with (\d+) worker threads?'), 1),              # zstd
]
ALGORITHM = {
 'tar (okomprimerad)': 'none (container only)', 'tar.gz -6': 'deflate (LZ77+Huffman)',
 'tar.gz -9 (pigz)': 'deflate, parallel', 'tar.bz2 -9': 'BWT+Huffman',
 'tar.zst -3 (dagens)': 'LZ77+tANS', 'tar.zst -19 --long': 'LZ77+tANS, 128M window',
 'tar.zst -22 --ultra': 'LZ77+tANS, 2G window', 'tar.xz -9e': 'LZMA2',
 'tar.br -q 11': 'brotli (LZ77+Huffman+dictionary)',
 'nvz snabb': 'hz: dedup+sort+zstd', 'nvz normal': 'hz: transforms+probe',
 'nvz liten': 'hz: + context mixing',
}


def cpu_busy_seconds() -> float:
    """Busy CPU seconds across all cores, for foreign-load detection."""
    f = Path('/proc/stat').read_text().split('\n')[0].split()
    v = [int(x) for x in f[1:]]
    ticks = os.sysconf('SC_CLK_TCK')
    return (sum(v) - v[3] - v[4]) / ticks          # total minus idle and iowait


def run_measured(cmd: str, log: Path) -> dict:
    """Run one command, charging exactly its own process subtree.

    os.wait4 rather than getrusage(RUSAGE_CHILDREN) deltas: ru_maxrss is
    a high-water mark that never resets, so one big pack would poison
    every later peak-memory figure in the same process. bash waits for
    every pipeline member, so a `tar | zstd` pipeline folds into this
    one rusage — which is why no command may contain `&`.
    """
    fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    busy0, t0 = cpu_busy_seconds(), time.monotonic()
    pid = os.posix_spawn('/bin/bash', ['bash', '-o', 'pipefail', '-c', cmd], os.environ,
                         file_actions=[(os.POSIX_SPAWN_DUP2, fd, 1),
                                       (os.POSIX_SPAWN_DUP2, fd, 2)])
    _, status, ru = os.wait4(pid, 0)
    wall, busy1 = time.monotonic() - t0, cpu_busy_seconds()
    os.close(fd)
    cpu = ru.ru_utime + ru.ru_stime
    return dict(wall=wall, user=round(ru.ru_utime, 2), sys=round(ru.ru_stime, 2),
                cpu=round(cpu, 2), maxrss_mib=round(ru.ru_maxrss / 1024, 1),
                exit=os.waitstatus_to_exitcode(status),
                foreign_cpu=round(max(0.0, (busy1 - busy0) - cpu), 1),
                log=log.read_text(errors='replace')[-4000:])


def tool_threads(log: str):
    for rx, group in TOOL_THREADS:
        hit = rx.search(log)
        if hit:
            return int(hit.group(group))
    return None


def free_gib() -> float:
    for line in Path('/proc/meminfo').read_text().split('\n'):
        if line.startswith('MemAvailable:'):
            return round(int(line.split()[1]) / 1048576, 1)
    return 0.0


def machine() -> dict:
    def sh(cmd):
        return subprocess.run(['bash', '-c', cmd], capture_output=True, text=True).stdout.strip()
    return dict(
        model=sh("lscpu | sed -n 's/^Model name: *//p'"),
        logical=os.cpu_count(),
        physical=int(sh("lscpu | sed -n 's/^Core(s) per socket: *//p'") or 0)
                 * int(sh("lscpu | sed -n 's/^Socket(s): *//p'") or 1),
        governor=sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null"),
        max_mhz=sh("lscpu | sed -n 's/^CPU max MHz: *//p'"),
        free_gib_at_start=free_gib(),
        kernel=sh("uname -r"),
        scratch=str(SCRATCH),
        scratch_fs=sh(f"df -T {SCRATCH} 2>/dev/null | awk 'NR==2{{print $2}}'"),
        versions={t: sh(f"{t} --version 2>&1 | head -1") for t in
                  ('gzip', 'pigz', 'bzip2', 'zstd', 'xz', 'brotli')},
        hz_version=sh(f"{HZ} -V"),
        hz_commit=sh("git -C %s rev-parse --short HEAD" % ROOT),
        measured=time.strftime('%Y-%m-%dT%H:%M:%S'),
    )


def measure_config(name, mode, corpus, cmds) -> dict:
    """One packer in one thread configuration: pack, unpack, verify."""
    SCRATCH.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r'[^a-z0-9]+', '-', f'{name}-{mode}'.lower()).strip('-')
    archive = SCRATCH / f'{slug}.ark'
    out = SCRATCH / 'out'
    for p in (archive,):
        p.unlink(missing_ok=True)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    fields = dict(c=str(corpus), cd=str(corpus.parent), cn=corpus.name,
                  a=str(archive), u=str(out))

    before = m.fingerprint(corpus)
    load = os.getloadavg()
    free0 = free_gib()

    pack = run_measured(cmds[0].format(**fields), SCRATCH / f'{slug}-pack.log')
    if pack['exit'] != 0:
        shutil.rmtree(out, ignore_errors=True); archive.unlink(missing_ok=True)
        return dict(failed=f"pack exit {pack['exit']}", log=pack['log'][-1500:])
    size = archive.stat().st_size

    unpack = run_measured(cmds[1].format(**fields), SCRATCH / f'{slug}-unpack.log')
    if unpack['exit'] != 0:
        shutil.rmtree(out, ignore_errors=True); archive.unlink(missing_ok=True)
        return dict(failed=f"unpack exit {unpack['exit']}", log=unpack['log'][-1500:])

    same = subprocess.run(['diff', '-rq', '--no-dereference', str(corpus),
                           str(out / corpus.name)], capture_output=True).returncode == 0
    stable = m.fingerprint(corpus) == before
    requested = 1 if mode == '1' else 24
    granted = tool_threads(pack['log']) or requested
    effective = round(pack['cpu'] / pack['wall'], 2) if pack['wall'] > 0.01 else None

    shutil.rmtree(out, ignore_errors=True)
    archive.unlink(missing_ok=True)
    return dict(
        bytes=size,
        packsek=round(pack['wall'], 2), uppacksek=round(unpack['wall'], 2),
        packcpu=pack['cpu'], uppackcpu=unpack['cpu'],
        packmaxrss_mib=pack['maxrss_mib'],
        threads_requested=requested, threads_granted=granted, threads_effective=effective,
        identisk=same, korpus_stabil=stable,
        free_gib=free0, loadavg=round(load[0], 2),
        foreign_cpu=pack['foreign_cpu'],
        disturbed=pack['foreign_cpu'] > max(2.0, 0.05 * pack['wall'] * 24),
        packkommando=cmds[0].format(**fields), uppackkommando=cmds[1].format(**fields),
    )


def derived(entry: dict) -> dict:
    one, mx = entry.get('konfig', {}).get('1'), entry.get('konfig', {}).get('max')
    if not one or not mx or 'bytes' not in one or 'bytes' not in mx:
        return {}
    speed = one['packsek'] / mx['packsek'] if mx['packsek'] else None
    dsize = 100 * (mx['bytes'] - one['bytes']) / one['bytes'] if one['bytes'] else 0
    eff = (mx['packcpu'] / (mx['packsek'] * mx['threads_granted'])
           if mx['packsek'] and mx['threads_granted'] else None)
    if abs(dsize) > 1.0:
        verdict = 'faster but a different archive'
    elif speed and speed > 1.2:
        verdict = 'scales'
    else:
        verdict = 'does not scale'
    return dict(speedup_pack=round(speed, 2) if speed else None,
                speedup_unpack=round(one['uppacksek'] / mx['uppacksek'], 2)
                               if mx['uppacksek'] else None,
                efficiency_pack=round(eff, 2) if eff else None,
                cpu_ratio_pack=round(mx['packcpu'] / one['packcpu'], 2) if one['packcpu'] else None,
                size_delta_pct=round(dsize, 2),
                verdict=verdict)


def audit(data: dict) -> list:
    """Invariants that must hold, or the numbers are not evidence."""
    notes = []
    for name, entry in data['matningar'].items():
        for mode, c in entry.get('konfig', {}).items():
            if 'bytes' not in c:
                continue
            if not c['identisk']:
                notes.append(f'{name}/{mode}: restore was NOT bit-exact')
            if not c['korpus_stabil']:
                notes.append(f'{name}/{mode}: the corpus changed during the measurement')
            if c['disturbed']:
                notes.append(f'{name}/{mode}: foreign load {c["foreign_cpu"]} cpu-s')
            if c['threads_effective'] and c['threads_effective'] > c['threads_granted'] + 0.5:
                notes.append(f'{name}/{mode}: cpu/wall {c["threads_effective"]} exceeds '
                             f'the granted {c["threads_granted"]} threads')
        d = entry.get('harledt', {})
        if d.get('speedup_pack') and d['speedup_pack'] < 0.95:
            notes.append(f'{name}: max threads was SLOWER ({d["speedup_pack"]}x) '
                         f'- expected when threading costs ratio, explain it')
        if d.get('speedup_pack') and d['speedup_pack'] > 14:
            notes.append(f'{name}: speedup {d["speedup_pack"]}x above the plausible '
                         f'ceiling for 12 physical cores')
    return notes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('corpus')
    ap.add_argument('rows', nargs='*', help='row keys; default all')
    ap.add_argument('--only', choices=['1', 'max'], help='measure one configuration')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--audit', action='store_true', help='check invariants, measure nothing')
    ap.add_argument('--force', action='store_true', help='re-measure rows already present')
    args = ap.parse_args()

    corpus = ROOT / 'corpus' / args.corpus
    outfile = ROOT / 'results' / f'threads-{args.corpus}.json'
    data = json.loads(outfile.read_text()) if outfile.exists() else {}

    if args.audit:
        notes = audit(data)
        print('\n'.join('  ' + n for n in notes) if notes else '  no complaints')
        print(f'{len(notes)} notes')
        return 1 if notes else 0

    if not corpus.is_dir():
        print(f'{corpus}: no such corpus', file=sys.stderr); return 1

    data.setdefault('schema', 'hz-threads-1')
    data['korpus'] = args.corpus
    data['rabyte'] = m.corpus_bytes(corpus)
    data['filer'] = sum(1 for p in corpus.rglob('*') if p.is_file())
    data['maskin'] = machine()
    data.setdefault('matningar', {})

    rows = args.rows or ORDER
    modes = [args.only] if args.only else ['1', 'max']
    print(f'{args.corpus}: {data["rabyte"]/1048576:.1f} MiB in {data["filer"]} files, '
          f'{data["maskin"]["free_gib_at_start"]} GiB free')

    for name in rows:
        if name not in CONFIGS:
            print(f'  unknown row: {name}'); continue
        entry = data['matningar'].setdefault(name, {'packare': name,
                                                    'algoritm': ALGORITHM.get(name, ''),
                                                    'konfig': {}})
        for mode in modes:
            spec = CONFIGS[name][mode]
            if isinstance(spec[0], str) and spec[0] in ('enkeltradad', 'saknas'):
                entry['konfig'][mode] = {'not_expressible': {'class': spec[0], 'why': spec[1]}}
                print(f'  {name:<22} {mode:>3}  — {spec[1][:60]}')
                continue
            if mode in entry['konfig'] and 'bytes' in entry['konfig'][mode] and not args.force:
                print(f'  {name:<22} {mode:>3}  already measured, skipping')
                continue
            if args.dry_run:
                print(f'  {name:<22} {mode:>3}  {spec[0][:100]}')
                continue
            r = measure_config(name, mode, corpus, spec)
            entry['konfig'][mode] = r
            if 'failed' in r:
                print(f'  {name:<22} {mode:>3}  FAILED: {r["failed"]}')
            else:
                print(f'  {name:<22} {mode:>3}  {r["bytes"]/1048576:8.2f} MiB  '
                      f'pack {r["packsek"]:7.1f} s  unpack {r["uppacksek"]:6.1f} s  '
                      f'cpu/wall {r["threads_effective"]}  '
                      f'{"exact" if r["identisk"] else "DIFFERS"}')
            entry['harledt'] = derived(entry)
            outfile.parent.mkdir(exist_ok=True)
            outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))

    notes = audit(data)
    data['granskning'] = {'invarianter_ok': not notes, 'anmarkningar': notes}
    outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))
    if notes:
        print('\naudit:'); print('\n'.join('  ' + n for n in notes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
