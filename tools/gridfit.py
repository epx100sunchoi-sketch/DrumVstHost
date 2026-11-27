"""'gridfit': a beat / tempo / downbeat estimator written with plain numpy so every step maps 1:1 onto C++ (JUCE dsp::FFT + loops).

Steps
 1. STFT (Hann 1024, hop 256 at 22.05 kHz = 11.6 ms), log magnitude log(1 + 100|X|).
 2. Spectral-flux novelty per band: full, LF 30-150 Hz (kick), MID 150-2000 Hz (snare body), HF 5-11 kHz (cymbals);
    each minus its 0.5 s moving average, half-wave rectified, divided by its 99th percentile.
 3. Coarse tempo: autocorrelation of (full+LF) novelty; score S(bpm) = AC(tau) + 0.5 AC(2 tau) + 0.25 AC(4 tau)  (beat, half bar,
    bar), times a log-normal prior centred on 120 BPM (sigma 0.7 octave), searched 60-200 BPM in 0.1 BPM steps.
 4. Fine tempo + beat phase: maximise sum of novelty (max over +-1 frame) on the constant beat grid T + k P over the whole song;
    stage 1 P +-3 % in 0.05 % steps, phase in 1-frame steps; stage 2 P +-0.06 % in 0.002 % steps, phase in 1/4-frame steps.
 5. Optional drift follow: 8-beat windows (hop 4 beats), local offset searched +-30 ms around the previous window's offset,
    5-window median smoothing, interpolated per beat.
 6. Downbeat: for each of the 4 bar positions p, mean over beats k = p (mod 4) of
      LF  = kick-band onset at the beat,  CC = chord change (1 - cos of beat-synchronous chroma before/after the beat),
      ACC = full-band onset at the beat,  HF = cymbal-band onset at the beat;
    each cue z-scored over the 4 positions; default decision = argmax(zLF + zCC).
"""
import time
import numpy as np

SR = 22050
NFFT = 1024
HOP = 256
FPS = SR / HOP


