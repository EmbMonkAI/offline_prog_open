#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_font_sd.py — 从 SD 字库点阵生成 20/24 号字库（V2.2.5）

用户定稿：20/24 号观感要「和 16 号一样的字体」且保持 16 号的清晰感——
统一从 16×16/8×16 原点阵做确定性整数映射放大（非重采样）：
  - 24 号 = 16 → ×1.5（2:3 映射）：目标像素 = 源(x*2/3, y*2/3)
  - 20 号 = 16 → ×1.25（4:5 映射）
  映射法笔画粗细均匀、绝不断笔，墨密度与 16 号源完全一致——LANCZOS+二值化
  在 1px 细笔画上会时断时续（实测），12×12→2× 的笔画偏粗（36% vs 25%）均弃用。

字符集/码表/输出格式与 ui_font_16.h 完全一致（宏名/符号名同名，互斥 include）。

输出：app/ui_font_20.h / app/ui_font_24.h（覆盖等线渲染版；git 里有历史）
      tools/opfp_gen/_font_sd_preview.png（16/20/24 三档预览）
用法：python tools/opfp_gen/gen_font_sd.py
"""

import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BIN = os.path.join(REPO, "modules", "app", "font_config", "kp_font_lib.bin")

# 字符集与 gen_font_24/16 一致（增删 UI 文案后同步四处）
CHS_CHARS = (
    "中件保关制功回固地址失始序开录成护择按数文无校次滚烧片码确程芯认败过返选通限验"
    "择"
)
CHS_CHARS = "".join(sorted(set(CHS_CHARS)))
EXTRA_GBK = {"→": 0xA1FA}
ASCII_FIRST, ASCII_LAST = 0x20, 0x7E

# SD 库区基址（kp_font_lib.h）
CHS16_BASE = 0x3000      # 汉字 16×16 网格（32B/字）
ASC8_BASE = 0x3BC0       # ASCII 8×16（16B/字）


def gbk_of(ch):
    return struct.unpack(">H", ch.encode("gbk"))[0]


def gb_index(code):
    return ((code >> 8) - 0xA1) * 94 + (code & 0xFF) - 0xA1


def unpack_page(data, off, w, h):
    """列页格式点阵 → 1-bit PIL 图（bit0=上，与 svc_display_tft_area_pixel 一致）"""
    img = Image.new("1", (w, h), 1)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if data[off + (y // 8) * w + x] & (1 << (y % 8)):
                px[x, y] = 0
    return img


def pack_page(img):
    """1-bit PIL 图 → 列页格式 bytes"""
    w, h = img.size
    px = img.load()
    pages = (h + 7) // 8
    bm = bytearray(pages * w)
    for y in range(h):
        for x in range(w):
            if px[x, y] == 0:                      # 0=墨
                bm[(y // 8) * w + x] |= 1 << (y % 8)
    return bytes(bm)


def map_scale(img, k_num, k_den):
    """确定性整数映射放大 k_num/k_den 倍：目标(x,y)=源(x*k_den//k_num,
    y*k_den//k_num)。每个源像素按坐标稳定映到 1..k 目标格——笔画粗细
    均匀、绝不断笔（比 LANCZOS+二值化稳：重采样在 1px 细笔画上时断时续），
    墨密度与源一致（16 号的清晰感来源）。"""
    src = img.load()
    w, h = img.size
    w2, h2 = w * k_num // k_den, h * k_num // k_den
    out = Image.new("1", (w2, h2), 1)
    op = out.load()
    for Y in range(h2):
        for X in range(w2):
            if src[min(X * k_den // k_num, w - 1), min(Y * k_den // k_num, h - 1)] == 0:
                op[X, Y] = 0
    return out


def gen_sizes(data):
    """返回 {size: (chs_w, chs_h, asc_w, asc_h, chs_items, asc_items)}。
    20/24 号统一从 16×16/8×16 原点阵确定性映射放大（同字体同清晰度）：
      24 号 = 16 → ×1.5（2:3 映射，每源像素映 2~3 目标格）
      20 号 = 16 → ×1.25（4:5 映射，每源像素映 1~2 目标格）"""
    out = {}
    for size, k in ((24, (3, 2)), (20, (5, 4))):
        chs_items = []
        for ch in CHS_CHARS:
            code = gbk_of(ch)
            img = unpack_page(data, CHS16_BASE + gb_index(code) * 32, 16, 16)
            chs_items.append((code, pack_page(map_scale(img, *k))))
        for ch, code in EXTRA_GBK.items():
            img = unpack_page(data, CHS16_BASE + gb_index(code) * 32, 16, 16)
            chs_items.append((code, pack_page(map_scale(img, *k))))
        chs_items.sort(key=lambda t: t[0])
        asc_items = [(c, pack_page(map_scale(unpack_page(data, ASC8_BASE + (c - ASCII_FIRST) * 16, 8, 16), *k)))
                     for c in range(ASCII_FIRST, ASCII_LAST + 1)]
        cw, ch_ = 16 * k[0] // k[1], 16 * k[0] // k[1]
        aw = 8 * k[0] // k[1]
        out[size] = (cw, ch_, aw, ch_, chs_items, asc_items)
    return out


def page_bytes(w, h):
    """列页格式字节数 = ⌈h/8⌉ 页 × w 宽/页。8 不整除的高度（如 20）第 3 页
    只含尾部 4 行，仍占整页字节——UIF_*_BYTES 必须按此算，不能 w*h/8。"""
    return ((h + 7) // 8) * w


def write_header(path, size, chs_w, chs_h, asc_w, asc_h, chs_items, asc_items):
    src_map = {24: "16×16 区 ×1.5（2:3 映射）", 20: "16×16 区 ×1.25（4:5 映射）"}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/**\n")
        f.write(" * @file ui_font_%d.h\n" % size)
        f.write(" * @brief 内置点阵字库 %d 号（SD 字库同源放大，V2.2.5）\n" % size)
        f.write(" *\n")
        f.write(" * 由 tools/opfp_gen/gen_font_sd.py 生成：kp_font_lib.bin\n")
        f.write(" * %s——观感与 16 号（SD 库原点阵）同字体。\n" % src_map[size])
        f.write(" * 勿手改；增删 UI 文案后重跑生成工具。\n")
        f.write(" *\n")
        f.write(" * 汉字 %d 字 %d×%d（列页格式，GB2312 码索引）\n" % (len(chs_items), chs_w, chs_h))
        f.write(" * ASCII %d 字 %d×%d（0x20~0x7E 顺序索引）\n" % (len(asc_items), asc_w, asc_h))
        f.write(" */\n")
        f.write("#ifndef __UI_FONT_%d_H\n#define __UI_FONT_%d_H\n\n#include <stdint.h>\n\n" % (size, size))

        f.write("#define UIF_CHS_N      %d\n" % len(chs_items))
        f.write("#define UIF_CHS_W      %d\n" % chs_w)
        f.write("#define UIF_CHS_H      %d\n" % chs_h)
        f.write("#define UIF_CHS_BYTES  %d\n" % page_bytes(chs_w, chs_h))
        f.write("#define UIF_ASC_FIRST  0x20\n")
        f.write("#define UIF_ASC_N      %d\n" % len(asc_items))
        f.write("#define UIF_ASC_W      %d\n" % asc_w)
        f.write("#define UIF_ASC_H      %d\n" % asc_h)
        f.write("#define UIF_ASC_BYTES  %d\n\n" % page_bytes(asc_w, asc_h))

        f.write("/* 汉字 GB2312 码表（升序；查找用二分或线性）*/\n")
        f.write("static const uint16_t uif_chs_code[UIF_CHS_N] = {\n")
        for i in range(0, len(chs_items), 12):
            f.write("    " + ",".join("0x%04X" % c for c, _ in chs_items[i:i + 12]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint8_t uif_chs_bits[UIF_CHS_N][UIF_CHS_BYTES] = {\n")
        for code, bm in chs_items:
            f.write("    { /* 0x%04X */\n" % code)
            for r in range(0, len(bm), chs_w):
                f.write("        " + ",".join("0x%02X" % b for b in bm[r:r + chs_w]) + ",\n")
            f.write("    },\n")
        f.write("};\n\n")

        f.write("static const uint8_t uif_asc_bits[UIF_ASC_N][UIF_ASC_BYTES] = {\n")
        for c, bm in asc_items:
            f.write("    { /* '%s' 0x%02X */\n" % (chr(c) if c != 0x5C else "\\\\", c))
            for r in range(0, len(bm), asc_w):
                f.write("        " + ",".join("0x%02X" % b for b in bm[r:r + asc_w]) + ",\n")
            f.write("    },\n")
        f.write("};\n\n")

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

#endif /* __UI_FONT_%d_H */
""" % size)
    print("生成 %s（%.1f KB）" % (path, os.path.getsize(path) / 1024))


