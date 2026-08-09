#ifndef UI_VIEW_PARAMS_H
#define UI_VIEW_PARAMS_H
/**
 * @file ui_view_params.h
 * @brief 参数页（params view）绘制接口 —— P2
 *
 * 纯绘制层：把「选中 .opfp 文件的参数」画到 TFT。参数由调用方从 .opfp
 * header 解出填充（见 target/opfp.h），本层只读渲染。
 */
#include <stdint.h>
#include "dev_lcd_tft.h"
#include "font_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 参数页数据（.opfp header 的用户视角投影，app 层解析填充） */
typedef struct {
    const uint8_t *filename;    /* 文件名（ASCII，超宽滚动） */
    const uint8_t *chip_model;  /* 芯片型号（ASCII，文件名反推） */
    uint32_t       fw_size;     /* 固件字节数 */
    uint32_t       flash_start; /* 起始地址（如 0x08000000） */
    uint32_t       flash_size;  /* flash 总大小（字节） */
    uint8_t        rdp;         /* 读保护：0=关 1=开 */
    uint32_t       burn_count;  /* 该文件累计烧录次数 */
} ui_params_state_t;

void ui_view_params_init(font_mgr_t *ascii_font, font_mgr_t *chs_font);
void ui_view_params_clear(void);

/**
 * @brief 绘制参数页（全量重绘）
 *   filename 超宽时按内部滚动相位滚动（ui_view_params_scroll_tick 驱动）。
 */
void ui_view_params_draw(const ui_params_state_t *state);

/** 驱动文件名滚动相位（主循环周期调用） */
void ui_view_params_scroll_tick(const uint8_t *filename);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIEW_PARAMS_H */
