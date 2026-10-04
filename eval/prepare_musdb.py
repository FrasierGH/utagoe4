"""Unpack the MUSDB18-HQ test set into the layout eval/run.py --stems expects.

    python eval/prepare_musdb.py path/to/musdb18hq.zip DEST [--md5 HASH] [--all] [--delete-zip]

Keeps vocals/drums/bass/other.wav of each test song (the benchmark makes its own
mixes, so mixture.wav is skipped). --all keeps the training songs too. MUSDB18-HQ:
https://zenodo.org/records/3338373 (non-commercial use only; never commit it).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import zipfile

STEMS = ('vocals.wav', 'drums.wav', 'bass.wav', 'other.wav')


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 24), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('zip')
    ap.add_argument('dest')
    ap.add_argument('--md5', help='expected MD5 of the zip')
    ap.add_argument('--all', action='store_true', help='train and test songs')
    ap.add_argument('--delete-zip', action='store_true', help='delete the zip after a successful extraction')
    args = ap.parse_args()

    if args.md5:
        got = md5(args.zip)
        if got != args.md5.lower():
            sys.exit(f'MD5 mismatch: {got} (expected {args.md5})')
        print('MD5 ok')

    n_songs = 0
    with zipfile.ZipFile(args.zip) as z:
        for info in z.infolist():
            parts = info.filename.replace('\\', '/').strip('/').split('/')
            # .../{train,test}/<song>/<stem>.wav
            if len(parts) < 3 or parts[-1] not in STEMS:
                continue
            subset, song, stem = parts[-3], parts[-2], parts[-1]
            if subset not in ('train', 'test') or (subset == 'train' and not args.all):
                continue
            out_dir = os.path.join(args.dest, subset, song)
            out = os.path.join(out_dir, stem)
            if os.path.exists(out) and os.path.getsize(out) == info.file_size:
                continue
            os.makedirs(out_dir, exist_ok=True)
            with z.open(info) as src, open(out + '.part', 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 24)
            os.replace(out + '.part', out)
            if stem == STEMS[0]:
                n_songs += 1
                print(f'{subset}/{song}', flush=True)

    # every kept song must be complete before the zip may go
    for subset in ('train', 'test'):
        root = os.path.join(args.dest, subset)
        if not os.path.isdir(root):
            continue
        for song in os.listdir(root):
            missing = [s for s in STEMS if not os.path.exists(os.path.join(root, song, s))]
            if missing:
                sys.exit(f'{subset}/{song} is missing {missing}; zip kept')
    print('extracted', n_songs, 'songs into', args.dest)
    if args.delete_zip:
        os.remove(args.zip)
        print('deleted', args.zip)


if __name__ == '__main__':
    main()
