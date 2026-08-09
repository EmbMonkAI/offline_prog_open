#ifndef FONT_MGR_H
#define FONT_MGR_H

/**
 * @file font_mgr.h
 * @brief 字体对象定义与通用操作接口
 *
 * 设计思路：
 *   font_mgr_t 是自包含的字体对象，包含存储配置（base_addr + read）
 *   和编码类型（encoding），不依赖外部全局状态。
 *
 *   通用处理函数根据 encoding 字段自动选择 ASCII 或 GB2312 解析逻辑，
 *   新增字体只需定义新的 font_mgr_t 实例并设置正确参数，无需修改本模块。
 *
 * 存储约定：
 *   - ASCII 字体：数据从 base_addr 开始，按字符编码顺序排列
 *   - GB2312 字体：中文数据从 base_addr 开始，ASCII 数据紧跟其后
 *     （偏移 = 94*94*height*2 字节）
 */
#include <stdint.h>

/**
 * @brief 字符编码类型
 */
typedef enum {
    FONT_MGR_ENCODING_ASCII,        /**< ASCII 单字节编码（0x00~0x7F） */
    FONT_MGR_ENCODING_GB2312,       /**< GB2312 双字节编码（首字节>=0x80） */
} font_mgr_encoding_t;

/**
 * @brief 字体对象
 *
 * 定义字体的存储位置、尺寸和读取方式。
 * 所有字段在使用前由用户填充，read 函数用于从存储介质读取点阵数据。
 */
typedef struct font_mgr {
    font_mgr_encoding_t encoding;   /**< 字符编码类型 */
    uint8_t width;                  /**< 字体宽度（像素） */
    uint8_t height;                 /**< 字体高度（像素） */
    uint32_t base_addr;             /**< 字体数据在存储中的起始地址 */
    uint8_t (*read)(uint32_t addr, uint8_t *buf, uint32_t len);  /**< 存储读取函数 */
} font_mgr_t;

/**
 * @brief 初始化字体对象（预留接口，当前无操作）
 * @param font 字体对象指针
 */
void font_mgr_init(font_mgr_t *font);

/**
 * @brief 从字符串中提取当前字符的编码值
 *
 * ASCII 字符返回单字节值（0x00~0x7F）。
 * GB2312 汉字返回双字节编码（高8位|低8位）。
 *
 * @param font  字体对象指针
 * @param str   字符串指针
 * @return 字符编码值
 */
uint32_t font_mgr_get_code(font_mgr_t *font, const uint8_t *str);

/**
 * @brief 获取指定字符的点阵数据
 *
 * 根据编码类型从存储中读取点阵数据：
 * - ASCII 模式：从 base_addr + (code-0x20)*height 读取
 * - GB2312 模式：中文从 base_addr + index*bytes_per_char 读取，
 *   ASCII 字符回退到 8x16 格式
 *
 * @param font  字体对象指针
 * @param code  字符编码值（由 font_mgr_get_code 获取）
 * @param buf   输出缓冲区，至少 height*2 字节
 * @return 字体像素宽度
 */
uint32_t font_mgr_get_pixel_data(font_mgr_t *font, uint32_t code, uint8_t *buf);

/**
 * @brief 计算字符串总像素宽度（含字间距）
 *
 * - ASCII 模式：width * 字符数 + space * (字符数-1)
 * - GB2312 模式：ASCII 字符占 8 像素，汉字占 width 像素
 *
 * @param font        字体对象指针
 * @param str         字符串指针
 * @param space_pixel 字符间距（像素）
 * @return 总像素宽度
 */
uint32_t font_mgr_get_width(font_mgr_t *font, const uint8_t *str, uint8_t space_pixel);

/**
 * @brief 获取字符串中下一个字符的指针
 *
 * - ASCII 模式：跳 1 字节
 * - GB2312 模式：ASCII 字符跳 1 字节，汉字跳 2 字节
 *
 * @param font 字体对象指针
 * @param str  当前字符指针
 * @return 下一个字符的指针
 */
uint8_t *font_mgr_get_next(font_mgr_t *font, const uint8_t *str);

/**
 * @brief 使用示例
 *
 * @code
 * #include "font_mgr.h"
 *
 * // 假设从 Flash 读取字体数据的函数
 * static uint8_t flash_read(uint32_t addr, uint8_t *buf, uint32_t len) {
 *     flash_read_data(addr, buf, len);
 *     return 0;
 * }
 *
 * // 定义 ASCII 6x12 字体对象
 * static font_mgr_t font_ascii_6x12 = {
 *     .encoding  = FONT_MGR_ENCODING_ASCII,
 *     .width     = 6,
 *     .height    = 12,
 *     .base_addr = 0x100000,
 *     .read      = flash_read,
 * };
 *
 * // 定义 GB2312 16x16 字体对象（含 ASCII 回退）
 * static font_mgr_t font_gb2312_16x16 = {
 *     .encoding  = FONT_MGR_ENCODING_GB2312,
 *     .width     = 16,
 *     .height    = 16,
 *     .base_addr = 0x200000,
 *     .read      = flash_read,
 * };
 *
 * void draw_string(font_mgr_t *font, const uint8_t *str, uint8_t space) {
 *     uint8_t buf[32];
 *     uint32_t code;
 *
 *     while (*str) {
 *         code = font_mgr_get_code(font, str);
 *         font_mgr_get_pixel_data(font, code, buf);
 *         // 将 buf 中的点阵数据渲染到屏幕...
 *         str = font_mgr_get_next(font, str);
 *     }
 * }
 *
 * void example(void) {
 *     font_mgr_init(&font_ascii_6x12);
 *     font_mgr_init(&font_gb2312_16x16);
 *
 *     // 计算字符串像素宽度
 *     uint32_t w = font_mgr_get_width(&font_gb2312_16x16, "Hello你好", 1);
 *
 *     // 绘制字符串
 *     draw_string(&font_ascii_6x12, "Hello", 1);
 *     draw_string(&font_gb2312_16x16, "Hello你好", 1);
 * }
 * @endcode
 */

#endif /* FONT_MGR_H */
