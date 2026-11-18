"""원곡에서 드럼을 뺀 소리를 만든다(Demucs). DrumVstHost의 '원곡 드럼 빼기'가 쓴다.

usage: python -I -X utf8 -u separate.py <원곡.wav> <출력 폴더> [모델]
결과: <출력 폴더>/<모델>/no_drums.wav (32비트 실수 WAV, 원곡과 같은 길이·크기)
앱이 drums = 원곡 − no_drums 로 드럼을 만든다(둘을 더하면 원곡과 똑같다).

- 연주를 방해하지 않게 프로세스 우선순위를 낮추고, CPU 코어를 두 개 남긴다.
- 모델은 이미 받아 둔 것만 쓴다('hf://' 이름이라 인터넷에서 몰래 받지 않는다). 없으면 'model not downloaded'로 끝난다.
- Demucs의 저장 함수는 소리가 크면 파일 전체를 줄이고(rescale) ±1로 자르므로, 결과를 직접 실수 WAV로 쓴다.
- 2026-11-18 이 PC 측정(htdemucs): 2분 17초 곡 80초, 메모리 약 1.1~1.9GB. htdemucs_ft는 4배 느리고 메모리가 부족해질 수 있다.
- 진행률은 Demucs의 진행 막대("NN%|")를 그대로 내보낸다.
- 입력은 앱이 직접 해독한 44.1kHz WAV여야 한다(MP3를 Demucs가 직접 읽으면 25ms 늦게 시작한다).
"""
import os
import sys


def lower_priority():
    if os.name != 'nt':
        return

    try:
        import ctypes
        below_normal = 0x00004000
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), below_normal)
    except Exception:
        pass


def main(argv):
    if len(argv) < 3:
        print('usage: separate.py <audio.wav> <out folder> [model]', file=sys.stderr)
        return 2

    source, out_dir = argv[1], argv[2]
    model = argv[3] if len(argv) > 3 else 'htdemucs'

    lower_priority()

    import numpy as np
    import soundfile as sf
    import torch

    torch.set_num_threads(max(1, min(4, (os.cpu_count() or 4) - 2)))

    from demucs.api import Separator

    try:
        separator = Separator(model='hf://' + model, device='cpu', progress=True)
    except Exception as e:  # 받아 둔 모델이 없거나 망가졌다
        print(f'error: model not downloaded ({model}): {e}', file=sys.stderr)
        return 6

    audio, rate = sf.read(source, dtype='float32', always_2d=True)   # (샘플, 채널)

    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)

    wav = torch.from_numpy(np.ascontiguousarray(audio.T))
    _, stems = separator.separate_tensor(wav, rate)

    rest = None

    for name, stem in stems.items():
        if name != 'drums':
            rest = stem if rest is None else rest + stem

    if rest is None:
        print('error: the model has no non-drum stems', file=sys.stderr)
        return 7

    rest = rest.detach().cpu().numpy().T.astype(np.float32)   # (샘플, 채널)

    if separator.samplerate != rate or rest.shape[0] != audio.shape[0]:
        print(f'error: unexpected output ({separator.samplerate} Hz, {rest.shape[0]} vs {audio.shape[0]} samples)', file=sys.stderr)
        return 8

    folder = os.path.join(out_dir, model)
    os.makedirs(folder, exist_ok=True)
    destination = os.path.join(folder, 'no_drums.wav')
    temporary = destination + '.part.wav'
    sf.write(temporary, rest, rate, subtype='FLOAT')
    os.replace(temporary, destination)
    print('100%| done', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
