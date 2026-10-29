#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
at_term.py  --  直连 4G 模组的 AT 终端 + HTTP 命令组自动识别

为什么要有这个工具
------------------
`http_ota.c` 里写的是合宙那套 `AT+HTTPINIT/PARA/ACTION/READ/TERM`，
但 Y100P 烧的是**移远 AT 固件**，HTTP 很可能是 `AT+QHTTPCFG/QHTTPURL/QHTTPGET/QHTTPREAD`。
这两套命令的语义完全不同，写错了上板只会得到一串 ERROR —— 而且固件里看不出来。
所以先用本工具直接问模组"你到底有哪一套"，再决定 `http_ota.c` 怎么写。

用法
----
1) 交互模式（自己敲命令，最灵活）
       python tools/at_term.py -p COM6
   接线：USB-TTL 的 TXD -> 模组 RXD，RXD -> 模组 TXD，GND 共地。
   ★ 这时 MCU 的 P1.0/P1.1 两根线要**拔掉**，否则两边会打架。

2) 自动识别（一条命令跑完，直接给结论）
       python tools/at_term.py -p COM6 --probe

3) 只发一次
       python tools/at_term.py -p COM6 --send "AT+CGMR"

行结束符
--------
默认发 CRLF（`AT\\r\\n`）。模组两种都吃，但 CRLF 在串口助手里看得见换行，
现场排查"到底发出去没有"更直观。要改回只发 CR 用 `--cr`。

