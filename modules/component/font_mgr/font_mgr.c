/**
 * @file font_mgr.c
 * @brief 字体通用处理实现
 *
 * 根据 font_mgr_t.encoding 字段自动选择解析逻辑：
 *   - FONT_MGR_ENCODING_ASCII:  单字节编码，直接映射点阵数据
 *   - FONT_MGR_ENCODING_GB2312: 双字节编码，中文用 GB2312 索引，ASCII 回退到 8x16
 *
 * GB2312 索引计算公式：
 *   index = (高字节 - 0xA1) * 94 + (低字节 - 0xA1)
 */
#include "font_mgr.h"
#include <string.h>

/** GB2312 字符编码转索引 */
#define GB2312_INDEX(code) \
    (((code >> 8) - 0xA1) * 94U + ((code) & 0xFF) - 0xA1U)

void font_mgr_init(font_mgr_t *font)
{
    (void)font;
}

uint32_t font_mgr_get_code(font_mgr_t *font, const uint8_t *str)
{
    (void)font;

    if (*str < 0x80)
        return *str;

    return (uint32_t)(*str << 8 | *(str + 1));
}

uint32_t font_mgr_get_pixel_data(font_mgr_t *font, uint32_t code, uint8_t *buf)
{
    if (!font || !font->read || !buf)
        return 0;

    if (font->encoding == FONT_MGR_ENCODING_ASCII) {
        uint32_t offset = font->base_addr + (code - 0x20) * font->height;
        font->read(offset, buf, font->height);
        return font->width;
    }

    /* GB2312 模式 */
    if (code > 0x80) {
        /* 汉字：从 GB2312 区域读取 */
        uint32_t index = GB2312_INDEX(code);
        uint32_t bytes = font->height * 2;
        font->read(font->base_addr + index * bytes, buf, bytes);
        return font->width;
    }

    /* ASCII 回退：数据紧跟在 GB2312 区域之后 */
    uint32_t gb2312_size = 94U * 94U * font->height * 2;
    font->read(font->base_addr + gb2312_size + (code - 0x20) * 16, buf, 16);
    return 8;
}

uint32_t font_mgr_get_width(font_mgr_t *font, const uint8_t *str, uint8_t space)
{
    if (!font || !str)
        return 0;

    if (font->encoding == FONT_MGR_ENCODING_ASCII) {
        uint32_t n = strlen((const char *)str);
        return n ? n * font->width + (n - 1) * space : 0;
    }

    /* GB2312 模式：逐字符累加宽度 */
    uint32_t width = 0;
    while (*str != '\0') {
        if (*str < 0x80) {
            width += (*str == 3) ? 12 + space : 8 + space;
            str++;
        } else {
            width += font->width + space;
            str += 2;
        }
    }
    return width ? width - space : 0;
}

uint8_t *font_mgr_get_next(font_mgr_t *font, const uint8_t *str)
{
    (void)font;

    if (*str < 0x80)
        return (uint8_t *)(str + 1);

    return (uint8_t *)(str + 2);
}
