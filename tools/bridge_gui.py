#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""=============================================================================
 bridge_gui.py  --  串口桥接 / 旁路监听  ·  图形界面版（打包成 SerialBridge.exe）

 为什么要有这一版
 ----------------
 `serial_bridge.py` 是命令行工具，现场用起来有两处别扭：
   1) 要装 Python + pyserial，换台电脑就得重来一遍；
   2) 命令行窗口不能往回翻、不能暂停截图、不能一键存盘。

 本版把同一套逻辑搬进窗口：**双击 exe 就能跑，不装任何东西**（PyInstaller
  onefile 打包，pyserial / PySide6 全部塞进 exe 内部）。

 与 CLI 版的关系
 ---------------
 · 判读规则**完全一致**（直接 import serial_bridge 里的 CTRL / visible /
   hexdump_block，行聚合状态机照抄，CR/LF 判定一行没改）；
 · CR/LF 仍然显式画成 `<CR>` `<LF>`，"只有 CR 没有 LF"仍然单独标红报警
   —— 这条规则是上一轮上板返工换来的，不许在本版里走样；
 · 新增的只是"怎么看"：颜色、暂停、清屏、翻页、存盘、发送栏。

 两种模式（与原版同名）
 ----------------------
 sniff   只接 A 口，只读不写，看 MCU 单方向发了什么
         USB-TTL.RXD <- MCU P1.1(TXD2) ; GND -- GND ; USB-TTL.TXD 悬空
 bridge  接 A、B 两个口，插在 MCU 与模组中间，双向透明转发
         TTL#1.TXD -> MCU.P1.0(RXD2)   TTL#1.RXD <- MCU.P1.1(TXD2)
         TTL#2.TXD -> 模组.RXD         TTL#2.RXD <- 模组.TXD
         三处 GND 必须共地

 打包（tools/build_exe.py 已封装）
 --------------------------------
   python -m PyInstaller --onefile --windowed --name SerialBridge \
          --paths tools tools/bridge_gui.py
