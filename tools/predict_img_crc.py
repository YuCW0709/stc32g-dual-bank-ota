#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线预言 OTA 镜像的正文 CRC / 镜像头 CRC（独立实现，用于交叉验证）。

原理：正文 = APP IAP 区 0x14000 起 img_len 字节（未写的字节读作 0xFF），
      镜像头 = 42 字节结构体，C251 大端，前 36 字节参与 hdr_crc32。

★ 本文件**故意不复用 make_img.py 的正文长度算法**，而是照同一条规则
  （ver_hdr 第 6 步：1 <= img_len <= 49088；打包时向上对齐 512 B）另写一份。
  共用一份代码就失去交叉验证的意义 —— 同一个 bug 会同时出现在两边，
  然后"两条独立路径都通过"这句话就成了假判据。

用法：
    python tools/predict_img_crc.py build/step54_xxx_BL_APP.hex
    python tools/predict_img_crc.py build/step54_xxx_BL_APP.hex --full
"""
import sys, os, zlib, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_img import read_ver, CFG_PATH        # 版本号单一真源：APP/app_config.h

APP_IAP_BASE  = 0x14000
APP_IAP_SIZE  = 0xC000
HDR_RESERVE   = 0x40
IMG_LEN       = APP_IAP_SIZE - HDR_RESERVE      # 49088（上限）
MAGIC         = 0x4F544149
ENTRY         = 0xFF4000
PHYS          = 0xFE0000                        # code = iap + 0xFE0000
SECTOR        = 0x200
STARTUP_OP    = 0x75                            # C251 启动代码首字节

# ★ 不再硬编码：跟 make_img.py 读同一份 app_config.h。
#   写死过 0,1,0,1 的坑：出 v0.2.0 时这里算出的 hdr crc 与真实镜像对不上，
#   于是"头 crc 不符"会被误判成固件算错，白查一轮。
VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD = read_ver(CFG_PATH)
TARGET = b"STC32G12"


def read_hex(path):
    mem = {}
    base = 0
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


def img_len_of(mem, force_full=False):
    """独立实现：与 make_img.calc_img_len 同规则、不同写法。"""
    start = APP_IAP_BASE + PHYS
    if force_full:
        return IMG_LEN

    app_keys = [a for a in mem if a >= start]
    if not app_keys:
        sys.exit("FATAL: hex 里没有 APP 区内容（0x%06X 起）" % start)
    top_all = max(app_keys)
    if top_all >= start + IMG_LEN:
        sys.exit("FATAL: APP code 到 0x%06X，超出正文容量 %d B ⇒ OTA 会静默截断"
                 % (top_all, IMG_LEN))

    used = top_all - start + 1
    return (used + SECTOR - 1) // SECTOR * SECTOR


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    force_full = "--full" in sys.argv
    path = argv[0] if argv else "tools/_out_combined.hex"
    mem = read_hex(path)

    img_len = img_len_of(mem, force_full)
    start = APP_IAP_BASE + PHYS
    body = bytes(mem.get(start + i, 0xFF) for i in range(img_len))
    body_crc = zlib.crc32(body) & 0xFFFFFFFF

    # C251 大端；字段顺序见 COMMON/ota_types.h
    hdr = struct.pack(">IHHBBBBIIII8s",
                      MAGIC, HDR_RESERVE, 0x0001,
                      VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD,
                      img_len, body_crc, ENTRY, 0,
                      TARGET)
    assert len(hdr) == 36, len(hdr)
    hdr_crc = zlib.crc32(hdr) & 0xFFFFFFFF

    print("hex        : %s" % path)
    print("img_len    : %d (0x%X)%s" % (img_len, img_len,
          "  [--full]" if force_full else "  [auto]"))
    print("body  crc  : %08x" % body_crc)
    print("hdr   crc  : %08x" % hdr_crc)
    print("ver        : %d.%d.%d.%d" % (VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD))
    print("entry      : %06x" % ENTRY)
    print("body[0]    : 0x%02X (expect 0x%02X)" % (body[0], STARTUP_OP))
    # 首末各 16 字节，肉眼对一眼 hex 便于发现"读错区域"
    print("body[0..15]: %s" % body[:16].hex(" "))
    print("body[-16:] : %s" % body[-16:].hex(" "))

    # 与 BL v0.40/v0.41 的 `p1 o=%lx c=%lx` 探针逐行对照用
    #   o=0080 是第一块（单独特判），之后每 4 KB 一行
    print("--- 累计 CRC（对上 = 这一段读对了）---")
    for off in [0x80] + list(range(0x1000, img_len, 0x1000)):
        print("p1 o=%04x c=%08x" % (off, zlib.crc32(body[:off]) & 0xFFFFFFFF))


if __name__ == "__main__":
    main()
