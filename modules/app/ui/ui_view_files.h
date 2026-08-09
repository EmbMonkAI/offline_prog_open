#ifndef UI_VIEW_FILES_H
#define UI_VIEW_FILES_H
/**
 * @file ui_view_files.h
 * @brief 文件列表页（files view）绘制接口 —— P1
 *
 * 纯绘制层：把「SD 卡 .opfp 文件列表 + 选中项」画到 TFT。不含业务逻辑，
 * 文件列表由调用方扫描 SD 卡后填充（app 层），本层只读渲染。
 */
#include <stdint.h>
#include "dev_lcd_tft.h"
#include "font_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 列表容量上限（app 层填充时约束；界面按 UI_FILES_VISIBLE 滚动显示） */
#define UI_FILES_MAX  32

/**
 * @brief 初始化文件列表页（绑定字体对象）
 *   ascii_font : ASCII 6×12（文件名）
 *   chs_font   : GB2312 12×12（标题/提示中文）
 */
void ui_view_files_init(font_mgr_t *ascii_font, font_mgr_t *chs_font);

/**
 * @brief 绘制文件列表页
 *   files[0..count-1] : 文件名（ASCII，不含路径；超长自动截断加 "..."）
 *   count             : 文件数（0 = 显示「无文件」）
 *   selected          : 选中项下标（0 基）
 *   调用前建议先 ui_view_files_clear()。
 */
void ui_view_files_draw(const uint8_t *const files[], uint16_t count, uint16_t selected);

/** 清屏（黑底） */
void ui_view_files_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIEW_FILES_H */
