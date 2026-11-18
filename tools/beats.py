"""원곡의 박자(초)·마디 첫 박·BPM을 찾아 JSON으로 쓴다. DrumVstHost의 메트로놈이 쓴다.

usage: python -I -X utf8 -u beats.py <원곡.wav> <beats.json>

방법은 gridfit.py(같은 폴더): 대역별 시작점 → 자기상관으로 대략의 템포 → 곡 전체에 고정 격자를 맞춰 템포·박 위치를
정밀하게 → 8박 창으로 느린 흔들림 따라가기 → 킥(저음) 시작점으로 마디 첫 박.
2026-11-18 이 PC의 정답이 있는 5곡(94·114·114·125·140 BPM)에서 BPM 오차 0.005 이하, 박 F=1.000, 마디 첫 박 5곡 모두 맞음.
진행률은 "NN%|" 꼴로 출력한다(앱이 읽는다).
"""
import importlib.util
import json
import os
import sys
import time


def progress(percent, text=''):
    print(f'{percent:3d}%| {text}', flush=True)


def refine_to_attacks(y, sr, beats, search=0.03, smooth=8):
    """격자로 찾은 박을 원곡의 실제 어택(소리가 갑자기 커지는 순간)에 맞춘다.

    gridfit은 46ms 창의 가운데 시각을 쓰므로 박이 실제 타격보다 10~17ms 이르게 나온다(2026-11-18 검토).
    3ms 창(128샘플, 32샘플 간격)의 고역 에너지가 가장 가파르게 오르는 곳을 박마다 ±30ms 안에서 찾고,
    앞뒤 8박의 중앙값만큼만 옮긴다(박 하나하나의 잡음 대신 곡 전체의 일정한 어긋남을 고친다).
    """
    import numpy as np

    x = np.diff(y, prepend=y[:1]).astype(np.float64)   # 고역 강조: 북·심벌의 어택이 두드러진다
    win, hop = 128, 32
    n = (len(x) - win) // hop

    if n < 4 or len(beats) < 4:
        return list(beats), 0.0

    c = np.concatenate([[0.0], np.cumsum(x * x)])
    starts = np.arange(n) * hop
    energy = c[starts + win] - c[starts]
    level = np.log10(energy + 1e-12)
    rise = np.maximum(0.0, level[1:] - level[:-1])
    # 창의 앞쪽 끝이 어택에 닿는 순간 오르므로, 오른 프레임의 창 끝 시각에서 반 간격을 빼면 어택 시각이다.
    rise_time = (starts[1:] + win - hop / 2) / sr

    shifts = []

    for b in beats:
        lo = np.searchsorted(rise_time, b - search)
        hi = np.searchsorted(rise_time, b + search)

        if hi - lo < 3:
            shifts.append(np.nan)
            continue

        i = lo + int(np.argmax(rise[lo:hi]))
        shifts.append(rise_time[i] - b if rise[i] > 0.15 else np.nan)   # 뚜렷한 어택이 없으면 쓰지 않는다

    shifts = np.array(shifts)
    valid = ~np.isnan(shifts)

    if valid.sum() < max(4, len(beats) // 4):
        return list(beats), 0.0

    overall = float(np.median(shifts[valid]))
    refined = []

    for k, b in enumerate(beats):
        window = shifts[max(0, k - smooth): k + smooth + 1]
        window = window[~np.isnan(window)]
        refined.append(float(b + (np.median(window) if len(window) >= 3 else overall)))

    return refined, overall


def load_gridfit():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location('gridfit', os.path.join(here, 'gridfit.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv):
    if len(argv) < 3:
        print('usage: beats.py <audio.wav> <beats.json>', file=sys.stderr)
        return 2

    source, destination = argv[1], argv[2]
    started = time.time()
    progress(5, 'loading')

    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly

    y, sr = sf.read(source, dtype='float32', always_2d=True)
    y = y.mean(axis=1)
    original = y          # 어택 맞추기는 원래 샘플 속도로 한다
    original_sr = sr
    duration = len(y) / sr

    if duration < 5.0:
        print('error: audio is too short', file=sys.stderr)
        return 3

    gf = load_gridfit()
    progress(25, 'resampling')

    if sr != gf.SR:
        g = np.gcd(int(sr), int(gf.SR))
        y = resample_poly(y, gf.SR // g, int(sr) // g).astype(np.float32)   # 대칭 필터라 시각이 밀리지 않는다

    if np.max(np.abs(y)) < 1e-4:
        print('error: audio is silent', file=sys.stderr)
        return 4

    progress(45, 'analysing')
    result = gf.analyze(y, follow_drift=True)
    grid_beats = [float(b) for b in result['beats_drift']]
    grid_downbeats = [float(b) for b in result['downbeats']]

    if len(grid_beats) < 8:
        print('error: no steady beat found', file=sys.stderr)
        return 5

    progress(85, 'aligning to attacks')
    beats, attack_shift = refine_to_attacks(original, original_sr, grid_beats)

    # 마디 첫 박은 가장 가까운 박과 같은 만큼 옮긴다.
    grid = np.array(grid_beats)
    downbeats = []

    for d in grid_downbeats:
        k = int(np.argmin(np.abs(grid - d)))
        downbeats.append(float(d + (beats[k] - grid_beats[k])))

    progress(95, 'writing')
    out = {
        'version': 1,
        'method': 'gridfit',
        'bpm': float(result['bpm']),
        'bpm_coarse': float(result['bpm_coarse']),
        'beats': beats,
        'downbeats': downbeats,
        'downbeat_phase': int(result['downbeat_phase']),
        'attack_shift_ms': round(attack_shift * 1000.0, 2),   # 격자를 실제 어택에 맞추느라 옮긴 양(중앙값)
        'duration': duration,
        'seconds': round(time.time() - started, 2),
    }

    tmp = destination + '.part'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(out, f)

    os.replace(tmp, destination)
    progress(100, f"{out['bpm']:.2f} BPM, {len(beats)} beats")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
