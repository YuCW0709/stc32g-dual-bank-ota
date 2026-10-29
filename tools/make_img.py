#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 APP.hex 打包成 OTA 镜像 .bin（64 字节头 + 正文）。

镜像格式与 BL 的 ota_verify_staging 完全对齐（见 OTA/ota_verify.c 的 ver_hdr）：
    [0x00-0x29]  42 字节镜像头（C251 大端）
    [0x2A-0x3F]  22 字节 0xFF 填充
    [0x40-... ]  正文（img_len 字节，1 <= img_len <= 49088）

★ 正文长度默认**按 hex 实际内容自动裁剪**（向上对齐 512 B 扇区）。

  依据：ota_verify.c 的 ver_hdr() 第 6 步只要求
      img_len != 0 && img_len <= OTA_STAGE_PAYLOAD_MAX
  并不要求等于 49088；ota_apply_staging() 也是按 s_p_len 擦/搬/校验。
  所以一个 1.5 KB 的最小 APP 只需要 1.5 KB 正文，不必陪跑 47 KB 的 0xFF。

  这不是"偷工减料"，是**降低对线路的要求**：正文 CRC 只覆盖 img_len 字节，
  下载量同比减少。现场 link quality 只有 5/8 时，48 KB 必错、1.5 KB 有机会。

  ★ 需要老的固定长度（对照 / 救砖）就加 --full（正文恒 49088）。

用法：
    python tools/make_img.py <APP.hex> <out.bin>            # 自动裁剪
    python tools/make_img.py <APP.hex> <out.bin> --full     # 固定 49088
