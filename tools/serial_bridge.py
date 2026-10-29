#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""=============================================================================
 serial_bridge.py  --  串口桥接 / 旁路监听工具（实时观察双向 AT 交互）

 为什么要有这个工具
 ------------------
 OTA 调试里最费时间的一件事是"线上到底发生了什么"。串口助手只能看一头，
 而且 CR / LF 这类不可见字符它不显示 —— 上一轮"AT 后面没有换行"就是因此
 多花了一轮上板。本工具把**双向**数据按方向、带毫秒时间戳、**把 CR/LF
 显式画出来**，并且主动标出"这一行以 CR 结尾却没有 LF"。

 两种工作模式
 ------------
 1) sniff（旁路监听，只需 1 个 USB-TTL）
      只读不写，看的是**一个方向**。最常用于确认 MCU 到底发了什么字节。
      接线：  USB-TTL.RXD  <-  MCU P1.1(TXD2)
              USB-TTL.GND  --  MCU GND
      ★ 千万别把 USB-TTL.TXD 也接上去，会和 MCU 的 TXD 打架。

 2) bridge（串口桥接，需要 2 个 USB-TTL）
      插在 MCU 和模组中间，原样转发并记录两个方向。
      接线：  TTL#1.TXD -> MCU.P1.0(RXD2)     TTL#1.RXD <- MCU.P1.1(TXD2)
              TTL#2.TXD -> 模组.RXD           TTL#2.RXD <- 模组.TXD
              三个 GND 必须共地
      ★ 桥接是**透明**的：不改波特率、不改字节、不加延时，只做搬运。

 用法
 ----
   python tools/serial_bridge.py --list
   python tools/serial_bridge.py -a COM5                      # 只听 MCU 发
   python tools/serial_bridge.py -a COM5 -b COM6              # 桥接
   python tools/serial_bridge.py -a COM5 --hex                # 十六进制视图
   python tools/serial_bridge.py -a COM5 -b COM6 --stats      # 每 5 s 报统计

 运行中按键
 ----------
   p 暂停/继续显示（转发不受影响）   c 清屏并清零计数
   h 切换 line/hex 视图              q 退出
