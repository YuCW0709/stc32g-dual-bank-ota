#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_layout.py  --  构建期布局守门（BL -> APP 跳转能否成立的最后一道检查）

为什么需要它
    "BL 跳到 0xFF4000 就能跑 APP"这个约定，依赖一个**很脆**的前提：
    APP 的 `?C_C51STARTUP` 段必须正好落在 CODE 类起点 0xFF4000。

    而 L251 在 CODE 类内部的排序是：`?CO?`（常量段）**排在** `?C_C51STARTUP` 前面。
    也就是说，只要有人在 APP 侧新增一个 `const xxx code` 表，启动代码就会被挤到
    0xFF4000 之后，BL 跳过去执行的其实是常量数据 —— 直接跑飞，
    现场表现是"灯乱闪 + 串口全乱码"，极难定位。

    2026-10-25 实测踩到过：`ota_partition.c` 里的 s_parts[] 带 `code` 修饰，
    导致 ?C_C51STARTUP 被挤到 0xFF4058。去掉 code（让常量进 HCONST）后才回到 0xFF4000。

检查项
    1. APP.map : ?C_C51STARTUP 起始 == OTA_APP_CODE_BASE (0xFF4000)
    2. APP.map : 没有任何 CODE 段落在 0xFF4000 之前
    3. APP.hex : 0xFF4000 首字节不是 0xFF / 0x00（空片或脏数据）
    4. BL .map : ?CO?start251 在 0xFF0000 且 ?C_C51STARTUP 紧随其后
    5. 两个 hex 的段不越出各自分区（BL <= 0x13FFF，APP <= 0x1FFFF）
    6. XDATA 类必须整体落在 16 位以内（BASE < 0x10000）—— 见下方长注释

------------------------------------------------------------------------------
为什么第 6 项是"必须"而不是"建议"（2026-10-26 上板实测，代价是八轮返工）

  C251 对 xdata 有**两种**访问指令，寻址能力不一样：
      MOVX @DPTR     16 位（opcode F0 等）—— 只能到 0xFFFF
      MOV  @DRk      24 位 —— 能到 16MB（far 指针/结构体成员走这条）
  同一个变量，只要源码里有的地方按"绝对符号"访问、有的地方按"指针"访问，
  编译器就会分别生成这两种指令。

  如果 XDATA 类被放到 0x010000 以上（L251 不写 XDATA(...) 时的默认行为！）：
      写 s_v[i]        -> MOVX @DPTR,#0F46    实际落到 0x0F46
      读 &s_v 通过指针 -> 24 位 0x010F46      实际落到 0x010F46
  **同一个数组，写在一个地方、读在另一个地方**，于是：
      · crc32 自检每次上电给出不同的值（读的是从没写过的 RAM）
      · param_save 写进去的是旧内容、flash_iap_verify 拿它比对必然不等
      · 修一处布局就换一个函数受害，表现成"ABI 会修好一处坏掉一处"

  修法：CLASSES 里显式写 XDATA(0x1000-0x2EFF)，让两种指令算出同一个地址。
  0x1000 起跳是为了避开 EDATA(?STACK 0x0008-0x0107) 和 reg bank。
------------------------------------------------------------------------------

用法
    python tools/check_layout.py
    python tools/check_layout.py --out-dir tools/_out_bl --out-dir2 tools/_out_app
退出码
    0 通过；1 参数/文件缺失；6 布局不合规
