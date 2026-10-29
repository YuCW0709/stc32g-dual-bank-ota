#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""=============================================================================
 port_cleaner.py  --  运行前强制释放串口占用（Windows）

 为什么需要
 ----------
 Windows 一个 COM 口同一时刻只能被一个句柄打开。现实里把它锁住的常客是：
   · STC-ISP（烧录时独占，烧完不退出就一直占着）
   · 各种串口助手（sscom / 友善 / UartAssist ...）
   · 上一个没退干净的 serial_bridge.py 自己
 被占用时 pyserial 只回一句 PermissionError(13, '拒绝访问。')，
 现场很难立刻想到"是别的软件占着"，于是误判成硬件坏了。

 本模块做三件事
 --------------
 1. probe()  逐个端口试着独占打开 0.05 s，判断"现在到底哪些口能用"
 2. sweep()  结束已知会独占串口的进程（白名单，可关）
 3. free()   合起来：先探测 → 清占用 → 再探测 → 出报告

 安全性
 ------
 · 只杀白名单里的**串口工具**，绝不碰系统进程；
 · python.exe 只在命令行明确含 serial_bridge 时才杀（避免误伤别的脚本）；
 · dry=True 时只报告不动手。

 单独使用
 --------
   python tools/port_cleaner.py          # 清理并报告
   python tools/port_cleaner.py --dry    # 只看谁占着，不动手
