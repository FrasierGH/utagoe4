"""Benchmark: build test cases, run every engine on them, score against the known vocal.

    python eval/run.py                                   # dev set: 3 synthetic songs
    python eval/run.py --set test                        # held-out synthetic songs
    python eval/run.py --stems DIR --set dev             # MUSDB18-HQ test folder: first 10 songs
    python eval/run.py --stems DIR --set test            # the other 40 (held out)
    python eval/run.py --scenarios clean,wow --engines 3.0,proto-eq
    python eval/run.py --stems DIR --set test --shard 0/8 ...   # 8 processes in parallel,
    python eval/run.py --stems DIR --set test --report          # then merge their results

Settings may only be tuned on the dev set; results are reported on the test set.

Engines: "3.0" is the real Utagoe Rip 3.0 engine (tests/engine_test, default
settings); "v4-cpp" is the C++ separation as the program runs it (tests/v4_cli: 4.3),
"v41-cpp" the same as 4.1 (v4_cli --v41), "v40-cpp" as 4.0, with 4.0's EQ passes and
3.0's keep-or-delete rule (v4_cli --hard);
"ceiling" is the best
achievable output where the target is approximate (MP3); the rest are prototypes
from proto.ENGINES. Cases and scores are cached in
eval/work/ under a hash of the code and settings that produced them (outputs are
only kept with --keep-audio). The 50 MUSDB songs' cases take about 8 GB; --prune
removes what older code versions left behind.
"""
from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import degrade  # noqa: E402
import metrics  # noqa: E402
import proto    # noqa: E402
import synth    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(HERE, 'work')
RATE = 44100

# 'fresh' was added after fixes prompted by failures in 'test'; three of its songs were
# then looked at while diagnosing fast drift. 'final' was added for 4.0's final numbers,
# 'final41' for 4.1's (and scored after each of 4.1's last revisions); 'holdout41' was
# added after them and scored once; 'holdout43' likewise for 4.3. Nothing was tuned on any
# of these.
SYNTH_SEEDS = {'dev': range(1000, 1003), 'test': range(2000, 2010), 'fresh': range(3000, 3020),
               'final': range(4000, 4020), 'final41': range(5000, 5020), 'holdout41': range(6000, 6020),
               'holdout43': range(7000, 7020)}
STEMS_SPLIT = {'dev': slice(0, 10), 'test': slice(10, None), 'fresh': slice(0, 0), 'final': slice(0, 0),
               'final41': slice(0, 0), 'holdout41': slice(0, 0), 'holdout43': slice(0, 0)}


def _hash(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode())
    return h.hexdigest()[:10]


def _src(name):
    with open(os.path.join(HERE, name), 'rb') as f:
        return f.read()


def find_exe(name):
    for p in (f'build/Release/{name}.exe', f'build/{name}.exe'):
        if os.path.exists(os.path.join(ROOT, p)):
            return os.path.join(ROOT, p)
    return None


def find_baseline():
    return find_exe('engine_test')


def load_stems(folder, seconds, start):
    """A song folder with vocals.wav plus accompaniment.wav (or drums/bass/other.wav)."""
    def rd(name):
        x, r = sf.read(os.path.join(folder, name), always_2d=True)
        if r != RATE:
            raise SystemExit(f'{folder}: {r} Hz (44100 Hz needed)')
        return x[:, :2] if x.shape[1] >= 2 else np.repeat(x, 2, 1)
    v = rd('vocals.wav')
    if os.path.exists(os.path.join(folder, 'accompaniment.wav')):
        i = rd('accompaniment.wav')
    else:
        i = sum(rd(f'{s}.wav') for s in ('drums', 'bass', 'other'))
    a = int(start * RATE) if len(v) > int((start + seconds) * RATE) else 0
    b = a + int(seconds * RATE)
    v, i = v[a:b], i[a:b]
    # same headroom as the synthetic songs (peaks at -12 dBFS), so the mastering
    # scenarios' EQ boost doesn't clip
    s = 0.25 / max(np.max(np.abs(v + i)), 1e-9)
    return v * s, i * s


