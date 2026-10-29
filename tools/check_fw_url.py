#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""直链体检 —— 在给 MCU 填 URL 之前，先确认这条链模组的 AT 栈吃得下。

★ 为什么必须有这个工具（2026-10-27）：
  AT 的 HTTP 栈（合宙手册 §13.5~13.7）比浏览器弱得多，下面每一条都会让
  下载在"模组回 ERROR / +HTTPACTION 拿不到 200"这一步就死掉，而现场看起来
  和"线坏了""固件有 bug"一模一样。先把服务器这一侧排除掉，再谈固件：

    1) **http:// 和 https:// 都行**（2026-10-27 起）：固件已加
       `AT+HTTPSSL=1` + `AT+SSLCFG="seclevel",153,0`（手册 p.146 seclevel 0 =
       No authentication），不需要 CA 证书文件。代价是不防中间人，测试期可接受。
    2) **状态码必须 200**，不能 301/302/307 —— AT+HTTPPARA 不跟跳转。
       网盘/图床给的"直链"十个有八个是跳转，这就是最大的坑。
    3) **Content-Length 必须精确等于本地那份镜像的大小**，不能 chunked。
       下载器按 `+HTTPACTION:0,200,<len>` 定总量，len 拿不到就是 0，
       固件会立刻判 `s_total==0` 失败。
       ★ 长度**不是**固定 49152：镜像正文按 hex 实际内容裁剪（最小验证包
         只有 23104 B）。所以这个值必须跟着本地 bin 走，不能写死 ——
         写死会把"正确的文件"判成错的，比没有判据更坏（已经犯过一次）。
    4) **不许 gzip/br 压缩** —— AT 栈不会解压。
    5) **URL 纯 ASCII、长度 <= 127** —— PARAM_URL_MAX=128（含结尾 0），
       超了会被静默截断成另一条链。
    6) 字节内容必须是那一份镜像（md5 比对）。
    7) **必须支持 Range（206 + Content-Range）** —— v0.6.8 新增，这是能不能
       下载成功的硬前提：模组 HTTP 响应缓冲只有 3356 B（手册 p.196 的
       <start_address>/<byte_size> 上限），整包 GET 23104 B 必回 602。
       固件靠 AT+HTTPPARA="BREAK/BREAKEND" 分片，服务器不回 206 就无解。

用法：
    python tools/check_fw_url.py <url>
    python tools/check_fw_url.py <url> --bin build/fw_min_v0.2.0_ota.bin
    python tools/check_fw_url.py <url> --md5 <32 位十六进制>

  ★ 不给 --bin 时，自动取 build/ 下**修改时间最新**的 .bin 当基准
    （长度 + md5 都从它现算）。这样刚打完包直接验，永远比的是刚出的那份。

