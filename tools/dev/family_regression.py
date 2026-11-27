# 곡 분석 폴더 전체에 새 gridfit(템포 가족 확인)을 돌려, 저장된 BPM과 비교한다.
# usage: python family_regression.py <tools 폴더> <곡 분석 폴더>
import importlib.util
import io
import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main(argv):
    tools, root = argv[1], argv[2]
    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly

    gf = load('gridfit', os.path.join(tools, 'gridfit.py'))

    for name in sorted(os.listdir(root)):
        folder = os.path.join(root, name)
        wav = os.path.join(folder, 'original.wav')
        bj = os.path.join(folder, 'beats.json')

        if not (os.path.isdir(folder) and os.path.isfile(wav)):
            continue

        old = '-'
        if os.path.isfile(bj):
            try:
                old = '%.2f' % json.load(io.open(bj, encoding='utf-8'))['bpm']
            except Exception:
                pass

        try:
            y, sr = sf.read(wav, dtype='float32', always_2d=True)
            y = y.mean(axis=1)
            if sr != gf.SR:
                g = np.gcd(int(sr), int(gf.SR))
                y = resample_poly(y, gf.SR // g, int(sr) // g).astype(np.float32)
            r = gf.analyze(y, follow_drift=False)
            fam = r['family']
            tried = ' '.join('x%.2f f%.3f h%.3f' % (t['factor'], t['fit'], t['half']) for t in fam.get('tried', []))
            print('%-44s old %8s -> new %8.2f | switched %s (x%.2f) base f%.3f h%.3f | %s'
                  % (name[:44], old, r['bpm'], fam['switched'], fam['factor'], fam['fit'], fam['half'], tried), flush=True)
        except Exception as ex:
            print('%-44s ERROR %s' % (name[:44], ex), flush=True)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