def songs(args):
    if args.stems:
        dirs = [d for d in sorted(glob.glob(os.path.join(args.stems, '*'))) if os.path.exists(os.path.join(d, 'vocals.wav'))]
        dirs = dirs[STEMS_SPLIT[args.set]][: args.limit or None][args.shard_i::args.shard_n]
        for d in dirs:
            yield os.path.basename(d), lambda d=d: load_stems(d, args.seconds, args.start)
    else:
        seeds = list(SYNTH_SEEDS[args.set])[: args.limit or None][args.shard_i::args.shard_n]
        for s in seeds:
            yield f'synth{s}', lambda s=s: synth.song(s, args.seconds)


def short(name):
    """A filesystem-friendly folder name for a song title."""
    return ''.join(c if c.isalnum() else '_' for c in name)[:40]


def build_case(song_dir, scenario, make):
    d = os.path.join(song_dir, scenario)
    paths = {k: os.path.join(d, k + '.wav') for k in ('mix', 'kar', 'target', 'ceiling')}
    done = os.path.join(d, 'stats.json')
    if os.path.exists(done):
        try:
            with open(done) as f:
                return paths, json.load(f)
        except (OSError, ValueError):
            pass                        # interrupted (e.g. a full disk): build it again
    os.makedirs(d, exist_ok=True)
    v, i = make()
    c = degrade.SCENARIOS[scenario](v, i)
    for k, x in (('mix', c.mix), ('kar', c.kar)):
        if np.max(np.abs(x)) > 1:
            raise SystemExit(f'{d}: {k} clips')
        sf.write(paths[k], x, RATE, subtype='PCM_16')   # what users feed Utagoe Rip
    # 24-bit: a -144 dB floor is far below anything measured, at 3/4 of the size of float
    sf.write(paths['target'], c.target, RATE, subtype='PCM_24')
    if c.ceiling is not None:
        sf.write(paths['ceiling'], c.ceiling, RATE, subtype='PCM_24')
    with open(done, 'w') as f:          # written last: marks the case complete
        json.dump(c.stats, f)
    return paths, c.stats


def engine_version(name):
    if name == '3.0':
        exe = find_baseline()
        if not exe:
            raise SystemExit('build the 3.0 engine first: cmake -S . -B build -A x64 && '
                             'cmake --build build --config Release --target engine_test')
        with open(exe, 'rb') as f:
            return _hash(f.read())
    if name == 'ceiling':
        return 'case'
    if name in CPP_ARGS:
        exe = find_exe('v4_cli')
        if not exe:
            raise SystemExit('build the C++ v4 first: cmake --build build --config Release --target v4_cli')
        with open(exe, 'rb') as f:
            return _hash(f.read(), CPP_ARGS[name])
    return _hash(_src('proto.py'), sorted(proto.ENGINES[name].items()))


# the C++ separation's configurations (tests/v4_cli arguments)
CPP_ARGS = {'v4-cpp': [], 'v41-cpp': ['--v41'], 'v40-cpp': ['--hard']}


def run_engine(name, case):
    """The engine's output for a case, or None if the engine doesn't apply."""
    if name == 'ceiling':
        if not os.path.exists(case['ceiling']):
            return None
        return sf.read(case['ceiling'], always_2d=True)[0]
    if name == '3.0' or name in CPP_ARGS:
        exe = find_baseline() if name == '3.0' else find_exe('v4_cli')
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'out.wav')
            subprocess.run([exe, case['mix'], case['kar'], out] + CPP_ARGS.get(name, []), check=True, capture_output=True)
            return sf.read(out, always_2d=True)[0]
    mix, _ = sf.read(case['mix'], always_2d=True)
    kar, _ = sf.read(case['kar'], always_2d=True)
    return proto.separate(mix, kar, RATE, **proto.ENGINES[name])


