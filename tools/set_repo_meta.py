#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""设置 GitHub 仓库的 About 元信息（description / topics / issues / wiki）。

为什么要有这个脚本：
    网页上点齿轮改描述只是"能改"，但不可复现、也留不下记录。
    仓库元信息其实是项目的一部分（决定别人能不能搜到你），
    所以把它做成一条可重复执行的命令。

认证：
    1) 优先读环境变量 GITHUB_TOKEN
    2) 否则调 `git credential fill` 取 Windows/系统凭据管理器里已缓存的 token
       —— 也就是说，只要 `git push` 能成，这个脚本就能跑，不用手输密码。
    ★ token 不会写进任何文件，也不会被打印。

用法：
    # 看当前状态
    python tools/set_repo_meta.py

    # 改描述 + 标签
    python tools/set_repo_meta.py \
        --desc "一句话说明这个仓库是什么" \
        --topics stc32g,ota,bootloader,8051

    # 只改标签
    python tools/set_repo_meta.py --topics stc32g,ota

    # 指定别的仓库（默认取 git remote origin）
    python tools/set_repo_meta.py --repo owner/name --desc "..."
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

UA = "set-repo-meta/1.0"


# ---------------------------------------------------------------------------
# 认证
# ---------------------------------------------------------------------------
def get_token():
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok

    # `git credential fill` 会从系统凭据管理器取，输出 username/password 两行
    try:
        p = subprocess.run(
            ["git", "credential", "fill"],
            input="protocol=https\nhost=github.com\n\n",
            capture_output=True, text=True, timeout=20,
        )
    except (OSError, subprocess.SubprocessError) as e:
        sys.exit("取凭据失败：%s" % e)

    cred = {}
    for line in p.stdout.strip().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            cred[k.strip()] = v.strip()

    tok = cred.get("password", "")
    if not tok:
        sys.exit(
            "没有可用的 GitHub token。两种办法：\n"
            "  1) export GITHUB_TOKEN=<你的 PAT>\n"
            "  2) 先成功 `git push` 一次，让系统凭据管理器记住凭据"
        )
    return tok


# ---------------------------------------------------------------------------
# 仓库名
# ---------------------------------------------------------------------------
def detect_repo():
    """从 git remote origin 的 URL 里解析 owner/name。"""
    try:
        url = subprocess.run(
            ["git", "remote", "get-url", "origin"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        url = ""

    m = re.search(r"github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?$", url)
    if not m:
        sys.exit("无法从 origin 解析仓库名（当前 origin=%r），请用 --repo owner/name" % url)
    return "%s/%s" % (m.group(1), m.group(2))


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------
class Client(object):
    def __init__(self, token):
        self.hdr = {
            "Authorization": "Bearer " + token,
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": UA,
        }

    def call(self, method, url, body=None):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, headers=self.hdr, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode("utf-8") or "{}")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")
            hint = ""
            if e.code in (401, 403):
                hint = "\n  → token 无效或权限不足：classic PAT 需要 repo 权限"
            elif e.code == 404:
                hint = "\n  → 仓库不存在，或 token 无权访问该仓库"
            sys.exit("HTTP %s %s\n%s%s" % (e.code, method, detail[:400], hint))


def main():
    ap = argparse.ArgumentParser(description="设置 GitHub 仓库 About 元信息")
    ap.add_argument("--repo", help="owner/name，默认从 git remote origin 解析")
    ap.add_argument("--desc", help="仓库描述（留空字符串可清除）")
    ap.add_argument("--topics", help="逗号分隔的标签，如 stc32g,ota,8051")
    ap.add_argument("--no-issues", action="store_true", help="关闭 Issues")
    ap.add_argument("--wiki", action="store_true", help="开启 Wiki")
    ap.add_argument("--dry-run", action="store_true", help="只打印，不提交")
    args = ap.parse_args()

    repo = args.repo or detect_repo()
    base = "https://api.github.com/repos/" + repo
    c = Client(get_token())

    cur = c.call("GET", base)
    print("仓库   :", cur.get("full_name"), "(%s)" % ("私有" if cur.get("private") else "公开"))
    print("原描述 :", cur.get("description"))
    print("原标签 :", ", ".join(sorted(cur.get("topics") or [])) or "(无)")

    patch = {}
    if args.desc is not None:
        patch["description"] = args.desc
    if args.no_issues:
        patch["has_issues"] = False
    if args.wiki:
        patch["has_wiki"] = True

    if patch:
        if args.dry_run:
            print("\n[dry-run] PATCH", patch)
        else:
            c.call("PATCH", base, patch)
            print("\n[PATCH] 已更新:", ", ".join(sorted(patch)))

    if args.topics:
        # GitHub 规则：小写字母/数字/连字符，单个 <=50 字符，最多 20 个
        names = []
        for t in re.split(r"[,\s]+", args.topics.strip()):
            t = t.strip().lower()
            if not t:
                continue
            t = re.sub(r"[^a-z0-9\-]", "-", t)
            if t not in names:
                names.append(t)
        if len(names) > 20:
            print("警告: 标签超过 20 个，只取前 20 个")
            names = names[:20]
        if args.dry_run:
            print("[dry-run] PUT topics", names)
        else:
            r = c.call("PUT", base + "/topics", {"names": names})
            print("[PUT]  已更新标签:", ", ".join(sorted(r.get("names") or [])))

    # 复核
    if not args.dry_run:
        now = c.call("GET", base)
        print("\n== 复核 ==")
        print("描述 :", now.get("description"))
        print("标签 :", ", ".join(sorted(now.get("topics") or [])) or "(无)")
        print("页面 :", now.get("html_url"))


if __name__ == "__main__":
    main()
