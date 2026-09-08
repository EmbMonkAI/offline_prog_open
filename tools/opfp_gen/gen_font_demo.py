#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_font_demo.py — 合并 16/20/24 三字库为 ui_font_demo.h（V2.2.4）

ui_font_16/20/24.h 符号完全同名（uif_/UIF_），不能同时 include。本工具把
三份头文件整体改名（16 号→f16_/F16_，20 号→f20_/F20_，24 号→f24_/F24_）
后合并成一个头，供 app_ui.c 的「字体比对」演示页同屏渲染三档字体。

代价：与主 UI 已选字库（UI_FONT_SEL 选中的那份）数据重复一份——演示页是
调试用途，可接受。

输出：app/ui_font_demo.h
用法：python tools/opfp_gen/gen_font_demo.py（改任一字库后需重跑）
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(REPO, "app", "ui_font_demo.h")


def load_renamed(path, prefix):
    """读一份字库头，去 include guard，符号加前缀（UIF_→F16_ 等，uif_→f16_ 等）。"""
    src = open(path, encoding="utf-8").read()
    src = re.sub(r"#ifndef\s+__UI_FONT_\d+_H\s*\n#define\s+__UI_FONT_\d+_H\s*\n", "", src)
    src = re.sub(r"#endif\s*/\*\s*__UI_FONT_\d+_H\s*\*/\s*$", "", src)
    src = re.sub(r"\bUIF_", prefix.upper() + "_", src)      # 宏 UIF_CHS_W → F16_CHS_W
    src = re.sub(r"\buif_", prefix + "_", src)              # 函数/数组 uif_chs → f16_chs
    return src.strip()


def main():
    parts = []
    for size, note in ((16, "原 SD 字库提取"), (20, "等线渲染折中档"), (24, "等线渲染")):
        p = load_renamed(os.path.join(REPO, "app", "ui_font_%d.h" % size), "f%d" % size)
        parts.append("/* ==================== %d 号（%s）==================== */\n%s"
                     % (size, note, p))

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("/**\n")
        f.write(" * @file ui_font_demo.h\n")
        f.write(" * @brief 16/20/24 三字库合并（字体比对演示页用，V2.2.4）\n")
        f.write(" *\n")
        f.write(" * 由 tools/opfp_gen/gen_font_demo.py 生成——勿手改；改任一字库\n")
        f.write(" * 后需重跑本工具。符号前缀：f16 与 F16、f20 与 F20、f24 与 F24\n")
        f.write(" *（与主 UI 的 uif UIF 前缀不冲突，可同时 include）。\n")
        f.write(" */\n")
        f.write("#ifndef __UI_FONT_DEMO_H\n#define __UI_FONT_DEMO_H\n\n#include <stdint.h>\n\n")
        f.write("\n\n".join(parts))
        f.write("\n#endif /* __UI_FONT_DEMO_H */\n")

    print("生成 %s（%.1f KB）" % (OUT, os.path.getsize(OUT) / 1024))


if __name__ == "__main__":
    main()
