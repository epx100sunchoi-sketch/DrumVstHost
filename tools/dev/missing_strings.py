# 영어 번역이 빠진 문장을 번역 파일 형식("한국어<TAB>")으로 보여 준다.
#   python missing_strings.py strings_en.txt
import io, os, sys

sys.stdout.reconfigure(encoding='utf-8')

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.argv = [sys.argv[0], 'noop'] + sys.argv[1:]

source = io.open(os.path.join(HERE, 'extract_strings.py'), encoding='utf-8').read()
source = source.replace('\nmain()\n', '\n')
namespace = {'__file__': os.path.join(HERE, 'extract_strings.py'), '__name__': 'extract_strings'}
exec(compile(source, 'extract_strings.py', 'exec'), namespace)

order, _ = namespace['collect']()
translations = namespace['read_translations'](sys.argv[2], order)

for i, value in enumerate(order):
    if i not in translations or translations[i] == '':
        print('%s\t' % namespace['escape'](value))
