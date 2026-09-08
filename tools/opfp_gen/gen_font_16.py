#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_font_16.py — 从 SD 字库 kp_font_lib.bin 提取 16×16/8×16 点阵生成 ui_font_16.h（V2.2.3）

与 gen_font_24.py（等线 TTF 渲染 24×24）互补：本工具不渲染，直接从工程
SD 字库 modules/app/font_config/kp_font_lib.bin 抄原点阵（V2.2.0 之前固件
实际加载的就是这份 16×16 观感）：
  - 汉字 16×16：偏移 0x3000 起 GB2312 94×94 网格（idx=(hi-0xA1)*94+(lo-0xA1)），
    每字 32B，列页格式（页=8 行，每页 16 列字节，bit0=上）——bin 内即此格式，原样拷贝
  - ASCII 8×16：偏移 0x3BC0 起（kp_font_lib.h FONTS_ASCII_8X16_ADDR）0x20~0x7E
    顺序，每字 16B，同样页格式原样拷贝
字符集与 ui_font_24.h 完全一致（app_ui.c 全部用字 + →），宏名/数组名/查找 API
同名（两头文件不可同时 include）——app_ui.c 经 UI_FONT_SEL 宏二选一，
改宏重编译即可 A/B 比对显示效果。

输出：
  app/ui_font_16.h                    —— C 数组 + 码表 + 查找 API（git 入库）
  tools/opfp_gen/_font16_preview.png  —— 预览图（生成后人工确认效果）

用法：python tools/opfp_gen/gen_font_16.py
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))           # offline_prog/ 根
BIN = os.path.join(REPO, "modules", "app", "font_config", "kp_font_lib.bin")
OUT_H = os.path.join(REPO, "app", "ui_font_16.h")
PREVIEW = os.path.join(HERE, "_font16_preview.png")

# UI 全部用字（app_ui.c 的 GBK 串去重）——与 gen_font_24.py 的 CHS_CHARS 保持
# 一致（增删 UI 文案后两处同步）
CHS_CHARS = (
    "中件保关制功回固地址失始序开录成护择按数文无校次滚烧片码确程芯认败过返选通限验"
    "择"  # 择 重复占位去重
)
CHS_CHARS = "".join(sorted(set(CHS_CHARS)))
ARROW_GBK = 0xA1FA                                      # → GB2312 符号区

ASCII_FIRST, ASCII_LAST = 0x20, 0x7E                    # 95 个可打印字符

CHS_W, CHS_H = 16, 16
ASC_W, ASC_H = 8, 16
CHS_BASE = 0x3000                                       # kp_font_lib.h FONTS_CHS_16X16_ADDR
ASC_BASE = 0x3BC0                                       # kp_font_lib.h FONTS_ASCII_8X16_ADDR


def gbk_of(ch):
    return struct.unpack(">H", ch.encode("gbk"))[0]     # GBK 双字节 → u16 码位


