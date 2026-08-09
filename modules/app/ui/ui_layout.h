#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H
/**
 * @file ui_layout.h
 * @brief 界面坐标抽象 —— 集中定义 240×240 屏上所有界面的像素坐标
 *
 * 设计目的：所有界面元素的 (x, y, 宽, 高) 都在这里用宏定义，
 * 调整布局时只改本文件，绘图逻辑（ui_view_*.c）不碰坐标。
 * 后续新增「设置界面」「固件升级界面」时，各自加一组坐标宏即可。
 *
 * 屏参数：TFT 240×240 RGB565（DEV_LCD_TFT_WIDTH/HEIGHT，§13.10 起 240×240）。
 * 字体：中文 16×16 / ASCII 8×16，行高 16px（2026-08-28 统一切 8x16 大字号，
 * 与真机 app_ui.c 一致；原 12×12/6×12 布局废弃）。
 *
 * 三页流程（2026-08-16 设计，见 §13.14）：
 *   P1 文件列表页（ui_view_files）  ：SD 卡 .opfp 列表，↑↓ 选、OK 确认
 *   P2 参数页    （ui_view_params） ：选中文件的芯片/固件/地址/大小/保护，OK 开始烧录
 *   P3 烧录页    （ui_view_program）：进度条 + 结果（成功绿/失败红反白）
 */
#include "dev_lcd_tft.h"   /* DEV_LCD_TFT_WIDTH/HEIGHT */

/* ==================== 通用 ==================== */
#define UI_LCD_W           DEV_LCD_TFT_WIDTH     /* 240 */
#define UI_LCD_H           DEV_LCD_TFT_HEIGHT    /* 240 */
#define UI_FONT_H          16                    /* 行高（8x16 字体） */
#define UI_FONT_W_CHS      16                    /* 中文字宽 */
#define UI_FONT_W_ASCII    8                     /* ASCII 字宽 */

/* ==================== P1 文件列表页（files view）布局 ====================
 *
 *   ┌────────────────────────────┐ y=0
 *   │ [标题栏] SD卡文件（3个）    │  标题行   h=20（青底黑字，16 高字）
 *   ├────────────────────────────┤ y=20
 *   │ ▸fw1-stm32f103c8.opfp      │  列表项   h=20/项，反白高亮选中项
 *   │  fw2-stm32f407vg.opfp      │
 *   │  ...                       │
 *   │                            │
 *   ├────────────────────────────┤ y=212
 *   │ ↑↓选择  OK确认             │  底部提示 h=28
 *   └────────────────────────────┘ y=240
 */
#define UI_FILES_TITLE_Y          0
#define UI_FILES_TITLE_H          20
#define UI_FILES_LIST_Y           20
#define UI_FILES_ITEM_H           20
#define UI_FILES_ITEM_TEXT_X      4      /* 文件名文字左缘（选中标记再缩进 16px） */
#define UI_FILES_MARK_W           16     /* 选中标记「▸」占宽（中文箭头 16px） */
#define UI_FILES_VISIBLE          9      /* 可见列表项数（(204-20)/20 = 9.2 → 9） */
#define UI_FILES_HINT_Y           204
#define UI_FILES_HINT_H           36

/* ==================== P2 参数页（params view）布局 ====================
 *
 *   ┌────────────────────────────┐ y=0
 *   │ [标题栏] 文件参数           │  标题行   h=20
 *   ├────────────────────────────┤ y=20
 *   │ fw1-stm32f103c8.opfp       │  文件名行（超宽滚动，黄字）h=18
 *   ├────────────────────────────┤ y=40 分隔线
 *   │ 芯片:  STM32F103C8         │  参数行 h=18/行（标签青 值白，16 高字 + 2 间距）
 *   │ 固件:  13692 B             │
 *   │ 地址:  0x08000000          │
 *   │ 大小:  512 KB              │
 *   │ 保护:  开                  │
 *   │ 次数:  128                 │
 *   │                            │
 *   ├────────────────────────────┤ y=176
 *   │      〔 开始烧录 〕        │  动作区   h=64（OK 触发，绿底白字）
 *   └────────────────────────────┘ y=240
 */