def main():
    with open(BIN, "rb") as f:
        data = f.read()

    sizes = gen_sizes(data)
    for size, (cw, ch_, aw, ah, ci, ai) in sorted(sizes.items()):
        write_header(os.path.join(REPO, "app", "ui_font_%d.h" % size),
                     size, cw, ch_, aw, ah, ci, ai)

    # ---- 三档预览（16 原点阵 / 20 / 24）----
    sys.path.insert(0, HERE)
    from gen_font_compare import Font
    f16 = Font(os.path.join(REPO, "app", "ui_font_16.h"))
    f20 = Font(os.path.join(REPO, "app", "ui_font_20.h"))
    f24 = Font(os.path.join(REPO, "app", "ui_font_24.h"))
    rows = ["烧录中", "程序烧录", "校验通过", "按确认返回", "滚码:100", "STM32F407"]
    ROW_H = 32
    w16 = max(f16.width(t) for t in rows)
    w20 = max(f20.width(t) for t in rows)
    w24 = max(f24.width(t) for t in rows)
    PAD, GAP = 8, 12
    W = PAD * 2 + w16 + w20 + w24 + GAP * 2
    H = ROW_H * (len(rows) + 1) + 4
    from PIL import ImageDraw
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)
    f16.draw(d, "16h", PAD, 2)
    f20.draw(d, "20h", PAD + w16 + GAP, 2)
    f24.draw(d, "24h", PAD + w16 + GAP + w20 + GAP, 2)
    d.line([(0, ROW_H - 2), (W - 1, ROW_H - 2)], fill=0)
    for x in (PAD + w16 + GAP // 2, PAD + w16 + GAP + w20 + GAP // 2):
        d.line([(x, 2), (x, H - 3)], fill=0)
    y = ROW_H
    for t in rows:
        f16.draw(d, t, PAD, y + (ROW_H - 16) // 2)
        f20.draw(d, t, PAD + w16 + GAP, y + (ROW_H - 20) // 2)
        f24.draw(d, t, PAD + w16 + GAP + w20 + GAP, y + (ROW_H - 24) // 2)
        y += ROW_H
    img.resize((W * 3, H * 3), Image.NEAREST).save(
        os.path.join(HERE, "_font_sd_preview.png"))
    print("预览 %s" % os.path.join(HERE, "_font_sd_preview.png"))


if __name__ == "__main__":
    main()
