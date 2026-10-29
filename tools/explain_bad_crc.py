#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""反推「首趟 CRC 算错」到底错在哪一段。

背景：BL 每次上电第一次读 STAGING 正文算出的 CRC 是错的（v0.39 起
      首趟 / 第二趟 已经走同一个调用点，所以「调用形状」这条解释已被证伪），
      同一个上电周期里紧接着再算一遍就一定对。每次构建的错值都不同。

用法：
    python tools/explain_bad_crc.py <hex> <bad_crc_hex> [good_crc_hex]

思路：用 hex 重建正文（49088 B），穷举"单处结构性损坏"，看哪一种能把
      good_crc 变成 bad_crc。命中唯一解 ⇒ 损坏形态直接定死。
"""
import sys, zlib

APP_IAP_BASE = 0x14000
HDR_RESERVE  = 0x40
IMG_LEN      = 0xC000 - HDR_RESERVE      # 49088
PHYS         = 0xFE0000
CHUNK        = 128


def read_hex(path):
    mem, base = {}, 0
    for line in open(path, "r", encoding="ascii", errors="ignore"):
        line = line.strip()
        if not line.startswith(":") or len(line) < 11:
            continue
        raw = bytes.fromhex(line[1:])
        n, addr, typ = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4:4 + n]
        if typ == 0x04:
            base = ((data[0] << 8) | data[1]) << 16
        elif typ == 0x02:
            base = ((data[0] << 8) | data[1]) << 4
        elif typ == 0x00:
            for i, b in enumerate(data):
                mem[base + addr + i] = b
    return mem


def main():
    path = sys.argv[1]
    bad  = int(sys.argv[2], 16)
    mem  = read_hex(path)

    start = APP_IAP_BASE + PHYS
    body  = bytearray(mem.get(start + i, 0xFF) for i in range(IMG_LEN))
    good  = zlib.crc32(body) & 0xFFFFFFFF
    print("hex       : %s" % path)
    print("good crc  : %08x" % good)
    print("bad  crc  : %08x" % bad)
    if len(sys.argv) > 3:
        want = int(sys.argv[3], 16)
        print("arg  校验  : %s" % ("OK" if want == good else "不符! 期望 %08x" % want))

    # 分块：384 块，最后一块 64 B
    chunks, off = [], 0
    while off < IMG_LEN:
        n = min(CHUNK, IMG_LEN - off)
        chunks.append((off, n))
        off += n
    print("chunks    : %d (末块 %d B)" % (len(chunks), chunks[-1][1]))

    hits = []

    # ---- 1. 单块被换成"邻居块"（v0.33 曾出现过 chunk38:=chunk37）----
    for i, (o, n) in enumerate(chunks):
        for d, name in ((-1, "prev"), (1, "next")):
            j = i + d
            if j < 0 or j >= len(chunks):
                continue
            src_off, src_n = chunks[j]
            if src_n < n:
                continue
            t = bytearray(body)
            t[o:o + n] = body[src_off:src_off + n]
            if (zlib.crc32(t) & 0xFFFFFFFF) == bad:
                hits.append("chunk[%d] @0x%05x (%dB) := chunk[%d] (%s)"
                            % (i, o, n, j, name))

    # ---- 2. 单块被读成全 0xFF / 全 0x00 ----
    for i, (o, n) in enumerate(chunks):
        for fill, name in ((0xFF, "全FF"), (0x00, "全00")):
            t = bytearray(body)
            t[o:o + n] = bytes([fill]) * n
            if (zlib.crc32(t) & 0xFFFFFFFF) == bad:
                hits.append("chunk[%d] @0x%05x (%dB) := %s" % (i, o, n, name))

    # ---- 3. 单块被换成首块 / 末块 ----
    for i, (o, n) in enumerate(chunks):
        for j, name in ((0, "首块"), (len(chunks) - 1, "末块")):
            src_off, src_n = chunks[j]
            if src_n < n:
                continue
            t = bytearray(body)
            t[o:o + n] = body[src_off:src_off + n]
            if (zlib.crc32(t) & 0xFFFFFFFF) == bad:
                hits.append("chunk[%d] @0x%05x (%dB) := %s" % (i, o, n, name))

    # ---- 4. 单字节被改成本块的邻居字节 / 0xFF / 0x00 ----
    for o, n in chunks:
        for k in range(n):
            p = o + k
            for val, name in ((0xFF, "FF"), (0x00, "00")):
                if body[p] == val:
                    continue
                t = bytearray(body)
                t[p] = val
                if (zlib.crc32(t) & 0xFFFFFFFF) == bad:
                    hits.append("byte @0x%05x (=%02x) -> %s" % (p, body[p], name))

    if not hits:
        print("\n结论：上述形态全部不命中 ⇒ 不是「单块/单字节」这一类结构性损坏。")
    else:
        print("\n命中 %d 条：" % len(hits))
        for h in hits:
            print("   " + h)


if __name__ == "__main__":
    main()