#define UI_PARAMS_TITLE_Y         0
#define UI_PARAMS_TITLE_H         20
#define UI_PARAMS_FILENAME_Y      20
#define UI_PARAMS_FILENAME_H      18
#define UI_PARAMS_SEP_Y           40
#define UI_PARAMS_ROW1_Y          44      /* 芯片 */
#define UI_PARAMS_ROW2_Y          62      /* 固件大小 */
#define UI_PARAMS_ROW3_Y          80      /* 起始地址 */
#define UI_PARAMS_ROW4_Y          98      /* flash 大小 */
#define UI_PARAMS_ROW5_Y          116     /* 读保护 */
#define UI_PARAMS_ROW6_Y          134     /* 累计次数 */
#define UI_PARAMS_ROW_H           18
#define UI_PARAMS_LABEL_X         4       /* 标签左缘 */
#define UI_PARAMS_LABEL_W         52      /* 标签区宽（「芯片:」3 汉字 48px + 冒号） */
#define UI_PARAMS_VALUE_X         60      /* 值左缘 */
#define UI_PARAMS_VALUE_W         (UI_LCD_W - 60 - 4)
#define UI_PARAMS_ACTION_Y        176
#define UI_PARAMS_ACTION_H        64
#define UI_PARAMS_ACTION_X        24
#define UI_PARAMS_ACTION_W        (UI_LCD_W - 48)

/* ==================== P3 烧录页（program view）布局 ====================
 *
 *   ┌────────────────────────────┐ y=0
 *   │ fw1-stm32f103c8.opfp       │  文件名行 h=18（超宽滚动，黄字）
 *   ├────────────────────────────┤ y=19 分隔线
 *   │ 芯片: STM32F103C8          │  y=22
 *   │ 次数: 128      保护: 开    │  y=40
 *   │ 烧录中...                  │  状态行   y=60 h=16
 *   │ ████████░░░░░░░░░░░░      │  进度条   y=80 h=24（240 宽屏加高）
 *   │                            │
 *   │      〔 烧录成功 〕        │  结果区   y=120 h=120（反白大字）
 *   │                            │
 *   └────────────────────────────┘ y=240
 */
#define UI_PROG_FILENAME_X        0
#define UI_PROG_FILENAME_Y        0
#define UI_PROG_FILENAME_W        UI_LCD_W
#define UI_PROG_FILENAME_H        18

#define UI_PROG_SEP_Y             19

#define UI_PROG_PARAM1_Y          22      /* 芯片: <型号> */
#define UI_PROG_PARAM2_Y          40      /* 次数: <n>  保护: <开/关> */
#define UI_PROG_PARAM_H           18

/* 参数行分栏：左列区/右列区，区内「标签: 值」紧凑排列 */
#define UI_PROG_COL1_X            2
#define UI_PROG_COL1_MAX          (UI_LCD_W / 2 - 4)
#define UI_PROG_COL2_X            (UI_LCD_W / 2 + 2)
#define UI_PROG_COL2_MAX          (UI_LCD_W - 2)
#define UI_PROG_KV_GAP            2     /* 键值对里标签与值之间的像素间距 */

#define UI_PROG_STATUS_Y          60
#define UI_PROG_STATUS_H          16

#define UI_PROG_BAR_X             4
#define UI_PROG_BAR_Y             80
#define UI_PROG_BAR_W             (UI_LCD_W - 8)         /* 整行宽（左右各留 4px） */
#define UI_PROG_BAR_H             24
#define UI_PROG_BAR_EDGE          1

#define UI_PROG_RESULT_X          6
#define UI_PROG_RESULT_Y          120
#define UI_PROG_RESULT_W          (UI_LCD_W - 12)
#define UI_PROG_RESULT_H          120

#endif /* UI_LAYOUT_H */
