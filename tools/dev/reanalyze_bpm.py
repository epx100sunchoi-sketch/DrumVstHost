# beats.py와 같은 절차로, BPM을 주어진 값 주변에서만 찾아 beats.json을 다시 쓴다.
# usage: python reanalyze_bpm.py <tools 폴더> <original.wav> <beats.json> <bpm>
import importlib.util
import json
import os
import sys
import time


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main(argv):
    tools, source, destination, bpm = argv[1], argv[2], argv[3], float(argv[4])
    started = time.time()

    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly

    beats_mod = load('beats_mod', os.path.join(tools, 'beats.py'))
    gf = load('gridfit', os.path.join(tools, 'gridfit.py'))

    y, sr = sf.read(source, dtype='float32', always_2d=True)
    y = y.mean(axis=1)
    original, original_sr = y, sr
    duration = len(y) / sr

    if sr != gf.SR:
        g = np.gcd(int(sr), int(gf.SR))
        y = resample_poly(y, gf.SR // g, int(sr) // g).astype(np.float32)

    result = gf.analyze(y, prior_bpm=bpm, prior_sigma_oct=0.12,
                        bpm_range=(bpm * 0.9, bpm * 1.1), follow_drift=True)
    grid_beats = [float(b) for b in result['beats_drift']]
    grid_downbeats = [float(b) for b in result['downbeats']]
    beats, attack_shift = beats_mod.refine_to_attacks(original, original_sr, grid_beats)

    grid = np.array(grid_beats)
    downbeats = []

    for d in grid_downbeats:
        k = int(np.argmin(np.abs(grid - d)))
        downbeats.append(float(d + (beats[k] - grid_beats[k])))

    out = {
        'version': 1,
        'method': 'gridfit',
        'bpm': float(result['bpm']),
        'bpm_coarse': float(result['bpm_coarse']),
        'beats': beats,
        'downbeats': downbeats,
        'downbeat_phase': int(result['downbeat_phase']),
        'attack_shift_ms': round(attack_shift * 1000.0, 2),
        'duration': duration,
        'seconds': round(time.time() - started, 2),
    }

    tmp = destination + '.part'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(out, f)

    os.replace(tmp, destination)
    print(f"{out['bpm']:.3f} BPM, {len(beats)} beats, downbeat_phase {out['downbeat_phase']}, "
          f"attack_shift {out['attack_shift_ms']} ms, first beats {[round(b, 3) for b in beats[:6]]}, "
          f"first downbeats {[round(b, 3) for b in downbeats[:4]]}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
