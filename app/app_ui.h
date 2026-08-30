#ifndef __APP_UI_H
#define __APP_UI_H

/**
 * @file app_ui.h
 * @brief 脱机烧录器三页 UI 状态机（真机）
 *
 *   P1 文件列表（SD .opfp）→ P2 参数页 → P3 烧录进度/结果
 *
 * 单按键（PA15）交互：
 *   P1：短按 = 选下一个文件；长按(≥1s) = 确认进参数页
 *   P2：短按 = 无/刷新；长按 = 开始烧录（短按可改为返回，见 app_ui.c）
 *   P3：烧录中按键无效；结束后长按 = 回 P1 重选
 *
 * 依赖：FATFS(扫卡)、app_flash(烧录+进度)、ui_view_* 三页视图、test_tft 的 TFT 设备。
 */

void app_ui_init(void);
void app_ui_poll(void);   /* 主循环周期调用（按键扫描 + 页面渲染） */

#endif /* __APP_UI_H */