"""

import argparse
import importlib.util
import os
import re
import sys

APP_CODE_BASE = 0xFF4000
BL_CODE_BASE  = 0xFF0000
BL_TOP        = 0xFF3FFF     # BL 槽 16K
APP_TOP       = 0xFFFFFF     # 与 CODE 类上限一致；真正的 APP 槽见下方 48K
APP_SLOT_TOP  = 0xFF4000 + 48 * 1024 - 1    # 0xFFFFFF，48K 槽正好到顶

LINE_RE = re.compile(
    r"^([0-9A-Fa-f]{6,8})H\s+([0-9A-Fa-f]{6,8})H\s+([0-9A-Fa-f]{6,8})H\s+(.+)$")


def load_merge_hex(tools_dir):
    """复用 merge_hex.py 的 hex 解析，避免两份实现走偏。"""
    path = os.path.join(tools_dir, "merge_hex.py")
    spec = importlib.util.spec_from_file_location("merge_hex", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_map_segments(path):
    """返回 [(start, end, cls, name), ...]

    map 的 SEGMENT 行形如（列数随 TYPE/RELOCATION 变化，不能按固定列切）：
        FF4000H   FF4012H   000013H   BYTE   UNIT     CODE    ?C_C51STARTUP
        FF0000H   FF0002H   000003H   ---    OFFS..   CODE    ?CO?start251?4
    """
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line.strip())
            if not m:
                continue
            toks = m.group(4).split()
            cls = None
            for c in ("CODE", "ECODE", "HCONST", "NCONST"):
                if c in toks:
                    cls = c
                    break
            if cls is None:
                continue
            names = [t for t in toks if t.startswith("?")]
            if not names:
                continue
            out.append((int(m.group(1), 16), int(m.group(2), 16), cls, names[-1]))
    return out


def find_seg(segs, name):
    for s, e, c, n in segs:
        if n == name:
            return s, e, c, n
    return None


CLASS_ROW_RE = re.compile(
    r"^\s*([0-9A-Fa-f]{6,8})H\s+([0-9A-Fa-f]{6,8})H\s+([0-9A-Fa-f]{6,8})H"
    r"\s+(?:([0-9A-Fa-f]{6,8})H)?\s+([A-Z][A-Z0-9]*)\s*$")


def parse_class_table(path):
    """解析 map 的 'ACTIVE MEMORY CLASSES' 表 -> {类名: (base, start, end, used)}

    ★ v0.7.0 修：这张表**会被分页切开**。程序一变大，表头正好落在第 1 页末、
      数据行落到第 2 页，中间夹一行 "L251 LINKER/LOCATER ... PAGE 2" 页眉。
      旧逻辑"遇到任何非空非 === 行就收摊"，于是整张表读成 {}，
      报出 "APP 里没有 XDATA 类 —— CLASSES 少写了 XDATA(...)" 这种**假 FAIL**。
      （v0.7.0 加 MQTT 之后第一次撞上，尺寸刚好把分页点顶到表头上。）
      现在把页眉行/空行/=== 分隔行都当"可跳过"，只对真正的其它内容收摊。
    """
    out = {}
    in_tbl = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "BASE" in line and "START" in line and "MEMORY CLASS" in line:
                in_tbl = True
                continue
            if not in_tbl:
                continue
            s = line.strip()
            # 分页噪声：空行 / === 分隔 / 页眉（含 "PAGE n"）
            if (not s) or s.startswith("=") or "L251 LINKER/LOCATER" in line:
                continue
            m = CLASS_ROW_RE.match(line)
            if m:
                out[m.group(5)] = (int(m.group(1), 16), int(m.group(2), 16),
                                   int(m.group(3), 16), m.group(4) or "")
            else:
                in_tbl = False
    return out


def parse_xdata_segments(path):
    """XDATA 类的段列表 [(start, end, name), ...]（含 ?BYTE 参数块所在的段）"""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line.strip())
            if not m:
                continue
            toks = m.group(4).split()
            if "XDATA" not in toks:
                continue
            names = [t for t in toks if t.startswith("?")]
            out.append((int(m.group(1), 16), int(m.group(2), 16),
                        names[-1] if names else "?"))
    return out


def check_xdata_16bit(path, label, ok, bad):
    """XDATA 类必须整体落在 16 位以内，否则 MOVX @DPTR 与 24 位指针会指向不同内存。

    这不是理论问题：v0.25 上板实测，crc32 自检每次上电给出不同的值、
    param_save 写进去的是旧内容、flash_iap_verify 恒判不等，
    根因就是 XDATA 被链接器放到了 0x010000 之后。
    """
    cls_tbl = parse_class_table(path)
    xd = cls_tbl.get("XDATA")
    if xd is None:
        bad.append("%s 里没有 XDATA 类 —— CLASSES 少写了 XDATA(...)" % label)
        return
    base, start, end, used = xd
    if base >= 0x10000 or start >= 0x10000:
        bad.append("%s 的 XDATA 类起址 = 0x%06X（>= 0x10000），超出 16 位可寻址范围" % (label, base))
        bad.append("  -> 同一个变量会被两条指令访问到两个不同地址：")
        bad.append("     MOVX @DPTR（16 位，只落到低 16 位）  vs  MOV @DRk（24 位指针）")
        bad.append("  -> crc32 自检随机、param_save 写旧值、verify 恒判不等")
        bad.append("  修法：CLASSES 里写 XDATA(0x1000-0x2EFF)（两工程都要）")
    elif start < 0x0108:
        bad.append("%s 的 XDATA 起址 = 0x%06X，会压到 reg bank / ?STACK(0x0008-0x0107)"
                   % (label, start))
    else:
        ok.append("%s XDATA 在 0x%04X-0x%04X（16 位可寻址，不与栈重叠）"
                  % (label, start, end))

    for s, e, n in parse_xdata_segments(path):
        if s > 0xFFFF or e > 0xFFFF:
            bad.append("%s 段 %s 越出 16 位：0x%06X-0x%06X" % (label, n, s, e))
            break


def check(out_bl, out_app, tools_dir):
    ok, bad, warn = [], [], []

    mh = load_merge_hex(tools_dir)

    # ---------- 1/2/4：map 里的段布局 ----------
    app_map = os.path.join(out_app, "APP.map")
    bl_map  = os.path.join(out_bl, "Bootloader.map")
    for p in (app_map, bl_map):
        if not os.path.exists(p):
            print("[错误] 找不到 %s" % p)
            return 1

    app_segs = parse_map_segments(app_map)
    bl_segs  = parse_map_segments(bl_map)

    startup = find_seg(app_segs, "?C_C51STARTUP")
    if startup is None:
        bad.append("APP.map 里找不到 ?C_C51STARTUP —— 链接选项被改过？")
    elif startup[0] != APP_CODE_BASE:
        bad.append("APP 启动段 ?C_C51STARTUP 起始 = 0x%06X，不是 0x%06X"
                   % (startup[0], APP_CODE_BASE))
        bad.append("  -> BL 跳到 0x%06X 会执行那之前的常量数据，必跑飞" % APP_CODE_BASE)
        ahead = [n for s, e, c, n in app_segs if c == "CODE" and s < APP_CODE_BASE]
        if ahead:
            bad.append("  挤在启动代码前面的段：%s" % "、".join(ahead))
            bad.append("  修法：把这些常量表的 `code` 修饰去掉（常量会进 HCONST）")
    else:
        ok.append("APP ?C_C51STARTUP 正好在 0x%06X（BL 跳转入口成立）" % APP_CODE_BASE)

    st251 = find_seg(bl_segs, "?CO?start251?4") or find_seg(bl_segs, "?CO?start251")
    bstart = find_seg(bl_segs, "?C_C51STARTUP")
    if st251 is None:
        warn.append("BL.map 里没找到 ?CO?start251?4（链接器版本不同？）")
    elif st251[0] != BL_CODE_BASE:
        bad.append("BL 的 start251 在 0x%06X，应为 0x%06X" % (st251[0], BL_CODE_BASE))
    else:
        ok.append("BL start251 在 0x%06X（硬件复位入口）" % BL_CODE_BASE)
    if bstart is not None:
        ok.append("BL ?C_C51STARTUP 在 0x%06X（LJMP 目标）" % bstart[0])

    # ---------- 6：XDATA 必须落在 16 位以内 ----------
    check_xdata_16bit(app_map, "APP", ok, bad)
    check_xdata_16bit(bl_map, "BL", ok, bad)

    # ---------- 3/5：hex 实体 ----------
    hex_bl  = os.path.join(out_bl, "Bootloader.hex")
    hex_app = os.path.join(out_app, "APP.hex")
    for p in (hex_bl, hex_app):
        if not os.path.exists(p):
            print("[错误] 找不到 %s" % p)
            return 1

    def mem_of(p):
        r = mh.parse_hex(p)
        return r[0] if isinstance(r, tuple) else r

    mem_bl  = mem_of(hex_bl)
    mem_app = mem_of(hex_app)

    first = mem_app.get(APP_CODE_BASE, 0xFF)
    if first == 0xFF:
        bad.append("APP.hex 在 0x%06X 处是 0xFF（空片）" % APP_CODE_BASE)
    elif first == 0x00:
        bad.append("APP.hex 在 0x%06X 处是 0x00（脏数据）" % APP_CODE_BASE)
    else:
        head = " ".join("%02X" % mem_app.get(APP_CODE_BASE + i, 0xFF) for i in range(4))
        ok.append("APP.hex 入口首字节 = %s（可执行）" % head)

    for name, mem, top in (("Bootloader.hex", mem_bl, BL_TOP),
                           ("APP.hex", mem_app, APP_SLOT_TOP)):
        over = [a for a in mem if a > top]
        if over:
            bad.append("%s 有 %d 字节越出分区上限 0x%06X（最高 0x%06X）"
                       % (name, len(over), top, max(over)))
        else:
            ok.append("%s 未越出 0x%06X" % (name, top))

    # ---------- 输出 ----------
    for s in ok:
        print("  OK   %s" % s)
    for s in warn:
        print("  WARN %s" % s)
    for s in bad:
        print("  FAIL %s" % s)

    if bad:
        print("\n[布局不合规] BL->APP 跳转会失败，先修上面 FAIL 项")
        return 6
    print("\n[布局合规] 0xFF4000 = APP 启动代码，BL 可直接跳转")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-bl", default="tools/_out_bl")
    ap.add_argument("--out-app", default="tools/_out_app")
    ap.add_argument("--tools", default="tools")
    a = ap.parse_args()
    return check(a.out_bl, a.out_app, a.tools)


if __name__ == "__main__":
    sys.exit(main())
