#ifndef UI_VIEW_PROGRAM_H
#define UI_VIEW_PROGRAM_H
/**
 * @file ui_view_program.h
 * @brief 烧录界面（program view）绘制接口
 *
 * 纯绘制层：接收「烧录状态」结构体，把它画到 TFT。不含任何业务逻辑、
 * 不读硬件（键盘/Flash），状态由调用方（app_prog_sim / 真机 main）填充。
 *
 * 本界面是脱机编程器主界面；后续「设置界面」「固件升级界面」各建一个
 * ui_view_*.c，结构与本文件一致，坐标在 ui_layout.h。
 */
#include <stdint.h>
#include "dev_lcd_tft.h"       /* 颜色类型 */
#include "font_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 烧录结果 ==================== */
typedef enum {
    UI_PROG_RESULT_NONE = 0,    /* 未烧录/进行中：结果区显示提示 */
    UI_PROG_RESULT_OK,          /* 成功：绿底白字 */
    UI_PROG_RESULT_FAIL,        /* 失败：红底白字 */
} ui_prog_result_t;

/* ==================== 烧录状态（调用方填充，本层只读渲染）====================
 * 字符串字段为 GBK 字节（源文件 UTF-8，故用 uint8_t* 表达，不写中文字面量）。
 * 芯片型号/文件名为 ASCII（如 "STM32F103C8"），中文标签在 .c 内部用字节常量。
 *
 * 参数选择依据（见 tools/opfp_gen + target/opfp.h）：
 *   filename   ← SD 卡 .opfp 文件名（find_opfp）
 *   chip_model ← 文件名反推（.opfp 不带型号字段）
 *   burn_count ← 设备本地累计（需持久化，sim 用演示值）
 *   rdp        ← .opfp header 的 rdp_type ≠ 0
 */
typedef struct {
    const uint8_t *filename;    /* 烧录文件名（ASCII，可能超宽→滚动） */
    const uint8_t *chip_model;  /* 芯片型号（ASCII） */
    uint32_t       burn_count;  /* 烧录次数 */
    uint8_t        rdp;         /* 读保护：0=关 1=开（opfp rdp_type≠0） */
    uint8_t        progress;    /* 进度 0~100 */
    ui_prog_result_t result;    /* 烧录结果 */
} ui_prog_state_t;

/* ==================== 对外接口 ==================== */

/**
 * @brief 初始化烧录界面（绑定字体对象）
 *   ascii_font : ASCII 8×16 字体对象
 *   chs_font   : GB2312 16×16 字体对象（含 ASCII 回退）
 *   必须在首次 ui_view_program_draw 前调用一次。
 */
void ui_view_program_init(font_mgr_t *ascii_font, font_mgr_t *chs_font);

/**
 * @brief 绘制整个烧录界面（按 state 全量重绘）
 *   每帧调用前建议先 ui_view_program_clear()。
 *   filename 超宽时按内部滚动相位滚动显示（相位由调用方周期驱动）。
 */
void ui_view_program_draw(const ui_prog_state_t *state);

/**
 * @brief 清屏（黑色背景）
 */
void ui_view_program_clear(void);

/**
 * @brief 驱动文件名滚动相位（主循环周期调用，约每 N 帧调一次）
 *   仅当文件名超宽时才推进；不超宽则无操作。
 *   返回当前相位（调试用）。
 */
uint8_t ui_view_program_scroll_tick(const uint8_t *filename);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIEW_PROGRAM_H */
