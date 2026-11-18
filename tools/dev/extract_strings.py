# 소스에서 한글이 든 문자열(화면 글자)을 뽑아 번호를 붙인다. 영어 번역표(Source/Language.cpp)를 만들 때 쓴다.
#   python extract_strings.py list   <out.txt>          뽑은 문자열을 "번호<TAB>한국어" 로 쓴다
#   python extract_strings.py build  <en.txt>           "번호<TAB>English" 파일로 Source/Language.cpp를 만든다
#   python extract_strings.py check  <en.txt>           번역이 빠진 번호를 알려 준다
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', '..', 'Source'))
HANGUL = re.compile('[가-힣ㄱ-ㆎ]')
SKIP_BEFORE = re.compile(r'(pick|dataChild)\s*\([^()]*$')
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', "'": "'", '\\': '\\', '0': '\0'}


def literals(text):
    """(문자열, 시작 위치) 목록. 주석·문자 상수·원시 문자열은 건너뛰고, 나란히 놓인 문자열은 하나로 합친다."""
    out, i, n = [], 0, len(text)
    current, start, last_end = None, 0, 0

    def flush():
        nonlocal current
        if current is not None:
            out.append((current, start))
            current = None

    while i < n:
        c = text[i]
        if text.startswith('//', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif text.startswith('/*', i):
            i = text.find('*/', i) + 2
        elif c == 'R' and text.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', text[i:])
            end = text.find(')' + m.group(1) + '"', i)
            i = end + len(m.group(1)) + 2
            flush()
        elif c == "'":
            i += 1
            while text[i] != "'":
                i += 2 if text[i] == '\\' else 1
            i += 1
        elif c == '"':
            if current is not None and text[last_end:i].strip() != '':
                flush()
            if current is None:
                current, start = '', i
            i += 1
            raw = bytearray()
            while text[i] != '"':
                if text[i] == '\\':
                    e = text[i + 1]
                    if e == 'x':
                        m = re.match(r'[0-9a-fA-F]{1,2}', text[i + 2:])
                        raw.append(int(m.group(0), 16))
                        i += 2 + len(m.group(0))
                    else:
                        raw.extend(ESCAPES[e].encode('utf-8'))
                        i += 2
                else:
                    raw.extend(text[i].encode('utf-8'))
                    i += 1
            i += 1
            last_end = i
            current += raw.decode('utf-8')
        else:
            if current is not None and not c.isspace():
                flush()
            i += 1
    flush()
    return out


def collect():
    seen, order = {}, []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(('.cpp', '.h')) or name == 'Language.cpp':
            continue
        text = io.open(os.path.join(SRC, name), encoding='utf-8').read()
        for value, pos in literals(text):
            if not HANGUL.search(value):
                continue
            if SKIP_BEFORE.search(text[max(0, pos - 80):pos]):
                continue
            if value not in seen:
                seen[value] = name
                order.append(value)
    return order, seen


def cstr(s):
    out = []
    for ch in s:
        if ch == '\\': out.append('\\\\')
        elif ch == '"': out.append('\\"')
        elif ch == '\n': out.append('\\n')
        elif ch == '\r': out.append('\\r')
        elif ch == '\t': out.append('\\t')
        else: out.append(ch)
    return '"' + ''.join(out) + '"'


BACKSLASH = chr(92)


def unescape(text):
    # 파일에서는 줄바꿈을 (역슬래시)n, 줄 끝 빈칸을 (역슬래시)s, 역슬래시 자체는 두 번 겹쳐 적는다.
    out, i = [], 0
    while i < len(text):
        if text[i] == BACKSLASH and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({'n': chr(10), 's': ' ', BACKSLASH: BACKSLASH}.get(nxt, BACKSLASH + nxt))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def escape(text):
    text = text.replace(BACKSLASH, BACKSLASH * 2).replace(chr(10), BACKSLASH + 'n')
    stripped = text.rstrip(' ')
    return stripped + (BACKSLASH + 's') * (len(text) - len(stripped))


def read_translations(path, order):
    """번역 파일을 읽어 {번호: 영어}로 돌려준다. 줄은 "소스의 한국어<TAB>English"(예전 방식은 "번호<TAB>English")."""
    index = {value: i for i, value in enumerate(order)}
    result = {}
    for line in io.open(path, encoding='utf-8').read().split('\n'):
        if line.startswith('#'):
            continue
        key, tab, english = line.partition('\t')
        if unescape(key) in index:
            result[index[unescape(key)]] = unescape(english)
        elif key.strip().isdigit() and int(key) < len(order):
            result[int(key)] = unescape(english)
    return result


def main():
    mode = sys.argv[1]
    order, seen = collect()

    if mode == 'list':
        with io.open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
            for i, value in enumerate(order):
                f.write('%d\t%s\n' % (i, value.replace('\n', '\\n')))
        print(len(order), 'strings')
        return

    english = read_translations(sys.argv[2], order)

    if mode == 'export':   # 번호 방식 파일을 "한국어<TAB>English" 방식으로 바꿔 쓴다. 한국어가 빈칸으로 끝나면 영어도 맞춘다.
        with io.open(sys.argv[3], 'w', encoding='utf-8', newline='\n') as f:
            f.write('# 영어 번역: "소스의 한국어<TAB>English". 줄바꿈은 \\n, 줄 끝 빈칸은 \\s 로 적는다.\n')
            f.write('# python extract_strings.py build strings_en.txt 로 Source/Language.cpp를 만든다. check 로 빠진 번역을 본다.\n')
            for i, value in enumerate(order):
                text = english.get(i, '')
                if value.endswith(' ') and not text.endswith(' ') and '--keep' not in sys.argv:
                    text += ' '
                f.write('%s\t%s\n' % (escape(value), escape(text)))
        print('exported', len(order))
        return
    missing = [i for i in range(len(order)) if i not in english]

    if mode == 'check':
        print('strings', len(order), 'translated', len(order) - len(missing), 'missing', missing[:40])
        return

    lines = ['// 영어 번역표. tools/dev/extract_strings.py build 로 만든다(직접 고치지 않는다).',
             '// 왼쪽은 소스에 적힌 한국어 그대로, 오른쪽은 영어 화면에서 보일 글자다. 표에 없는 글자는 한국어로 보인다.',
             '', '#include "Common.h"', '#include <string_view>', '#include <unordered_map>', '',
             'namespace', '{', 'struct Pair { const char* korean; const char* english; };', '',
             'const Pair pairs[] =', '{']
    for i, value in enumerate(order):
        if i in english:
            lines.append('    { %s,\n      %s },' % (cstr(value), cstr(english[i])))
    lines += ['};', '',
              'bool english = false;', '} // namespace', '',
              'bool lang::isEnglish() noexcept                       { return english; }',
              'void lang::setEnglish (bool shouldBeEnglish) noexcept { english = shouldBeEnglish; }', '',
              'const char* lang::toEnglish (const char* koreanUtf8)',
              '{',
              '    static const auto table = []',
              '    {',
              '        std::unordered_map<std::string_view, const char*> map;',
              '',
              '        for (const auto& pair : pairs)',
              '            map.emplace (pair.korean, pair.english);',
              '',
              '        return map;',
              '    }();',
              '',
              '    if (koreanUtf8 == nullptr)',
              '        return nullptr;',
              '',
              '    const auto found = table.find (koreanUtf8);',
              '    return found != table.end() ? found->second : nullptr;',
              '}', '']
    io.open(os.path.join(SRC, 'Language.cpp'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print('wrote Language.cpp with', len(order) - len(missing), 'pairs; missing', len(missing))


main()
