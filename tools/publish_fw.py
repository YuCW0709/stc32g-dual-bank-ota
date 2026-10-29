#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
publish_fw.py —— 一条命令搞定「上传目标包 -> 体检直链 -> 改宏 -> 重编 -> 存档」

为什么需要它
------------
`OTA_TEST_FW_URL` 是编译期宏，而免费托管每次上传都会换文件名。
所以「链接过期」这件事无法在固件里绕开，只能在构建侧自动化：
    上传 -> 拿新 URL -> 写进 app_config.h -> 重编 APP -> 出新的合并 hex

用法
----
    python tools/publish_fw.py                 # 上传 -> 体检 -> 改宏 -> 重编
    python tools/publish_fw.py --upload-only   # 只上传，打印 URL，不动代码
    python tools/publish_fw.py --bin build/xxx.bin

退出码
------
    0  成功（hex 已生成）
    1  上传或体检失败（不写代码，不留半成品）
    2  编译失败

设计纪律
--------
* 只有**体检全绿**才改 app_config.h —— 否则会把一个拉不动的地址烧进固件，
  上板后表现为「HTTP 失败」，与固件 bug 无法区分（已经踩过一次）。
* 改之前把原文件备份成 app_config.h.bak，编译失败自动回滚。
* 版本 patch 号自动 +1，并同步 APP_VER_STRING，保证每次上板的日志能区分。
"""
import os
import re
import sys
import shutil
import hashlib
import subprocess
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CFG = os.path.join(ROOT, "APP", "app_config.h")
CFG_BAK = CFG + ".bak"
BUILD_SH = os.path.join(HERE, "build_local.sh")
CHECK = os.path.join(HERE, "check_fw_url.py")

DEFAULT_BIN = os.path.join(ROOT, "build", "fw_min_v0.2.0_ota.bin")
UGUU = "https://uguu.se/upload"
KAPPA = "https://kappa.lol/api/upload"

# 上传/体检要绕开本机代理：模组走 4G 直连，走代理会看到假 5xx（踩过一次）
_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def die(msg, code=1):
    print("!! " + msg)
    sys.exit(code)


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(65536), b""):
            h.update(c)
    return h.hexdigest()


def _multipart(path, field):
    """手工拼 multipart，不引入 requests 依赖。返回 (body, boundary)。"""
    name = os.path.basename(path)
    data = open(path, "rb").read()
    boundary = "----stc32gota7f3a1b2c"
    head = (
        "--%s\r\n"
        'Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
        % (boundary, field, name)
    ).encode("ascii")
    tail = ("\r\n--%s--\r\n" % boundary).encode("ascii")
    return head + data + tail, boundary


def _post(url, body, boundary):
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type",
                   "multipart/form-data; boundary=%s" % boundary)
    req.add_header("User-Agent", "Mozilla/5.0 (compatible; STC32G-OTA-publish)")
    with _NO_PROXY.open(req, timeout=120) as r:
        return r.read().decode("utf-8", "replace")


def upload_kappa(path):
    """kappa.lol —— **首选**：短链（22 字符）、长期保存、支持 Range。

    ★ 2026-10-28 换掉 uguu 的原因：uguu 只保留 3 小时，每轮上板前都要重传重编，
      "直链过期"已经变成固定损耗。kappa.lol 是 pomf 系（/api/upload），
      返回 JSON 里带 checksum，正好可以和本地 md5 对一遍。
    """
    body, bnd = _multipart(path, "file")
    raw = _post(KAPPA, body, bnd)
    m = re.search(r'"link"\s*:\s*"([^"]+)"', raw)
    if not m:
        raise RuntimeError("kappa 返回里找不到 link：\n" + raw[:400])
    ck = re.search(r'"checksum"\s*:\s*"([0-9a-f]+)"', raw)
    if ck and ck.group(1) != md5_of(path):
        raise RuntimeError("kappa 回传 checksum=%s 与本地 md5=%s 不符"
                           % (ck.group(1), md5_of(path)))
    return m.group(1).replace("\\/", "/")


def upload_uguu(path):
    """uguu —— 兜底：只保留 3 小时，但一直可用，且 nginx 支持 Range。"""
    body, bnd = _multipart(path, "files[]")
    raw = _post(UGUU, body, bnd)
    m = re.search(r'"url"\s*:\s*"([^"]+)"', raw)
    if not m:
        raise RuntimeError("uguu 返回里找不到 url：\n" + raw[:400])
    return m.group(1).replace("\\/", "/")


def upload(path):
    """依次尝试各托管，返回 (url, 提供方名字)。第一个成功就用它。"""
    for name, fn in (("kappa.lol", upload_kappa), ("uguu(3h)", upload_uguu)):
        print("\n---- 上传到 %s ----" % name)
        try:
            return fn(path), name
        except Exception as e:
            print("!! %s 失败：%s" % (name, e))
    die("所有托管都上传失败 —— 检查网络/代理，或在浏览器里手动传一份")


def verify(url):
    """调用同一个体检工具，保证判据只有一份实现。"""
    print("\n---- 体检直链 ----")
    p = subprocess.run([sys.executable, CHECK, url],
                       cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    print(out.rstrip())
    return p.returncode == 0


def patch_cfg(url, bump):
    src = open(CFG, encoding="utf-8").read()
    shutil.copy2(CFG, CFG_BAK)

    # 1) 换 URL
    new, n = re.subn(r'(#define\s+OTA_TEST_FW_URL\s+)"[^"]*"',
                     lambda m: m.group(1) + '"' + url + '"', src)
    if n != 1:
        die("在 app_config.h 里找不到唯一的 OTA_TEST_FW_URL（n=%d），不动代码" % n)

    # 2) 版本 patch +1（让每次上板的日志可区分）
    if bump:
        mm = re.search(r'#define\s+APP_VER_PATCH\s+(\d+)', new)
        if not mm:
            die("找不到 APP_VER_PATCH")
        np_ = int(mm.group(1)) + 1
        new = re.sub(r'(#define\s+APP_VER_PATCH\s+)\d+',
                     lambda m: m.group(1) + str(np_), new)

        ma = re.search(r'#define\s+APP_VER_MAJOR\s+(\d+)', new)
        mi = re.search(r'#define\s+APP_VER_MINOR\s+(\d+)', new)
        if not (ma and mi):
            die("找不到 APP_VER_MAJOR/MINOR")
        s = "APP v%d.%d.%d" % (int(ma.group(1)), int(mi.group(1)), np_)
        new = re.sub(r'(#define\s+APP_VER_STRING\s+)"[^"]*"',
                     lambda m: m.group(1) + '"' + s + '"', new)
        print("版本 -> %s" % s)

    open(CFG, "w", encoding="utf-8").write(new)
    return os.path.exists(CFG_BAK)


def main():
    args = sys.argv[1:]
    # ★★ 必须先拦 --help：本脚本没有任何"只打印帮助"的分支，
    #    2026-10-28 实测 `python tools/publish_fw.py --help` 会上传+改宏+重编，
    #    把版本号推了两格。凡"看一眼用法"都不该产生副作用。
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0

    upload_only = "--upload-only" in args
    bin_path = DEFAULT_BIN
    if "--bin" in args:
        bin_path = os.path.abspath(args[args.index("--bin") + 1])

    if not os.path.isfile(bin_path):
        die("找不到要上传的 bin：%s" % bin_path)

    size = os.path.getsize(bin_path)
    print("bin  : %s" % bin_path)
    print("size : %d B" % size)
    print("md5  : %s" % md5_of(bin_path))

    url, provider = upload(bin_path)
    print("URL  : %s  (%d 字符, %s)" % (url, len(url), provider))
    if len(url) > 127:
        die("URL 超过 PARAM_URL_MAX-1=127，模组会截断；换更短的文件名再试")

    if not verify(url):
        die("体检未全绿 —— 不写进固件（否则上板只会看到一个查不出的 HTTP 失败）")
    print("\n体检全绿。")

    if upload_only:
        print("\n--upload-only：代码未改动。把这个 URL 发我，或自己跑一次本脚本。")
        return 0

    okay = patch_cfg(url, bump=True)
    print("\n---- 重编 ----")
    p = subprocess.run(["bash", BUILD_SH], cwd=ROOT,
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    log = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0 or " 0 ERROR(S)" not in log:
        if okay:
            shutil.copy2(CFG_BAK, CFG)
            print("!! 编译失败，已把 app_config.h 回滚（URL 是活的，可重试）")
        print(log[-2500:])
        return 2

    # 尺寸与守门摘要
    for ln in log.splitlines():
        if ("WARNING(S)" in ln or "PASS" in ln or "布局合规" in ln
                or "不合规" in ln):
            print("  " + ln.strip())

    m = re.search(r'#define\s+APP_VER_(?:MAJOR|MINOR|PATCH|BUILD)\s+(\d+)',
                  open(CFG, encoding="utf-8").read())
    ver = "?"
    ms = re.search(r'#define\s+APP_VER_STRING\s+"APP v([^"]+)"',
                   open(CFG, encoding="utf-8").read())
    if ms:
        ver = ms.group(1)

    out_hex = os.path.join(ROOT, "build",
                           "step_dl_v%s_BL_APP.hex" % ver.replace(".", "_"))
    shutil.copy2(os.path.join(ROOT, "tools", "_out_combined.hex"), out_hex)

    # 确认 URL 真的编进去了（防"改了宏但没重编"）
    sys.path.insert(0, HERE)
    from make_img import read_hex
    mem = read_hex(os.path.join(ROOT, "tools", "_out_combined.hex"))
    blob = bytes(mem[i] for i in sorted(mem))
    if url.encode("ascii") not in blob:
        die("hex 里找不到 URL 字符串 —— 编进去的不是这一份，别烧")

    print("\n================ 完成 ================")
    print("目标包 : %s  (%d B, md5 %s)" % (os.path.basename(bin_path), size,
                                            md5_of(bin_path)))
    print("直链   : %s" % url)
    print("烧录   : %s" % out_hex)
    print("md5    : %s" % md5_of(out_hex))
    if provider.startswith("uguu"):
        print("\n★ 这次用的是 uguu（只有 3 小时）。过期后重跑本脚本即可。")
    else:
        print("\n★ 直链长期有效（%s）。镜像内容不变就不用再传；"
              "只有换了 bin 才需要重跑。" % provider)
    return 0


if __name__ == "__main__":
    sys.exit(main())
