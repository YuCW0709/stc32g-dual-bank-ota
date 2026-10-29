#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
merge_hex.py  --  把 Bootloader.hex 与 APP.hex 合并成一个可一次烧录的 Intel HEX

背景
    本工程 BL 定位在代码域 0xFF0000（FF:0000h），APP 定位在 0xFF4000（FF:4000h）。
    STC-ISP 一次只能烧一个 hex，因此需要合并。合并前必须确认两段不重叠、
    且都落在代码块内、没有误落到暂存区/参数区。

用法
    python tools/merge_hex.py --in BL.hex --in APP.hex --out combined.hex --check
    python tools/merge_hex.py --in BL.hex --in APP.hex --out combined.hex   # 不打印详情

退出码
    0 成功；1 参数错误；2 解析失败；3 重叠；4 越界/落在数据区
"""

import argparse
import sys

# ---- 与 ota_partition.h 保持一致的布局（改分区表后同步改这里）----------------
CODE_BASE = 0xFE0000          # IAP 地址 0 对应的代码域物理基址（24 位 far 视图）
NEAR_ALIAS_TOP = 0x010000     # 低 64K（0x000000-0x00FFFF）是 IAP 0x0000-0xFFFF
                              # 的 near-code 别名窗口，C251 的 16 位常量只能落在这里

# (IAP 起始, IAP 结束, 名称, 用途)
#   用途 = "code"  允许放代码       -> 分区合法
#          "const" 只放 near 常量   -> 合法，但一段 hex 只应出现属于自己的那一个
#          "data"  运行时数据区     -> 代码落进来就是地址设错了
PARTITIONS = [
    (0x00000, 0x0BFFF, "STAGING",    "data"),
    (0x0C000, 0x0CFFF, "PARAM",      "data"),
    (0x0D000, 0x0DFFF, "RESERVED",   "data"),
    (0x0E000, 0x0EFFF, "NCONST_BL",  "const"),
    (0x0F000, 0x0FFFF, "NCONST_APP", "const"),
    (0x10000, 0x13FFF, "BOOTLOADER", "code"),
    (0x14000, 0x1FFFF, "APP",        "code"),
]
FLASH_TOP = 0x20000           # IAP 域上限（EEPROM=128K）

# 代码域分区（与 ota_partition.h v5 保持一致）
OTA_BL_IAP_BASE_DEFAULT  = 0x10000
OTA_BL_END_DEFAULT       = 0x13FFF
OTA_APP_IAP_BASE_DEFAULT = 0x14000
OTA_APP_END_DEFAULT      = 0x1FFFF
OTA_NCONST_BL            = 0x0E000
OTA_NCONST_APP           = 0x0F000


def to_iap(addr):
    """代码域地址 -> IAP 相对地址；不合法返回 None。

    两种合法写法指向同一片 flash：
      0x000000-0x00FFFF  near-code 别名（16 位寻址唯一能摸到的窗口，NCONST 落这里）
      0xFE0000-0xFFFFFF  far 视图，IAP = addr - 0xFE0000
    """
    if 0 <= addr < NEAR_ALIAS_TOP:
        return addr
    if CODE_BASE <= addr < CODE_BASE + FLASH_TOP:
        return addr - CODE_BASE
    return None


def part_of(iap):
    """IAP 地址 -> (名称, 用途)；不在任何分区返回 (None, None)。"""
    for (ps, pe, name, kind) in PARTITIONS:
        if ps <= iap <= pe:
            return name, kind
    return None, None


# ---------------------------------------------------------------- Intel HEX 解析
def parse_hex(path):
    """返回 {线性地址: 字节} 与最大地址。支持记录类型 00/01/02/04。"""
    mem = {}
    base = 0
    seen_eof = False
    with open(path, "r", encoding="ascii", errors="strict") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError("%s:%d 不是 Intel HEX 行" % (path, lineno))
            try:
                data = bytes.fromhex(line[1:])
            except ValueError:
                raise ValueError("%s:%d 十六进制字符非法" % (path, lineno))
            if len(data) < 5:
                raise ValueError("%s:%d 记录过短" % (path, lineno))
            count, addr, rtype = data[0], (data[1] << 8) | data[2], data[3]
            payload = data[4:4 + count]
            if len(payload) != count:
                raise ValueError("%s:%d 长度字段不符" % (path, lineno))
            chk = (-(sum(data[:4 + count]))) & 0xFF
            if chk != data[4 + count]:
                raise ValueError("%s:%d 校验和错误" % (path, lineno))

            if rtype == 0x00:                       # 数据
                a = base + addr
                for i, b in enumerate(payload):
                    mem[a + i] = b
            elif rtype == 0x01:                     # 文件结束
                seen_eof = True
                break
            elif rtype == 0x02:                     # 扩展段地址（x16）
                base = (((payload[0] << 8) | payload[1]) << 4)
            elif rtype == 0x04:                     # 扩展线性地址
                base = (((payload[0] << 8) | payload[1]) << 16)
            elif rtype in (0x03, 0x05):             # 起始地址，忽略
                pass
            else:
                pass
    if not mem:
        raise ValueError("%s 没有数据记录" % path)
    return mem, seen_eof


def segments(mem):
    """把 {地址:字节} 压成 [(起, 止含, 长度)] 连续段。"""
    addrs = sorted(mem)
    segs = []
    s = prev = addrs[0]
    for a in addrs[1:]:
        if a == prev + 1:
            prev = a
            continue
        segs.append((s, prev, prev - s + 1))
        s = prev = a
    segs.append((s, prev, prev - s + 1))
    return segs


# ---------------------------------------------------------------- 写出 HEX-386
def write_hex(path, mem):
    addrs = sorted(mem)
    lines = []
    upper = None
    i = 0
    while i < len(addrs):
        a = addrs[i]
        # 同一条记录内地址必须连续，且不跨 64KB 边界（Intel HEX 要求）
        chunk = [a]
        j = i + 1
        while (j < len(addrs) and addrs[j] == chunk[-1] + 1
               and len(chunk) < 16 and (addrs[j] >> 16) == (a >> 16)):
            chunk.append(addrs[j])
            j += 1
        hi = a >> 16
        if hi != upper:
            rec = bytes([2, 0, 0, 4, (hi >> 8) & 0xFF, hi & 0xFF])
            lines.append(enc(rec))
            upper = hi
        lo = a & 0xFFFF
        payload = bytes(mem[x] for x in chunk)
        rec = bytes([len(payload), (lo >> 8) & 0xFF, lo & 0xFF, 0]) + payload
        lines.append(enc(rec))
        i = j
    lines.append(":00000001FF")
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def enc(rec_no_chk):
    chk = (-sum(rec_no_chk)) & 0xFF
    return ":" + (rec_no_chk + bytes([chk])).hex().upper()


# ---------------------------------------------------------------- 归属裁剪
# 为什么需要"裁剪"：
#   C251 链接器会在 CODE 类的 **BASE** 地址（设备默认 0xFF0000）强制生成 3 字节
#   复位跳转 `?CO?start251`，无论怎么设置 CODE()/CLASSES() 都搬不走它（实测：
#   设了 CLASSES(CODE(0xFF4000-...)) 后 CODE 段从 0xFF4000 起，但这段仍在 0xFF0000）。
#   于是 **APP.hex 里必然有 0xFF0000~0xFF0002 这 3 个字节**，而这段属于 Bootloader。
#   合并不裁剪就会报"值不同"（APP 的复位跳转指向 FF:4058，BL 的指向自己的启动代码）。
#
#   处理原则：每个输入文件只保留落在"自己分区"里的字节，其余打印出来并丢弃。
#   这比"让 BL 优先"更明确——因为被丢的字节本来就不该出现在这个文件的镜像里。
ALLOWED_PARTS = {
    "BL":  ("BOOTLOADER", "NCONST_BL"),
    "APP": ("APP", "NCONST_APP"),
}


def classify(mem):
    """判断这份 hex 是 BL、APP，还是已经合并好的整片镜像。

    判据：看字节落在哪个分区。整片镜像（BL+APP 都在）不能裁剪，
    否则 --info 回读自己刚生成的合并文件时会把另一半当"杂质"丢掉。
    """
    cnt = {}
    for a in mem:
        iap = to_iap(a)
        nm = part_of(iap)[0] if iap is not None else None
        cnt[nm] = cnt.get(nm, 0) + 1
    bl = cnt.get("BOOTLOADER", 0) + cnt.get("NCONST_BL", 0)
    app = cnt.get("APP", 0) + cnt.get("NCONST_APP", 0)
    if bl == 0 and app == 0:
        return None
    if bl > 256 and app > 256:
        return "MERGED"          # 两边都有实体内容 -> 已合并，不动
    return "BL" if bl >= app else "APP"


def trim_to_own(mem, kind):
    """只保留属于该工程分区的字节；返回 (新 mem, 被丢弃的分区统计)。"""
    allowed = ALLOWED_PARTS[kind]
    keep, drop = {}, {}
    for a, b in mem.items():
        iap = to_iap(a)
        nm = part_of(iap)[0] if iap is not None else None
        if nm in allowed:
            keep[a] = b
        else:
            drop[nm] = drop.get(nm, 0) + 1
    return keep, drop


# ---------------------------------------------------------------- 检查
ISP_ADDR_MIN = 0xFE0000       # STC-ISP 可寻址下界（far 视图）
ISP_ADDR_MAX = 0xFFFFFF       # EEPROM=128K 时的上界


def isp_compat(mem):
    """STC-ISP 可寻址范围检查 —— 返回 True 表示这份 hex 能直接喂给 STC-ISP。

    依据（国芯论坛官方回复）：
        「isp 烧录位置，就是图里"程序文件"选项卡内显示的位置，一般 0xFF0000 开始」
        「怎么看超范围，根据 isp 下载时选的 eeprom 大小，假设 eeprom 大小为 M，
          范围就是 FE:0000 到 FE:0000 + M」

    也就是说 **hex 里的地址必须写成 far 视图（0xFE0000 起）**。
    Keil 的 near-code 别名窗口 0x0000~0xFFFF 在硬件上完全合法
    （NCONST near 常量就落在这儿），但 STC-ISP 不认，打开文件时直接报：

        HEX地址异常,超出可寻址范围 !

    这是本项目真实踩过的坑：LARGE 模型改造**之前**生成的旧 hex 里
    有一小段 0x00E000 起的 near 常量，STC-ISP 拒收。
    """
    bad = sorted(a for a in mem if a < ISP_ADDR_MIN)
    if not bad:
        return True

    lo, hi, n = bad[0], bad[-1], len(bad)
    print("[ISP兼容] !! %d 字节落在 0x%06X 以下 —— STC-ISP 会报" % (n, ISP_ADDR_MIN))
    print("           「HEX地址异常,超出可寻址范围 !」")
    print("           问题段：0x%06X-0x%06X" % (lo, hi))
    if hi < NEAR_ALIAS_TOP:
        print("           这是 near-code 别名窗口（0x0000-0xFFFF）：硬件合法，但 STC-ISP 只认")
        print("           FE:0000 起的 far 视图。典型来源是 NCONST（near 常量）段。")
        print("           → 检查 C251 Misc controls 是否漏了 LARGE；")
        print("           → 或 L251 CLASSES 里 NCONST 没锁到 0xE000 以上。")
        print("           → 若是很久以前编出来的 hex，直接 Rebuild 重生成。")
    else:
        print("           地址既不在 near 别名窗口、也不在 far 视图 —— 链接脚本写错了。")
    return False


def check(mem, quiet_ok=False, skip_isp=False):
    """重叠 -> 3；越界或落进非代码区 -> 4；STC-ISP 不认 -> 5；通过 -> 0。"""
    segs = segments(mem)
    rc = 0

    if not skip_isp and not isp_compat(mem):
        rc = 5
    for i in range(1, len(segs)):
        if segs[i][0] <= segs[i - 1][1]:
            print("[错误] 数据重叠：段 0x%06X-0x%06X 与 0x%06X-0x%06X"
                  % (segs[i - 1][0], segs[i - 1][1], segs[i][0], segs[i][1]))
            rc = 3

    for (s, e, n) in segs:
        iap_s, iap_e = to_iap(s), to_iap(e)
        if iap_s is None or iap_e is None:
            print("[错误] 0x%06X-0x%06X 不是合法 flash 地址" % (s, e))
            rc = 4
            continue
        name, kind = part_of(iap_s)
        name_e, _ = part_of(iap_e)
        if name is None or name_e != name:
            print("[错误] 0x%06X-0x%06X (IAP 0x%05X-0x%05X) 跨分区或落在分区缝隙"
                  % (s, e, iap_s, iap_e))
            rc = 4
        elif kind == "data":
            print("[错误] 0x%06X-0x%06X 落在 %s 区 —— 地址设错了？"
                  % (s, e, name))
            rc = 4
        elif not quiet_ok:
            print("       %-10s 0x%06X-0x%06X  %6d B" % (name, s, e, n))
    return rc


def info(mem, had_ext=True):
    """体检：判断这份 hex 是 BL 还是 APP、格式对不对、地址合法与否。"""
    segs = segments(mem)
    lo = min(s[0] for s in segs)
    hi = max(s[1] for s in segs)

    # 只按"代码段"判身份：near 常量段（0x00Exxx/0x00Fxxx）不参与
    code_addrs, const_addrs = [], []
    for (s, e, n) in segs:
        iap_s = to_iap(s)
        name, kind = part_of(iap_s) if iap_s is not None else (None, None)
        (const_addrs if kind == "const" else code_addrs).append((s, e, n, name))

    print("数据段数 : %d  (代码段 %d + 常量段 %d)"
          % (len(segs), len(code_addrs), len(const_addrs)))
    print("起始地址 : 0x%06X   (IAP %s)"
          % (lo, ("0x%05X" % to_iap(lo)) if to_iap(lo) is not None else "非法"))
    print("结束地址 : 0x%06X" % hi)
    print("总字节数 : %d" % len(mem))

    print("\n[格式] ", end="")
    if had_ext:
        print("OK —— 文件含 04 型扩展地址记录（HEX-386）")
    else:
        print("!! 没有任何 04 型记录，是 HEX-80，凡 >64KB 的地址都会被截断")

    print("[分段]")
    for (s, e, n) in segs:
        iap_s = to_iap(s)
        name, kind = part_of(iap_s) if iap_s is not None else ("非法", None)
        tag = {"code": "代码", "const": "常量", "data": "!!数据区"}.get(kind, "!!非法")
        print("       0x%06X-0x%06X %6d B  IAP %s  %-11s %s"
              % (s, e, n,
                 ("0x%05X" % iap_s) if iap_s is not None else "  ---- ",
                 name or "?", tag))

    # 低 64K 只允许出现 near 常量；出现别的说明 ECODE 又掉回 FE0000 了
    bad_low = [(s, e, n) for (s, e, n, nm) in code_addrs if s < NEAR_ALIAS_TOP]
    if bad_low:
        print("\n[连续性] !! 有代码段落在低 64K 别名窗口 —— 函数体没被 ECODE 搬走")
        for (s, e, n) in bad_low:
            print("           0x%06X-0x%06X  %d B" % (s, e, n))
    else:
        # 同一分区内的碎块是中断向量表（未用向量不产生字节），属正常
        regions = set(nm for (_, _, _, nm) in code_addrs)
        if regions == {"BOOTLOADER", "APP"}:
            print("\n[连续性] OK —— BL 与 APP 相邻拼成整片 FF 块")
        elif len(regions) > 1:
            print("\n[连续性] !! 代码段跨了多个分区：%s"
                  % "、".join(sorted(str(r) for r in regions)))

    print("[身份] ", end="")
    if not code_addrs:
        print("!! 没有代码段")
    else:
        c_lo = min(x[0] for x in code_addrs)
        c_hi = max(x[1] for x in code_addrs)
        iap_lo, iap_hi = to_iap(c_lo), to_iap(c_hi)
        regions = set(nm for (_, _, _, nm) in code_addrs)
        if regions == {"BOOTLOADER", "APP"}:
            print("合并镜像 BL+APP（0x%06X~0x%06X，IAP 0x%05X~0x%05X）"
                  % (c_lo, c_hi, iap_lo, iap_hi))
        elif iap_lo >= OTA_BL_IAP_BASE_DEFAULT and iap_hi <= OTA_BL_END_DEFAULT:
            print("Bootloader（0x%06X~0x%06X，IAP 0x%05X~0x%05X）"
                  % (c_lo, c_hi, iap_lo, iap_hi))
        elif iap_lo >= OTA_APP_IAP_BASE_DEFAULT and iap_hi <= OTA_APP_END_DEFAULT:
            print("APP（0x%06X~0x%06X，IAP 0x%05X~0x%05X）"
                  % (c_lo, c_hi, iap_lo, iap_hi))
        else:
            print("!! 代码段 0x%06X~0x%06X (IAP 0x%05X~0x%05X) 不在 BL/APP 任一区内"
                  % (c_lo, c_hi, iap_lo, iap_hi))

    for (s, e, n, nm) in const_addrs:
        want = "NCONST"
        if nm and nm != "NCONST_BL" and nm != "NCONST_APP":
            print("[警告] 常量段 0x%06X-0x%06X 落在 %s 区，会与暂存区/参数区冲突"
                  % (s, e, nm))
        else:
            print("[常量] 0x%06X-0x%06X  %d B  在 %s —— %s"
                  % (s, e, n, nm, "符合 BL" if nm == "NCONST_BL" else "符合 APP"))

    # STC-ISP 能不能收，比分区归属更优先——收不下的 hex 其它都白说
    ok_isp = isp_compat(mem)

    print("\n[分区占用]")
    rc = check(mem, skip_isp=True)
    if not ok_isp:
        return 5
    return rc


def main():
    ap = argparse.ArgumentParser(description="合并 BL/APP 的 Intel HEX（HEX-386）")
    ap.add_argument("--in", dest="inputs", action="append", required=True,
                    help="输入 hex，可重复")
    ap.add_argument("--out", dest="out", help="输出 hex（--info 模式下可省略）")
    ap.add_argument("--check", action="store_true", help="打印分段并做重叠/越界检查")
    ap.add_argument("--info", action="store_true",
                    help="只体检不合并：看起始地址/HEX 格式/是否落在合法分区")
    ap.add_argument("--no-trim", action="store_true",
                    help="不按分区裁剪（默认会丢掉 APP 文件里 C251 强制的 0xFF0000 复位跳转）")
    args = ap.parse_args()

    total = {}
    for k, path in enumerate(args.inputs, 1):
        try:
            mem, eof = parse_hex(path)
        except (OSError, ValueError) as ex:
            print("[错误] 解析失败：%s" % ex)
            return 2
        segs = segments(mem)
        lo = min(s[0] for s in segs)
        hi = max(s[1] for s in segs)
        print("[%d] %-42s 0x%06X-0x%06X  %7d B%s"
              % (k, path, lo, hi, len(mem), "" if eof else "  (缺 EOF 记录)"))

        kind = classify(mem)
        if kind and kind != "MERGED" and not args.no_trim:
            mem, drop = trim_to_own(mem, kind)
            if drop:
                desc = "、".join("%s %d B" % (n or "未知", c)
                                for n, c in sorted(drop.items(), key=lambda x: -x[1]))
                print("        [裁剪] 判定为 %s 镜像，丢弃不属于它的字节：%s"
                      % (kind, desc))
                print("               （0xFF0000 上的 ?CO?start251 是链接器强制生成的"
                      "复位跳转，归 Bootloader）")
            if not mem:
                print("[错误] %s 裁剪后为空 —— 分区表或链接地址不对" % path)
                return 4

        for a, b in mem.items():
            if a in total and total[a] != b:
                print("[错误] 0x%06X 处两个文件的值不同（0x%02X vs 0x%02X）"
                      % (a, total[a], b))
                return 3
            total[a] = b

    if args.info:
        had = any(l.startswith(":02000004") for l in
                  open(args.inputs[0], encoding="ascii", errors="ignore").read().splitlines())
        return info(total, had)

    if args.check:
        rc = check(total)
        if rc:
            return rc
        segs = segments(total)
        gap = 0
        for i in range(1, len(segs)):
            # 只统计同一 64K 块内的空洞；跨块的空洞是 near 常量与 FF 块之间的
            # 地址空间，属布局本身，不是问题
            if (segs[i][0] >> 16) != (segs[i - 1][1] >> 16):
                continue
            gap = max(gap, segs[i][0] - segs[i - 1][1] - 1)
        print("[check] 无重叠，同块内最大空洞 0x%04X 字节，合计 %d B"
              % (gap, len(total)))

    try:
        write_hex(args.out, total)
    except OSError as ex:
        print("[错误] 写文件失败：%s" % ex)
        return 1
    print("[out] %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