def score_case(name, case, target, mix, out_base, keep_audio):
    """Scores (cached as JSON next to `out_base`), or None if the engine doesn't apply."""
    js = out_base + '.json'
    if os.path.exists(js):
        try:
            with open(js) as f:
                return json.load(f)
        except (OSError, ValueError):
            pass
    est = run_engine(name, case)
    if est is None:
        return None
    sc = metrics.score(target, est, mix)
    os.makedirs(os.path.dirname(js), exist_ok=True)
    if keep_audio:
        sf.write(out_base + '.wav', est, RATE, subtype='FLOAT')
    with open(js, 'w') as f:
        json.dump(sc, f)
    return sc


def prune(keep):
    """Delete cached cases and scores whose version folder is not in `keep`."""
    import shutil
    for top in ('cases', 'out'):
        root = os.path.join(WORK, top)
        if not os.path.isdir(root):
            continue
        for d in os.listdir(root):
            if d not in keep:
                shutil.rmtree(os.path.join(root, d))
                print('pruned', top + '/' + d)


def _length_tag(args):
    """Results of other excerpt lengths are kept apart from the default 30 s ones."""
    return '' if (args.seconds, args.start) == (30.0, 30.0) else f'_len{args.seconds:g}from{args.start:g}'


def bootstrap_ci(d, n=4000, seed=0):
    if len(d) < 2:
        return float('nan'), float('nan')
    rng = np.random.default_rng(seed)
    m = rng.choice(d, (n, len(d))).mean(axis=1)
    return float(np.percentile(m, 2.5)), float(np.percentile(m, 97.5))


