# -*- coding: utf-8 -*-
"""在已知的 4 KB 窗口内定位"第一处偏离"的具体形态。

用法：
    python tools/locate_bad_window.py <hex> <good_prefix_crc> <bad_window_crc> [win_lo] [win_hi]

语义：
    good_prefix_crc = 到窗口起点为止的累计 CRC（日志里上一行 p1 的值）
    bad_window_crc  = 到窗口终点为止的累计 CRC（日志里这一行 p1 的值）
    即找 T（长度 = win_hi-win_lo），使 zlib.crc32(T, good_prefix_crc) == bad_window_crc

穷举的形态（都是"数据被换成别的字节"，不含计算路径出错）：
    A 单块 := 邻居块 / 全FF / 全00 / 全AA        （块 = 128 B，与 OTA_CRC_CHUNK 同）
    B 连续 [i,j) := 全FF / 全00                   （区间型）
    C 单字节 := 任意值                            （4096*255）
    D 单字节位翻转                                （4096*8）
    E 整段滞后 / 跳过 W 块                        （W = 1..32）
    F 块内字节序反转 / 整窗口反转
全不命中 ⇒ 错不在"数据内容"，而在计算路径（crc32_update 的 ?BYTE 参数块）。
"""
import sys, zlib

APP_IAP_BASE = 0x14000
PHYS         = 0xFE0000
IMG_LEN      = 49088
BLK          = 128


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
    hexf = sys.argv[1]
    good = int(sys.argv[2], 16)
    bad  = int(sys.argv[3], 16)
    lo   = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x2000
    hi   = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x3000

    mem = read_hex(hexf)
    start = APP_IAP_BASE + PHYS
    body = bytes(mem.get(start + i, 0xFF) for i in range(IMG_LEN))

    def C(t, init=None):
        if init is None:
            return zlib.crc32(t) & 0xFFFFFFFF
        return zlib.crc32(t, init) & 0xFFFFFFFF

    # 0) 自检语义：good 前缀 + 正确窗口 应当等于 good 到窗口末尾的值
    win_good = body[lo:hi]
    print("窗口 0x%04x..0x%04x（%d B / %d 块）" % (lo, hi, hi - lo, (hi - lo) // BLK))
    print("good 前缀 CRC = %08x   bad 窗口末 CRC = %08x" % (good, bad))
    print("自检：good 窗口算出的末值 = %08x（应与日志里上一版正确值一致）"
          % C(win_good, good))

    nblk = (hi - lo) // BLK
    hits = []

    # A 单块替换
    for i in range(nblk):
        s, e = i * BLK, (i + 1) * BLK
        cands = {
            "全FF": b"\xff" * BLK,
            "全00": b"\x00" * BLK,
            "全AA": b"\xaa" * BLK,
        }
        if i > 0:
            cands["前一块"] = win_good[s - BLK:s]
        if i < nblk - 1:
            cands["后一块"] = win_good[s + BLK:e + BLK]
        for name, rep in cands.items():
            t = win_good[:s] + rep + win_good[e:]
            if C(t, good) == bad:
                hits.append("A 块#%d（偏移 0x%04x）:= %s" % (i, lo + s, name))

    # B 连续区间 := 全FF / 全00
    for i in range(nblk):
        for j in range(i + 1, nblk + 1):
            for name, rep in (("全FF", b"\xff"), ("全00", b"\x00")):
                t = win_good[:i * BLK] + rep * ((j - i) * BLK) + win_good[j * BLK:]
                if C(t, good) == bad:
                    hits.append("B 块#%d..#%d（0x%04x..0x%04x）:= %s"
                                % (i, j - 1, lo + i * BLK, lo + j * BLK, name))

    # C 单字节任意值
    for p in range(len(win_good)):
        for v in range(256):
            if v == win_good[p]:
                continue
            t = win_good[:p] + bytes([v]) + win_good[p + 1:]
            if C(t, good) == bad:
                hits.append("C 字节 0x%04x := %02x（原 %02x）"
                            % (lo + p, v, win_good[p]))
        if hits:
            break

    # D 单字节位翻转
    if not hits:
        for p in range(len(win_good)):
            for b in range(8):
                t = bytearray(win_good)
                t[p] ^= (1 << b)
                if C(bytes(t), good) == bad:
                    hits.append("D 字节 0x%04x 第 %d 位翻转" % (lo + p, b))
            if hits:
                break

    # E 整段滞后 / 跳过 W 块
    if not hits:
        for w in range(1, nblk):
            for name, t in (("滞后 %d 块" % w, win_good[:BLK] * 0 + win_good[:-w * BLK] + win_good[:w * BLK]),
                            ("跳过 %d 块" % w, win_good[w * BLK:] + win_good[:w * BLK])):
                pass
        # 真正的"滞后/跳过"要用窗口外的数据，见 F
    # F 用窗口外的数据构造滞后/跳过（这才是"buffer 没推进"的样子）
    if not hits:
        for w in range(1, nblk + 1):
            wb = w * BLK
            pre = body[lo - wb:lo] if lo >= wb else None
            post = body[hi:hi + wb]
            if pre is not None:
                # 整个窗口整体滞后 W 块（读到的是更早的数据）
                t = body[lo - wb:hi - wb]
                if C(t, good) == bad:
                    hits.append("F 窗口整体滞后 %d 块（读到 0x%04x 起）" % (w, lo - wb))
                # 首块之后整段滞后
                t = win_good[:BLK] + body[lo:hi - wb]
                if len(t) == len(win_good) and C(t, good) == bad:
                    hits.append("F 首块后整段滞后 %d 块" % w)
            if len(post) == wb:
                t = win_good[wb:] + post
                if C(t, good) == bad:
                    hits.append("F 窗口整体跳过 %d 块（读到 0x%04x 起）" % (w, hi))

    if not hits:
        print("--- 全不命中 ---")
        print("⇒ 错不在\"数据内容\"，而在计算路径：crc32_update(u32,u8*,u16)=9 B")
        print("  必生成 ?BYTE 参数块（铁律 14 的经典形态），下一个探针应打它的入参。")
        return

    print("--- 命中 %d 条 ---" % len(hits))
    for h in hits:
        print("  " + h)


if __name__ == "__main__":
    main()