"""
import sys, os, re, zlib, struct, hashlib

APP_IAP_BASE  = 0x14000
APP_IAP_SIZE  = 0xC000
HDR_RESERVE   = 0x40
IMG_LEN       = APP_IAP_SIZE - HDR_RESERVE      # 49088（上限）
MAGIC         = 0x4F544149                      # "OTAI"
ENTRY         = 0xFF4000
PHYS          = 0xFE0000
SECTOR        = 0x200                           # OTA_IAP_SECTOR_SIZE
CODE_TOP      = 0xFFFFFF                        # APP code 域上界

# ★ 版本号不再硬编码：从 APP/app_config.h 现读（同一份源码真源）。
#   硬编码的坑：改了 app_config.h 忘了改这里 ⇒ 镜像头写的版本 ≠ 正文真实版本，
#   升级后日志打在头里的假版本，六本账就永远对不上。
CFG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "APP", "app_config.h")

TARGET = b"STC32G12"

# C251 启动代码在 0xFF4000 的首字节恒为 0x75（MOV direct,#data）。
# BL 是 0x02（LJMP），所以这个字节同时能证明"读到的是 APP，不是 BL"。
STARTUP_OP = 0x75


def read_ver(cfg_path):
    """从 app_config.h 取 APP_VER_MAJOR/MINOR/PATCH/BUILD。读不到就报错，不猜。"""
    try:
        txt = open(cfg_path, "r", encoding="utf-8", errors="ignore").read()
    except OSError as e:
        sys.exit("FATAL: cannot read %s (%s)" % (cfg_path, e))

    out = []
    for name in ("APP_VER_MAJOR", "APP_VER_MINOR", "APP_VER_PATCH", "APP_VER_BUILD"):
        m = re.search(r"^[ \t]*#[ \t]*define[ \t]+%s[ \t]+(\d+)" % name, txt, re.M)
        if not m:
            sys.exit("FATAL: %s not found in %s" % (name, cfg_path))
        v = int(m.group(1))
        if not (0 <= v <= 255):
            sys.exit("FATAL: %s=%d out of u8 range" % (name, v))
        out.append(v)
    return tuple(out)


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


def calc_img_len(mem, force_full=False):
    """按 hex 实际内容算正文长度（向上对齐扇区）。返回 (img_len, 说明)。

    ★ predict_img_crc.py 里**另有一份独立实现**，两边算出的值必须一致。
      共用同一个函数就失去交叉验证的意义（同一个 bug 会同时出现在两边）。
    """
    start = APP_IAP_BASE + PHYS

    if force_full:
        return IMG_LEN, "forced by --full"

    # 1) 先查越界：APP 域里超出正文容量的字节 = 代码涨过 48 KB ⇒ OTA 会静默丢尾
    over = [a for a in mem if start + IMG_LEN <= a <= CODE_TOP]
    if over:
        sys.exit("FATAL: APP code reaches 0x%06X, beyond image capacity "
                 "(start 0x%06X + %d B). OTA would silently truncate."
                 % (max(over), start, IMG_LEN))

    # 2) 找最后一个有内容的地址
    top = -1
    for a in mem:
        if start <= a < start + IMG_LEN and a > top:
            top = a
    if top < 0:
        sys.exit("FATAL: no APP content in hex at 0x%06X" % start)

    used = top - start + 1
    img_len = (used + SECTOR - 1) & ~(SECTOR - 1)
    if img_len == 0 or img_len > IMG_LEN:
        sys.exit("FATAL: img_len=%d out of range (1..%d)" % (img_len, IMG_LEN))
    return img_len, "auto: top=0x%06X used=%d B -> %d B (512-aligned)" % (
        top, used, img_len)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    force_full = "--full" in sys.argv
    if len(args) < 2:
        print(__doc__)
        sys.exit(2)
    hex_path, out_path = args[0], args[1]

    VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD = read_ver(CFG_PATH)

    mem = read_hex(hex_path)
    start = APP_IAP_BASE + PHYS

    img_len, how = calc_img_len(mem, force_full)

    # 起点校验：必须是 C251 启动代码。不是就说明 hex 里的区域搞错了，
    # 继续往下打包会产出一个"校验全过但一跑就飞"的镜像。
    first = mem.get(start)
    if first != STARTUP_OP:
        sys.exit("FATAL: byte at 0x%06X is %s, expect 0x%02X (C251 startup). "
                 "Wrong hex or wrong link base?"
                 % (start, "missing" if first is None else "0x%02X" % first,
                    STARTUP_OP))

    body = bytes(mem.get(start + i, 0xFF) for i in range(img_len))
    body_crc = zlib.crc32(body) & 0xFFFFFFFF

    # C251 大端；字段顺序见 COMMON/ota_types.h，前 36 字节参与 hdr_crc32
    hdr36 = struct.pack(">IHHBBBBIIII8s",
                        MAGIC, HDR_RESERVE, 0x0001,
                        VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD,
                        img_len, body_crc, ENTRY, 0,
                        TARGET)
    assert len(hdr36) == 36, len(hdr36)
    hdr_crc = zlib.crc32(hdr36) & 0xFFFFFFFF

    # 完整 42 字节结构体 = 前 36 字节 + hdr_crc32(4) + reserved(2)
    hdr42 = hdr36 + struct.pack(">IH", hdr_crc, 0xFFFF)
    assert len(hdr42) == 42, len(hdr42)

    # 64 字节头 = 42 字节结构体 + 22 字节 0xFF 填充
    hdr = hdr42 + b"\xFF" * (HDR_RESERVE - 42)

    img = hdr + body
    assert len(img) == HDR_RESERVE + img_len, len(img)

    with open(out_path, "wb") as f:
        f.write(img)

    print("APP.hex     : %s" % hex_path)
    print("out.bin     : %s" % out_path)
    print("img_len     : %d B  (%s)" % (img_len, how))
    print("total       : %d bytes (hdr %d + body %d)" % (len(img), HDR_RESERVE, img_len))
    print("body  crc   : %08x" % body_crc)
    print("hdr   crc   : %08x" % hdr_crc)
    print("ver         : %d.%d.%d.%d  (from APP/app_config.h)" % (VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD))
    print("entry       : %06x" % ENTRY)
    print("magic       : %s" % hdr[:4].decode("ascii", "replace"))
    print("first byte  : 0x%02X (C251 startup, expect 0x%02X)" % (body[0], STARTUP_OP))
    print("md5         : %s" % hashlib.md5(img).hexdigest())


if __name__ == "__main__":
    main()
