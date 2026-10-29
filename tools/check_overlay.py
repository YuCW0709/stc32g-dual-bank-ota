#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""链接后守门：参数块（?F??BYTE）与局部变量的重叠风险。

────────────────────────────────────────────────────────────────────────────
为什么需要它
────────────────────────────────────────────────────────────────────────────
C251 把一个函数"装不进寄存器的形参"放到编译器生成的 `?函数名??BYTE` 段里。
这个段名看着像属于那个函数，**但 L251 把它当孤儿**：

  · 它只按大小往 `_XDATA_GROUP_` 池里塞，不知道它属于谁；
  · 因此它可以和**别的函数的局部变量**分配在同一地址；
  · 而 overlay 的语义是"不同时活跃就复用"——链接器以为自己做过分析，
    实际上对一个不知道归属的段，它**做不了**任何分析。

2026-10-25 v0.9 的两次翻车都是这么来的：

    _XDATA_GROUP_        010E4A-010E95
    ?log_x??BYTE         010E4D          16 B -> 010E4D-010E5C
    log_x?/log           0E52-0E65       重叠 11 B
      -> LOG_I2 的第二个槽 lo(0x4000) 被写成 0x5000

    ?crc32_update??BYTE  010E55          10 B -> 010E55-010E5E
    crc32_selftest?/crc32 0E52-0E5A      重叠 6 B（v[3..8]）
      -> CRC32 自检必失败 -> STAGING 不可写 -> A1 复制 rc=2

────────────────────────────────────────────────────────────────────────────
判据
────────────────────────────────────────────────────────────────────────────
map 里参数块**只给起始地址、不给长度**，所以无法精确求交。本脚本用两个
保守但足够灵敏的判据：

  A. 源码级：任何**非 static 的局部 xdata 数组**都是定时炸弹 —— 它一定进池，
     而且一定是"被踩的那一方"。发现即 FAIL。
     （数组的地址要传给别的函数，一旦被踩就是整块数据错，不是单个数字错。）

  B. map 级：参数块 P 的起始地址落在局部段 S 的 [start, end] 之内，
     且 P 所属函数 F 与 S 所属函数 G 在调用图上**互相可达**（F==G 除外，
     那是"函数自己的参数块和自己的局部变量"，入口拷完即可覆盖，属正常），
     则 FAIL。

只落在别人区间里、但调用图上不可达的，按"不同时活跃"放行 —— 这正是
overlay 的合法用法，不报。

另有三条不看池、命中即 FAIL 的硬判据（v0.10 之后加的）：

  0.  `?log_x??BYTE` / `?log_x2??BYTE` 存在 —— log 又产生参数块了。
  0b. `_XDATA_GROUP_` 存在 —— NOOVERLAY 没生效。
  0c. CODE 类里出现 `?CO?` 常量段（`?CO?start251` 除外）—— 本工程代码基址
      在 0xFF0000 以上，超出 16 位 `code` 空间，取数地址会被截断成低 16 位，
      读到的完全是另一片 Flash。v0.10 的 `crc32 selftest FAIL` 就是这个。
      修法：声明里去掉 `code`（进 HCONST，24 位 far 寻址）。

────────────────────────────────────────────────────────────────────────────
用法
────────────────────────────────────────────────────────────────────────────
    python tools/check_overlay.py                       # 默认查两个工程
    python tools/check_overlay.py --map xxx.map         # 查单个
    python tools/check_overlay.py --quiet               # 只返回码
