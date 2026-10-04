"""Benchmark: build test cases, run every engine on them, score against the known vocal.

    python eval/run.py                          # synthetic songs, all scenarios, all engines
    python eval/run.py --songs 2 --scenarios clean,master_eq --engines 3.0,proto-perbin
    python eval/run.py --stems path/to/stems    # real multitracks (see eval/README.md)

The baseline "3.0" is the real Utagoe Rip 3.0 engine (tests/engine_test, default
settings). Everything is cached under eval/work/; delete it to rebuild.
"""
from __future__ import annotations

import argparse
import csv
import glob
import os
import subprocess
import sys
import time

import numpy as np
import soundfile as sf

sys.path.insert(0, os.path.dirname(__file__))
import degrade  # noqa: E402
import metrics  # noqa: E402
import proto    # noqa: E402
import synth    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(HERE, 'work')
RATE = 44100


def find_baseline():
    for p in ('build/Release/engine_test.exe', 'build/engine_test.exe'):
        if os.path.exists(os.path.join(ROOT, p)):
            return os.path.join(ROOT, p)
    return None


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
    return v[a:b], i[a:b]


def songs(args):
    if args.stems:
        for d in sorted(glob.glob(os.path.join(args.stems, '*'))):
            if os.path.exists(os.path.join(d, 'vocals.wav')):
                yield os.path.basename(d), lambda d=d: load_stems(d, args.seconds, args.start)
    else:
        for s in range(args.songs):
            yield f'synth{s}', lambda s=s: synth.song(1000 + s, args.seconds)


def build_case(song_dir, scenario, make):
    d = os.path.join(song_dir, scenario)
    paths = {k: os.path.join(d, k + '.wav') for k in ('mix', 'kar', 'target')}
    if all(os.path.exists(p) for p in paths.values()):
        return paths
    os.makedirs(d, exist_ok=True)
    v, i = make()
    mix, kar, target = degrade.SCENARIOS[scenario](v, i)
    for k, x in (('mix', mix), ('kar', kar)):
        if np.max(np.abs(x)) > 1:
            raise SystemExit(f'{d}: {k} clips')
        sf.write(paths[k], x, RATE, subtype='PCM_16')   # what users feed Utagoe Rip
    sf.write(paths['target'], target, RATE, subtype='FLOAT')
    return paths


def run_engine(name, case, out):
    if os.path.exists(out):
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if name == '3.0':
        exe = find_baseline()
        if not exe:
            raise SystemExit('build the 3.0 engine first: cmake -S . -B build && cmake --build build --config Release')
        subprocess.run([exe, case['mix'], case['kar'], out], check=True, capture_output=True)
    else:
        mix, _ = sf.read(case['mix'], always_2d=True)
        kar, _ = sf.read(case['kar'], always_2d=True)
        y = proto.separate(mix, kar, RATE, **proto.ENGINES[name])
        sf.write(out, y, RATE, subtype='FLOAT')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--songs', type=int, default=3, help='number of synthetic songs')
    ap.add_argument('--seconds', type=float, default=30.0)
    ap.add_argument('--stems', help='folder of real multitrack songs instead of synthetic ones')
    ap.add_argument('--start', type=float, default=30.0, help='excerpt start for --stems (s)')
    ap.add_argument('--scenarios', default=','.join(degrade.SCENARIOS))
    ap.add_argument('--engines', default=','.join(['3.0'] + list(proto.ENGINES)))
    ap.add_argument('--metric', default='median_sdr', choices=['median_sdr', 'sdr', 'si_sdr', 'bleed'])
    args = ap.parse_args()
    scenarios = args.scenarios.split(',')
    engines = args.engines.split(',')
    tag = 'stems' if args.stems else 'synth'

    rows = []
    for song, make in songs(args):
        song_dir = os.path.join(WORK, 'cases', tag, song)
        for sc in scenarios:
            case = build_case(song_dir, sc, make)
            target, _ = sf.read(case['target'], always_2d=True)
            mix, _ = sf.read(case['mix'], always_2d=True)
            for en in engines:
                out = os.path.join(WORK, 'out', en, tag, song, sc + '.wav')
                t0 = time.time()
                run_engine(en, case, out)
                est, _ = sf.read(out, always_2d=True)
                r = dict(song=song, scenario=sc, engine=en, **metrics.score(target, est, mix))
                rows.append(r)
                print(f'{song:>10} {sc:>12} {en:>20}  median SDR {r["median_sdr"]:6.2f} dB'
                      f'  SDR {r["sdr"]:6.2f}  SI-SDR {r["si_sdr"]:6.2f}  bleed {r["bleed"]:7.2f}'
                      f'  ({time.time() - t0:.1f} s)', flush=True)

    os.makedirs(WORK, exist_ok=True)
    with open(os.path.join(WORK, f'results_{tag}.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    # summary: mean over songs
    m = args.metric
    print(f'\n{m} (dB, mean over songs; {"lower" if m == "bleed" else "higher"} is better)\n')
    print('| scenario | ' + ' | '.join(engines) + ' |')
    print('|---|' + '---:|' * len(engines))
    for sc in scenarios + ['mean']:
        sel = [r for r in rows if sc == 'mean' or r['scenario'] == sc]
        vals = [np.nanmean([r[m] for r in sel if r['engine'] == en]) for en in engines]
        print(f'| {sc} | ' + ' | '.join(f'{v:.2f}' for v in vals) + ' |')


if __name__ == '__main__':
    main()
