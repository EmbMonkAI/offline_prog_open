#ifndef TEST_TFT_H
#define TEST_TFT_H

/**
 * @file test_tft.h
 * @brief TFT LCD 自检画面（设备无关，真屏/PC 共用）
 *
 * test_tft_draw_pattern() 用 svc_display_tft 接口画一张自检画面，
 * 上电后肉眼即可判断颜色/偏移/反转/字体位序是否正确。
 *
 * 设备装配/初始化入口（原 test_tft_run/test_tft_get_dev，真屏版在
 * test_tft_hw.c）已按分层约定移入 app 层：app_main.c 的 tft_setup() /
 * app_main_get_tft()。本文件只保留共用画图逻辑声明。
 */
#include "dev_lcd_tft.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 绘制自检画面（设备无关，真屏/PC 共用）
 *
 * 前提：调用方已先 svc_display_tft_init(dev) 绑定设备。
 * 画完后由调用方自行 dev_lcd_tft_refresh(dev) 刷新。
 */
void test_tft_draw_pattern(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_TFT_H */