退出码：0 通过 / 4 发现风险（build_local.sh 会据此中止）
"""

import argparse
import os
import re
import sys

# 参数块在 map 里只给起始地址，这里给一个"参与判定"的最小长度。
# 取 4 是保守下界（最少的溢出段也是一个 u32）；真正用来判定的其实是
# "起始地址是否落在别人区间内"，长度只影响边界 1~2 字节的情形。
_MIN_PB_LEN = 4

_RE_POOL = re.compile(
    r"^([0-9A-F]{6})H\s+([0-9A-F]{6})H\s+[0-9A-F]{6}H\s+\S+\s+\S+\s+XDATA\s+_XDATA_GROUP_",
    re.M)
_RE_PB = re.compile(
    r"^\s*([0-9A-F]{6,8})H\s+XDATA\s+BYTE\s+\?(\w+)\?\?BYTE\s*$", re.M)
_RE_SEG = re.compile(
    r"^(\S+?)\?/(\w+)\s+([0-9A-F]{4,6})H\s+([0-9A-F]{4,6})H\s*$")
_RE_CALL = re.compile(r"^\s*\+\-->\s+(\S+?)\?/(\w+)")
_RE_MEMSEG = re.compile(
    r"^([0-9A-F]{6})H\s+([0-9A-F]{6})H\s+([0-9A-F]{6})H\s+\S+\s+\S+\s+(\S+)\s+(\S+)\s*$",
    re.M)


def find_code_consts(path):
    """找出所有落在 **CODE 类** 里的 `?CO?` 常量段（`?CO?start251` 除外）。

    为什么这是必错的：
        本工程代码基址是 0xFF0000(BL) / 0xFF4000(APP)，**超出 16 位 code 空间**。
        C251 的 `code` 限定 = 16 位绝对地址，取址只搬低 16 位（实测汇编）：
            ADD  WR10,#WORD0 crc32_tab4      ; 没有 WORD2
        于是 FF006F 被截成 006F、FF4048 截成 4048 —— 正好落进 STAGING 区，
        读到的是"刚擦干净或正在被写"的字节。v0.10 的 crc32 恒 FAIL 就是这个。
        `?CO?start251` 是硬件复位入口，只被 LJMP 用到、不当数据读，允许存在。
    """
    out = []
    for m in _RE_MEMSEG.finditer(open(path, "r", errors="ignore").read()):
        cls, name = m.group(4), m.group(5).strip('"')
        if cls != "CODE" or not name.startswith("?CO?"):
            continue
        if name.startswith("?CO?start251"):
            continue
        out.append((name, int(m.group(1), 16), int(m.group(2), 16)))
    return out


def parse_map(path):
    """返回 pool / 参数块 / 局部段 / 调用图。"""
    txt = open(path, "r", errors="ignore").read()

    m = _RE_POOL.search(txt)
    pool = (int(m.group(1), 16), int(m.group(2), 16)) if m else None
    hi = (pool[0] & 0xFF0000) if pool else 0

    pblocks = {m.group(2): int(m.group(1), 16) for m in _RE_PB.finditer(txt)}

    # overlay 明细：段行 + 紧随其后的 +--> 行
    segs = []          # (owner, start, end)
    edges = {}         # caller -> set(callee)
    cur = None
    in_ovl = False
    for line in txt.split("\n"):
        if "OVERLAY MAP OF MODULE" in line:
            in_ovl = True
            continue
        if not in_ovl:
            continue
        if line.startswith("PUBLIC SYMBOLS") or "MEMORY MAP OF MODULE" in line:
            in_ovl = False
            continue

        ms = _RE_SEG.match(line)
        if ms:
            name, mod = ms.group(1), ms.group(2)
            owner = "%s?/%s" % (name, mod)
            start = int(ms.group(3), 16) | hi
            end = int(ms.group(4), 16) | hi
            segs.append((owner, start, end))
            cur = owner
            edges.setdefault(owner, set())
            continue

        mc = _RE_CALL.match(line)
        if mc and cur:
            edges.setdefault(cur, set()).add("%s?/%s" % (mc.group(1), mc.group(2)))

    return pool, pblocks, segs, edges


def reachable(edges):
    """全部配对的可达性（图很小，直接 DFS，不做记忆化优化）。"""
    memo = {}

    def go(src, dst):
        key = (src, dst)
        if key in memo:
            return memo[key]
        seen, stack = set(), [src]
        while stack:
            n = stack.pop()
            if n == dst:
                memo[key] = True
                return True
            if n in seen:
                continue
            seen.add(n)
            stack.extend(edges.get(n, ()))
        memo[key] = False
        return False

    return go


def check(mapfile, quiet=False):
    if not os.path.isfile(mapfile):
        print("  [skip] 找不到 %s" % mapfile)
        return 0

    pool, pblocks, segs, edges = parse_map(mapfile)
    name = os.path.basename(mapfile)
    go = reachable(edges)
    fails = []

    # ---- 判据 0（最硬）：log 绝不能再产生参数块 ----
    # v0.9 加一个 ota_verify.c 就让它回来了，代价是 APP 侧数字全乱。
    # 这条不看池、不看调用图，出现即 FAIL。
    if "log_x" in pblocks or "log_x2" in pblocks:
        key = "log_x" if "log_x" in pblocks else "log_x2"
        fails.append(
            "?%s??BYTE @%06X 存在 —— log 的参数块回来了（v0.9 的坑），"
            "检查 log.h 的宏是否被改回" % (key, pblocks[key]))

    # ---- 判据 0b：NOOVERLAY 必须生效（池不存在） ----
    if pool is not None:
        fails.append(
            "_XDATA_GROUP_ 仍在 %06X-%06X —— NOOVERLAY 没生效。"
            "局部变量仍会被参数块覆盖，P0 那次 0x4000->0x5000 就是它。"
            "检查 L251 的 Misc controls 有没有 NOOVERLAY" % pool)

    # ---- 判据 0c：CODE 类里不许出现 `?CO?` 常量段 ----
    # v0.10 上板：crc32 查表是 `static const u32 code`，落在 FF006F，
    # 而 `code` 指针只有 16 位 → 实际读 0x006F（STAGING 区）→ selftest 恒 FAIL
    # → 顺带把 "STAGING NOT writable" 和 "[VER] fail rc=12" 全带出来了。
    for nm, s, e in find_code_consts(mapfile):
        fails.append(
            "%s @%06X-%06X 是 CODE 类常量段 —— 代码基址 >=0xFF0000 已超出 16 位 "
            "code 空间，取址只搬 WORD0，实际读到的是截断后的低 16 位地址。"
            "把声明里的 `code` 去掉，让它进 HCONST" % (nm, s, e))

    # ---- 判据 B：只在池存在时才需要（NOOVERLAY 下段根本不会重叠）----
    #
    # 只用一跳直接调用，不用传递可达：
    #   · 直接调用时，调用者的局部变量在调用点**一定还活着**（循环计数器、
    #     累加和之类），被被调者的参数块覆盖就是确定的错误；
    #   · 传递可达的场合（main -> bl_boot -> ... -> crc32_update）大多是
    #     "栈已经退了几层、局部变量早已失效"，报出来全是误报，会淹掉真问题。
    # 宁可漏报远层、也要让每一行报出来的都值得改。
    # 两种方向都要查，它们是同一件事的两面：
    #   ① F 直接调用 owner  -> owner 的局部变量，被 F 的参数块覆盖
    #   ② owner 直接调用 F  -> owner 的局部变量还活着，被 F 的参数块覆盖
    # ② 尤其隐蔽：调用者的循环计数器被被调者的参数写掉，循环直接乱，
    # 表现为"校验永远不过"而不是"某个数字不对"。
    for fname, addr in sorted(pblocks.items()):
        if pool is None:
            break                      # 没有池 = 段独占，判据 B 无事可做
        owners_of_f = [o for o in edges if o.split("?")[0] == fname]
        callees = set()
        for o in owners_of_f:
            callees |= edges.get(o, set())

        for owner, s, e in segs:
            if not (s <= addr <= e):
                continue
            gname = owner.split("?")[0]
            if gname == fname:
                continue                   # 自己的参数块 vs 自己的局部变量：正常

            direct = owner in callees     # ① F 调 owner
            if not direct:                # ② owner 调 F
                for co in edges.get(owner, set()):
                    if co.split("?")[0] == fname:
                        direct = True
                        break
            if direct:
                rel = "F 直接调用它" if owner in callees else "它直接调用 F"
                fails.append(
                    "?%s??BYTE @%06X 压在 %s 的局部段 %06X-%06X 上（%s）"
                    % (fname, addr, owner, s, e, rel))

    # ---- 判据 A 在源码侧做（见 check_sources），这里只报告池占用 ----
    if fails:
        print("  [%s] !! 守门未通过（%d 项）：" % (name, len(fails)))
        for f in fails[:12]:
            print("      - " + f)
        if len(fails) > 12:
            print("      ... 还有 %d 处" % (len(fails) - 12))
        return 4

    if not quiet:
        if pool is None:
            print("  [%s] NOOVERLAY 生效，无 overlay 池；参数块 %d 个各自独占 OK"
                  % (name, len(pblocks)))
        else:
            print("  [%s] 池 %06X-%06X (%d B) 内无可达性重叠 OK"
                  % (name, pool[0], pool[1], pool[1] - pool[0] + 1))
    return 0


# ── 源码级：非 static 的局部 xdata 数组 ────────────────────────────────────
_RE_LOCAL_ARR = re.compile(r"^\s+(?!static\b)(?:const\s+)?u8\s+xdata\s+\w+\s*\[")


def check_sources(roots, quiet=False):
    bad = []
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            if "_deprecated" in dirpath or "_out_" in dirpath:
                continue
            for fn in files:
                if not fn.endswith(".c"):
                    continue
                p = os.path.join(dirpath, fn)
                for i, line in enumerate(open(p, "r", errors="ignore"), 1):
                    if _RE_LOCAL_ARR.match(line):
                        bad.append("%s:%d  %s" % (p.replace("\\", "/"), i, line.strip()))
    if bad:
        print("  !! 发现 %d 处『非 static 局部 xdata 数组』（必进 overlay 池）：" % len(bad))
        for b in bad:
            print("      - " + b)
        print("      -> 加 static 即可退出池子；数组一旦被参数块踩，是整块数据错。")
        return 4
    if not quiet:
        print("  [源码] 无非 static 局部 xdata 数组 OK")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", action="append", default=None,
                    help="要检查的 map（可多次指定）")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    rc = 0
    maps = a.map or ["tools/_out_bl/Bootloader.map", "tools/_out_app/APP.map"]
    for m in maps:
        rc = max(rc, check(m, a.quiet))
    rc = max(rc, check_sources(["./BSP", "./COMMON", "./OTA", "./APP",
                                "./Project", "./DRV"], a.quiet))
    if rc:
        print("  [FAIL] overlay 守门未通过（退出码 %d）" % rc)
    elif not a.quiet:
        print("  [PASS] overlay 守门全部通过")
    return rc


if __name__ == "__main__":
    sys.exit(main())