============================================================================="""

import argparse
import datetime
import os
import queue
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.stderr.write("需要 pyserial：python -m pip install pyserial\n")
    sys.exit(2)

# ---------------------------------------------------------------- 输出 & 颜色

USE_COLOR = True

# 方向配色：A→B 青，B→A 黄，其余按语义
C_RESET = "\x1b[0m"
C_DIM = "\x1b[2m"
C_BOLD = "\x1b[1m"
C_CYAN = "\x1b[36m"
C_YELLOW = "\x1b[33m"
C_GREEN = "\x1b[32m"
C_RED = "\x1b[31m"
C_MAG = "\x1b[35m"
C_GREY = "\x1b[90m"


def enable_ansi():
    """Windows 控制台默认不解析 ANSI，需要置 ENABLE_VIRTUAL_TERMINAL_PROCESSING。"""
    global USE_COLOR
    if os.name != "nt":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if not k.GetConsoleMode(h, ctypes.byref(mode)):
            USE_COLOR = False
            return
        if not k.SetConsoleMode(h, mode.value | 0x0004):
            USE_COLOR = False
    except Exception:
        USE_COLOR = False


def _setup_stdio():
    """把 stdout/stderr 的编码错误降级成 '?'，任何一次打印都不许让工具退出。

    ★ "闪退"最常见的一种真身就是这个：Windows 控制台代码页（cp936 / utf-8）
      与源码里的中文、制表符不一致时，print 会抛 UnicodeEncodeError，
      进程当场结束 —— 窗口一闪而过，看着像"程序崩了"，其实只是打印炸了。
    """
    for name in ("stdout", "stderr"):
        s = getattr(sys, name, None)
        if s is None:
            continue
        try:
            s.reconfigure(errors="replace")
        except Exception:
            pass


def _is_tty():
    """判断是不是真的挂在交互式窗口上（重定向/管道时不许等键盘）。"""
    try:
        return sys.stdin is not None and sys.stdin.isatty()
    except Exception:
        return False


def col(s, c):
    return s if (not USE_COLOR or not c) else (c + s + C_RESET)


def out(s):
    """打印一行（失败不致命：管道关闭/重定向时也可能报错）。"""
    try:
        sys.stdout.write(s + "\n")
        sys.stdout.flush()
    except Exception:
        pass


# ---------------------------------------------------------------- 字节美化

# 控制字符映射：**这是本工具的核心价值** —— CR/LF 这类不能靠肉眼看的字符
# 必须展开成文字，否则"AT 后面到底有没有换行"永远只能猜。
CTRL = {
    0x00: "<NUL>", 0x01: "<SOH>", 0x02: "<STX>", 0x03: "<ETX>",
    0x04: "<EOT>", 0x05: "<ENQ>", 0x06: "<ACK>", 0x07: "<BEL>",
    0x08: "<BS>",  0x09: "<TAB>", 0x0A: "<LF>",  0x0B: "<VT>",
    0x0C: "<FF>",  0x0D: "<CR>",  0x0E: "<SO>",  0x0F: "<SI>",
    0x10: "<DLE>", 0x11: "<DC1>", 0x12: "<DC2>", 0x13: "<DC3>",
    0x14: "<DC4>", 0x15: "<NAK>", 0x16: "<SYN>", 0x17: "<ETB>",
    0x18: "<CAN>", 0x19: "<EM>",  0x1A: "<SUB>", 0x1B: "<ESC>",
    0x1C: "<FS>",  0x1D: "<GS>",  0x1E: "<RS>",  0x1F: "<US>",
    0x7F: "<DEL>",
}


def visible(data):
    """把字节串转成可读文本：可打印字符原样，控制字符展开成 <CR> 之类。"""
    parts = []
    for b in data:
        if b in CTRL:
            parts.append(CTRL[b])
        elif 0x20 <= b < 0x7F:
            parts.append(chr(b))
        else:
            parts.append("<%02X>" % b)
    return "".join(parts)


def hexdump_block(data):
    """16 字节一行的 hexdump，返回若干行（不含前缀）。"""
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexs = " ".join("%02X" % b for b in chunk)
        hexs = hexs.ljust(16 * 3 - 1)
        asci = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append("%s  |%s|" % (hexs, asci))
    return lines


# ---------------------------------------------------------------- 按键（非阻塞）

def poll_key():
    if os.name == "nt":
        try:
            import msvcrt
            if msvcrt.kbhit():
                ch = msvcrt.getwch()
                return ch
        except Exception:
            pass
        return None
    try:
        import select
        if select.select([sys.stdin], [], [], 0)[0]:
            return sys.stdin.read(1)
    except Exception:
        pass
    return None


# ---------------------------------------------------------------- 串口占用清理

def _load_cleaner():
    """加载同目录的 port_cleaner.py。找不到就返回 None（工具照常能用）。"""
    try:
        import port_cleaner as pc
        return pc
    except Exception:
        pass
    try:
        import importlib.util
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "port_cleaner.py")
        spec = importlib.util.spec_from_file_location("port_cleaner", p)
        pc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pc)
        return pc
    except Exception:
        return None


# ---------------------------------------------------------------- 主逻辑

class Endpoint:
    """一路串口。tag 是显示用的方向名，peer 是桥接时要把数据写过去的对端。"""

    def __init__(self, port, baud, tag):
        self.port_name = port
        self.baud = baud
        self.tag = tag
        self.ser = None
        self.peer = None
        self.nbytes = 0
        self.nlines = 0

    def open(self):
        self.ser = serial.Serial(
            port=self.port_name,
            baudrate=self.baud,
            bytesize=8,
            parity="N",
            stopbits=1,
            timeout=0.02,
            write_timeout=2.0,
            rtscts=False,
            dsrdtr=False,
            xonxoff=False,
        )
        # 清掉打开瞬间可能存在的残留，避免第一屏就是上一轮的半截数据
        self.ser.reset_input_buffer()
        return self


class Bridge:
    def __init__(self, args):
        self.args = args
        self.stop = threading.Event()
        self.q = queue.Queue()
        self.hex_view = args.hex
        self.paused = False
        self.lock = threading.Lock()
        self.t0 = time.monotonic()
        self.t_last_stats = time.monotonic()
        # 行聚合状态：每个方向一份
        self.cur = {}            # tag -> bytearray（尚未成行的字节）
        self.saw_cr = {}         # tag -> bool（上一个字节是 CR，等 LF 配对）
        self.hold_since = {}     # tag -> 本行最后一个字节的时刻（判超时 flush）
        self.logf = None
        self.log_path = None
        self.eps = []

    # ---- 日志 ----
    def _open_log(self):
        if self.args.no_log:
            return
        try:
            path = self.args.log
            if not path:
                d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
                os.makedirs(d, exist_ok=True)
                path = os.path.join(d, "bridge_%s.log" %
                                    datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
            else:
                d = os.path.dirname(os.path.abspath(path))
                if d:
                    os.makedirs(d, exist_ok=True)
            self.logf = open(path, "a", encoding="utf-8", newline="\n")
            self.log_path = os.path.abspath(path)
        except Exception as e:
            out(col("[!] 日志文件打不开（继续只显示不落盘）: %s" % e, C_RED))
            self.logf = None
            self.log_path = None
    def _logline(self, plain):
        if self.logf:
            try:
                self.logf.write(plain + "\n")
                self.logf.flush()
            except Exception:
                pass

    # ---- 输出一行（屏幕 + 文件）----
    def emit(self, body_text, plain_text):
        """body_text: 屏幕用的（已带方向与配色），plain_text: 落盘用的纯文本。"""
        now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        if not self.paused:
            out(col(now, C_GREY) + "  " + body_text)
        self._logline(now + "  " + plain_text)

    # ---- 方向标签配色 ----
    def _tagstyle(self, tag):
        if tag.endswith(">MOD"):
            return col("%-9s" % tag, C_CYAN) + "  "
        return col("%-9s" % tag, C_YELLOW) + "  "

    # ---- 一行成型 ----
    def flush_line(self, tag, line_bytes, complete, missing_lf):
        """complete=False 表示这一行不是以换行结束的（超时强制吐出）。
        missing_lf=True 表示已确认"行尾只有 CR、后面没有 LF"（真缺换行）。"""
        for e in self.eps:
            if e.tag == tag:
                e.nlines += 1
                break

        txt = visible(line_bytes)
        stripped = line_bytes.strip()

        style_c = ""
        if stripped.upper().startswith(b"AT"):
            style_c = C_BOLD + C_CYAN
        elif stripped in (b"OK", b"RDY"):
            style_c = C_GREEN
        elif b"ERROR" in stripped or b"FAIL" in stripped:
            style_c = C_RED

        suffix = ""
        plain_suffix = ""
        if missing_lf:
            # ★ 这是本工具最该报警的一件事（上一轮就是它害得多烧了一版）。
            style_c = C_MAG
            suffix = col("   <<< 只有 CR，没有 LF", C_MAG)
            plain_suffix = "   <<< CR without LF"
        elif not complete:
            suffix = col("   <<< 未以换行结束（超时吐出）", C_MAG)
            plain_suffix = "   <<< no newline (timeout flush)"

        body = self._tagstyle(tag) + col(txt, style_c) + suffix
        plain = "%-9s  " % tag + txt + plain_suffix
        self.emit(body, plain)

    # ---- 喂字节（CR/LF 状态机）----
    #
    # 为什么要这么绕：模组最常见的回显是 `AT\r\r\nOK\r\n` —— 命令行自己是 CR 结尾，
    # 后面再跟一个 CRLF 空行。如果"见到 CR 就断行"，第一行就是 `AT<CR>`，
    # 会被判成"缺 LF"而**天天误报**，告警立刻失去意义。
    #
    # 规则（遇到 CR 后先挂起，看下一个字节再定性）：
    #   下一个是 LF  -> CRLF 行尾，正常收尾，不报警
    #   下一个是 CR  -> 前面的 CR 属于"冗余行尾"（紧跟分隔符），不报警
    #   下一个是普通 -> 真的只有 CR 没有 LF，**报警**
    #   50 ms 内无后续 -> 同上，真的只有 CR，**报警**
    LINE_HARD_CAP = 4096   # 单行上限：二进制流不会把内存吃爆

    def feed(self, tag, data):
        with self.lock:
            if self.hex_view:
                # hex 视图不聚合行，按 read 块直接输出，保持时序真实
                for ln in hexdump_block(data):
                    self.emit(self._tagstyle(tag) + col(ln, C_GREY),
                              "%-9s  %s" % (tag, ln))
                for e in self.eps:
                    if e.tag == tag:
                        e.nlines += 1
                        break
                return

            cur = self.cur.setdefault(tag, bytearray())
            pend = self.saw_cr.get(tag, False)
            self.hold_since[tag] = time.monotonic()

            for b in data:
                if pend:
                    pend = False
                    if b == 0x0A:
                        cur.append(b)
                        self.flush_line(tag, bytes(cur), True, False)
                        cur.clear()
                        continue
                    elif b == 0x0D:
                        # 冗余行尾：本行收尾但不报警，新行从这个 CR 起
                        self.flush_line(tag, bytes(cur), True, False)
                        cur.clear()
                        cur.append(b)
                        pend = True
                        continue
                    else:
                        # 真的只有 CR
                        self.flush_line(tag, bytes(cur), True, True)
                        cur.clear()
                        cur.append(b)
                        continue

                if b == 0x0D:
                    cur.append(b)
                    pend = True
                elif b == 0x0A:
                    cur.append(b)
                    self.flush_line(tag, bytes(cur), True, False)
                    cur.clear()
                else:
                    cur.append(b)

                if len(cur) >= self.LINE_HARD_CAP:
                    self.flush_line(tag, bytes(cur), False, False)
                    cur.clear()
                    pend = False

            self.saw_cr[tag] = pend

    # ---- 超时 flush：半截行不许永远压着 ----
    def pump_timeout(self):
        now = time.monotonic()
        with self.lock:
            for tag, cur in list(self.cur.items()):
                if not cur:
                    continue
                ts = self.hold_since.get(tag, now)
                if now - ts < 0.05:
                    continue
                if self.saw_cr.get(tag, False):
                    # 以 CR 结尾、50 ms 内没等到 LF ⇒ 这一帧真就是"只有 CR 没有 LF"
                    self.saw_cr[tag] = False
                    self.flush_line(tag, bytes(cur), True, True)
                else:
                    # 根本没等到任何换行符（半截命令/二进制块），不算缺 LF
                    self.flush_line(tag, bytes(cur), False, False)
                cur.clear()

    # ---- 读线程 ----
    #
    # ★ 掉线不再一走了之（v0.51，现场逼出来的）：
    #   15:18 那次日志的最后一行是
    #       [!] COM7 读失败: ClearCommError failed
    #           (PermissionError(13, '设备不识别此命令。', None, 22))
    #   22 = ERROR_BAD_COMMAND，这是 **USB 转串口被拔掉 / 驱动掉了** 的典型错，
    #   不是"被别的程序占用"。旧版遇到它直接 self.stop.set() 退出，
    #   现场看到的就是"跑到一半突然结束，什么都没说清"。
    #   现在改成：打印原因 → 关句柄 → 最多重试 RECONNECT_S 秒 → 接上就继续。
    RECONNECT_S = 20.0

    def _reconnect(self, ep, err):
        """端口掉线后重连。成功 True。"""
        out(col("[!] %s 断开: %s" % (ep.port_name, err), C_RED))
        out(col("    22=ERROR_BAD_COMMAND 通常是 USB 转串口被拔/驱动掉了", C_DIM))
        try:
            if ep.ser:
                ep.ser.close()
        except Exception:
            pass

        out(col("    开始重连（最多 %.0f 秒，插回去即可）..." % self.RECONNECT_S, C_GREY))
        deadline = time.monotonic() + self.RECONNECT_S
        tries = 0
        while not self.stop.is_set() and time.monotonic() < deadline:
            tries += 1
            time.sleep(0.4)
            try:
                ep.open()
            except Exception:
                if (tries % 5) == 0:
                    out(col("    %s 还不行 ... (%d 次)" % (ep.port_name, tries), C_DIM))
                continue
            # 断线期间攒下的半截行必须扔掉：否则重连后第一行会拼出
            # "断线前的尾巴 + 重连后的开头"，看起来像一条从没发生过的命令。
            with self.lock:
                self.cur.pop(ep.tag, None)
                self.saw_cr.pop(ep.tag, None)
                self.hold_since.pop(ep.tag, None)
            out(col("[+] %s 已重连（第 %d 次尝试）" % (ep.port_name, tries), C_GREEN))
            return True

        if not self.stop.is_set():
            out(col("[!] %s 重连失败，退出" % ep.port_name, C_RED))
        return False

    def _pump(self, ep):
        while not self.stop.is_set():
            try:
                n = ep.ser.in_waiting
                data = ep.ser.read(n if n else 1)
            except Exception as e:
                if self.stop.is_set():
                    return
                if not self._reconnect(ep, e):
                    self.stop.set()
                    return
                continue
            if not data:
                continue
            ep.nbytes += len(data)
            if ep.peer is not None:
                try:
                    ep.peer.ser.write(data)
                except Exception as e:
                    # 写不进去只丢这一块，不退出：对端若真掉线，它自己的读线程
                    # 会失败并触发 _reconnect，这里再退出会重复报错、还会
                    # 让两个线程同时操作同一个句柄。
                    out(col("[!] %s 写失败: %s（丢弃本块）"
                            % (ep.peer.port_name, e), C_RED))
                    continue
            self.q.put((ep.tag, data))

    # ---- 统计 ----
    def stats_line(self):
        parts = []
        for e in self.eps:
            parts.append("%s %d B / %d 行" % (e.tag, e.nbytes, e.nlines))
        el = time.monotonic() - self.t0
        return "[统计 t=%02d:%02d] %s" % (int(el // 60), int(el % 60), " | ".join(parts))

    # ---- 运行前强制释放串口占用 ----
    #
    # ★ 用户明确要求："运行前强制关闭所有串口"。
    #   Windows 一个 COM 口同时只能被一个句柄打开，被 STC-ISP / 串口助手 /
    #   上一个没退干净的自己锁住时，pyserial 只回一句 PermissionError(13)，
    #   现场极容易误判成"硬件坏了"。这里先探占用、再结束已知的串口工具、
    #   最后复测一遍，把结论明明白白打出来。
    #
    #   只探测**本次要用的两个口**（A/B），不去碰其它口：
    #   打开别的端口会拉 DTR/RTS，可能把别人正在跑的板子复位。
    def _free_ports(self):
        pc = _load_cleaner()
        if pc is None:
            out(col("[free] port_cleaner.py 不在同目录，跳过占用清理", C_DIM))
            return

        targets = [p for p in (self.args.a, self.args.b) if p]
        if not targets:
            return

        # ★ 必须区分"被占用"和"端口不存在"：Windows 两者都报 PermissionError(13)。
        #   不区分就会把写错的 COM99 当成"有软件占着"，然后去杀进程。
        busy = []
        ok = []
        for p in targets:
            m = pc.probe(p)
            if m is None:
                ok.append(p)
            elif pc.is_busy(m):
                busy.append(p)
            else:
                out(col("[free] %s 打不开：%s" % (p, m), C_RED))

        if not busy:
            if ok:
                out(col("[free] %s 空闲，可直接使用" % " / ".join(ok), C_GREY))
            return

        out(col("[free] 被占用: %s —— 开始强制释放" % " ".join(busy), C_YELLOW))
        dry = bool(getattr(self.args, "free_dry", False))
        r = pc.free(targets, kill_tools=True, dry=dry,
                    log=lambda s: out(col(s, C_GREY) if not s.startswith("   ")
                                      else col(s, C_DIM)))

        if r["killed"]:
            out(col("[free] 已结束 %d 个占口进程" % len(r["killed"]), C_GREEN))
        if r["busy_after"]:
            out(col("[!] 仍被占用: %s" % " ".join(r["busy_after"]), C_RED))
            out(col("    请手动关掉占用它的软件（向导能列出可用串口）", C_DIM))
        else:
            out(col("[free] 全部释放完成: %s 可用" % " ".join(r["idle"]), C_GREEN))
        out("")

    # ---- 主循环 ----
    def run(self):
        enable_ansi()
        self._open_log()

        if not getattr(self.args, "no_free", False):
            self._free_ports()

        # 同口校验必须在开端口之前：否则会先占住 A 再抱怨，报错信息更绕
        if self.args.b and os.path.normcase(self.args.a) == os.path.normcase(self.args.b):
            out(col("[!] -a 和 -b 不能是同一个串口", C_RED))
            return 2

        a = Endpoint(self.args.a, self.args.baud_a, "MCU>MOD")
        self.eps.append(a)
        try:
            a.open()
        except Exception as e:
            out(col("[!] 打不开 %s: %s" % (self.args.a, e), C_RED))
            out(self._port_help())
            return 2

        b = None
        if self.args.b:
            b = Endpoint(self.args.b, self.args.baud_b, "MOD>MCU")
            self.eps.append(b)
            try:
                b.open()
            except Exception as e:
                out(col("[!] 打不开 %s: %s" % (self.args.b, e), C_RED))
                out(self._port_help())
                return 2
            a.peer = b
            b.peer = a

        # ---- 启动横幅：把接线和模式写在最前面，现场不用翻文档 ----
        out(col("=" * 68, C_GREY))
        if b is None:
            out(col("  模式: sniff（旁路监听，只读不写）", C_BOLD))
            out("  接线: USB-TTL.RXD <- MCU P1.1(TXD2) ; GND -- GND")
            out(col("        USB-TTL.TXD 悬空，不要接（会和 MCU 的 TXD 打架）", C_DIM))
            out("  速率: %d 8N1" % self.args.baud_a)
        else:
            out(col("  模式: bridge（串口桥接，双向透明转发）", C_BOLD))
            out("  A 口: %s @%d  <->  MCU (P1.0=RXD2 / P1.1=TXD2)"
                % (self.args.a, self.args.baud_a))
            out("  B 口: %s @%d  <->  模组 (RXD / TXD)"
                % (self.args.b, self.args.baud_b))
            out(col("  注意: A.TXD->MCU.RXD, A.RXD<-MCU.TXD ; B 同 ; 三处 GND 共地", C_DIM))
        out(col("  视图: %s  日志: %s" % ("hex" if self.hex_view else "line",
                                          self.log_path if self.logf else "关闭"), C_GREY))
        out(col("  按键: p 暂停  c 清屏  h 切视图  q 退出", C_GREY))
        out(col("=" * 68, C_GREY))

        ths = []
        for e in self.eps:
            t = threading.Thread(target=self._pump, args=(e,), daemon=True)
            t.start()
            ths.append(t)

        try:
            while not self.stop.is_set():
                # 1) 取数据并喂给行聚合
                got = False
                deadline = time.monotonic() + 0.02
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    try:
                        tag, data = self.q.get(timeout=remaining)
                    except queue.Empty:
                        break
                    self.feed(tag, data)
                    got = True

                # 2) 半截行超时吐出
                self.pump_timeout()

                # 3) 按键
                k = poll_key()
                if k:
                    k = k.lower()
                    if k == "q":
                        break
                    elif k == "p":
                        self.paused = not self.paused
                        out(col("[%s] 显示%s（转发继续）"
                                % ("PAUSE" if self.paused else "RESUME",
                                   "已暂停" if self.paused else "已恢复"), C_GREEN))
                    elif k == "c":
                        if USE_COLOR:
                            sys.stdout.write("\x1b[2J\x1b[H")
                        for e in self.eps:
                            e.nbytes = 0
                            e.nlines = 0
                        self.t0 = time.monotonic()
                        out(col("[CLEAR] 计数已清零", C_GREEN))
                    elif k == "h":
                        self.hex_view = not self.hex_view
                        out(col("[VIEW] %s" % ("hex" if self.hex_view else "line"), C_GREEN))

                # 4) 周期统计
                if self.args.stats and (time.monotonic() - self.t_last_stats) >= 5.0:
                    self.t_last_stats = time.monotonic()
                    out(col(self.stats_line(), C_GREY))
        except KeyboardInterrupt:
            pass
        finally:
            self.stop.set()
            for t in ths:
                t.join(timeout=0.5)
            for e in self.eps:
                try:
                    if e.ser:
                        e.ser.close()
                except Exception:
                    pass
            out(col("", C_RESET))
            out(col("=== 汇总 ===", C_BOLD))
            for e in self.eps:
                out("  %-9s %8d B / %d 行" % (e.tag, e.nbytes, e.nlines))
            if self.logf:
                out("  日志: %s" % self.log_path)
                try:
                    self.logf.close()
                except Exception:
                    pass
        return 0

    def _port_help(self):
        lines = ["", "可用串口:"]
        for p in sorted(list_ports.comports(), key=lambda x: x.device):
            lines.append("  %-8s %s" % (p.device, p.description))
        lines.append("")
        lines.append("提示: STC-ISP 下载时会独占 COM 口，先关掉 ISP 再开本工具。")
        return "\n".join(lines)


# ---------------------------------------------------------------- 交互向导
#
# 为什么必须有：旧版 main() 在缺少 -a 时只"打印帮助 + 列端口"然后 return 0。
# 双击 .py（或双击 bat 后直接回车）时窗口一闪即关，现场看到的就是"闪退"。
# 现在无参数就进向导：列端口 -> 问 A/B/波特率 -> 直接开跑，不用记命令行。

def _ask(prompt, default=""):
    """读一行输入。返回 None = 用户中断/EOF（调用方应当放弃）。"""
    try:
        s = input(prompt)
    except (EOFError, KeyboardInterrupt):
        out("")
        return None
    s = s.strip()
    return s if s else default


def _resolve_port(s, ports):
    """把"编号"或"COMx"统一成设备名。认不出返回 None。"""
    s = (s or "").strip()
    if not s:
        return None
    if s.isdigit():
        i = int(s) - 1
        return ports[i].device if 0 <= i < len(ports) else None
    return s.upper()


def wizard():
    """无命令行参数时的交互向导。返回 Namespace；None 表示用户放弃。"""
    ports = sorted(list_ports.comports(), key=lambda x: x.device)
    if not ports:
        out(col("[!] 没有发现任何串口。先插 USB-TTL，确认驱动装好。", C_RED))
        return None

    out(col("=" * 68, C_GREY))
    out(col("  串口桥接 / 旁路监听  ——  交互向导", C_BOLD))
    out(col("=" * 68, C_GREY))
    out(col("  可用串口:", C_BOLD))
    for i, p in enumerate(ports, 1):
        out("   [%d] %-8s %s" % (i, p.device, (p.description or "")[:44]))
    out("")

    sel_a = _ask("A 口（MCU 侧，输编号或 COMx；回车退出）: ")
    if not sel_a:
        out(col("已取消。", C_GREY))
        return None
    port_a = _resolve_port(sel_a, ports)
    if port_a is None:
        out(col("[!] 认不出这个端口: %s" % sel_a, C_RED))
        return None

    sel_b = _ask("B 口（模组侧；回车 = 只监听 A）: ")
    port_b = _resolve_port(sel_b, ports) if sel_b else None
    if port_b is not None and os.path.normcase(port_b) == os.path.normcase(port_a):
        out(col("[!] A、B 不能是同一个口，已按「只监听 A」处理", C_YELLOW))
        port_b = None

    baud_s = _ask("波特率（回车 = 115200）: ", "115200")
    try:
        baud = int(baud_s)
        if baud <= 0:
            baud = 115200
    except Exception:
        baud = 115200

    out("")
    return argparse.Namespace(
        list=False, a=port_a, b=port_b,
        baud_a=baud, baud_b=baud,
        hex=False, log=None, no_log=False,
        stats=True, no_color=not USE_COLOR,
        no_free=False, free_dry=False,
    )


# ---------------------------------------------------------------- 入口

def do_list():
    ports = sorted(list_ports.comports(), key=lambda x: x.device)
    if not ports:
        out("没有发现串口。")
        return 0
    out("%-10s %-42s %s" % ("PORT", "DESCRIPTION", "HWID"))
    out("-" * 100)
    for p in ports:
        out("%-10s %-42s %s" % (p.device, (p.description or "")[:42], p.hwid or ""))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="串口桥接/旁路监听：实时观察 MCU <-> 4G 模组的 AT 交互"
                    "（CR/LF 显式可见）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例:\n"
               "  python tools/serial_bridge.py --list\n"
               "  python tools/serial_bridge.py -a COM5                 # 只听 MCU 发\n"
               "  python tools/serial_bridge.py -a COM5 -b COM6         # 桥接\n"
               "  python tools/serial_bridge.py -a COM5 --hex --stats\n")
    ap.add_argument("--list", action="store_true", help="列出串口后退出")
    ap.add_argument("-a", "--a", metavar="PORT", help="A 口（MCU 侧）")
    ap.add_argument("-b", "--b", metavar="PORT", help="B 口（模组侧）；不给 = 只监听 A")
    ap.add_argument("--baud-a", type=int, default=115200, help="A 口波特率（默认 115200）")
    ap.add_argument("--baud-b", type=int, default=115200, help="B 口波特率（默认 115200）")
    ap.add_argument("--hex", action="store_true", help="启动即用十六进制混合视图")
    ap.add_argument("--log", metavar="FILE", help="日志文件路径（默认 logs/bridge_时间.log）")
    ap.add_argument("--no-log", action="store_true", help="不写日志文件")
    ap.add_argument("--stats", action="store_true", help="每 5 秒打印一行统计")
    ap.add_argument("--no-color", action="store_true", help="关闭 ANSI 颜色")
    ap.add_argument("--no-free", action="store_true",
                    help="启动时不强制释放串口占用（默认会清理）")
    ap.add_argument("--free-dry", action="store_true",
                    help="只报告谁占着串口，不结束任何进程")
    args = ap.parse_args()

    global USE_COLOR
    if args.no_color:
        USE_COLOR = False

    if args.list:
        return do_list()

    if not args.a:
        # ★ 无参数 -> 交互向导，而不是"打印帮助后直接退出"。
        #   旧版这里是 return 0 —— 双击 .py 时窗口会瞬间关闭，
        #   现场看到的就是"运行闪退"。这一改把它修掉。
        if not _is_tty():
            # 被管道/重定向调用（自动化脚本）时不能等键盘，保持原行为
            ap.print_help()
            out("")
            return do_list()
        args = wizard()
        if args is None:
            # 返回 1（而不是 0）是故意的：让 _guarded_main 停在窗口上，
            # 用户能看清"已取消"或"认不出这个端口"的提示，不会一闪就没。
            return 1

    return Bridge(args).run()


def _guarded_main():
    """顶层兜底：任何异常都打印出来并留住窗口，绝不再"一闪就没"。"""
    _setup_stdio()
    enable_ansi()
    # 只有"双击 .py 启动"（无参数 + 真终端 + 不是经 bat 转手）才需要在
    # 失败后停一下，避免窗口一闪就没。bat 自己末尾已经 pause，不重复等。
    interactive = (_is_tty() and len(sys.argv) <= 1
                   and not os.environ.get("SB_LAUNCHER"))

    try:
        rc = main()
    except SystemExit:
        raise
    except KeyboardInterrupt:
        out("")
        rc = 130
    except Exception:
        import traceback
        out("")
        out(col("[!] 出错了，下面是完整堆栈（把这段发我就能定位）:", C_RED))
        try:
            traceback.print_exc()
        except Exception:
            pass
        rc = 1

    if interactive and rc != 0:
        try:
            input("\n按回车关闭窗口 ...")
        except Exception:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(_guarded_main())
