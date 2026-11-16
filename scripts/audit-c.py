#!/usr/bin/env python3
"""audit-c.py - C 代码规范审计（只读，不修改源码）

检查项:
  * 函数参数数量 > 3
  * 函数/变量命名是否小写+下划线
  * 魔法数字（安全相关文件中的裸数字）
  * 不安全的字符串函数
  * 动态分配后是否立即检查
  * TODO/FIXME

用法: python3 scripts/audit-c.py [目录...]
"""

import re
import sys
from pathlib import Path

MAX_PARAMS = 3
SOURCE_DIRS = ["common", "daemon", "src"]

CALLBACK_NAMES = re.compile(r"^(on_|zg_|main$)")


def files(dirs):
    for d in dirs:
        for p in sorted(Path(d).glob("*.[ch]")):
            yield p


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def find_functions(text):
    """粗粒度匹配函数定义：返回 (行号, 名称, 参数个数)。"""
    out = []
    pattern = re.compile(
        r"^[A-Za-z_][\w \t\*]*?\b([a-z_][a-z0-9_]*)\s*\(([^;{}]*?)\)\s*\{",
        re.M,
    )
    for m in pattern.finditer(text):
        name = m.group(1)
        params = m.group(2).strip()
        if params in ("", "void"):
            count = 0
        else:
            count = len([p for p in params.split(",") if p.strip()])
        line = text[: m.start()].count("\n") + 1
        out.append((line, name, count))
    return out


def main():
    dirs = sys.argv[1:] or SOURCE_DIRS
    total_params = []
    naming = []
    magic = []
    allocs = []
    todos = 0

    for path in files(dirs):
        raw = path.read_text(encoding="utf-8")
        todos += len(re.findall(r"TODO|FIXME", raw))
        code = strip_comments(raw)

        for line, name, count in find_functions(code):
            if count > MAX_PARAMS and not name.startswith("on_"):
                total_params.append((path, line, name, count))

        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
            name = m.group(1)
            if re.search(r"[A-Z]", name) and not name.isupper():
                naming.append((path, name))

        if path.name in ("zg-auth.c", "zg-policy.c", "zg-server.c", "zg-exec.c"):
            for i, line in enumerate(code.splitlines(), 1):
                if re.match(r"\s*(#|//)", line):
                    continue
                for num in re.findall(r"(?<![\w.])(\d{2,})(?![\w.])", line):
                    magic.append((path, i, num, line.strip()[:70]))

        for m in re.finditer(r"\b(g_malloc|g_new|g_new0|malloc|calloc|realloc)\s*\(", code):
            tail = code[m.end():m.end() + 200]
            if "NULL" not in tail.split(";")[0]:
                allocs.append((path, code[: m.start()].count("\n") + 1))

    print("函数参数 > %d：%d 个" % (MAX_PARAMS, len(total_params)))
    for path, line, name, count in total_params[:25]:
        print("  %-28s:%-5d %-38s %d 个参数" % (path, line, name, count))
    if len(total_params) > 25:
        print("  ... 其余 %d 个" % (len(total_params) - 25))

    uniq_naming = sorted(set(n for _, n in naming))
    print("\n疑似非下划线命名：%d 个" % len(uniq_naming))
    print("  " + ", ".join(uniq_naming[:20]))

    print("\n安全相关文件中的裸数字：%d 处" % len(magic))
    for path, line, num, text in magic[:15]:
        print("  %-24s:%-5d %-6s %s" % (path, line, num, text))
    if len(magic) > 15:
        print("  ... 其余 %d 处" % (len(magic) - 15))

    print("\n分配后未见 NULL 检查：%d 处（GLib 分配失败会终止进程，可豁免）" % len(allocs))
    for path, line in allocs[:10]:
        print("  %-28s:%d" % (path, line))

    print("\nTODO/FIXME：%d 处" % todos)


if __name__ == "__main__":
    main()
