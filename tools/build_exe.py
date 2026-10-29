#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""=============================================================================
 build_exe.py  --  把 bridge_gui.py 打包成单文件 SerialBridge.exe

 产物
 ----
   tools/SerialBridge.exe        双击即用，不需要装 Python / pyserial / PySide6
   tools/serialbridge.ico        程序图标（自动生成）

 用法
 ----
   python tools/build_exe.py            # 完整打包
   python tools/build_exe.py --icon-only   # 只重生成图标

 为什么是 onefile
 ----------------
 现场电脑大概率没有 Python，也不该要求现场装。"一个 exe 拷进去就能跑"是
 这个工具唯一的交付标准。代价是启动时要把内部 ~60 MB 解压到临时目录，
 冷启动约 2~4 秒 —— 换来的是不用配环境。

 中间产物全部落在 tools/_build/，最后只把 exe 留在 tools/，工程目录不乱。
============================================================================="""

import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ICON = os.path.join(HERE, "serialbridge.ico")
BUILD = os.path.join(HERE, "_build")
ENTRY = os.path.join(HERE, "bridge_gui.py")
NAME = "SerialBridge"


def make_icon(path=ICON, size=256):
    """画一个图标：深蓝圆角底 + 两条对向箭头（=双向桥接），存成 PNG-in-ICO。

    ★ Windows Vista 以后允许 ICO 里直接塞 PNG 数据，比 BMP+掩码简单得多，
      也自带 alpha。手写 22 字节的 ICO 头就够了，不用 Pillow。
    """
    from PySide6.QtCore import QPointF, QRectF, Qt
    from PySide6.QtGui import (QColor, QImage, QPainter, QPen, QPolygonF)

    img = QImage(size, size, QImage.Format_ARGB32)
    img.fill(Qt.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing, True)

    # 底：圆角方块
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("#1565C0"))
    p.drawRoundedRect(QRectF(8, 8, size - 16, size - 16), size * 0.2, size * 0.2)

    # 两条对向箭头：上→右，下→左
    white = QColor("#FFFFFF")
    p.setBrush(white)
    p.setPen(QPen(white, size * 0.062, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))

    y1, y2 = size * 0.385, size * 0.615
    x_l, x_r = size * 0.235, size * 0.765
    head = size * 0.105

    p.drawLine(QPointF(x_l, y1), QPointF(x_r - head * 0.4, y1))
    p.drawPolygon(QPolygonF([QPointF(x_r - head, y1 - head * 0.8),
                             QPointF(x_r, y1),
                             QPointF(x_r - head, y1 + head * 0.8)]))

    p.drawLine(QPointF(x_r, y2), QPointF(x_l + head * 0.4, y2))
    p.drawPolygon(QPolygonF([QPointF(x_l + head, y2 - head * 0.8),
                             QPointF(x_l, y2),
                             QPointF(x_l + head, y2 + head * 0.8)]))
    p.end()

    tmp_png = os.path.join(BUILD, "_icon.png")
    os.makedirs(BUILD, exist_ok=True)
    img.save(tmp_png, "PNG")
    png = open(tmp_png, "rb").read()
    os.remove(tmp_png)

    # ICONDIR(6) + ICONDIRENTRY(16) + PNG 数据
    dir_hdr = struct.pack("<HHH", 0, 1, 1)
    w = 0 if size >= 256 else size          # 256 在 ICO 里记作 0
    entry = struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(png), 6 + 16)
    with open(path, "wb") as f:
        f.write(dir_hdr + entry + png)
    return path


def run(cmd):
    print("+ " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    return subprocess.call(cmd)


def build():
    if not os.path.isfile(ENTRY):
        print("[!] 找不到 %s" % ENTRY)
        return 2

    # 0) 语法与自检先行：打包 3 分钟才发现写错一个字很亏
    if run([sys.executable, ENTRY, "--selftest"]) != 0:
        print("[!] 自检没通过，先修好再打包")
        return 3
    st = os.path.join(HERE, "self_test.txt")
    if os.path.isfile(st):
        print("---- self_test.txt ----")
        print(open(st, encoding="utf-8").read().strip())
        os.remove(st)
        print("-----------------------")

    # 1) 图标
    try:
        make_icon()
        print("[icon] %s (%d B)" % (ICON, os.path.getsize(ICON)))
    except Exception as e:
        print("[icon] 生成失败，改用默认图标: %s" % e)

    # 2) 清掉上一次的中间产物（不动 dist 外的任何东西）
    for d in (os.path.join(BUILD, "work"), os.path.join(BUILD, "dist")):
        shutil.rmtree(d, ignore_errors=True)

    # 3) PyInstaller
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm", "--clean",
        "--onefile",
        "--windowed",                       # 不要控制台窗口
        "--name", NAME,
        "--paths", HERE,                    # 让 serial_bridge / port_cleaner 可被 import
        "--distpath", os.path.join(BUILD, "dist"),
        "--workpath", os.path.join(BUILD, "work"),
        "--specpath", BUILD,
        "--exclude-module", "tkinter",
        "--exclude-module", "PySide6.QtWebEngineCore",
        "--exclude-module", "PySide6.QtQml",
        "--exclude-module", "PySide6.QtQuick",
        ENTRY,
    ]
    if os.path.isfile(ICON):
        cmd[-1:-1] = ["--icon", ICON]

    if run(cmd) != 0:
        print("[!] PyInstaller 失败")
        return 4

    src = os.path.join(BUILD, "dist", NAME + ".exe")
    if not os.path.isfile(src):
        print("[!] 没找到产物 %s" % src)
        return 5
    dst = os.path.join(HERE, NAME + ".exe")
    shutil.copy2(src, dst)
    print("\n[ok] %s  (%.1f MB)" % (dst, os.path.getsize(dst) / 1048576.0))
    return 0


def verify():
    """跑一次 exe 的无界面自检，确认打包没漏东西。"""
    exe = os.path.join(HERE, NAME + ".exe")
    if not os.path.isfile(exe):
        return 1
    print("\n---- 验证 exe ----")
    subprocess.call([exe, "--selftest"])
    st = os.path.join(HERE, "self_test.txt")
    if os.path.isfile(st):
        print(open(st, encoding="utf-8").read().strip())
        os.remove(st)
    return 0


if __name__ == "__main__":
    if "--icon-only" in sys.argv:
        print(make_icon())
        sys.exit(0)
    rc = build()
    if rc == 0:
        verify()
    sys.exit(rc)