def summarize(rows, scenarios, engines, metric, ref):
    lower = metric == 'leak'
    rows = [r for r in rows if np.isfinite(r[metric])]      # e.g. leak needs a pause in the vocal
    print(f'\n{metric} (dB, mean over the songs every engine has, {len({r["song"] for r in rows})} in all; '
          f'{"lower" if lower else "higher"} is better)\n')
    print('| scenario | ' + ' | '.join(engines) + ' |')
    print('|---|' + '---:|' * len(engines))
    for sc in scenarios:
        # every engine averaged over the same songs: those all of them have a score for
        have = [{r['song'] for r in rows if r['scenario'] == sc and r['engine'] == en} for en in engines]
        common = set.intersection(*[h for h in have if h]) if any(have) else set()
        cells = []
        for en in engines:
            v = [r[metric] for r in rows if r['scenario'] == sc and r['engine'] == en and r['song'] in common]
            cells.append(f'{np.mean(v):.2f}' if v else '')
        print(f'| {sc} | ' + ' | '.join(cells) + ' |')
    if ref not in engines:
        return
    others = [e for e in engines if e not in (ref, 'ceiling')]
    print(f'\ndifference from {ref} per song, positive = better: mean [95% CI] wins/songs\n')
    print('| scenario | ' + ' | '.join(others) + ' |')
    print('|---|' + '---:|' * len(others))
    for sc in scenarios:
        base = {r['song']: r[metric] for r in rows if r['scenario'] == sc and r['engine'] == ref}
        cells = []
        for en in others:
            d = np.array([r[metric] - base[r['song']] for r in rows
                          if r['scenario'] == sc and r['engine'] == en and r['song'] in base])
            if lower:
                d = -d
            lo, hi = bootstrap_ci(d)
            cells.append(f'{d.mean():+.2f} [{lo:+.2f}, {hi:+.2f}] {int(np.sum(d > 0))}/{len(d)}' if len(d) else '')
        print(f'| {sc} | ' + ' | '.join(cells) + ' |')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--set', choices=list(SYNTH_SEEDS), default='dev')
    ap.add_argument('--stems', help='a folder of real multitrack songs (e.g. MUSDB18-HQ test/)')
    ap.add_argument('--limit', type=int, default=0, help='only the first N songs of the set')
    ap.add_argument('--seconds', type=float, default=30.0)
    ap.add_argument('--start', type=float, default=30.0, help='excerpt start for --stems (s)')
    ap.add_argument('--scenarios', default=','.join(degrade.SCENARIOS))
    ap.add_argument('--engines', default=','.join(['3.0', 'ceiling'] + list(proto.ENGINES)))
    ap.add_argument('--metric', default='median_sdr', choices=['median_sdr', 'sdr', 'si_sdr', 'leak'])
    ap.add_argument('--ref', default='3.0', help='engine the per-song differences are taken against')
    ap.add_argument('--quiet', action='store_true', help='only print the summary')
    ap.add_argument('--shard', default='0/1', help='i/n: run every n-th song starting at i')
    ap.add_argument('--report', action='store_true', help="don't run; summarize the saved results (all shards)")
    ap.add_argument('--keep-audio', action='store_true', help='also keep every output as a WAV (large)')
    ap.add_argument('--prune', action='store_true',
                    help='delete cached cases and scores made by other code versions, then exit')
    args = ap.parse_args()
    args.shard_i, args.shard_n = (int(x) for x in args.shard.split('/'))
    scenarios = args.scenarios.split(',')
    engines = args.engines.split(',')
    tag = 'stems' if args.stems else 'synth'
    keys = ('median_sdr', 'sdr', 'si_sdr', 'leak')
    if args.report:
        # only rows made by the current code of each engine; among those, the newest file
        # wins for every (song, scenario, engine)
        current = {en: engine_version(en) for en in engines}
        latest, stale = {}, 0
        base = os.path.join(WORK, f'results_{tag}_{args.set}{_length_tag(args)}')
        paths = glob.glob(base + '.csv') + glob.glob(base + '_shard*.csv')
        for path in sorted(paths, key=os.path.getmtime):
            with open(path, newline='') as f:
                for r in csv.DictReader(f):
                    if r['engine'] in engines and r['scenario'] in scenarios:
                        if r.get('version') != current[r['engine']]:
                            stale += 1
                            continue
                        latest[(r['song'], r['scenario'], r['engine'])] = {**r, **{k: float(r[k]) for k in keys}}
        if stale:
            print(f'note: {stale} saved rows from other code versions ignored')
        summarize(list(latest.values()), scenarios, engines, args.metric, args.ref)
        return
    case_ver = _hash(_src('degrade.py'), _src('synth.py'), args.seconds, args.start, tag)
    versions = {en: engine_version(en) for en in engines}
    if args.prune:
        # every engine's current version, for both kinds of songs
        keep = {f'{t}-{_hash(_src("degrade.py"), _src("synth.py"), args.seconds, args.start, t)}' for t in ('synth', 'stems')}
        keep |= {f'{en}-{engine_version(en)}' for en in ['3.0', 'ceiling'] + list(CPP_ARGS) + list(proto.ENGINES)}
        prune(keep)
        return

    rows, nan = [], 0
    for song, make in songs(args):
        song_dir = os.path.join(WORK, 'cases', f'{tag}-{case_ver}', short(song))
        for sc in scenarios:
            case, stats = build_case(song_dir, sc, make)
            target, _ = sf.read(case['target'], always_2d=True)
            mix, _ = sf.read(case['mix'], always_2d=True)
            for en in engines:
                out = os.path.join(WORK, 'out', f'{en}-{versions[en]}', f'{tag}-{case_ver}', short(song), sc)
                t0 = time.time()
                scores = score_case(en, case, target, mix, out, args.keep_audio)
                if scores is None:
                    continue
                r = dict(song=song, scenario=sc, engine=en, version=versions[en], **scores)
                nan += sum(1 for k in ('median_sdr', 'sdr', 'si_sdr', 'leak') if not np.isfinite(r[k]))
                rows.append(r)
                if not args.quiet:
                    print(f'{song[:24]:>24} {sc:>12} {en:>16}  median SDR {r["median_sdr"]:6.2f}  SDR {r["sdr"]:6.2f}'
                          f'  leak {r["leak"]:7.2f}  ({time.time() - t0:.1f} s)', flush=True)

    os.makedirs(WORK, exist_ok=True)
    suffix = f'_shard{args.shard_i}of{args.shard_n}' if args.shard_n > 1 else ''
    with open(os.path.join(WORK, f'results_{tag}_{args.set}{_length_tag(args)}{suffix}.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    if nan:
        print(f'\nnote: {nan} scores are undefined (e.g. leak when the vocal never pauses); '
              'each metric skips only its own')
    summarize(rows, scenarios, engines, args.metric, args.ref)


if __name__ == '__main__':
    main()