def stft_mag(y, n_fft=NFFT, hop=HOP):
    win = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n_fft) / n_fft)).astype(np.float32)
    pad = n_fft // 2
    yp = np.concatenate([np.zeros(pad, np.float32), y.astype(np.float32), np.zeros(pad, np.float32)])
    nfr = 1 + (len(yp) - n_fft) // hop
    out = np.empty((nfr, n_fft // 2 + 1), np.float32)
    base = np.arange(n_fft)[None, :]
    for a in range(0, nfr, 2048):
        b = min(nfr, a + 2048)
        idx = (np.arange(a, b) * hop)[:, None] + base
        out[a:b] = np.abs(np.fft.rfft(yp[idx] * win, axis=1))
    return out


def moving_avg(x, n):
    n = max(1, int(n))
    c = np.cumsum(np.concatenate([[0.0], x]))
    h = n // 2
    i0 = np.clip(np.arange(len(x)) - h, 0, len(x))
    i1 = np.clip(np.arange(len(x)) + h + 1, 0, len(x))
    return (c[i1] - c[i0]) / (i1 - i0)


def novelty(Lm, fr, lo, hi):
    sel = (fr >= lo) & (fr < hi)
    X = Lm[:, sel]
    d = np.maximum(np.diff(X, axis=0, prepend=X[:1]), 0).sum(1)
    z = np.maximum(d - moving_avg(d, 0.5 * FPS), 0)
    return z / (np.percentile(z, 99) + 1e-9)


def melbank(fr, nb=40, lo=30.0, hi=11000.0):
    m = lambda f: 2595 * np.log10(1 + f / 700.0)
    im = lambda x: 700.0 * (10 ** (x / 2595.0) - 1)
    edges = im(np.linspace(m(lo), m(hi), nb + 2))
    W = np.zeros((nb, len(fr)), np.float32)
    for i in range(nb):
        a, c, b = edges[i], edges[i + 1], edges[i + 2]
        W[i] = np.clip(np.minimum((fr - a) / (c - a), (b - fr) / (b - c)), 0, None)
    return W


def mel_novelty(M, fr):
    E = np.log1p(100.0 * (M @ melbank(fr).T))
    d = np.maximum(np.diff(E, axis=0, prepend=E[:1]), 0).sum(1)
    z = np.maximum(d - moving_avg(d, 0.5 * FPS), 0)
    return z / (np.percentile(z, 99) + 1e-9)


def autocorr(x, maxlag):
    x = x - x.mean()
    n = len(x)
    F = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(F * np.conj(F))[:maxlag + 1]
    return ac / (ac[0] + 1e-12)


def maxpool3(x):
    p = np.concatenate([[x[0]], x, [x[-1]]])
    return np.maximum(np.maximum(p[:-2], p[1:-1]), p[2:])


def grid_scores(pooled, P, phases, k0, k1):
    """sum of pooled novelty at phase + k*P for k in [k0, k1) (frames, fractional, linear interpolation)."""
    k = np.arange(k0, k1)
    pos = phases[:, None] + k[None, :] * P
    pos = np.clip(pos, 0, len(pooled) - 1.001)
    i = pos.astype(int)
    f = pos - i
    v = pooled[i] * (1 - f) + pooled[i + 1] * f
    return v.sum(1)


def chroma_frames(M, fr):
    sel = (fr >= 65) & (fr < 2100)
    pc = (np.round(12 * np.log2(fr[sel] / 440.0)).astype(int) + 9) % 12   # 0 = C
    C = np.zeros((M.shape[0], 12), np.float32)
    E = M[:, sel] ** 2
    for q in range(12):
        C[:, q] = E[:, pc == q].sum(1)
    return C


def analyze(y, sr=SR, prior_bpm=120.0, prior_sigma_oct=0.7, bpm_range=(60, 200), follow_drift=True, return_debug=False,
            w2=1.0, w4=1.0, downbeat_rule='LF'):
    assert sr == SR
    t0 = time.time()
    tim = {}
    y = y / (np.max(np.abs(y)) + 1e-9)
    M = stft_mag(y)
    fr = np.fft.rfftfreq(NFFT, 1 / SR)
    Lm = np.log1p(100.0 * M)
    nov = {name: novelty(Lm, fr, lo, hi) for name, (lo, hi) in
           {'lf': (30, 150), 'mid': (150, 2000), 'hf': (5000, 11000)}.items()}
    nov['full'] = mel_novelty(M, fr)
    tim['features'] = time.time() - t0
    # NB: the kick band is NOT used for the grid: in 4 of 5 test mixes its onsets are stronger on the 8th off-beat
    # (bass / kick on the "&"); mel-weighted full band and the 150-2000 Hz snare band favour the beat in every song.
    fit = nov['full'] + nov['mid']
    pooled = maxpool3(fit)
    n = len(fit)

    # --- coarse tempo
    t1 = time.time()
    lo, hi = bpm_range
    maxlag = int(4 * 60.0 / lo * FPS) + 3
    ac = autocorr(fit, maxlag)
    bpms = np.arange(lo, hi + 1e-9, 0.1)
    tau = 60.0 / bpms * FPS
    lag = np.arange(len(ac))
    # beat, half-bar and bar lags weighted equally: the beat period must be consistent with a 4-beat bar. With weights
    # (1, .5, .25) the syncopated drum stems of Sunnyside Cruise locked onto the dotted 8th (152 BPM = 4/3 x 114).
    S = np.interp(tau, lag, ac) + w2 * np.interp(2 * tau, lag, ac) + w4 * np.interp(4 * tau, lag, ac)
    w = np.exp(-0.5 * (np.log2(bpms / prior_bpm) / prior_sigma_oct) ** 2)
    b0 = float(bpms[np.argmax(S * w)])
    tim['coarse_tempo'] = time.time() - t1

    # --- fine tempo and phase (constant grid over whole song)
    t2 = time.time()
    act = np.flatnonzero(fit > 0.05)
    f_first, f_last = (act[0], act[-1]) if len(act) else (0, n - 1)

    def search(Ps, phase_list):
        best = (-1, None, None)
        for P in Ps:
            phases = phase_list(P)
            k0 = int(np.ceil((f_first - phases.max()) / P))
            k1 = int(np.floor((f_last - phases.min()) / P)) + 1
            sc = grid_scores(pooled, P, phases, max(k0, 0), k1) / max(1, k1 - max(k0, 0))
            j = int(np.argmax(sc))
            if sc[j] > best[0]:
                best = (float(sc[j]), float(P), float(phases[j]))
        return best

    P0 = 60.0 / b0 * FPS
    s1, P1, ph1 = search(P0 * (1 + np.arange(-0.03, 0.03 + 1e-9, 0.0005)), lambda P: np.arange(0, P, 1.0))
    s2, P2, ph2 = search(P1 * (1 + np.arange(-0.0006, 0.0006 + 1e-12, 0.00002)), lambda P: ph1 + np.arange(-5, 5.001, 0.25))
    tim['fine_fit'] = time.time() - t2

    # --- 3:2 tempo-family check. A grid at 2/3 (or 3/2) of the true tempo spaces its beats three (two) true
    # 8ths apart, so every grid beat still lands on a real onset and the fit score alone cannot tell them apart
    # (RESCENE "Pinball", true 170 BPM, locked onto 113.33 = 2/3 x 170 because the coarse prior favours 120).
    # What does tell them apart: the half-beat positions. On the true grid the half-beats are the song's 8ths
    # (onsets in any straight-8ths song); on the 2/3-locked grid they fall 1.5 true 8ths apart, between onsets.
    # So refit at x1.5 and x2/3 and compare (beat + half-beat) novelty; switch only when the candidate is
    # clearly better and its beats hit onsets about as well. Octave errors (x2, x1/2) are left alone: they stay
    # on the beat, and the app's metronome has double / half speed for them.
    t2b = time.time()

    def grid_stats(P, ph):
        kk = np.arange(max(0, int(np.ceil((f_first - ph) / P))), int(np.floor((f_last - ph) / P)) + 1)
        if len(kk) < 8:
            return 0.0, 0.0

        def mean_at(positions):
            pos = np.clip(positions, 0, n - 1.001)
            i = pos.astype(int)
            f = pos - i
            return float((pooled[i] * (1 - f) + pooled[i + 1] * f).mean())

        return mean_at(ph + kk * P), mean_at(ph + (kk + 0.5) * P)

    fit_cur, half_cur = grid_stats(P2, ph2)
    family = dict(switched=False, factor=1.0, fit=fit_cur, half=half_cur)
    for factor in (1.5, 2.0 / 3.0):
        bpm_f = 60.0 * FPS / P2 * factor
        if not (lo - 1e-9 <= bpm_f <= hi + 1e-9):
            continue
        Pc = P2 / factor
        s1c, P1c, ph1c = search(Pc * (1 + np.arange(-0.01, 0.01 + 1e-9, 0.0005)), lambda P: np.arange(0, P, 1.0))
        s2c, P2c, ph2c = search(P1c * (1 + np.arange(-0.0006, 0.0006 + 1e-12, 0.00002)), lambda P: ph1c + np.arange(-5, 5.001, 0.25))
        fit_c, half_c = grid_stats(P2c, ph2c)
        family.setdefault('tried', []).append(dict(factor=factor, fit=fit_c, half=half_c))
        if fit_c + half_c > 1.15 * (fit_cur + half_cur) and fit_c > 0.85 * fit_cur:
            P2, ph2 = P2c, ph2c
            fit_cur, half_cur = fit_c, half_c
            family.update(switched=True, factor=factor, fit=fit_c, half=half_c)
    tim['family'] = time.time() - t2b

    P_s, T_s = P2 / FPS, (ph2 % P2) / FPS

    dur = len(y) / SR
    k = np.arange(int(np.ceil((f_first / FPS - T_s) / P_s)) - 1, int((f_last / FPS - T_s) / P_s) + 2)
    beats = T_s + k * P_s
    keep = (beats >= 0) & (beats <= dur)
    beats, k = beats[keep], k[keep]

    # --- drift follow
    t3 = time.time()
    beats_d = beats.copy()
    if follow_drift and len(beats) > 16:
        cands = np.arange(-0.06, 0.0601, 1.0 / FPS)
        centers, offs = [], []
        prev = 0.0
        for a in range(0, len(beats) - 8, 4):
            seg = beats[a:a + 8]
            c = cands[np.abs(cands - prev) <= 0.03 + 1e-9]
            pos = (seg[None, :] + c[:, None]) * FPS
            pos = np.clip(pos, 0, n - 1.001)
            i = pos.astype(int)
            f = pos - i
            sc = (pooled[i] * (1 - f) + pooled[i + 1] * f).sum(1)
            if sc.max() < 1e-6:
                o = prev
            else:
                o = float(c[int(np.argmax(sc))])
            centers.append(seg.mean())
            offs.append(o)
            prev = o
        offs = np.array(offs)
        sm = np.array([np.median(offs[max(0, i - 2):i + 3]) for i in range(len(offs))])
        beats_d = beats + np.interp(beats, centers, sm)
    tim['drift'] = time.time() - t3

    # --- downbeat
    t4 = time.time()
    fb = np.clip(np.round(beats_d * FPS).astype(int), 2, n - 3)

    def at(x):
        return np.max(np.stack([x[fb + d] for d in (-2, -1, 0, 1, 2)]), 0)

    C = chroma_frames(M, fr)
    cs = np.vstack([np.zeros((1, 12), np.float32), np.cumsum(C, 0)])
    bb = np.concatenate([fb, [min(n - 1, fb[-1] + int(P_s * FPS))]])
    seg = (cs[bb[1:]] - cs[bb[:-1]]) / np.maximum(1, (bb[1:] - bb[:-1]))[:, None]
    seg = seg / (np.linalg.norm(seg, axis=1, keepdims=True) + 1e-9)
    cc = np.zeros(len(fb))
    cc[1:] = 1 - np.sum(seg[1:] * seg[:-1], axis=1)
    cues = {'LF': at(nov['lf']), 'CC': cc, 'ACC': at(nov['full']), 'HF': at(nov['hf'])}
    z = {}
    for name, v in cues.items():
        m = np.array([v[(k % 4) == p].mean() if np.any((k % 4) == p) else 0 for p in range(4)])
        z[name] = (m - m.mean()) / (m.std() + 1e-9)
    combo = sum(z[c] for c in downbeat_rule.split('+'))
    p_best = int(np.argmax(combo))
    srt = np.sort(combo)
    db_margin = float(srt[-1] - srt[-2])
    per_cue = {name: int(np.argmax(v)) for name, v in z.items()}
    tim['downbeat'] = time.time() - t4
    tim['total'] = time.time() - t0
    out = dict(bpm=60.0 / P_s, bpm_coarse=b0, P=P_s, T=T_s, beats=beats, beats_drift=beats_d,
               downbeats=beats_d[(k % 4) == p_best], downbeat_phase=p_best,
               downbeats_by_cue={c: beats_d[(k % 4) == p] for c, p in per_cue.items()}, z=z, timing=tim,
               family=family)
    if return_debug:
        out.update(ac=ac, S=S, w=w, bpms=bpms, nov=nov)
    return out
