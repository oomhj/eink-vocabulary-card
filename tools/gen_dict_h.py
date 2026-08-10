#!/usr/bin/env python3
"""
从 ECDICT 裁剪词库并生成固件头文件（PROGMEM 字符串 + 词条数）

用法:
  python3 tools/gen_dict_h.py --level ielts --out src/dict_ielts.h
  python3 tools/gen_dict_h.py --level cet4  --out src/dict_cet4.h

ECDICT 默认在项目同级目录：../ECDICT/ecdict.csv（可用 --csv 指定）

生成的格式：每行 word|phonetic|meaning，meaning 内换行转义为字面 \\n；
按 word 排序（供二分查词/按字母浏览）。
"""
import sys, os, csv, argparse


def esc(s: str) -> str:
    """字段内 `|` 替换为空格，真实换行转义为字面 \\n"""
    return s.replace('|', ' ').replace('\n', '\\n')


# ECDICT 音标用的 IPA 变体字符，U8g2 字体无法渲染，转成 ASCII 近似（保留结构）
PHONETIC_MAP = {
    'æ': 'ae', 'ð': 'dh', 'ŋ': 'ng', 'ɑ': 'a', 'ɒ': 'o', 'ɔ': 'o',
    'ə': 'e', 'ә': 'e', 'ɚ': 'er', 'ɜ': 'e', 'ɡ': 'g', 'ɪ': 'i',
    'ʃ': 'sh', 'ʊ': 'u', 'ʌ': 'u', 'ʒ': 'zh', 'ˈ': "'", 'ˌ': ',', 'ː': ':',
    'ε': 'e', 'θ': 'th', 'є': 'e',
}


def sanitize_phonetic(s: str) -> str:
    """把 ECDICT 音标转成 ASCII 近似（U8g2 无 IPA 字体，直接渲染会是方块）"""
    return ''.join(PHONETIC_MAP.get(c, c) for c in s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--level', default='ielts', help='ECDICT tag，如 ielts/cet4/cet6/ky/gk')
    ap.add_argument('--out', default='src/dict_ielts.h')
    ap.add_argument('--csv', default=None, help='ECDICT CSV 路径（默认项目同级 ECDICT/ecdict.csv）')
    args = ap.parse_args()

    if args.csv:
        csv_path = args.csv
    else:
        csv_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), '..', 'ECDICT', 'ecdict.csv')
    if not os.path.exists(csv_path):
        print(f'未找到 ECDICT: {csv_path}')
        sys.exit(1)

    entries = []
    with open(csv_path, encoding='utf-8-sig', newline='') as f:
        for row in csv.DictReader(f):
            if args.level not in (row.get('tag') or '').split():
                continue
            w = (row.get('word') or '').strip()
            if not w or '|' in w:
                continue
            ph = (row.get('phonetic') or '').strip()
            mn = (row.get('translation') or '').strip()
            if not mn:
                continue
            entries.append((w, ph, mn))

    # 按 word 排序（大小写不敏感）
    entries.sort(key=lambda e: e[0].lower())
    total = len(entries)

    lines = []
    lines.append(f'// 由 tools/gen_dict_h.py 生成：ECDICT {args.level} 词库，{total} 词条')
    lines.append(f'// 每行格式 word|phonetic|meaning（meaning 内换行为字面 \\\\n），按 word 排序')
    lines.append(f'#ifndef DICT_WORDS')
    lines.append(f'#define DICT_WORDS {total}')
    lines.append(f'#endif')
    lines.append(f'static const char dictText[] PROGMEM =')
    for w, ph, mn in entries:
        # 音标转 ASCII 近似；C 字符串内反斜杠、双引号需转义
        ph_ascii = sanitize_phonetic(ph)
        content = f'{esc(w)}|{esc(ph_ascii)}|{esc(mn)}'
        content = content.replace('\\', '\\\\').replace('"', '\\"')
        lines.append(f'    "{content}\\n"')
    lines.append(f'    ;')

    with open(args.out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    size = sum(len(f'{esc(w)}|{esc(ph_ascii)}|{esc(mn)}') + 1 for w, ph, mn in entries)
    print(f'OK: {args.out}  {total} 词, 文本 {size} 字节')


if __name__ == '__main__':
    main()