退出码：0 = 全部合规；1 = 有 BLOCK（模组一定拉不动）；2 = 只有 WARN
"""
import sys, os, glob, hashlib, urllib.request, urllib.error

URL_MAX = 127                                    # PARAM_URL_MAX(128) - 1
UA = "Mozilla/5.0 (compatible; STC32G-OTA-check)"

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")


def find_local_bin(explicit=None):
    """返回 (path, bytes) 或 (None, None)。

    ★ 长度和 md5 都**从实际文件现算**，绝不写死。
      写死过两次（md5 一次、长度一次），每次的后果都一样：把正确文件判成错的，
      然后人去改本来没错的东西。假判据比没有判据更坏。
    """
    if explicit:
        try:
            return explicit, open(explicit, "rb").read()
        except OSError as e:
            sys.exit("FATAL: 打不开 --bin %s (%s)" % (explicit, e))
    cands = [p for p in glob.glob(os.path.join(BUILD_DIR, "*.bin"))
             if "DEPRECATED" not in os.path.basename(p).upper()]
    if not cands:
        return None, None
    newest = max(cands, key=os.path.getmtime)
    try:
        return newest, open(newest, "rb").read()
    except OSError:
        return None, None

BLOCK, WARN, OK = [], [], []


def add(kind, msg):
    {"B": BLOCK, "W": WARN, "O": OK}[kind].append(msg)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    url = sys.argv[1]
    if "--bin" in sys.argv:
        bin_path, bin_bytes = find_local_bin(sys.argv[sys.argv.index("--bin") + 1])
    else:
        bin_path, bin_bytes = find_local_bin()
    want_len = len(bin_bytes) if bin_bytes is not None else None
    want_md5 = hashlib.md5(bin_bytes).hexdigest() if bin_bytes is not None else None
    if "--md5" in sys.argv:
        want_md5 = sys.argv[sys.argv.index("--md5") + 1].lower()

    print("URL : %s" % url)
    print("len : %d 字符" % len(url))
    if bin_path is not None:
        print("base: %s  (%d B)" % (os.path.normpath(bin_path), want_len))
        print("      md5 %s" % want_md5)
    else:
        print("base: 本地没有 .bin 基准 —— 长度与 md5 判据降级为 WARN（用 --bin 指定）")

    # ---- 1) URL 本身 ----
    #   ★ 2026-10-27 更新：固件已加 HTTPS 支持（hota_ssl_setup：
    #     AT+HTTPSSL=1 + AT+SSLCFG="seclevel",153,0，手册 p.146 seclevel 0 =
    #     No authentication）⇒ **https 不再是 BLOCK**。
    #     这条判据之前会误伤 —— 工具比固件落后，反而把人往 http 死路上赶。
    if url.lower().startswith("https://"):
        add("O", "https://（固件已支持，证书不验证 —— 仅测试期可接受）")
    elif not url.lower().startswith("http://"):
        add("B", "不是 http:// 或 https:// 开头")
    if len(url) > URL_MAX:
        add("B", "长度 %d > %d（PARAM_URL_MAX=128 含结尾 0），会被固件截断"
                 % (len(url), URL_MAX))
    else:
        add("O", "长度 %d <= %d" % (len(url), URL_MAX))
    try:
        url.encode("ascii")
        add("O", "纯 ASCII")
    except UnicodeEncodeError:
        add("B", "含非 ASCII 字符（中文/全角/空格）—— AT 命令行传不了")
    if " " in url:
        add("B", "含空格 —— 会被 AT 解析器切断")

    # ---- 2) HEAD 探元信息（不跟跳转）----
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    # ★★★ 默认**禁用系统代理**：本工具要回答的是"模组能不能拉这条链"，
    #   而模组走 4G 直连，不经过本机任何代理。
    #   现场踩过：隧道明明是通的，但本机 https_proxy=127.0.0.1:4290 对
    #   该域名返回 502，于是工具报 BLOCK —— 一个纯属本地环境的假判据，
    #   会让人去改本来没错的服务器。
    #   要模拟"经过代理"的路径（排查代理白名单）才加 --proxy。
    if "--proxy" in sys.argv:
        opener = urllib.request.build_opener(NoRedirect)
    else:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect)
    req = urllib.request.Request(url, method="HEAD",
                                headers={"User-Agent": UA,
                                         "Accept-Encoding": "identity"})
    code = None
    hdrs = {}
    try:
        with opener.open(req, timeout=15) as r:
            # ★ 键统一转小写：服务器常发小写 content-length，
            #   而 dict(r.headers) 保留原始大小写 ⇒ 查不到就误报"缺 Content-Length"。
            code, hdrs = r.status, {k.lower(): v for k, v in r.headers.items()}
    except urllib.error.HTTPError as e:
        code, hdrs = e.code, {k.lower(): v for k, v in (e.headers or {}).items()}
    except Exception as e:
        add("B", "HEAD 请求失败：%s" % e)
        print_report()
        return 1

    print("\n[HEAD] %s" % code)
    if code in (301, 302, 303, 307, 308):
        add("B", "返回 %d 跳转 -> %s —— AT 栈不跟跳转，必须换成最终地址"
                 % (code, hdrs.get("location", "?")))
    elif code == 405 or code == 501:
        add("W", "服务器不支持 HEAD（%d），下面改用 GET 复核" % code)
    elif code != 200:
        add("B", "状态码 %d，必须 200" % code)
    else:
        add("O", "状态码 200")

    cl = hdrs.get("content-length")
    te = (hdrs.get("transfer-encoding") or "").lower()
    ce = (hdrs.get("content-encoding") or "").lower()
    ct = (hdrs.get("content-type") or "").lower()

    if "chunked" in te:
        add("B", "Transfer-Encoding: chunked —— 没有 Content-Length，"
                 "固件拿不到总长度；换不 chunk 的直链")
    if ce and ce != "identity":
        add("B", "Content-Encoding: %s —— 模组不解压；服务器关掉 gzip/br" % ce)
    if ct and "text/html" in ct:
        add("B", "Content-Type 是 text/html —— 收到的是**网页**不是文件。"
                 "典型原因：① P2P/网盘分享页（file.pizza、蓝奏云等服务器上"
                 "根本没文件）；② 链接是跳转落地页；③ 下载需要 JS/登录。"
                 "要的是**静态文件地址**，不是分享地址。")
    if cl is None:
        add("W", "HEAD 没给 Content-Length，GET 阶段再核")
    elif want_len is None:
        add("W", "HEAD 报 Content-Length=%s，但本地没有基准 bin 可比（用 --bin）" % cl)
    elif int(cl) != want_len:
        add("B", "Content-Length=%s，必须 %d —— 传错文件或服务器改写了长度"
                 % (cl, want_len))
    else:
        add("O", "Content-Length = %d" % want_len)

    # ---- 3) GET 真身（算 md5，看实际长度）----
    req2 = urllib.request.Request(url, headers={"User-Agent": UA,
                                                "Accept-Encoding": "identity"})
    try:
        with opener.open(req2, timeout=30) as r:
            body = r.read()
            gcode = r.status
    except urllib.error.HTTPError as e:
        add("B", "GET 失败：HTTP %d" % e.code)
        print_report()
        return 1
    except Exception as e:
        add("B", "GET 失败：%s" % e)
        print_report()
        return 1

    if gcode != 200:
        add("B", "GET 状态码 %d" % gcode)
    print("[GET ] %s  %d 字节" % (gcode, len(body)))
    if want_len is not None and len(body) != want_len:
        add("B", "实际收到 %d 字节，必须 %d" % (len(body), want_len))
    got = hashlib.md5(body).hexdigest()
    if want_md5 is None:
        add("W", "本地没有基准 bin，md5 未比对（实际收到 %s）" % got)
    elif got != want_md5:
        add("B", "md5=%s 与期望 %s 不符 —— 上传的不是这一份（或传送被改）"
                 % (got, want_md5))
    else:
        add("O", "md5 与期望完全一致")
    if body[:4] != b"OTAI":
        add("B", "首 4 字节不是 'OTAI'（实际 %r）—— 不是 OTA 镜像"
                 % body[:4])
    else:
        add("O", "首 4 字节 = OTAI")

    # ---- 4) Range 支持（v0.6.8 新增，**下载能不能跑通就靠这一条**）----
    #   ★ 为什么必须有（v0.6.5/step60 现场实证）：
    #     `+HTTPACTION: 0,602,0` 里的 602 = **内存不足**；手册 p.196 给出
    #     `AT+HTTPREAD` 的 <start_address>/<byte_size> 上限都是 **3356**，
    #     即模组 HTTP 栈响应缓冲只有 3356 B。一次 GET 23104 B 必然 602，
    #     与 PDP/SSL/线路全无关。唯一解法是 `AT+HTTPPARA="BREAK/BREAKEND"`
    #     分片 ⇒ **服务器必须支持 Range 并回 206**。
    #   服务器若忽略 Range 回 200 全量，模组照样 602 ⇒ 这条链直接判死。
    range_ok = False
    try:
        req3 = urllib.request.Request(
            url, headers={"User-Agent": UA,
                          "Accept-Encoding": "identity",
                          "Range": "bytes=0-1023"})
        with opener.open(req3, timeout=30) as r:
            rcode = r.status
            rcr = r.headers.get("content-range")
            rbody = r.read()
        if rcode != 206:
            add("B", "Range 请求回 %d（不是 206）—— 服务器不支持断点分片 ⇒ "
                     "模组只能整包 GET ⇒ 必回 602（缓冲 3356 B）" % rcode)
        elif len(rbody) != 1024:
            add("B", "Range 回了 %d 字节（不是 1024）—— 分片长度不可控"
                     % len(rbody))
        elif not rcr:
            add("B", "206 但没有 Content-Range 头 —— 无法确认分片边界")
        else:
            tail = rcr.strip().split("/")[-1]
            if want_len is not None and tail.isdigit() and int(tail) != want_len:
                add("B", "Content-Range 总长 %s ≠ 本地 %d" % (tail, want_len))
            else:
                add("O", "支持 Range（206 + Content-Range: %s）" % rcr.strip())
                range_ok = True
    except urllib.error.HTTPError as e:
        if e.code == 416:
            add("B", "Range 请求回 416 —— 分片边界不被接受")
        else:
            add("B", "Range 请求失败 HTTP %d" % e.code)
    except Exception as e:
        add("B", "Range 请求失败：%s" % e)

    print_report()
    if BLOCK:
        return 1
    return 2 if WARN else 0


def print_report():
    print("\n" + "=" * 62)
    for m in OK:
        print("  [OK]    %s" % m)
    for m in WARN:
        print("  [WARN]  %s" % m)
    for m in BLOCK:
        print("  [BLOCK] %s" % m)
    print("=" * 62)
    if BLOCK:
        print("结论：有 %d 项 BLOCK —— 这条链模组一定拉不动，先换/先修服务器。"
              % len(BLOCK))
    elif WARN:
        print("结论：可以试，但有 %d 项 WARN，失败时先回来怀疑它们。" % len(WARN))
    else:
        print("结论：全部合规，可以把这个 URL 填进 OTA_TEST_FW_URL。")


if __name__ == "__main__":
    sys.exit(main())