判读要点
--------
· `AT` 回 `OK`          -> 链路和波特率都对
· 回 `ERROR` 但不是乱码  -> 波特率对、命令不认识（正是我们要的信息）
· 回一坨不可打印字节     -> 波特率不对，试 --baud 换档
· 什么都不回             -> TX/RX 没交叉 / 共地 / 没开机
"""

import argparse
import datetime
import os
import sys
import time

try:
    import serial
except ImportError:
    print("[!] 缺少 pyserial：python -m pip install pyserial")
    sys.exit(3)

# ---------------------------------------------------------------- 控制台

USE_COLOR = True
C_RESET = "\033[0m"
C_RED = "\033[31m"
C_GREEN = "\033[32m"
C_YEL = "\033[33m"
C_CYAN = "\033[36m"
C_MAG = "\033[35m"
C_GREY = "\033[90m"
C_BOLD = "\033[1m"


def _setup_stdio():
    """把 stdout 的编码错误降级成 '?'。

    教训：cp936 控制台 + UTF-8 源码时，一次 print 失败会让整个进程退出，
    表现就是"双击闪退"。这里宁可变问号，也不许因为打印而死。
    """
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except Exception:
            pass


def _is_tty():
    try:
        return sys.stdout.isatty() and sys.stdin.isatty()
    except Exception:
        return False


def enable_ansi():
    global USE_COLOR
    if os.name != "nt":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, mode.value | 0x0004)   # ENABLE_VIRTUAL_TERMINAL
        else:
            USE_COLOR = False
    except Exception:
        USE_COLOR = False


def col(s, c):
    return s if (not USE_COLOR or not c) else (c + s + C_RESET)


def out(s=""):
    try:
        print(s)
    except Exception:
        try:
            print(s.encode("ascii", "replace").decode("ascii"))
        except Exception:
            pass


def visible(bs):
    """把字节流变成可读文本：控制字符展开，非 ASCII 用 <XX>。"""
    parts = []
    for b in bs:
        if b == 0x0D:
            parts.append("<CR>")
        elif b == 0x0A:
            parts.append("<LF>")
        elif b == 0x09:
            parts.append("<TAB>")
        elif 0x20 <= b <= 0x7E:
            parts.append(chr(b))
        else:
            parts.append("<%02X>" % b)
    return "".join(parts)


def hexdump(bs, per=16):
    lines = []
    for i in range(0, len(bs), per):
        chunk = bs[i:i + per]
        h = " ".join("%02X" % b for b in chunk)
        t = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in chunk)
        lines.append("%04X  %-*s  %s" % (i, per * 3 - 1, h, t))
    return lines


# ---------------------------------------------------------------- 终端

class Term:
    def __init__(self, port, baud, eol, logf=None, verbose=True):
        self.port = port
        self.baud = baud
        self.eol = eol                # b"\r\n" 或 b"\r"
        self.logf = logf
        self.verbose = verbose
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.05,
                                 write_timeout=1.0)
        self.ser.reset_input_buffer()

    # ---- 底层 ----
    def write(self, line):
        data = line.encode("ascii", "replace") + self.eol
        self.ser.write(data)
        self.ser.flush()
        self._log(">>> " + line)
        if self.verbose:
            out(col(">>> ", C_CYAN) + line)

    def _log(self, s):
        if not self.logf:
            return
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        try:
            self.logf.write("%s  %s\n" % (ts, s))
            self.logf.flush()
        except Exception:
            pass

    def read_block(self, timeout):
        """读到"静默 timeout 秒"或超时，返回原始字节。"""
        buf = bytearray()
        end = time.monotonic() + timeout
        while True:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
                continue
            if time.monotonic() >= end:
                break
            time.sleep(0.002)
        return bytes(buf)

    def read_until_status(self, timeout):
        """读到出现 OK / ERROR / +CME ERROR 或超时。返回 (原始字节, 状态)。"""
        buf = bytearray()
        end = time.monotonic() + timeout
        status = None
        while time.monotonic() < end:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
                txt = bytes(buf).replace(b"\r", b"\n")
                for ln in txt.split(b"\n"):
                    s = ln.strip()
                    if s == b"OK":
                        status = "OK"
                    elif s == b"ERROR" or s.startswith(b"+CME ERROR"):
                        status = "ERROR"
                if status:
                    # 再捞一小段，防止后续 URC 还在路上
                    time.sleep(0.05)
                    more = self.ser.in_waiting
                    if more:
                        buf += self.ser.read(more)
                    break
            else:
                time.sleep(0.002)
        return bytes(buf), status

    @staticmethod
    def _lines(raw):
        txt = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        return [ln for ln in txt.split(b"\n")]

    def show(self, raw):
        for ln in self._lines(raw):
            if not ln.strip():
                continue
            self._log("<<< " + visible(ln))
            printable = all(0x20 <= b <= 0x7E or b == 0x09 for b in ln)
            if printable:
                s = ln.decode("ascii", "replace")
                if s.strip() in ("OK", "RDY"):
                    out(col("<<< " + s, C_GREEN))
                elif "ERROR" in s:
                    out(col("<<< " + s, C_RED))
                else:
                    out("<<< " + s)
            else:
                out(col("<<< " + visible(ln), C_MAG))
                out(col("     可能波特率不对；原始：" + ln.hex(" "), C_GREY))

    # ---- 对外 ----
    def cmd(self, line, timeout=1.0, quiet=False):
        self.ser.reset_input_buffer()
        self.write(line)
        raw, st = self.read_until_status(timeout)
        if not quiet:
            self.show(raw)
        return raw, st

    def ping(self, tries=8, wait=0.3):
        """反复发 AT 直到拿到 OK；返回 True/False。"""
        for i in range(tries):
            raw, st = self.cmd("AT", timeout=wait, quiet=True)
            if st == "OK":
                out(col("<<< OK  (AT 应答正常，波特率 %d 正确)" % self.baud, C_GREEN))
                return True
            if raw.strip():
                out(col("<<< 收到 %d B 但没 OK：%s" % (len(raw), visible(raw[:40])), C_YEL))
            time.sleep(0.15)
        return False


# ---------------------------------------------------------------- 探测

def _get(raw, prefix):
    """从原始应答里取某条 +XXX: 行（prefix 为 str）。"""
    for ln in Term._lines(raw):
        s = ln.strip().decode("ascii", "replace")
        if s.startswith(prefix):
            return s
    return ""


def _payload(raw, echo=None):
    """取"第一条有内容、又不是 OK/ERROR/回显"的行。

    ATI / AT+CGMR 的答复就一行正文，用这个取最干净。
    把回显行排除掉很关键：模组 ATE1 时会把命令原样回一遍，
    不排除就会把 "ATI" 当成型号名报出来。
    """
    for ln in Term._lines(raw):
        s = ln.strip().decode("ascii", "replace")
        if not s:
            continue
        if s in ("OK", "ERROR") or s.startswith("+CME ERROR"):
            continue
        if echo and s == echo:
            continue
        if s.upper().startswith("AT"):
            continue
        return s
    return ""


def probe(t):
    out("")
    out(col("=" * 68, C_GREY))
    out(col("  4G 模组识别 + HTTP 命令组判定", C_BOLD))
    out(col("=" * 68, C_GREY))

    if not t.ping():
        out("")
        out(col("AT 无应答 —— 先解决链路，别往下走：", C_RED))
        out("  1) TX/RX 必须交叉：USB-TTL.TXD -> 模组 RXD，USB-TTL.RXD -> 模组 TXD")
        out("  2) GND 必须共地")
        out("  3) 模组要真开机（Y100P: VIN 5-12V，>10W；PWRKEY 要'拉低>=1.2s 再松开'）")
        out("  4) 波特率：试 --baud 9600 / 38400 / 57600 / 230400")
        out("  5) 串口被别的软件占着（先关 STC-ISP / sscom）")
        return 2

    info = {}

    raw, st = t.cmd("ATE0", timeout=0.6)          # 关回显，后面读起来干净
    if st != "OK":
        out(col("[!] ATE0 没回 OK（不致命，继续）", C_YEL))

    for name, c, key in (
        ("型号",     "ATI",        "ATI"),
        ("固件版本", "AT+CGMR",    "CGMR"),
        ("波特率",   "AT+IPR?",    "IPR"),
        ("SIM",      "AT+CPIN?",   "CPIN"),
        ("信号",     "AT+CSQ",     "CSQ"),
        ("注网",     "AT+CEREG?",  "CEREG"),
    ):
        raw, st = t.cmd(c, timeout=1.2)
        if key in ("ATI", "CGMR"):
            info[key] = _payload(raw, echo=c) or "(空)"
        else:
            info[key] = _get(raw, "+") or "(空)"
        if st is None:
            out(col("[!] %s 超时" % c, C_YEL))

    out("")
    out(col("-" * 68, C_GREY))
    out(col("  HTTP 命令组判定（这是本轮唯一目的）", C_BOLD))
    out(col("-" * 68, C_GREY))

    # 移远风格
    q_ok = 0
    for c in ("AT+QHTTPCFG=?", "AT+QHTTPCFG?", "AT+QHTTPURL=?"):
        raw, st = t.cmd(c, timeout=1.2)
        if st == "OK" or _get(raw, "+QHTTP"):
            q_ok += 1
    # 3GPP 标准风格
    std_ok = 0
    for c in ("AT+HTTPINIT", "AT+HTTPPARA=?", ):
        raw, st = t.cmd(c, timeout=1.2)
        if st == "OK":
            std_ok += 1
    t.cmd("AT+HTTPTERM", timeout=1.0, quiet=True)

    # PDP 上下文风格也可以顺带判一下
    raw, st = t.cmd("AT+QICSGP?", timeout=1.0, quiet=True)
    pdp_q = (st == "OK") or bool(_get(raw, "+QICSGP"))
    raw, st = t.cmd("AT+CSTT?", timeout=1.0, quiet=True)
    pdp_std = (st == "OK") or bool(_get(raw, "+CSTT:"))

    out("")
    out(col("=" * 68, C_GREY))
    out(col("  结论", C_BOLD))
    out(col("=" * 68, C_GREY))
    out("  型号      : %s" % info.get("ATI", "?"))
    out("  固件      : %s" % info.get("CGMR", "?"))
    out("  IPR       : %s" % info.get("IPR", "?"))
    out("  SIM       : %s" % info.get("CPIN", "?"))
    out("  信号      : %s" % info.get("CSQ", "?"))
    out("  注网      : %s" % info.get("CEREG", "?"))
    out("")

    if q_ok:
        out(col("  HTTP 命令组 = 移远 QHTTP  (AT+QHTTPCFG/QHTTPURL/QHTTPGET/QHTTPREAD)", C_GREEN))
        out(col("  ==> http_ota.c 必须改成 QHTTP 那一套，现在这版 HTTPINIT 上板必 ERROR", C_YEL))
    elif std_ok:
        out(col("  HTTP 命令组 = 3GPP 标准  (AT+HTTPINIT/PARA/ACTION/READ/TERM)", C_GREEN))
        out(col("  ==> http_ota.c 现有实现可用，直接填 URL 就能跑", C_GREEN))
    else:
        out(col("  HTTP 命令组 = 两套都没应答", C_RED))
        out("      → 读日志确认：是返回 ERROR（命令不支持），还是超时（PDP 没激活）")
        out("      → 移远固件部分版本要求先激活 PDP 才认 HTTP 命令：")
        out("           AT+QICSGP=1,1,\"CMNET\" / AT+QIACT=1 / AT+QHTTPCFG=\"contextid\",1")

    out("")
    if pdp_q:
        out("  PDP 风格  : 移远  AT+QICSGP / AT+QIACT  （与 QHTTP 配套）")
    elif pdp_std:
        out("  PDP 风格  : 标准  AT+CSTT / AT+CIICR   （与 HTTPINIT 配套）")
    else:
        out("  PDP 风格  : 未识别（可能要先插好 SIM 卡）")

    out("")
    out(col("  日志已保存，可直接贴回来。", C_GREY))
    return 0


# ---------------------------------------------------------------- 入口

def main():
    _setup_stdio()
    enable_ansi()

    ap = argparse.ArgumentParser(
        description="直连 4G 模组的 AT 终端 + HTTP 命令组自动识别")
    ap.add_argument("-p", "--port", required=False, help="串口，如 COM6")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--cr", action="store_true", help="只发 CR（默认发 CRLF）")
    ap.add_argument("--probe", action="store_true", help="自动识别模组 + HTTP 命令组")
    ap.add_argument("--send", metavar="CMD", help="只发一条命令然后退出")
    ap.add_argument("--log", help="日志路径（默认 logs/at_<时间>.log）")
    ap.add_argument("--no-log", action="store_true")
    ap.add_argument("--list", action="store_true", help="列出可用串口")
    args = ap.parse_args()

    if args.list or not args.port:
        try:
            from serial.tools import list_ports
            ports = list(list_ports.comports())
        except Exception:
            ports = []
        out("可用串口：")
        for p in ports:
            out("  %-8s %s" % (p.device, p.description))
        if not ports:
            out("  (没有)" + ("" if os.name != "nt" else "  —— 检查 USB-TTL 驱动"))
        if not args.port:
            out("")
            out("用 -p COMx 指定；加 --probe 自动识别 HTTP 命令组。")
        return 0

    logf = None
    if not args.no_log:
        path = args.log
        if not path:
            d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, "at_%s.log" %
                                datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
        else:
            d = os.path.dirname(os.path.abspath(path))
            if d:
                os.makedirs(d, exist_ok=True)
        logf = open(path, "a", encoding="utf-8", newline="\n")
        out(col("[log] " + os.path.abspath(path), C_GREY))

    eol = b"\r" if args.cr else b"\r\n"
    try:
        t = Term(args.port, args.baud, eol, logf=logf)
    except Exception as e:
        out(col("[!] 打不开 %s: %s" % (args.port, e), C_RED))
        out("    常见原因：被 STC-ISP / 串口助手 占着；端口号写错。")
        return 2

    try:
        if args.probe:
            return probe(t)

        if args.send:
            raw, st = t.cmd(args.send, timeout=2.0)
            return 0 if st == "OK" else 1

        # 交互模式
        out("")
        out(col("AT 终端  端口=%s 波特率=%d  行尾=%s"
                % (args.port, args.baud, "CR" if args.cr else "CRLF"), C_BOLD))
        out(col("直接敲命令（不用带 AT 也行，会自动补）；:q 退出；:h 帮助", C_GREY))
        out("")
        if not t.ping():
            out(col("[!] AT 没应答。仍可继续手敲，先试 :h 看排查清单。", C_YEL))
        while True:
            try:
                line = input(col("at> ", C_CYAN)).strip()
            except (EOFError, KeyboardInterrupt):
                out("")
                break
            if not line:
                continue
            if line in (":q", ":quit", "exit"):
                break
            if line in (":h", ":help"):
                out("  :q 退出   :r 重新 ping   :hex 原始视图")
                out("  排查：TX/RX 交叉? GND 共地? VIN 5-12V? PWRKEY 拉低1.2s后松开?")
                continue
            if line == ":r":
                t.ping()
                continue
            t.cmd(line, timeout=2.0)
        return 0
    finally:
        try:
            t.ser.close()
        except Exception:
            pass
        if logf:
            logf.close()


def _guarded_main():
    try:
        return main()
    except SystemExit:
        raise
    except Exception:
        import traceback
        out(col("[!] 异常退出：", C_RED))
        traceback.print_exc()
        if _is_tty() and len(sys.argv) <= 1:
            try:
                input("按回车关闭窗口...")
            except Exception:
                pass
        return 1


if __name__ == "__main__":
    sys.exit(_guarded_main())