============================================================================="""

import datetime
import os
import queue
import sys
import threading
import time
import traceback

# ---------------------------------------------------------------- 依赖自检
#
# --windowed 打包后没有控制台，任何 import 失败都会表现为"双击没反应"。
# 所以这里把依赖失败收集起来，等 QApplication 起来后用弹窗告诉用户，
# 而不是让进程静默退出。

_MISSING = []
try:
    import serial
    from serial.tools import list_ports
except Exception as _e:                                   # pragma: no cover
    serial = None
    list_ports = None
    _MISSING.append("pyserial (%s)" % _e)

try:
    from PySide6.QtCore import Qt, QTimer, QUrl
    from PySide6.QtGui import (QFont, QFontDatabase, QTextCursor, QIcon,
                               QDesktopServices, QGuiApplication,
                               QTextBlockFormat, QTextCharFormat)
    from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QLabel,
                                   QComboBox, QPushButton, QTextEdit, QCheckBox,
                                   QLineEdit, QVBoxLayout, QHBoxLayout, QFrame,
                                   QMessageBox, QSizePolicy, QToolButton)
except Exception as _e:                                   # pragma: no cover
    sys.stderr.write("PySide6 缺失: %s\n" % _e)
    raise

# 显示层复用 CLI 版：CTRL 表 / visible() / hexdump_block()
# ★ 不许在这里另写一份 —— 两份实现早晚会走样，而 CR/LF 判读是硬规则。
try:
    from serial_bridge import CTRL, visible, hexdump_block
except Exception:                                        # pragma: no cover
    # 兜底：万一打包时没带上，最小可用副本（保证程序能开，不会白屏）
    CTRL = {0x0D: "<CR>", 0x0A: "<LF>", 0x09: "<TAB>", 0x00: "<NUL>"}

    def visible(data):
        return "".join(CTRL.get(b, chr(b) if 0x20 <= b < 0x7F else "<%02X>" % b)
                       for b in data)

    def hexdump_block(data):
        out = []
        for off in range(0, len(data), 16):
            c = data[off:off + 16]
            out.append("%s  |%s|" % (
                " ".join("%02X" % b for b in c).ljust(47),
                "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in c)))
        return out

try:
    import port_cleaner as pc
except Exception:                                        # pragma: no cover
    pc = None


# ---------------------------------------------------------------- 路径

APP_NAME = "串口桥接 · OTA 调试"


def app_dir():
    """exe 所在目录（打包后）/ 脚本所在目录（开发时）。"""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def log_dir():
    """日志目录。

    ★ exe 就放在 tools/ 里，若日志写 tools/logs/ 就会和 CLI 版的
      <工程根>/logs/ 变成两份，事后找日志永远找错地方。所以只要 exe
      位于 tools/ 且工程根有 logs/，就写回工程根的 logs/。
    """
    base = app_dir()
    parent = os.path.dirname(base)
    d = os.path.join(base, "logs")
    if (os.path.basename(base).lower() == "tools"
            and os.path.isdir(os.path.join(parent, "logs"))):
        d = os.path.join(parent, "logs")
    try:
        os.makedirs(d, exist_ok=True)
    except Exception:
        d = os.path.join(os.path.expanduser("~"), "SerialBridge_logs")
        os.makedirs(d, exist_ok=True)
    return d


# ---------------------------------------------------------------- 行聚合
#
# ★ 状态机与原版逐行等价，只把"打印"换成了"返回列表"。
#   规则（遇到 CR 先挂起，看下一个字节再定性）：
#     下一个是 LF  -> CRLF 行尾，正常收尾，不报警
#     下一个是 CR  -> 前一个 CR 属冗余行尾，不报警
#     下一个是普通 -> 真的只有 CR 没有 LF，**报警**
#     50 ms 内无后续 -> 同上，报警
#   为什么不能"见 CR 就断行"：模组回显是 AT\r\r\nOK\r\n，第一行 AT\r 会被
#   天天误报成"缺 LF"，告警立刻失去意义。

class LineAssembler:
    LINE_HARD_CAP = 4096
    IDLE_FLUSH_S = 0.05

    def __init__(self):
        self.cur = {}          # tag -> bytearray
        self.saw_cr = {}       # tag -> bool
        self.hold = {}         # tag -> 最后收到字节的时刻

    def reset(self):
        self.cur.clear()
        self.saw_cr.clear()
        self.hold.clear()

    def drop(self, tag):
        self.cur.pop(tag, None)
        self.saw_cr.pop(tag, None)
        self.hold.pop(tag, None)

    def feed(self, tag, data):
        """喂入字节，返回 [(tag, bytes, complete, missing_lf), ...]。"""
        done = []
        now = time.monotonic()
        cur = self.cur.setdefault(tag, bytearray())
        pend = self.saw_cr.get(tag, False)
        self.hold[tag] = now

        for b in data:
            if pend:
                pend = False
                if b == 0x0A:                       # CRLF：正常收尾
                    cur.append(b)
                    done.append((tag, bytes(cur), True, False))
                    cur.clear()
                    continue
                elif b == 0x0D:                     # 冗余行尾，不报警
                    done.append((tag, bytes(cur), True, False))
                    cur.clear()
                    cur.append(b)
                    pend = True
                    continue
                else:                               # 真·只有 CR，报警
                    done.append((tag, bytes(cur), True, True))
                    cur.clear()
                    cur.append(b)
                    continue

            if b == 0x0D:
                cur.append(b)
                pend = True
            elif b == 0x0A:
                cur.append(b)
                done.append((tag, bytes(cur), True, False))
                cur.clear()
            else:
                cur.append(b)

            if len(cur) >= self.LINE_HARD_CAP:
                done.append((tag, bytes(cur), False, False))
                cur.clear()
                pend = False

        self.saw_cr[tag] = pend
        return done

    def pump(self):
        """半截行不许永远压着：静默 50 ms 就吐出来。"""
        done = []
        now = time.monotonic()
        for tag, cur in list(self.cur.items()):
            if not cur:
                continue
            if now - self.hold.get(tag, now) < self.IDLE_FLUSH_S:
                continue
            if self.saw_cr.get(tag, False):
                # 以 CR 结尾、50 ms 没等到 LF ⇒ 这一帧真就是"只有 CR 没有 LF"
                self.saw_cr[tag] = False
                done.append((tag, bytes(cur), True, True))
            else:
                done.append((tag, bytes(cur), False, False))
            cur.clear()
        return done


# ---------------------------------------------------------------- 串口引擎

class Endpoint:
    def __init__(self, port, baud, tag):
        self.port_name = port
        self.baud = baud
        self.tag = tag
        self.ser = None
        self.peer = None
        self.nbytes = 0
        self.nlines = 0
        # ★ 写队列：读线程只负责"把字节取出来丢进对端的队列"，绝不在这里
        #   阻塞。OTA 一次搬 49088 B，若写对端时卡住（write_timeout=2 s），
        #   本方向就停止读取，OS 缓冲（默认 4 KB）一满就**静默丢字节** ——
        #   现场表现是"OTA 跑到一半校验失败"，而日志里什么都看不出来。
        self.wq = queue.Queue(maxsize=4096)
        self.wthread = None
        self.wdrop = 0      # 队列满丢弃的字节数
        self.werr = 0       # 写异常次数

    def open(self):
        self.ser = serial.Serial(port=self.port_name, baudrate=self.baud,
                                 bytesize=8, parity="N", stopbits=1,
                                 # 10 ms：比原来的 20 ms 少一半单向延迟。
                                 # 115200 下 10 ms ≈ 115 B，正好一个来回的零头。
                                 timeout=0.01, write_timeout=2.0,
                                 rtscts=False, dsrdtr=False, xonxoff=False)
        # 清掉打开瞬间的残留，免得第一屏就是上一轮的半截数据
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        return self

    def close(self):
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None


class Engine:
    """串口读写 + 断线重连。所有输出走 self.q，由主线程定时器消费。

    q 里的元素：
      ("dat",  (tag, bytes))              收到的数据
      ("msg",  (text, level))             提示（level: info/ok/warn/err/dim）
      ("open", (ok, text))                启动结果，UI 据此切按钮状态
      ("shut", (text,))                   已停止
    """

    RECONNECT_S = 20.0

    def __init__(self, q):
        self.q = q
        self.stop_evt = threading.Event()
        self.eps = []
        self.threads = []
        self.running = False
        self.logf = None
        self.log_path = None
        self.t0 = time.monotonic()
        self.hex_view = False

    # ---- 小工具 ----
    def msg(self, text, level="info"):
        self.q.put(("msg", (text, level)))

    def _open_log(self):
        try:
            path = os.path.join(log_dir(), "bridge_%s.log" %
                                datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
            self.logf = open(path, "a", encoding="utf-8", newline="\n")
            self.log_path = path
        except Exception as e:
            self.logf = None
            self.log_path = None
            self.msg("日志文件打不开（只显示不落盘）: %s" % e, "warn")

    def log_line(self, plain):
        if self.logf:
            try:
                self.logf.write(plain + "\n")
                self.logf.flush()
            except Exception:
                pass

    # ---- 启动 ----
    def start_async(self, cfg):
        threading.Thread(target=self._start_worker, args=(cfg,),
                         daemon=True).start()

    def _start_worker(self, cfg):
        a_port, b_port = cfg["a"], cfg["b"]
        try:
            if cfg.get("free") and pc is not None:
                targets = [p for p in (a_port, b_port) if p]
                self.msg("检查串口占用: %s ..." % " ".join(targets), "dim")
                busy = []
                for p in targets:
                    m = pc.probe(p)
                    if m is None:
                        self.msg("  %s 空闲" % p, "dim")
                    elif pc.is_busy(m):
                        busy.append(p)

                if busy:
                    self.msg("  被占用: %s —— 结束占口进程" % " ".join(busy), "warn")
                    # ★ 2026-10-28：这一段以前没有 try，port_cleaner 里一个格式串
                    #   写错就抛 TypeError，整个启动直接 abort（"启动失败: 启动异常"），
                    #   连串口都没开。清理只是"帮忙腾口"，它失败绝不该挡住测量。
                    try:
                        r = pc.free(busy, kill_tools=True,
                                    log=lambda s: self.msg(s, "dim"))
                        for pid, exe, desc in r["killed"]:
                            self.msg("  已结束 %s (pid=%d, %s)" % (exe, pid, desc), "ok")
                        if r["busy_after"]:
                            self.msg("  仍被占用: %s，请手动关闭对应软件"
                                     % " ".join(r["busy_after"]), "err")
                    except Exception as e:
                        self.msg("  清理端口出错（忽略，继续打开）: %r" % (e,), "warn")

            a = Endpoint(a_port, cfg["baud_a"], "MCU>MOD")
            try:
                a.open()
            except Exception as e:
                self.msg("打不开 %s: %s" % (a_port, e), "err")
                self.msg("提示: STC-ISP 下载时会独占 COM 口，先关掉 ISP", "dim")
                self.q.put(("open", (False, "打不开 %s" % a_port)))
                return
            self.eps.append(a)

            b = None
            if b_port:
                b = Endpoint(b_port, cfg["baud_b"], "MOD>MCU")
                try:
                    b.open()
                except Exception as e:
                    self.msg("打不开 %s: %s" % (b_port, e), "err")
                    a.close()
                    self.eps.clear()
                    self.q.put(("open", (False, "打不开 %s" % b_port)))
                    return
                self.eps.append(b)
                a.peer = b
                b.peer = a

            self._open_log()
            self.t0 = time.monotonic()

            if b is None:
                self.msg("模式 sniff：只监听 %s，不向 MCU 写任何字节" % a_port, "ok")
            else:
                self.msg("模式 bridge：%s @%d  <->  %s @%d（透明转发）"
                         % (a_port, cfg["baud_a"], b_port, cfg["baud_b"]), "ok")
            if self.log_path:
                self.msg("日志: %s" % self.log_path, "dim")

            self.running = True
            for e in self.eps:
                t = threading.Thread(target=self._pump, args=(e,), daemon=True)
                t.start()
                self.threads.append(t)
                if e.peer is not None:          # 只有桥接才需要写线程
                    wt = threading.Thread(target=self._write_loop, args=(e,),
                                          daemon=True)
                    wt.start()
                    self.threads.append(wt)
            self.q.put(("open", (True, "")))
        except Exception:
            self.msg("启动失败: %s" % traceback.format_exc(limit=3), "err")
            self.q.put(("open", (False, "启动异常")))

    # ---- 读线程 ----
    def _pump(self, ep):
        while not self.stop_evt.is_set():
            try:
                n = ep.ser.in_waiting
                data = ep.ser.read(n if n else 1)
            except Exception as e:
                if self.stop_evt.is_set():
                    return
                if not self._reconnect(ep, e):
                    self.stop_evt.set()
                    self.q.put(("shut", ("%s 掉线无法恢复" % ep.port_name,)))
                    return
                continue
            if not data:
                continue
            ep.nbytes += len(data)
            if ep.peer is not None:
                # 只入队，不在这里 write：读线程必须永远立刻回到 read()
                try:
                    ep.peer.wq.put_nowait(data)
                except queue.Full:
                    ep.peer.wdrop += len(data)
            self.q.put(("dat", (ep.tag, data)))

    def _write_loop(self, ep):
        """专门负责往 ep 写。与读线程解耦，写阻塞再久也不影响收数据。"""
        while not self.stop_evt.is_set():
            try:
                data = ep.wq.get(timeout=0.1)
            except queue.Empty:
                continue
            if not data:
                continue
            try:
                if ep.ser:
                    ep.ser.write(data)
            except Exception as e:
                # 写不进去只丢这一块：对端若真掉线，它自己的读线程会失败
                # 并触发重连；这里再退出会重复报错、还会两个线程抢一个句柄
                ep.werr += 1
                if ep.werr <= 3 or (ep.werr % 50) == 0:
                    self.msg("%s 写失败(%d 次): %s（丢弃本块）"
                             % (ep.port_name, ep.werr, e), "err")

    def _reconnect(self, ep, err):
        self.msg("%s 断开: %s" % (ep.port_name, err), "err")
        self.msg("  22=ERROR_BAD_COMMAND 通常是 USB 转串口被拔/驱动掉了", "dim")
        ep.close()
        self.msg("  开始重连（最多 %.0f 秒，插回去即可）..." % self.RECONNECT_S, "warn")
        deadline = time.monotonic() + self.RECONNECT_S
        tries = 0
        while not self.stop_evt.is_set() and time.monotonic() < deadline:
            tries += 1
            time.sleep(0.4)
            try:
                ep.open()
            except Exception:
                if (tries % 5) == 0:
                    self.msg("  %s 还不行 ... (%d 次)" % (ep.port_name, tries), "dim")
                continue
            self.msg("%s 已重连（第 %d 次尝试）" % (ep.port_name, tries), "ok")
            self.q.put(("died", (ep.tag,)))     # 通知 UI 丢弃断线前的半截行
            return True
        if not self.stop_evt.is_set():
            self.msg("%s 重连失败" % ep.port_name, "err")
        return False

    # ---- 发送 ----
    def send(self, tag, data):
        for e in self.eps:
            if e.tag == tag and e.ser:
                try:
                    e.ser.write(data)
                    return True, ""
                except Exception as ex:
                    return False, str(ex)
        return False, "端口未打开"

    # ---- 停止 ----
    def stop(self):
        self.stop_evt.set()
        for t in self.threads:
            t.join(timeout=0.5)
        self.threads.clear()
        summary = []
        for e in self.eps:
            summary.append((e.tag, e.nbytes, e.nlines, e.wdrop, e.werr))
            e.close()
        self.eps.clear()
        if self.logf:
            try:
                self.logf.close()
            except Exception:
                pass
            self.logf = None
        self.running = False
        return summary

    def stats(self):
        el = time.monotonic() - self.t0
        return [(e.tag, e.nbytes, e.nlines, e.wdrop) for e in self.eps], el


# ---------------------------------------------------------------- 渲染

def esc(s):
    """HTML 转义 + 空格保护。

    ★ Qt 的富文本引擎不支持 CSS 的 white-space:pre，多个连续空格会被合并，
      hexdump 立刻对不齐。所以空格一律换成 &nbsp;。
    """
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace(" ", "&nbsp;"))


TAG_COLOR = {
    "MCU>MOD": "#1565C0",     # MCU 发出去 —— 蓝
    "MOD>MCU": "#E65100",     # 模组回过来 —— 橙
    "PC>A": "#6A1B9A",
    "PC>B": "#6A1B9A",
}
C_TIME = "#9AA0A6"
C_CTRL = "#B08968"
C_MISS = "#C2185B"
C_PART = "#7B1FA2"

# 超过这个长度的行按"二进制块"摘要渲染（见 line_html 里的说明）
LINE_SUMMARY = 512


def body_html(data):
    parts = []
    for b in data:
        if b in CTRL:
            # ★ CTRL 里的值本身就带尖括号（"<CR>"），必须转义后再塞进 HTML，
            #   否则 Qt 会把它当成未知标签直接吞掉 —— 界面上就只剩 "AT"，
            #   而"AT 后面到底有没有换行"恰恰是本工具唯一要回答的问题。
            parts.append('<span style="color:%s">%s</span>'
                         % (C_CTRL, esc(CTRL[b])))
        elif 0x20 <= b < 0x7F:
            parts.append(esc(chr(b)))
        else:
            parts.append('<span style="color:%s">&lt;%02X&gt;</span>' % (C_CTRL, b))
    return "".join(parts)


def line_html(tag, data, complete, missing_lf, hexmode=False):
    now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    color = TAG_COLOR.get(tag, "#424242")
    tagpad = "&nbsp;" * max(0, 9 - len(tag))
    tag_h = ('<span style="color:%s;font-weight:bold">%s</span>%s'
             % (color, tag.replace(">", "&gt;"), tagpad))

    if hexmode:
        # hex 视图不聚合行：块内每 16 字节一行
        html = []
        plain_lines = []
        for ln in hexdump_block(data):
            html.append('<p style="margin:0">'
                        '<span style="color:%s">%s</span>&nbsp;&nbsp;%s'
                        '<span style="color:#616161">%s</span></p>'
                        % (C_TIME, now, tag_h, esc(ln)))
            plain_lines.append("%-9s  %s" % (tag, ln))
        return "".join(html), now, plain_lines

    txt = visible(data)
    stripped = data.strip()

    # ★ 超长行（OTA 的二进制块）只画摘要，不逐字节上色。
    #   实测：逐字节 span 会让 HTML 膨胀 29 倍，49088 B 的镜像正文
    #   就是 45 MB 富文本，界面会直接卡死 —— 而"卡死的界面"反过来会让
    #   用户以为串口卡了。完整字节仍然一字不差地写进日志。
    if len(data) > LINE_SUMMARY:
        head = " ".join("%02X" % b for b in data[:32])
        tail = " ".join("%02X" % b for b in data[-8:]) if len(data) > 48 else ""
        body = ('<span style="color:%s;font-weight:bold">[%d B 二进制块]</span>'
                '&nbsp;&nbsp;<span style="color:#616161">%s%s</span>'
                '<span style="color:%s">&nbsp;&nbsp;… 完整字节见日志</span>'
                % (C_PART, len(data), esc(head),
                   ("&nbsp;…&nbsp;" + esc(tail)) if tail else "", C_TIME))
        html = ('<p style="margin:0"><span style="color:%s">%s</span>&nbsp;&nbsp;'
                '%s%s</p>' % (C_TIME, now, tag_h, body))
        plain = "%-9s  %s" % (tag, txt)
        return html, now, [plain]

    style_new = ""
    if stripped.upper().startswith(b"AT"):
        style_new = "font-weight:bold;color:#0D47A1;"
    elif stripped in (b"OK", b"RDY"):
        style_new = "color:#2E7D32;font-weight:bold;"
    elif b"ERROR" in stripped or b"FAIL" in stripped:
        style_new = "color:#C62828;font-weight:bold;"

    suffix = ""
    plain_suffix = ""
    if missing_lf:
        style_new = "color:%s;font-weight:bold;" % C_MISS
        suffix = ('<span style="color:%s;font-weight:bold">'
                  '&nbsp;&nbsp;&nbsp;&lt;&lt;&lt;&nbsp;只有&nbsp;CR，没有&nbsp;LF</span>'
                  % C_MISS)
        plain_suffix = "   <<< CR without LF"
    elif not complete:
        suffix = ('<span style="color:%s">'
                  '&nbsp;&nbsp;&nbsp;&lt;&lt;&lt;&nbsp;未以换行结束（超时吐出）</span>'
                  % C_PART)
        plain_suffix = "   <<< no newline (timeout flush)"

    html = ('<p style="margin:0"><span style="color:%s">%s</span>&nbsp;&nbsp;%s'
            '<span style="%s">%s</span>%s</p>'
            % (C_TIME, now, tag_h, style_new, body_html(data), suffix))
    plain = "%-9s  %s%s" % (tag, txt, plain_suffix)
    return html, now, [plain]


# ---------------------------------------------------------------- 主窗口

MAX_LINES = 8000
RENDER_BUDGET = 32 * 1024     # 单帧最多渲染的数据字节（见 _tick）


def _rate(bps):
    if bps >= 1048576:
        return "%.2f MB/s" % (bps / 1048576.0)
    if bps >= 1024:
        return "%.1f kB/s" % (bps / 1024.0)
    return "%.0f B/s" % bps


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.resize(1180, 760)

        self.q = queue.Queue()
        self.engine = Engine(self.q)
        self.asm = LineAssembler()
        self.paused = False
        self.hexmode = False
        self.running = False
        self.skipped = 0                    # 因渲染预算被省略显示的行数
        self._last_stat_t = time.monotonic()
        self._last_bytes = {}               # tag -> 上次采样字节数（算速率）

        self._build_ui()
        self.refresh_ports()

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)
        self.timer.start(30)

        self.stat_timer = QTimer(self)
        self.stat_timer.timeout.connect(self._update_stats)
        self.stat_timer.start(500)

        self._append_msg("就绪。选好串口 → 点「开始」。sniff 只接 A 口；bridge 再接 B 口。", "info")

    # ---------------- UI ----------------
    def _mono(self, size=10):
        fams = set(QFontDatabase.families())
        for name in ("Consolas", "Cascadia Mono", "Courier New"):
            if name in fams:
                return QFont(name, size)
        return QFont("Monospace", size)

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(10, 8, 10, 8)
        root.setSpacing(6)

        # ---- 第一行：端口与开关 ----
        bar = QHBoxLayout()
        bar.setSpacing(6)

        bar.addWidget(QLabel("A 口(MCU)"))
        self.cb_a = QComboBox()
        self.cb_a.setMinimumWidth(230)
        bar.addWidget(self.cb_a)

        bar.addWidget(QLabel("速率"))
        self.cb_baud = QComboBox()
        for b in (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600):
            self.cb_baud.addItem(str(b), b)
        self.cb_baud.setCurrentText("115200")
        self.cb_baud.setMinimumWidth(90)
        bar.addWidget(self.cb_baud)

        bar.addSpacing(8)
        bar.addWidget(QLabel("B 口(模组)"))
        self.cb_b = QComboBox()
        self.cb_b.setMinimumWidth(230)
        bar.addWidget(self.cb_b)

        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.setFixedWidth(58)
        self.btn_refresh.clicked.connect(self.refresh_ports)
        bar.addWidget(self.btn_refresh)

        self.chk_free = QCheckBox("先释放占用")
        self.chk_free.setChecked(True)
        self.chk_free.setToolTip("启动前探测并结束占用串口的程序（STC-ISP / 串口助手等）")
        bar.addWidget(self.chk_free)

        self.btn_run = QPushButton("开始")
        self.btn_run.setMinimumWidth(96)
        self.btn_run.setStyleSheet(
            "QPushButton{background:#1565C0;color:white;font-weight:bold;"
            "padding:5px 14px;border-radius:4px}"
            "QPushButton:hover{background:#1976D2}")
        self.btn_run.clicked.connect(self.toggle_run)
        bar.addWidget(self.btn_run)

        bar.addStretch(1)
        root.addLayout(bar)

        # ---- 第二行：接线提示（随模式变化）----
        self.lbl_wire = QLabel("")
        self.lbl_wire.setWordWrap(True)
        self.lbl_wire.setStyleSheet(
            "QLabel{background:#F1F3F4;color:#455A64;padding:4px 8px;"
            "border-radius:3px;font-size:12px}")
        root.addWidget(self.lbl_wire)
        self._update_wire_hint()

        # ---- 数据区 ----
        self.view = QTextEdit()
        self.view.setReadOnly(True)
        self.view.setFont(self._mono(10))
        self.view.setLineWrapMode(QTextEdit.NoWrap)
        self.view.document().setMaximumBlockCount(MAX_LINES)
        self.view.document().setDocumentMargin(2)
        self.view.setStyleSheet(
            "QTextEdit{background:#FFFFFF;border:1px solid #DADCE0;"
            "border-radius:4px;selection-background-color:#BBDEFB}")
        root.addWidget(self.view, 1)

        # ---- 第三行：工具条 ----
        tools = QHBoxLayout()
        tools.setSpacing(6)

        self.btn_pause = QPushButton("暂停")
        self.btn_pause.setCheckable(True)
        self.btn_pause.clicked.connect(self.on_pause)
        tools.addWidget(self.btn_pause)

        self.btn_clear = QPushButton("清屏")
        self.btn_clear.clicked.connect(self.on_clear)
        tools.addWidget(self.btn_clear)

        self.chk_hex = QCheckBox("HEX 视图")
        self.chk_hex.stateChanged.connect(self.on_hex)
        tools.addWidget(self.chk_hex)

        self.btn_log = QPushButton("打开日志目录")
        self.btn_log.clicked.connect(self.open_log_dir)
        tools.addWidget(self.btn_log)

        self.btn_help = QPushButton("接线说明")
        self.btn_help.clicked.connect(self.show_help)
        tools.addWidget(self.btn_help)

        tools.addStretch(1)

        self.lbl_stat = QLabel("")
        self.lbl_stat.setFont(self._mono(9))
        self.lbl_stat.setStyleSheet("color:#455A64")
        tools.addWidget(self.lbl_stat)
        root.addLayout(tools)

        # ---- 第四行：发送 ----
        send = QHBoxLayout()
        send.setSpacing(6)
        send.addWidget(QLabel("发送"))
        self.ed_send = QLineEdit()
        self.ed_send.setPlaceholderText("输入要发出的字节（如 AT 或 AT+CGMR），回车发送")
        self.ed_send.setFont(self._mono(10))
        self.ed_send.returnPressed.connect(self.on_send)
        send.addWidget(self.ed_send, 1)

        self.cb_target = QComboBox()
        self.cb_target.addItem("→ A 口", "MCU>MOD")
        self.cb_target.addItem("→ B 口", "MOD>MCU")
        send.addWidget(self.cb_target)

        self.chk_crlf = QCheckBox("CRLF")
        self.chk_crlf.setChecked(True)
        self.chk_crlf.setToolTip("发送时自动追加 \\r\\n（AT 命令必须带行结束符）")
        send.addWidget(self.chk_crlf)

        self.btn_send = QPushButton("发送")
        self.btn_send.clicked.connect(self.on_send)
        send.addWidget(self.btn_send)
        root.addLayout(send)

        self.status = self.statusBar()
        self.status.setStyleSheet("QStatusBar{color:#5F6368}")
        self.status.showMessage("未连接")

    def _update_wire_hint(self):
        b = self.cb_b.currentData() if hasattr(self, "cb_b") else None
        if b:
            self.lbl_wire.setText(
                "bridge 双向透明转发   A口→MCU: TXD→P1.0(RXD2) / RXD←P1.1(TXD2)　"
                "B口→模组: TXD→RXD / RXD←TXD　三处 GND 必须共地")
        else:
            self.lbl_wire.setText(
                "sniff 旁路监听（只读）   USB-TTL.RXD ← MCU P1.1(TXD2)，GND 共地，"
                "USB-TTL.TXD 悬空不要接（会和 MCU 的 TXD 打架）")

    # ---------------- 端口 ----------------
    def refresh_ports(self):
        cur_a = self.cb_a.currentData()
        cur_b = self.cb_b.currentData()
        self.cb_a.blockSignals(True)
        self.cb_b.blockSignals(True)
        self.cb_a.clear()
        self.cb_b.clear()
        self.cb_b.addItem("（不接 · 只监听 A）", None)

        ports = []
        if list_ports is not None:
            ports = sorted(list_ports.comports(), key=lambda p: p.device)

        for p in ports:
            label = "%s  %s" % (p.device, (p.description or "")[:36])
            self.cb_a.addItem(label, p.device)
            self.cb_b.addItem(label, p.device)

        if not ports:
            self.cb_a.addItem("（未发现串口，点刷新）", None)

        for cb, want in ((self.cb_a, cur_a), (self.cb_b, cur_b)):
            if want:
                i = cb.findData(want)
                if i >= 0:
                    cb.setCurrentIndex(i)
        # ★ B 口默认停在"（不接·只监听 A）"：桥接必须由人明确选，不能靠猜。
        #   猜错的代价是现场以为在看双向，其实只录了一半。

        self.cb_a.blockSignals(False)
        self.cb_b.blockSignals(False)
        self._update_wire_hint()
        self.status.showMessage("发现 %d 个串口" % len(ports))

    # ---------------- 启停 ----------------
    def toggle_run(self):
        if self.running:
            self.stop()
            return

        a = self.cb_a.currentData()
        if not a:
            QMessageBox.warning(self, APP_NAME,
                                "没有可用的 A 口。\n先插上 USB-TTL，确认驱动装好，再点「刷新」。")
            return
        b = self.cb_b.currentData()
        if b and os.path.normcase(b) == os.path.normcase(a):
            QMessageBox.warning(self, APP_NAME, "A 口和 B 口不能是同一个串口。")
            return

        baud = int(self.cb_baud.currentData())
        self._append_msg("=" * 60, "dim")
        self._append_msg("开始: A=%s@%d%s" % (a, baud, ("  B=%s@%d" % (b, baud)) if b else "  (sniff)"), "info")
        self.btn_run.setEnabled(False)
        self.btn_run.setText("启动中…")
        self.status.showMessage("正在打开串口 ...")

        self.engine.hex_view = self.hexmode
        self.engine.start_async({
            "a": a, "baud_a": baud,
            "b": b, "baud_b": baud,
            "free": self.chk_free.isChecked(),
        })

    def stop(self):
        summary = self.engine.stop()
        self.running = False
        self.btn_run.setEnabled(True)
        self.btn_run.setText("开始")
        self.btn_run.setStyleSheet(
            "QPushButton{background:#1565C0;color:white;font-weight:bold;"
            "padding:5px 14px;border-radius:4px}"
            "QPushButton:hover{background:#1976D2}")
        self.btn_refresh.setEnabled(True)
        self.cb_a.setEnabled(True)
        self.cb_b.setEnabled(True)
        self.cb_baud.setEnabled(True)
        self._append_msg("已停止。", "warn")
        for tag, n, ln, wd, we in summary:
            extra = ""
            if wd:
                extra += "  ⚠转发队列满丢弃 %d B" % wd
            if we:
                extra += "  写失败 %d 次" % we
            self._append_msg("  汇总 %-9s %8d B / %d 行%s"
                             % (tag, n, ln, extra), "dim")
        self.status.showMessage("未连接")
        self.lbl_stat.setText("")

    # ---------------- 定时消费 ----------------
    def _tick(self):
        lines = []
        msgs = []
        hexmode = self.hexmode
        # 上限 4000 次/轮：数据洪流时宁可下一轮再取，也不能把界面线程卡死
        for _ in range(4000):
            try:
                kind, payload = self.q.get_nowait()
            except queue.Empty:
                break
            if kind == "dat":
                tag, data = payload
                if hexmode:
                    lines.append((tag, data, True, False, True))
                else:
                    for it in self.asm.feed(tag, data):
                        lines.append((it[0], it[1], it[2], it[3], False))
            elif kind == "msg":
                msgs.append(payload)
            elif kind == "open":
                ok, text = payload
                self._on_open_result(ok, text)
            elif kind == "shut":
                self._append_msg("串口已停止: %s" % payload[0], "err")
                self.stop()
            elif kind == "died":
                self.asm.drop(payload[0])

        if not hexmode:
            for it in self.asm.pump():
                lines.append((it[0], it[1], it[2], it[3], False))

        for text, level in msgs:
            self._append_msg(text, level)

        if not lines:
            return
        if self.paused:
            # 暂停只停"看"，日志照写（与 CLI 版一致：屏幕可以停，证据不能丢）
            self._log_only(lines)
            return

        # 渲染预算：30 ms 一帧，115200 满速也才 345 B/帧。给到 32 KB 是 90 倍
        # 余量；再多就只写日志不画 —— 画不动的界面会被误读成"串口卡住了"。
        show, rest, acc = [], [], 0
        for it in lines:
            n = len(it[1])
            if acc + n <= RENDER_BUDGET or not show:
                show.append(it)
                acc += n
            else:
                rest.append(it)
        self._append_lines(show)
        if rest:
            self._log_only(rest)
            self.skipped += len(rest)

    def _log_only(self, items):
        for tag, data, complete, missing, hx in items:
            _, ts, ps = line_html(tag, data, complete, missing, hx)
            for p in ps:
                self.engine.log_line("%s  %s" % (ts, p))

    def _on_open_result(self, ok, text):
        self.btn_run.setEnabled(True)
        if ok:
            self.running = True
            self.btn_run.setText("停止")
            self.btn_run.setStyleSheet(
                "QPushButton{background:#C62828;color:white;font-weight:bold;"
                "padding:5px 14px;border-radius:4px}"
                "QPushButton:hover{background:#D32F2F}")
            self.cb_a.setEnabled(False)
            self.cb_b.setEnabled(False)
            self.cb_baud.setEnabled(False)
            self.btn_refresh.setEnabled(False)
            a = self.cb_a.currentData()
            b = self.cb_b.currentData()
            self.status.showMessage("已连接  %s%s" % (
                a, ("  <->  " + b) if b else "  (只监听)"))
        else:
            self.running = False
            self.btn_run.setText("开始")
            self._append_msg("启动失败: %s" % (text or "未知原因"), "err")
            self.status.showMessage("未连接")
            self.cb_a.setEnabled(True)
            self.cb_b.setEnabled(True)
            self.cb_baud.setEnabled(True)
            self.btn_refresh.setEnabled(True)

    # ---------------- 渲染 ----------------
    def _new_block_cursor(self):
        """取一个位于文档末尾的**新段落**游标。

        ★ QTextEdit 没有 appendHtml()；直接 insertHtml 会把内容接在最后一段
          的尾巴上（两条提示粘成一行、上一条数据行的末尾挂上本条时间戳）。
          所以当前段落非空时要先另起一段。
        """
        cur = self.view.textCursor()
        cur.movePosition(QTextCursor.End)
        if cur.block().length() > 1:
            bf = QTextBlockFormat()
            bf.setTopMargin(0)
            bf.setBottomMargin(0)
            cur.insertBlock(bf, QTextCharFormat())
        return cur

    def _append_lines(self, items):
        # items: (tag, bytes, complete, missing_lf, hexmode)
        at_bottom = self._at_bottom()
        html_parts = []
        plains = []
        for tag, data, complete, missing, hx in items:
            h, ts, ps = line_html(tag, data, complete, missing, hx)
            html_parts.append(h)
            for p in ps:
                plains.append("%s  %s" % (ts, p))

        # 分批插入：一次塞几千行会把界面顶住
        for i in range(0, len(html_parts), 200):
            self._new_block_cursor().insertHtml("".join(html_parts[i:i + 200]))
        for p in plains:
            self.engine.log_line(p)

        if at_bottom:
            sb = self.view.verticalScrollBar()
            sb.setValue(sb.maximum())

    def _at_bottom(self):
        sb = self.view.verticalScrollBar()
        return sb.value() >= sb.maximum() - 8

    MSG_MARK = {"info": "»", "ok": "✓", "warn": "!", "err": "×", "dim": "·"}

    def _append_msg(self, text, level="info"):
        color = {"info": "#455A64", "ok": "#2E7D32", "warn": "#E65100",
                 "err": "#C62828", "dim": "#9AA0A6"}.get(level, "#455A64")
        mark = self.MSG_MARK.get(level, "»")
        now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        at_bottom = self._at_bottom()
        html = ('<p style="margin:0"><span style="color:%s">%s</span>'
                '&nbsp;&nbsp;<span style="color:%s;font-weight:bold">%s</span>'
                '&nbsp;&nbsp;<span style="color:%s">%s</span></p>'
                % (C_TIME, now, color, mark, color, esc(text)))
        self._new_block_cursor().insertHtml(html)
        if at_bottom:
            sb = self.view.verticalScrollBar()
            sb.setValue(sb.maximum())
        # 提示也进日志，方便事后对照
        self.engine.log_line("%s  [%s] %s" % (now, level, text))


    def _update_stats(self):
        if not self.running:
            return
        now = time.monotonic()
        eps, el = self.engine.stats()
        dt = now - self._last_stat_t
        # 该波特率下的理论满速：8N1 = 10 bit/字节
        baud = self.engine.eps[0].baud if self.engine.eps else 0
        cap = baud / 10.0 if baud else 0.0
        parts = []
        for tag, n, ln, wd in eps:
            prev = self._last_bytes.get(tag, n)
            rate = (n - prev) / dt if dt > 1e-6 else 0.0
            self._last_bytes[tag] = n
            pct = ("%.0f%%" % (100.0 * rate / cap)) if cap else "--"
            s = "%s %s B / %d 行  %s %s" % (tag, format(n, ","), ln,
                                            _rate(rate), pct)
            if wd:
                s += "  ⚠丢弃 %d B" % wd
            parts.append(s)
        self._last_stat_t = now
        txt = "  |  ".join(parts) + "   t=%02d:%02d" % (int(el // 60), int(el % 60))
        if self.skipped:
            txt += "   (显示省略 %d 行)" % self.skipped
        self.lbl_stat.setText(txt)
        self.lbl_stat.setToolTip(
            "百分比 = 当前实测吞吐 / 该波特率理论满速(%s)\n"
            "115200 满速 = 11.5 kB/s；49088 B 的镜像纯传输约 4.3 s"
            % _rate(cap) if cap else "")

    # ---------------- 交互 ----------------
    def on_pause(self, checked):
        self.paused = checked
        self.btn_pause.setText("继续" if checked else "暂停")
        self.btn_pause.setStyleSheet("background:#FFF3E0" if checked else "")
        if checked:
            self.status.showMessage("显示已暂停（串口仍在收，日志仍在写）")

    def on_clear(self):
        self.view.clear()
        self.asm.reset()
        for e in self.engine.eps:
            e.nbytes = 0
            e.nlines = 0
            e.wdrop = 0
            e.werr = 0
        self.engine.t0 = time.monotonic()
        self.skipped = 0
        self._last_bytes.clear()
        self._last_stat_t = time.monotonic()
        self._append_msg("已清屏，计数清零", "dim")

    def on_hex(self, state):
        self.hexmode = bool(state)
        self.asm.reset()
        self.engine.hex_view = self.hexmode
        self._append_msg("视图: %s" % ("HEX" if self.hexmode else "LINE"), "dim")

    def on_send(self):
        if not self.running:
            self._append_msg("还没打开串口，先点「开始」", "warn")
            return
        text = self.ed_send.text()
        if not text:
            return
        target = self.cb_target.currentData()
        if target not in [e.tag for e in self.engine.eps]:
            self._append_msg("目标口未打开: %s" % target, "warn")
            return
        data = text.encode("utf-8", "replace")
        if self.chk_crlf.isChecked():
            data += b"\r\n"
        ok, err = self.engine.send(target, data)
        if ok:
            self._append_lines([("PC>%s" % target[0], data, True, False, False)])
        else:
            self._append_msg("发送失败: %s" % err, "err")
        self.ed_send.selectAll()

    def open_log_dir(self):
        d = log_dir()
        QDesktopServices.openUrl(QUrl.fromLocalFile(d))

    def show_help(self):
        QMessageBox.information(self, "接线说明", (
            "<b>sniff 旁路监听（只接一个 USB-TTL）</b><br>"
            "USB-TTL.RXD ← MCU P1.1(TXD2)<br>"
            "GND — GND<br>"
            "USB-TTL.TXD <b>悬空不要接</b>（会和 MCU 的 TXD 打架）<br><br>"
            "<b>bridge 桥接（两个 USB-TTL）</b><br>"
            "TTL#1.TXD → MCU.P1.0(RXD2)　TTL#1.RXD ← MCU.P1.1(TXD2)<br>"
            "TTL#2.TXD → 模组 RXD　　　　TTL#2.RXD ← 模组 TXD<br>"
            "三处 GND 必须共地<br><br>"
            "<b>判读</b><br>"
            "· <span style='color:#1565C0'><b>MCU&gt;MOD</b></span> 蓝 = MCU 发出去<br>"
            "· <span style='color:#E65100'><b>MOD&gt;MCU</b></span> 橙 = 模组回过来<br>"
            "· <span style='color:#B08968'>&lt;CR&gt; &lt;LF&gt;</span> 是真实字节，不是显示问题<br>"
            "· <span style='color:#C2185B'><b>只有 CR，没有 LF</b></span> = 行结束符缺一半，"
            "模组不会响应这条 AT<br><br>"
            "<b>吞吐（够不够跑 OTA）</b><br>"
            "· 115200 8N1 满速 = 11.5 kB/s，49088 B 的镜像纯传输约 <b>4.3 s</b><br>"
            "· 本工具转发实测上限约 <b>20 MB/s</b>（2000 倍余量），不改字节、不加延时，"
            "桥接不是瓶颈<br>"
            "· 右下角百分比 = 实测吞吐 / 该波特率满速。长时间低于 90% 说明数据没在跑满，"
            "时间花在 MCU 的 IAP 写 flash 或模组回包上，不是桥接慢<br>"
            "· 两侧共用同一个速率：桥接只搬运字节，A/B 设成不同速率会把数据喂快/喂慢"))

    # ---------------- 关闭 ----------------
    def closeEvent(self, ev):
        if self.running:
            r = QMessageBox.question(self, APP_NAME, "串口还开着，确定退出？",
                                     QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if r != QMessageBox.Yes:
                ev.ignore()
                return
            self.engine.stop()
        self.timer.stop()
        self.stat_timer.stop()
        ev.accept()


# ---------------------------------------------------------------- 入口

def _excepthook(exc_type, exc, tb):
    """窗口程序里异常没人接就会静默退出 —— 必须弹出来。"""
    text = "".join(traceback.format_exception(exc_type, exc, tb))
    try:
        with open(os.path.join(app_dir(), "SerialBridge_error.log"),
                  "a", encoding="utf-8") as f:
            f.write(datetime.datetime.now().isoformat() + "\n" + text + "\n")
    except Exception:
        pass
    try:
        QMessageBox.critical(None, APP_NAME,
                             "程序出错：\n\n%s\n\n详情已写入 SerialBridge_error.log" % text[-1500:])
    except Exception:
        pass


def _selftest_forward(size=8192):
    """自检用：一个假串口 + 真 Engine 的读写线程，验证字节一个不差地过去。

    为什么需要它：打包成 onefile 后，线程 / queue / 串口对象都在 exe 内部，
    界面上看不出哪一层坏了。这个探针把"读到→入队→写出"整条路径跑一遍，
    比对字节，并给出吞吐。
    """
    class FakeSerial:
        def __init__(self):
            self.buf = bytearray()
            self.out = bytearray()
            self.timeout = 0.001

        @property
        def in_waiting(self):
            return len(self.buf)

        def read(self, n):
            if not self.buf:
                time.sleep(self.timeout)
                return b""
            take, self.buf = bytes(self.buf[:n]), self.buf[n:]
            return take

        def write(self, d):
            self.out += d
            return len(d)

        def close(self):
            pass

    eng = Engine(queue.Queue())
    eng.stop_evt = threading.Event()
    a = Endpoint("A", 115200, "MCU>MOD")
    b = Endpoint("B", 115200, "MOD>MCU")
    a.ser, b.ser = FakeSerial(), FakeSerial()
    a.peer, b.peer = b, a
    eng.eps = [a, b]

    threading.Thread(target=eng._pump, args=(a,), daemon=True).start()
    threading.Thread(target=eng._write_loop, args=(b,), daemon=True).start()

    data = bytes((i * 37) % 256 for i in range(size))
    a.ser.buf += data
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 5 and len(b.ser.out) < size:
        time.sleep(0.001)
    el = time.perf_counter() - t0
    eng.stop_evt.set()
    time.sleep(0.05)
    return bytes(b.ser.out) == data, len(b.ser.out), el


def selftest():
    """--selftest：无界面自检，结果写文件（--windowed 下没有 stdout）。
    用法：SerialBridge.exe --selftest  → 同目录 self_test.txt"""
    lines = []
    lines.append("frozen      = %s" % bool(getattr(sys, "frozen", False)))
    lines.append("executable  = %s" % sys.executable)
    lines.append("app_dir     = %s" % app_dir())
    lines.append("log_dir     = %s" % log_dir())
    lines.append("pyserial    = %s" % (serial.__version__ if serial else "MISSING"))
    try:
        import PySide6
        lines.append("PySide6     = %s" % PySide6.__version__)
    except Exception as e:
        lines.append("PySide6     = MISSING (%s)" % e)
    lines.append("port_cleaner= %s" % ("ok" if pc else "MISSING"))
    try:
        ports = sorted(list_ports.comports(), key=lambda p: p.device) if list_ports else []
        lines.append("ports       = %s" % (", ".join("%s(%s)" % (p.device, p.description)
                                                     for p in ports) or "none"))
    except Exception as e:
        lines.append("ports       = ERROR %s" % e)
    # 行聚合自检：AT\r\r\nOK\r\n 必须出 3 行，且都不许报缺 LF
    asm = LineAssembler()
    got = asm.feed("T", b"AT\r\r\nOK\r\n")
    lines.append("assembler   = %d lines, missing_lf=%d (expect 3 / 0)"
                 % (len(got), sum(1 for g in got if g[3])))
    got2 = LineAssembler().feed("T", b"AT\r")
    lines.append("cr_only     = %d lines" % len(got2))

    # 转发路径自检：假串口跑一遍 读线程 -> 队列 -> 写线程，验证字节零丢失
    # （打包后最容易坏的就是线程/队列这一层，而它坏了在界面上看不出来）
    try:
        ok, n, el = _selftest_forward()
        lines.append("forward     = %s  %d B  %.1f MB/s"
                     % ("ok" if ok else "BYTE MISMATCH", n,
                        (8192 / 1048576.0) / max(el, 1e-9)))
    except Exception as e:
        lines.append("forward     = FAIL %s" % e)
    try:
        QApplication(sys.argv[:1])
        lines.append("Qt platform = ok")
    except Exception as e:
        lines.append("Qt platform = FAIL %s" % e)
    p = os.path.join(app_dir(), "self_test.txt")
    with open(p, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return 0


def main():
    # --windowed 下 stdout 可能是 None，任何 print 都会炸
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w")

    if "--selftest" in sys.argv:
        return selftest()

    if _MISSING:
        QApplication(sys.argv[:1])
        QMessageBox.critical(None, APP_NAME,
                             "缺少依赖，无法启动：\n\n" + "\n".join(_MISSING))
        return 2

    sys.excepthook = _excepthook
    app = QApplication(sys.argv)
    app.setApplicationName("SerialBridge")
    app.setFont(QFont("Microsoft YaHei UI", 9))
    w = MainWindow()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