============================================================================="""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

try:
    import serial
    from serial.tools import list_ports
except ImportError:                                    # pragma: no cover
    serial = None
    list_ports = None


# ---------------------------------------------------------------- 进程枚举

TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PROCESS_TERMINATE = 0x0001


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def iter_processes():
    """返回 [(pid, exe_name), ...]。失败返回空表（调用方按"查不到"处理）。"""
    if os.name != "nt":
        return []
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return []
    result = []
    try:
        e = _PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(ctypes.c_void_p(snap), ctypes.byref(e))
        while ok:
            result.append((int(e.th32ProcessID), e.szExeFile or ""))
            ok = k32.Process32NextW(ctypes.c_void_p(snap), ctypes.byref(e))
    finally:
        k32.CloseHandle(ctypes.c_void_p(snap))
    return result


# 已知会独占串口的程序（小写子串匹配，宁可少杀不可错杀）
KNOWN_HOLDERS = [
    ("stc-isp",             "STC-ISP 烧录工具"),
    ("stcisp",              "STC-ISP 烧录工具"),
    ("sscom",               "sscom 串口助手"),
    ("comassistant",        "串口助手"),
    ("serial_port_utility", "友善串口助手"),
    ("uartassist",          "UartAssist"),
    ("accessport",          "AccessPort"),
    ("comdebug",            "CommDebug"),
    ("putty",               "PuTTY"),
    ("securecrt",           "SecureCRT"),
    ("xshell",              "Xshell"),
    ("mobaxterm",           "MobaXterm"),
    ("termite",             "Termite"),
    ("hercules",            "Hercules"),
    ("minicom",             "minicom"),
    ("cutecom",             "CuteCom"),
    ("arduino",             "Arduino IDE"),
    ("platformio",          "PlatformIO"),
    ("esptool",             "esptool"),
    ("avrdude",             "avrdude"),
    ("stm32cubeprog",       "STM32CubeProgrammer"),
    ("jlink",               "J-Link"),
]


def _cmdline_of(pid):
    """取进程命令行（用于判断"这个 python 是不是上一个 serial_bridge"）。
    拿不到就返回空串 —— 那意味着"不杀"，安全优先。"""
    if os.name != "nt":
        return ""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command",
             "(Get-CimInstance Win32_Process -Filter 'ProcessId=%d')"
             ".CommandLine" % pid],
            capture_output=True, timeout=3)
        return (r.stdout or b"").decode("utf-8", "replace").strip()
    except Exception:
        return ""


def find_serial_tools(extra_names=()):
    """找出正在跑的串口工具进程。返回 [(pid, exe, 说明), ...]。"""
    me = os.getpid()
    found = []
    names = list(KNOWN_HOLDERS) + list(extra_names)
    for pid, exe in iter_processes():
        if pid == me or pid <= 4:
            continue
        low = exe.lower()
        hit = None
        for key, desc in names:
            if key in low:
                hit = desc
                break
        if hit is None and low in ("python.exe", "pythonw.exe", "py.exe"):
            # 只杀"跑着本工具"的残留实例，别的 python 一律不动
            cmd = _cmdline_of(pid).lower()
            if "serial_bridge" in cmd:
                hit = "上一次没退出的 serial_bridge"
        if hit:
            found.append((pid, exe, hit))
    return found


def kill(pid):
    """结束进程。返回 True/False。用 taskkill 而不是 TerminateProcess：
    前者能一并处理子进程和权限提示，失败也能给出明确原因。"""
    try:
        r = subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                           capture_output=True, timeout=5)
        return r.returncode == 0
    except Exception:
        return False


# ---------------------------------------------------------------- 端口探测

def list_ports_plain():
    """返回 [COM3, COM4, ...]（按号排序）。"""
    if list_ports is None:
        return []
    def _num(d):
        try:
            return int(d[3:])
        except Exception:
            return 9999
    return sorted([p.device for p in list_ports.comports()
                   if p.device.upper().startswith("COM")], key=_num)


def probe(port):
    """试着独占打开一次，立刻关掉。

    返回 None 表示"能打开，空闲"；否则返回错误描述。
    ★ Windows 上"端口不存在"和"被别人占用"都报 PermissionError(13)，
      不区分就会把写错的 COM99 当成"有软件占着"，然后去杀进程——既危险
      又误导。所以先用系统端口表判存在性。
    ★ exclusive=True 很关键：别人开着的时候我们**必须打不开**，
      否则这个探测就变成"把别人的口抢过来又放掉"，反而干扰现场。
    """
    if serial is None:
        return "pyserial not installed"
    known = list_ports_plain()
    if known and port.upper() not in known:
        return "端口不存在"
    try:
        s = serial.Serial(port=port, baudrate=115200, timeout=0.05,
                          write_timeout=0.05, exclusive=True)
        s.close()
        return None
    except Exception as e:
        msg = str(e)
        if "PermissionError" in msg or "拒绝访问" in msg or "Access is denied" in msg:
            return "被占用 (PermissionError 13)"
        return msg


def is_busy(result):
    """probe() 的结果是不是"被占用"（而不是端口不存在/驱动报错）。"""
    return result is not None and "占用" in result


def who_holds(port):
    """尽力猜测是谁占着。Windows 没有公开 API，这里只给"最可能是谁"。
    返回 [(pid, exe, desc), ...]；空表表示没找到线索。"""
    return find_serial_tools()


# ---------------------------------------------------------------- 主流程

def free(targets=None, kill_tools=True, dry=False, log=None):
    """强制释放串口占用。

    targets : None = 系统里所有 COM 口；或 ['COM7', 'COM10']
    kill_tools : 是否结束白名单里的串口工具
    dry : True = 只报告，不动手

    返回 dict：
      {'idle': [...], 'busy_before': [...], 'busy_after': [...],
       'killed': [(pid, exe, desc)], 'ports': [...]}
    """
    say = log or (lambda s: None)
    ports = targets or list_ports_plain()

    busy_before = []
    missing = []
    for p in ports:
        m = probe(p)
        if m is None:
            continue
        if is_busy(m):
            busy_before.append(p)
        else:
            missing.append((p, m))

    killed = []
    if busy_before and kill_tools:
        tools = find_serial_tools()
        if tools:
            for pid, exe, desc in tools:
                if dry:
                    say("   [dry] 将结束 %-22s pid=%-6d (%s)" % (exe, pid, desc))
                    continue
                if kill(pid):
                    say("   已结束 %-22s pid=%-6d (%s)" % (exe, pid, desc))
                    killed.append((pid, exe, desc))
                else:
                    # ★ 2026-10-28 修：原来这里 3 个 %s/%d 只配了 2 个占位符（desc 没写），
                    #   抛 TypeError ⇒ bridge_gui 整个"启动失败: 启动异常"，
                    #   一次都没真正打开串口。工具自己的 bug 绝不能挡住测量。
                    say("   结束失败 %-20s pid=%-6d (%s，可能需要管理员权限)"
                        % (exe, pid, desc))
            if killed:
                time.sleep(0.35)          # 等系统真正回收句柄

    busy_after = []
    idle = []
    for p in ports:
        m = probe(p)
        if m is None:
            idle.append(p)
        elif is_busy(m):
            busy_after.append(p)
        # 端口不存在/驱动报错的既不算空闲也不算占用，前面 missing 已报过

    return {
        "ports": ports,
        "busy_before": busy_before,
        "busy_after": busy_after,
        "idle": idle,
        "killed": killed,
        "missing": missing,
    }


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(
        description="强制释放被占用的串口（结束 STC-ISP / 串口助手等）")
    ap.add_argument("ports", nargs="*", metavar="COMx",
                    help="只处理这些口；不给 = 全部")
    ap.add_argument("--dry", action="store_true", help="只报告，不结束任何进程")
    ap.add_argument("--no-kill", action="store_true", help="只探测，不结束进程")
    args = ap.parse_args(argv)

    ports = [p.upper() for p in args.ports] or list_ports_plain()
    if not ports:
        print("没有发现串口。")
        return 0

    print("发现 %d 个串口: %s" % (len(ports), " ".join(ports)))
    print("正在探测占用 ...")
    r = free(ports, kill_tools=not args.no_kill, dry=args.dry, log=print)

    print("")
    print("原有占用: %s" % (" ".join(r["busy_before"]) or "无"))
    print("被结束  : %d 个进程" % len(r["killed"]))
    print("仍占用  : %s" % (" ".join(r["busy_after"]) or "无"))
    print("现在可用: %s" % (" ".join(r["idle"]) or "无"))
    if r.get("missing"):
        print("异常端口: " + "; ".join("%s(%s)" % (p, m) for p, m in r["missing"]))
    if r["busy_after"] and not args.dry:
        print("")
        print("还打不开的口，请手动关掉占用它的软件（本项目工具查不到句柄级归属）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
