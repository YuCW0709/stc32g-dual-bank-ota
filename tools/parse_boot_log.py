#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parse_boot_log.py —— 解析 Bootloader 串口日志，直接给出 P0 结论

用法:
    python tools/parse_boot_log.py <日志文件.txt>
    python tools/parse_boot_log.py --text "[BOOT-POLL] ... [BL] selftest OK ..."

为什么要工具化:
    1) 串口日志是**滚动**的，关键判据散在几十行里，人眼容易漏。
    2) 更危险的是**假阳性**：`[IAP] eeprom=128KB` 只是把形参原样念一遍
       （读不到芯片选项字节），而 `selftest OK` 在"探针被跳过"时也可能是假的。
       2026-10-25 就踩过：APP 区非空 -> 跳过探针 -> 硬设 writable=1 ->
       日志打出 "OTA arch (a) is valid"，其实一次写都没做过。
    3) 所以结论必须来自**实测的 stg/app 探针字段**，本脚本强制如此。

判据依据: docs/01_编译与烧录步骤.md §7.2 / §7.5
"""

import io
import re
import sys


def _out(s):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    print(s)


def read_log(path):
    raw = io.open(path, "rb").read()
    for enc in ("utf-8", "gbk", "gb18030", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1", errors="replace")


def _last(seq):
    return seq[-1] if seq else None


# ---------------------------------------------------------------------------
# v0.8 行校验和：每行尾 `*HH` = 行首到 `*` 之前所有字节的 8 位累加和
#
# 为什么需要它（2026-10-25 v0.7 上板实测）：数字全对了，但 1700 多字符里
# 有 2 处字符变成了空格（`[7 9]`、`APP head a=75  4`）。而 log.c 的数字通路
# 只可能输出 `0-9 a-f -`，物理上吐不出 0x20 —— 所以只剩两种可能：
#   (a) 缓冲区里就是空格 -> 格式化错（固件问题）
#   (b) 缓冲区对、线上被打坏 -> 传输错（硬件/时序问题）
# 光看日志分不清，而两种的修法完全相反。算一遍和值就能当场分开。
#
# 语义是单向的：和值错 => 这行一定坏；和值对 => 只说明没被改动。
# ---------------------------------------------------------------------------

SUM_RE = re.compile(r"\*([0-9a-f]{2})$")


def _strip_sum(line):
    m = SUM_RE.search(line)
    return line[:m.start()] if m else line


def verify_sums(txt):
    """逐行校验 *HH。返回 (带校验的行数, [(行号, 原文, 记录和, 实算和)])"""
    n = 0
    bad = []
    for i, ln in enumerate(txt.splitlines(), 1):
        s = ln.rstrip("\r")
        m = SUM_RE.search(s)
        if not m:
            continue
        n += 1
        body = s[:m.start()]
        calc = sum(ord(ch) & 0xFF for ch in body) & 0xFF
        if calc != int(m.group(1), 16):
            bad.append((i, s, m.group(1), calc))
    return n, bad


# ---------------------------------------------------------------------------

def collect(txt):
    """把日志里所有关心的判据抽出来。"""
    d = {}

    # --- 0) 先做行校验和，把坏行剔除、把 *HH 从各行尾摘掉 ----------------
    #   顺序很重要：必须先校验再摘尾巴，且摘完尾巴后续所有正则都不用改。
    n_sum, sum_bad = verify_sums(txt)
    d["sum_n"] = n_sum
    d["sum_bad"] = sum_bad
    if sum_bad:
        drop = set(i - 1 for i, _s, _w, _c in sum_bad)
        lines = [l for j, l in enumerate(txt.splitlines()) if j not in drop]
    else:
        lines = txt.splitlines()
    txt = "\n".join(_strip_sum(l) for l in lines)

    # --- 探针结果（新格式：stg@0x0=W / app@0x14000=untested）-------------
    d["stg"] = _last(re.findall(r"stg@0x[0-9A-Fa-f]+\s*=\s*(\w+)", txt))
    d["app"] = _last(re.findall(r"app@0x[0-9A-Fa-f]+\s*=\s*(\w+)", txt))
    # v0.3 起不再打印地址：`3addr-reg ok, stg=W, app=untested, BL intact`
    m = re.search(r"stg=(\w+)[,\s]+app=(\w+)", txt)
    if m:
        d["stg"], d["app"] = m.group(1), m.group(2)

    # --- 旧格式兼容：probe@0x14000=W -------------------------------------
    probes = re.findall(r"probe@0x([0-9A-Fa-f]+)\s*=\s*([A-Za-z]+)", txt)
    d["probes"] = probes
    if d["stg"] is None:
        for a, r in probes:
            if a.upper() in ("0", "00000"):
                d["stg"] = r
            elif a.upper().startswith("14"):
                d["app"] = r

    # --- 头字节对照（判断 IAP 读是否可信）--------------------------------
    m = re.search(r"bl head:\s*((?:[0-9a-fA-F]{1,2}\s+){3}[0-9a-fA-F]{1,2})", txt)
    d["bl_head"] = m.group(1).split() if m else None
    m = re.search(r"app head:\s*((?:[0-9a-fA-F]{1,2}\s+){3}[0-9a-fA-F]{1,2})", txt)
    if m:
        d["app_head"] = m.group(1).split()
    else:
        # v0.5+ 拆成两行各 2 字节：
        #   [IAP] app head: 75 84
        #   [IAP] app head: 1 7e (want 75 84 1 7e)
        two = re.findall(r"app head:\s*([0-9a-fA-F]{1,2})\s+([0-9a-fA-F]{1,2})", txt)
        d["app_head"] = [two[0][0], two[0][1], two[1][0], two[1][1]] if len(two) >= 2 else None

    d["bl_intact"] = ("BL intact" in txt)
    d["bl_changed"] = ("BL CHANGED" in txt)
    d["skip_probe"] = ("probe skipped" in txt or "skip probe" in txt)

    # --- ★ 读通路是否可信（2026-10-25 IAP_CMD 1/2 写反事故后的新增判据）---
    #   正常 BL 首字节必须是 0x02（链接器在 0xFF0000 生成的 3 字节 LJMP）。
    #   不是 0x02 就意味着"IAP 读"不可信，此时任何 stg/app 结论都无效。
    d["read_untrusted"] = ("IAP READ path untrusted" in txt)
    d["bl_head_bad"] = bool(
        d["bl_head"] and int(d["bl_head"][0], 16) != 0x02)
    d["bl_head_zero"] = bool(
        d["bl_head"] and all(int(x, 16) == 0 for x in d["bl_head"]))

    # --- selftest ------------------------------------------------------
    d["stl_ok"] = bool(re.search(r"selftest OK", txt))
    m = re.search(r"selftest FAIL rc=(\d+)", txt)
    d["stl_fail"] = int(m.group(1)) if m else None
    d["stl_warn"] = ("some region NOT writable" in txt
                     or "probe says NOT writable" in txt)

    # --- summary 重播行（每 8s 一次，最容易抓到）------------------------
    summ = re.findall(r"summary:\s*init=(\d+)\s+selftest=(\d+)\s+stg=(\w+)\s+app=(\w+)", txt)
    if not summ:   # 旧格式：init=x selftest=y
        old = re.findall(r"summary:\s*init=(\d+)\s+selftest=(\d+)", txt)
        if old:
            d["summary"] = old[-1] + (d["stg"], d["app"])
        else:
            d["summary"] = None
    else:
        d["summary"] = summ[-1]

    # --- 输出链路活性 ---------------------------------------------------
    d["n_poll"] = len(re.findall(r"BOOT-POLL", txt))
    d["n_alive"] = len(re.findall(r"alive #", txt))
    d["n_boot"] = len(re.findall(r"\[BL\] boot,", txt))

    # --- P3.1 引脚读回 --------------------------------------------------
    p3s = re.findall(r"p3=([0-9A-Fa-f]{1,2})", txt)
    d["p3"] = p3s[-1] if p3s else None
    # v0.6 起 alive 行去掉了 %s 三目（" (txd low!)" 不再出现），
    # 改为按 p3 的 bit1 直接判断，并统计"读到过低电平"的次数。
    d["p3_low_n"] = sum(1 for v in p3s if not (int(v, 16) & 0x02))
    d["txd_low"] = ("txd low" in txt) or (d["p3_low_n"] > 0)

    # --- 跳转入口字节（v0.6 起 ota_jump 只诊断不拦）----------------------
    #   [JUMP] entry via-base=75     <- 用 app_base - OTA_PHYS_BASE 算地址
    #   [JUMP] entry via-const=75    <- 用常量 OTA_APP_IAP_BASE
    #   两者不一致 = IAP 读在深调用栈上下文里不稳定，必须记下来。
    m = re.search(r"via-base=([0-9A-Fa-f]{1,2})", txt)
    d["head_base"] = m.group(1) if m else None
    m = re.search(r"via-const=([0-9A-Fa-f]{1,2})", txt)
    d["head_const"] = m.group(1) if m else None
    d["head_mismatch"] = bool(
        d["head_base"] and d["head_const"] and d["head_base"] != d["head_const"])

    m = re.search(r"jump entry hi=([0-9A-Fa-f]{1,4}) lo=([0-9A-Fa-f]{1,4})", txt)
    d["entry_hi"], d["entry_lo"] = (m.group(1), m.group(2)) if m else (None, None)
    d["entry_ok"] = bool(
        d["entry_hi"] and d["entry_lo"]
        and (int(d["entry_hi"], 16) << 16 | int(d["entry_lo"], 16)) == 0xFF4000)

    # 跳转结果：v0.6 正常应"没有"这两行（跳过去就不回来了）
    m = re.search(r"jump returned rc=(\d+)", txt)
    d["jump_rc"] = int(m.group(1)) if m else None
    d["app_alive"] = len(re.findall(r"\[APP\] alive", txt))
    d["app_boot"] = ("[APP]" in txt and "==== APP" in txt)

    # --- IAP 初始化 -----------------------------------------------------
    m = re.search(r"flash_iap_init\(\d+\)\s*->\s*(\d+)", txt)
    d["init_rc"] = int(m.group(1)) if m else None

    d["halt"] = ("IAP init failed" in txt or "cannot write anything" in txt)

    # --- 日志格式化自检（v0.4 起）--------------------------------------
    #   形如：  [BL] chkA %u: 0 1 9 10 15 16 255 256 65535
    #   抓 tag 之后的全部内容，和期望值逐字比对。
    d["chk"] = {}
    for k in ("A", "B", "C", "D", "E"):
        m = re.search(r"chk" + k + r"\b[^:]*:\s*([^\r\n]*)", txt)
        if m:
            # 后来自检可能被重复打印，取最后一行
            allm = re.findall(r"chk" + k + r"\b[^:]*:\s*([^\r\n]*)", txt)
            d["chk"][k] = allm[-1] if allm else m.group(1)
    return d


# 自检的期望输出（必须和 common/log.c 里 log_selftest() 的格式串一致）
#
# v0.7 起日志改成"固定 4 个 32 位槽、无 varargs"，每条最多 4 个值，
# 所以自检拆成 5 行、每行不超过 4 个值。%u 与 %lu 等价（都读一个槽），
# %x 抑制前导零且至少出 1 位，所以 0 打成 "0" 而不是 "0x0"。
# ★ 注意抑制粒度是**半字节**不是字节：0xFF -> "ff"（不是 "f"），
#   0x0F -> "f"（不是 "0f"）。v0.7 的文档曾把 chkD 写成 "0 f 10000 ff4000"，
#   错在少了第二个 f，实测一直是 "0 ff 10000 ff4000"。
CHK_EXPECT = {
    "A": "0 1 9 10",
    "B": "15 16 255 256",
    "C": "65535 4294967295",
    "D": "0 ff 10000 ff4000",
    "E": "65537|mid|9|14000",
}


# ---------------------------------------------------------------------------

def report(d):
    ok, warn, bad = [], [], []

    # ---- 0) ★ 行校验和：先把"日志本身"判一次 ----
    #   v0.8 起每行尾带 *HH。和值错 => 这行在传输中被改动（固件无辜）；
    #   和值对 => 没被改动（但内容逻辑正确性另说，还要看下面的自检行）。
    n_sum = d.get("sum_n", 0)
    sum_bad = d.get("sum_bad", [])
    if n_sum == 0:
        warn.append("日志里没有行校验和 *HH（v0.8 之前的固件）")
        warn.append("  -> 无法逐行自证真伪，个别字符对不上时只能靠重抓")
    elif sum_bad:
        bad.append("★★ 行校验和 %d/%d 不符 -> 这几行在传输中被改动，已剔除"
                   % (len(sum_bad), n_sum))
        for ln_no, s, want, calc in sum_bad[:5]:
            bad.append("  第%d行 记录和=%s 实算和=%02x : %s"
                       % (ln_no, want, calc, s[:56]))
        bad.append("  -> 属于**传输/硬件**问题，不是固件；主要数字请以"
                   "和值对的行（或重抓一次）为准")
    else:
        ok.append("行校验和 %d/%d 全对 -> 这份日志逐行可自证，没有传输损坏"
                  % (n_sum, n_sum))

    # ---- 0.5) ★ 再验证"格式化可不可信" ----
    #   2026-10-25 实测事故：程序功能完全正常（跳转成功、心跳不断），
    #   但日志里同一份字节在两个文件里打出两种结果 —— 一个 `75 84 1 7e`，
    #   一个 `    1  `；同一个地址一个 `0xff4000`、一个 `0x12`。
    #   根因是 log_printf 的参数列表里嵌套了函数调用 + 局部 xdata 数组
    #   进了共享 overlay。**代码没错，日志在撒谎。**
    #   所以这里第一步先对自检行，对不上就声明后面所有数字作废，
    #   否则会把排查引到完全无关的方向（比如误判成跳转地址算错）。
    chk = d["chk"]
    if not chk:
        warn.append("没找到 chkA~chkE 自检行（烧的是 v0.4 之前的固件）")
        warn.append("  -> 本份日志里的数字有可能是错的，见文档 §8.9")
    else:
        CHK_ALL_OK = True
        for k in ("A", "B", "C", "D", "E"):
            if k not in chk:
                warn.append("自检缺 chk%s 行（可能被抓日志截断）" % k)
                continue
            got = chk[k].strip()
            want = CHK_EXPECT[k]
            if got == want:
                ok.append("chk%s 正确 -> 这类值打印可信" % k)
            else:
                CHK_ALL_OK = False
                bad.append("chk%s 不符：日志说 '%s'，应为 '%s'"
                           % (k, got, want))
        if not CHK_ALL_OK:
            d["chk_ok"] = False
            bad.append("★★ 日志格式化仍在错位 —— 本份日志里所有数字都不可信")
            bad.append("  -> 不要据此判断探针/地址/跳转，先修 log.c（§8.9）")
        else:
            d["chk_ok"] = True

    # ---- 1) 输出链路 ----
    if d["n_alive"] > 0:
        ok.append("心跳 alive 出现 %d 次 -> EA=1 中断发送路径正常" % d["n_alive"])
    elif d["n_poll"] > 0:
        warn.append("只有 BOOT-POLL（轮询路径），没有 alive 心跳")
        warn.append("  -> 可能中断发送路径有问题，也可能日志抓得太短（心跳 2s 一行）")
    else:
        bad.append("完全没有输出：先按文档 §7.3 查 COM 口/波特率/接线")

    if d["txd_low"]:
        bad.append("P3.1 读回为低（日志出现 txd low）-> 板上有东西拽住该脚")

    # ---- 2) IAP 读是否可信（看 BL 头字节）----
    #   这是 2026-10-25 事故之后加的**最高优先级**判据：
    #   IAP_CMD 的 1/2 写反时，"读"其实在往 Flash 写 0x00，
    #   一次上电就能把 0xFF0000 的 LJMP 抹掉 —— 而普通日志看起来
    #   完全像"第三地址寄存器没生效"，会被误判成方案 (b)。
    if d["read_untrusted"] or d["bl_head_bad"]:
        bad.append("★★ IAP 读通路不可信：BL 首字节不是 0x02")
        if d["bl_head"]:
            bad.append("  bl head = %s（正常应为 2 xx xx xx）"
                       % " ".join(d["bl_head"]))
        if d["bl_head_zero"]:
            bad.append("  全 0 = 复位入口已被写坏 -> 下次复位起不来（ISP 下载仍可救）")
            bad.append("  已知元凶：IAP_CMD 的 1(读)/2(写) 写反，'读'变成了写 0x00")
        bad.append("  -> 重新烧 build/step4_BL_APP_v0.4.hex（已修正）")
        bad.append("  -> 下面的 stg/app 结论一律作废，不要据此选方案")
    elif d["bl_head"]:
        h = [int(x, 16) for x in d["bl_head"]]
        if all(v == 0xFF for v in h):
            warn.append("BL 头 4 字节全为 FF -> BL 区竟然空白？检查 EEPROM 设置")
        else:
            ok.append("BL 头字节 %s -> IAP 读通路正常（读到真实机器码）"
                      % " ".join(d["bl_head"]))

    # ---- 3) IAP 初始化 ----
    if d["init_rc"] is not None:
        if d["init_rc"] == 0:
            ok.append("flash_iap_init(128) = 0")
        else:
            bad.append("flash_iap_init 返回 %d -> IAP 初始化失败" % d["init_rc"])

    if d["halt"]:
        bad.append("IAP init 失败后已 halt，后续流程未执行")

    # ---- 4) 关键：探针实测结果 ----
    stg, app = d["stg"], d["app"]

    if d["bl_changed"]:
        bad.append("★ BL CHANGED after probe! 硬件行为异常，立即停手")

    if stg == "W":
        ok.append("STAGING 区（IAP 0x00000）= W -> **真的写进去了**，IAP 写通路成立")
    elif stg == "NA":
        bad.append("STAGING 区 = NA -> IAP 写不进去")
        bad.append("  先查『检测MCU选项』的用户EEPROM大小 是否真为 128K")
        bad.append("  确认是 128K 仍 NA -> 才置 OTA_IAP_CAN_REACH_CODE=0 走方案 (b)")
    elif stg is None:
        warn.append("日志里没有 stg 探针字段 -> 烧的是旧固件（step1e 或更早）")

    if app == "W":
        ok.append("APP 区（IAP 0x14000）= W -> apply-move 落点可写")
    elif app == "untested":
        warn.append("APP 区 = untested -> **可写性未验证**（该区非空，探针主动跳过）")
        warn.append("  这不是失败，但不能据此宣称 P0 通过")
        if d["app_head"]:
            warn.append("  app 头字节: %s（若像机器码，说明确实有固件残留）"
                        % " ".join(d["app_head"]))
    elif app == "NA":
        bad.append("APP 区 = NA -> 无法搬运 staging，apply-move 会失败")

    if d["bl_intact"]:
        ok.append("BL intact（探针没破坏自身代码）")
    if d["skip_probe"] and app != "untested":
        warn.append("日志出现 'probe skipped' 但 app 字段不是 untested，请核对固件版本")

    # ---- 5) selftest / summary ----
    if d["stl_ok"]:
        ok.append("selftest 返回 OK")
    elif d["stl_fail"] is not None:
        bad.append("selftest FAIL rc=%d" % d["stl_fail"])
    elif d["stl_warn"]:
        bad.append("selftest 报告某区域不可写")

    if d["summary"]:
        i, s, sg, ap = d["summary"]
        ok.append("summary 重播: init=%s selftest=%s stg=%s app=%s"
                  % (i, s, sg or "?", ap or "?"))

    # ---- 6) ★ BL -> APP 跳转（v0.6 新增）--------------------------------
    #   2026-10-25 事故：ota_jump 用 app_base-OTA_PHYS_BASE 重算地址再读首字节，
    #   读到 0xFF -> 判"空片" -> 返回 OTA_ERR_VERIFY(12) -> APP 永远起不来。
    #   而同一时刻 bl_boot 用常量 OTA_APP_IAP_BASE 读到的却是 0x75。
    #   v0.6 处置：闸门移回 bl_boot，ota_jump 只诊断不拦。
    if d["entry_ok"]:
        ok.append("跳转入口 = 0xFF4000（hi=%s lo=%s）" % (d["entry_hi"], d["entry_lo"]))
    elif d["entry_hi"]:
        bad.append("跳转入口不是 0xFF4000：hi=%s lo=%s" % (d["entry_hi"], d["entry_lo"]))

    if d["head_mismatch"]:
        bad.append("★ 两条地址算法读到的 APP 首字节不一致：via-base=%s / via-const=%s"
                   % (d["head_base"], d["head_const"]))
        bad.append("  -> IAP 读在'深调用栈 + 刚跑过日志'的上下文里不稳定，")
        bad.append("     这是 ota_verify.c 阶段必须先解决的问题（读不可信则 CRC 不可信）")
    elif d["head_base"]:
        ok.append("两条地址算法读到同一首字节 %s -> IAP 读一致" % d["head_base"])

    if d["app_alive"] > 0:
        ok.append("★ [APP] alive 出现 %d 次 -> BL->APP 跳转成功，APP 已在跑"
                  % d["app_alive"])
    elif d["jump_rc"] is not None:
        bad.append("跳转返回 rc=%d（0xFF4000 的 APP 没跑起来）" % d["jump_rc"])
        if d["jump_rc"] == 12:
            bad.append("  rc=12 = OTA_ERR_VERIFY，是 ota_jump 的'空片'闸门拦下的")
        elif d["jump_rc"] == 2:
            bad.append("  rc=2 = OTA_ERR_ADDR，入口地址不在 APP 代码域内")
        elif d["jump_rc"] == 255:
            bad.append("  rc=255 = OTA_ERR_UNKNOWN，entry() 竟然返回了 -> 跳到了非法代码")
    elif d["app_boot"]:
        ok.append("APP 启动横幅已出现")

    # ---- 输出 ----
    _out("")
    _out("=" * 68)
    _out(" Bootloader 串口日志解析")
    _out("=" * 68)
    _out("  抓到的行数:  BOOT-POLL x%d   [BL] boot x%d   alive x%d"
         % (d["n_poll"], d["n_boot"], d["n_alive"]))
    if d["p3"]:
        _out("  P3 读回:     %s%s" % (d["p3"], "  (P3.1 为低!)" if d["txd_low"] else ""))
    if d["bl_head"]:
        _out("  BL 头字节:   %s" % " ".join(d["bl_head"]))
    if d["app_head"]:
        _out("  APP 头字节:  %s" % " ".join(d["app_head"]))
    _out("  探针:        STAGING=%s   APP=%s"
         % (stg or "?", app or "?"))
    _out("")

    def dump(title, items, mark):
        if not items:
            return
        _out(title)
        for s in items:
            _out("  %s %s" % (mark, s))
        _out("")

    dump("[通过]", ok, "OK ")
    dump("[注意]", warn, "!  ")
    dump("[问题]", bad, "X  ")

    # ---- 结论 ----
    _out("-" * 68)
    if chk and not d.get("chk_ok", True):
        # ★ 最高优先级：日志本身在撒谎时，任何基于它的结论都是错的
        _out("结论: ★★ 日志格式化错位 —— 本份日志的数字不可信，先修日志")
        _out("      这不是硬件问题：程序功能可能是完全正常的，只是打印出来是鬼值。")
        _out("      典型症状: %x 字段变空白、0xff4000 打成 0x12、48K 打成 ' K'。")
        _out("      根因: LOG 参数列表里嵌套了函数调用 / 一行参数太多 / 三目字面量。")
        _out("      修法见文档 §8.9；v0.4 已修，重烧 build/step4_BL_APP_v0.4.hex。")
    elif d["read_untrusted"] or d["bl_head_bad"]:
        _out("结论: ★★ IAP 读通路不可信 —— BL 首字节不是 0x02")
        _out("      这不是'方案 (b)'的证据，而是 IAP 命令码/地址寄存器层面的问题。")
        if d["bl_head_zero"]:
            _out("      而且 BL 复位入口已经被写成 00 00 00 00：下次复位起不来。")
            _out("      补救：ISP 冷启动下载仍然有效（引导程序在独立 ROM），")
            _out("            重新烧 build/step4_BL_APP_v0.4.hex 即可恢复。")
        _out("      已知元凶: IAP_CMD 的 1(读)/2(写) 写反 —— '读'变成了'写 0x00'。")
        _out("      修正版已加: 命令码白名单 + 写分区硬闸门 + 读通路验证闸门。")
    elif d["bl_changed"]:
        _out("结论: 硬件异常，停止烧录，检查 flash_iap_check_range 白名单")
    elif d["halt"]:
        _out("结论: IAP 初始化失败，先解决 flash_iap_init")
    elif d["jump_rc"] is not None and d["app_alive"] == 0:
        # ★ 探针结论再好也没用 —— APP 跑不起来就说明 BL->APP 这一跳没通
        _out("结论: ★ BL->APP 跳转没成功（rc=%d），APP 没跑起来" % d["jump_rc"])
        if d["jump_rc"] == 12:
            _out("      rc=12 = OTA_ERR_VERIFY：ota_jump 的'空片'闸门拦下的。")
            _out("      但同一份日志里 bl_boot 用常量地址读到的是 75 84 1 7e ——")
            _out("      同一地址两条算法结论相反，说明闸门放错位置了。")
            _out("      v0.6 已把闸门移回 bl_boot，ota_jump 只诊断不拦。")
        elif d["jump_rc"] == 255:
            _out("      rc=255：entry() 返回了，说明跳到了非法代码（地址或选项字节问题）。")
        _out("      请重烧 build/step6_BL_APP_v0.6.hex，正常应看到 [APP] alive。")
    elif stg == "W" and app == "W":
        _out("结论: ✅ 全部实测通过 —— IAP 写通路 + APP 落点都实测可写")
    elif stg == "W" and app == "untested":
        _out("结论: IAP 写通路成立（STAGING 实测可写）；APP 区可写性未验证")
        _out("      这是**有条件通过**，不是完全通过。")
        _out("      原因: APP 区 0x14000 非空白，探针为避免毁固件而主动跳过。")
        _out("      推荐: 直接烧合并镜像把 APP 填满，再观察 BL 能否跳转。")

    elif stg == "NA":
        _out("结论: 探针不可写 —— 先排除 EEPROM 没设成 128K，再判方案 (b)")
    elif stg is None:
        _out("结论: 这份日志里没有 stg 探针字段")
        _out("      说明烧的是 step1e 或更早的固件（那时探针逻辑有假阳性 bug）")
        _out("      请烧 build/step1f_BL_realprobe_v0.1.hex 重新采集")
    elif d["n_alive"] == 0 and d["n_poll"] == 0:
        _out("结论: 没有任何串口输出，问题在链路层（见文档 §7.3）")
    else:
        _out("结论: 证据不完整 —— 日志抓得太短，请多抓 20 秒再跑一次")
        _out("      关键要找: == summary: init=0 selftest=0 stg=? app=? ==")
    _out("-" * 68)
    _out("")


# ---------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    if not args:
        _out(__doc__ or "")
        _out("把串口助手收到的内容另存为 txt，再跑:")
        _out("  python tools/parse_boot_log.py 日志.txt")
        return 1

    if args[0] == "--text":
        txt = " ".join(args[1:]).replace("\\n", "\n")
    else:
        try:
            txt = read_log(args[0])
        except OSError as e:
            _out("读不到文件: %s" % e)
            return 1

    if len(txt.strip()) == 0:
        _out("日志是空的 —— 串口一个字节都没收到，见文档 §7.3")
        return 2

    report(collect(txt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