def chs_glyph(data, code):
    """按 GB2312 网格取 32B 点阵（bin 内即列页格式，原样返回）。"""
    idx = ((code >> 8) - 0xA1) * 94 + (code & 0xFF) - 0xA1
    off = CHS_BASE + idx * (CHS_W * CHS_H // 8)
    if off + CHS_W * CHS_H // 8 > len(data):
        raise SystemExit("字库偏移越界: 0x%04X -> 0x%X" % (code, off))
    return data[off:off + CHS_W * CHS_H // 8]


def asc_glyph(data, c):
    off = ASC_BASE + (c - ASCII_FIRST) * (ASC_W * ASC_H // 8)
    if off + ASC_W * ASC_H // 8 > len(data):
        raise SystemExit("ASCII 偏移越界: 0x%02X -> 0x%X" % (c, off))
    return data[off:off + ASC_W * ASC_H // 8]


def main():
    with open(BIN, "rb") as f:
        data = f.read()

    # ---- 汉字（含 → 符号，统一 16×16 表）----
    chs_items = [(gbk_of(ch), chs_glyph(data, gbk_of(ch))) for ch in CHS_CHARS]
    chs_items.append((ARROW_GBK, chs_glyph(data, ARROW_GBK)))
    chs_items.sort(key=lambda t: t[0])

    # ---- ASCII 8×16 ----
    asc_items = [(c, asc_glyph(data, c)) for c in range(ASCII_FIRST, ASCII_LAST + 1)]

    # 空字检查（字库缺字 → 屏上空白，提前暴露；空格本就无墨）
    empty = ["%04X" % c for c, bm in chs_items if not any(bm)]
    empty += ["'%s'" % chr(c) for c, bm in asc_items
              if not any(bm) and chr(c) != " "]
    if empty:
        print("警告：以下字符在字库中为空白点阵：%s" % " ".join(empty))

    # ---- 输出 .h（符号名与 ui_font_24.h 完全同名，不可同时 include）----
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("/**\n")
        f.write(" * @file ui_font_16.h\n")
        f.write(" * @brief 内置点阵字库 16 号（A/B 比对用；观感=原 SD 字库 16×16）\n")
        f.write(" *\n")
        f.write(" * 由 tools/opfp_gen/gen_font_16.py 从 modules/app/font_config/\n")
        f.write(" * kp_font_lib.bin 直接提取（V2.2.0 之前固件加载的原点阵）——勿手改。\n")
        f.write(" * 符号名/查找 API 与 ui_font_24.h 完全同名，二者不可同时 include；\n")
        f.write(" * app_ui.c 经 UI_FONT_SEL 宏二选一（改宏重编译切换比对）。\n")
        f.write(" *\n")
        f.write(" * 汉字 %d 字 16×16（每字 32B，2 页×16 列字节，GB2312 码索引）\n" % len(chs_items))
        f.write(" * ASCII %d 字 8×16（每字 16B，0x20~0x7E 顺序索引）\n" % len(asc_items))
        f.write(" */\n")
        f.write("#ifndef __UI_FONT_16_H\n#define __UI_FONT_16_H\n\n#include <stdint.h>\n\n")

        f.write("#define UIF_CHS_N      %d\n" % len(chs_items))
        f.write("#define UIF_CHS_W      16\n")
        f.write("#define UIF_CHS_H      16\n")
        f.write("#define UIF_CHS_BYTES  32\n")
        f.write("#define UIF_ASC_FIRST  0x20\n")
        f.write("#define UIF_ASC_N      %d\n" % len(asc_items))
        f.write("#define UIF_ASC_W      8\n")
        f.write("#define UIF_ASC_H      16\n")
        f.write("#define UIF_ASC_BYTES  16\n\n")

        # 汉字码表 + 点阵
        f.write("/* 汉字 GB2312 码表（升序；查找用二分或线性）*/\n")
        f.write("static const uint16_t uif_chs_code[UIF_CHS_N] = {\n")
        for i in range(0, len(chs_items), 12):
            f.write("    " + ",".join("0x%04X" % c for c, _ in chs_items[i:i + 12]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint8_t uif_chs_bits[UIF_CHS_N][UIF_CHS_BYTES] = {\n")
        for code, bm in chs_items:
            f.write("    { /* 0x%04X */\n" % code)
            for r in range(0, len(bm), CHS_W):
                f.write("        " + ",".join("0x%02X" % b for b in bm[r:r + CHS_W]) + ",\n")
            f.write("    },\n")
        f.write("};\n\n")

        # ASCII 点阵
        f.write("static const uint8_t uif_asc_bits[UIF_ASC_N][UIF_ASC_BYTES] = {\n")
        for c, bm in asc_items:
            f.write("    { /* '%s' 0x%02X */\n" % (chr(c) if c != 0x5C else "\\\\", c))
            for r in range(0, len(bm), ASC_W):
                f.write("        " + ",".join("0x%02X" % b for b in bm[r:r + ASC_W]) + ",\n")
            f.write("    },\n")
        f.write("};\n\n")

        # ---- 查找 API（font_mgr 的 read 回调适配层直接调这两个）----
        f.write("""/* 取汉字（GBK 双字节码，高字节在 s[0]）点阵指针；未收录返回 NULL */
static const uint8_t *uif_chs(uint8_t hi, uint8_t lo)
{
    uint16_t code = (uint16_t)((hi << 8) | lo);
    for (uint16_t i = 0; i < UIF_CHS_N; i++)
        if (uif_chs_code[i] == code)
            return uif_chs_bits[i];
    return 0;
}

/* 取 ASCII（0x20~0x7E）点阵指针；范围外返回 NULL */
static const uint8_t *uif_asc(uint8_t c)
{
    if (c < UIF_ASC_FIRST || c >= UIF_ASC_FIRST + UIF_ASC_N)
        return 0;
    return uif_asc_bits[c - UIF_ASC_FIRST];
}

#endif /* __UI_FONT_16_H */
""")
    size = os.path.getsize(OUT_H)
    print("生成 %s（%.1f KB 源码）" % (OUT_H, size / 1024))
    print("汉字 %d / ASCII %d" % (len(chs_items), len(asc_items)))

    # ---- 预览图（页格式点阵 → 1-bit 图，3x 放大便于查看）----
    W = 16 * 21
    H = CHS_H + ASC_H + 8
    from PIL import Image, ImageDraw
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)

    def blit(bm, w, h, x0, y0):
        pages = (h + 7) // 8
        for page in range(pages):
            for col in range(w):
                bits = bm[page * w + col]
                for bit in range(8):
                    y = page * 8 + bit
                    if y >= h:
                        break
                    if bits & (1 << bit):
                        px_x, py = x0 + col, y0 + y
                        if px_x < W:
                            d.point((px_x, py), 0)

    x = 4
    for code, bm in chs_items:
        blit(bm, CHS_W, CHS_H, x, 0)
        x += CHS_W
    x = 4
    for c, bm in asc_items:
        blit(bm, ASC_W, ASC_H, x, CHS_H + 8)
        x += ASC_W
        if x > W - ASC_W:
            break
    img = img.resize((W * 3, H * 3), Image.NEAREST)
    img.save(PREVIEW)
    print("预览 %s" % PREVIEW)


if __name__ == "__main__":
    main()
