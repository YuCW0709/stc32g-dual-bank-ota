# -*- coding: utf-8 -*-
"""逐行核对日志行尾 *HH (8 位累加和, 含时间戳 '[...]' 前缀).

用法:  python tools/check_log_sum.py <file.txt>
       不给参数时用文件内嵌的那份（v0.14 的历史日志，留作回归样例）。

和值规则见 COMMON/log.c：HH = sum(整行字节) & 0xFF，
整行 = "[ms][TAG] 正文"，不含行尾的 '*' 与 HH 本身。
★ 单向判据：和值对 ⇒ 只能证明"线路没坏"；和值错 ⇒ 该行必定掉/换了字节，
  不可用于推理。
"""
import re, sys

def s8(t):
    return sum(t.encode('ascii', 'replace')) & 0xFF

def run(text):
    ok = bad = skip = 0
    for raw in text.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        m = re.match(r'^(.*)\*([0-9a-fA-F]{2})$', raw)
        if not m:
            skip += 1
            print('  SKIP  %s' % raw)
            continue
        body, want = m.group(1), int(m.group(2), 16)
        got = s8(body)
        if got == want:
            ok += 1
        else:
            bad += 1
            print('  BAD   want=%02x got=%02x (diff=%+d)  %s'
                  % (want, got, want - got, body))
    print('--- ok=%d bad=%d skip=%d ---' % (ok, bad, skip))

if __name__ == '__main__':
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'r', encoding='utf-8', errors='replace') as f:
            run(f.read())
    else:
        print('usage: python tools/check_log_sum.py <log.txt>')
