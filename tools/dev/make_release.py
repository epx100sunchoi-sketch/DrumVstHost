# 배포 묶음을 만든다: release/DrumVstHost-<버전>-win64.zip
#   python tools/dev/make_release.py
# 들어가는 것: DrumVstHost.exe, README.md, LICENSE, tools(분석 스크립트·설치 스크립트·BFD 키맵). 사용자 자료와 .pdb는 넣지 않는다.
import io, os, re, zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
version = re.search(r'project\(DrumVstHost VERSION ([0-9.]+)', io.open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8').read()).group(1)
out_dir = os.path.join(ROOT, 'release')
os.makedirs(out_dir, exist_ok=True)
name = 'DrumVstHost-%s-win64' % version
path = os.path.join(out_dir, name + '.zip')

files = ['DrumVstHost.exe', 'README.md', 'LICENSE',
         'tools/beats.py', 'tools/gridfit.py', 'tools/separate.py']
files += ['tools/bfd/' + f for f in sorted(os.listdir(os.path.join(ROOT, 'tools', 'bfd')))]

with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in files:
        z.write(os.path.join(ROOT, f), name + '/' + f)

print(path, os.path.getsize(path), 'bytes,', len(files), 'files')
