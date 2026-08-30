/**
 * @file app_ui.c
 * @brief 脱机烧录器三页 UI 状态机（真机，STM32）
 *
 * 页面与 PC 模拟（app_prog_sim.c）同一套视图（ui_view_files/params/program），
 * 区别仅在装配层：SD 卡真实扫描（FATFS）、单按键状态机（PA15 短按/长按）、
 * 真实烧录（app_flash_run2）+ 进度回调驱动 P3 进度条。
 *
 * 单按键时序（去抖 + 边沿 + 长按计时）：
 *   按下 → 40ms 去抖确认 → 持续 ≥1000ms 判长按（只触发一次）→ 释放时若未曾长按 → 短按。
 *
 * 页面流转：
 *   P1 --长按--> P2 --长按--> P3(烧录) --结束--> P3(结果) --长按--> P1
 *   P1 短按：选下一个文件（循环）；P2 短按：回 P1（换文件）。
 */

#include "app_ui.h"
#include "main.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "ff.h"
#include "bsp_gpio.h"           /* 按键 GPIO 注入 */
#include "dev_button.h"
#include "dev_lcd_tft.h"
#include "svc_display_tft.h"    /* 绘图层（P1 文件列表自绘用） */
#include "app_main.h"           /* app_main_get_tft()：TFT 设备（app_main 装配） */
#include "font_mgr.h"
#include "app_flash.h"
#include "app_count.h"      /* 烧录计数持久化 */
#include "app_usb.h"        /* 烧录互斥（忙拒 USB 传输） */
#include "opfp.h"
#include "app_sec.h"    /* V2.1：列表扫描解密加密文件头 */

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "ui"
#include "gen_log.h"

/* ==================== 按键（实例在 app_main.c 统一装配/初始化，这里只读）====================
 * SW_OK=PB14 / SW_UP=PA0 / SW_DOWN=PB13（PCB010-V3.0），经 getter 取实例。 */

#define s_button        (*app_main_get_btn_ok())
#define s_btn_up        (*app_main_get_btn_up())
#define s_btn_down      (*app_main_get_btn_down())

/* 单按键事件 */
#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_LONG   2
static uint8_t button_event(void)
{
    static uint8_t stable = 0;        /* 去抖后的当前状态 */
    static uint8_t last_raw = 0;
    static uint32_t down_tick = 0;
    static uint8_t long_fired = 0;

    uint8_t raw = dev_button_read(&s_button);
    uint32_t now = HAL_GetTick();

    if (raw != last_raw) {            /* 原始电平变化：重启去抖窗 */
        last_raw = raw;
        down_tick = now;
        return BTN_NONE;
    }
    if ((uint32_t)(now - down_tick) < 40)
        return BTN_NONE;              /* 去抖窗内 */

    if (raw && !stable) {             /* 确认按下 */
        stable = 1;
        long_fired = 0;
    } else if (!raw && stable) {      /* 确认释放 */
        stable = 0;
        if (!long_fired)
            return BTN_SHORT;
    } else if (raw && stable && !long_fired
               && (uint32_t)(now - down_tick) >= 1000) {
        long_fired = 1;
        return BTN_LONG;              /* 长按只触发一次 */
    }
    return BTN_NONE;
}

/* ==================== UP/DOWN 键：按下即响应 + 按住连发 ====================
 * 去抖同上（电平稳定 40ms 才认）；首次确认按下立即触发一次，之后每 300ms 连发
 * （文件少时按一下即可，文件多时按住滚动）。返回 -1=无 / 0=UP / 1=DOWN。
 */
static int8_t updown_event(void)
{
    static uint8_t  up_stable = 0,  up_last = 0;
    static uint8_t  dn_stable = 0,  dn_last = 0;
    static uint32_t up_tick = 0, dn_tick = 0;
    uint32_t now = HAL_GetTick();

    uint8_t up = dev_button_read(&s_btn_up);
    uint8_t dn = dev_button_read(&s_btn_down);

    /* UP：电平变化重启去抖窗 */
    if (up != up_last) { up_last = up; up_tick = now; }
    else if (up && (uint32_t)(now - up_tick) >= 40) {
        if (!up_stable) { up_stable = 1; return 0; }               /* 首按立即触发 */
        if ((uint32_t)(now - up_tick) >= 40 + 500                   /* 按住 0.5s 后连发 */
            && (now - up_tick) % 300 < 33) return 0;                /* 每 300ms 一次 */
    } else if (!up) {
        up_stable = 0;
    }

    /* DOWN 同理 */
    if (dn != dn_last) { dn_last = dn; dn_tick = now; }
    else if (dn && (uint32_t)(now - dn_tick) >= 40) {
        if (!dn_stable) { dn_stable = 1; return 1; }
        if ((uint32_t)(now - dn_tick) >= 40 + 500
            && (now - dn_tick) % 300 < 33) return 1;
    } else if (!dn) {
        dn_stable = 0;
    }

    return -1;
}

/* ==================== 字体对象（V2.2.0：内置字库，弃 SD 卡 kp_font_lib.bin）====================
 * 生成工具产出三套同 API 字库头（符号名完全同名，不可同时 include）：
 *   ui_font_24.h —— gen_font_24.py：等线 TTF 渲染 24×24/12×24
 *   ui_font_20.h —— gen_font_20.py：等线渲染 20×20/10×20（16/24 折中档）
 *   ui_font_16.h —— gen_font_16.py：从 kp_font_lib.bin 原样提取 16×16/8×16
 * UI_FONT_SEL 三选一（改宏重编译），A/B 比对显示效果后定稿。
 * 点阵格式=列优先页格式（与 svc_display_tft_area_pixel 一致）。
 * read 回调不再走 dev_font（SD），而是内存直读——两个小适配器按 font_mgr
 * 的「base_addr+偏移」约定从内置数组拷贝。 */

#define UI_FONT_SEL 20         /* 24 / 20 / 16 三档（V2.2.4 试 20 号折中档）*/
#if UI_FONT_SEL == 24
#include "ui_font_24.h"
#elif UI_FONT_SEL == 20
#include "ui_font_20.h"
#else
#include "ui_font_16.h"
#endif

/* 布局随字高走：行高/行距/基线偏移全部由此派生，两种字体共用同一套代码 */
#define UIF_H          UIF_CHS_H                        /* 汉字像素高（=字体总高）*/
#define UIF_ROW_H      (UIF_H + 4)                      /* 列表/参数行高（字高+4 间距）*/
#define UIF_TXT_H      (UIF_H)                          /* 文本矩形高（rect_text 垂直居中）*/
#define UIF_ASC_STEP   (UIF_ASC_W + 1)                  /* ASCII 逐字步进（截断估宽用）*/

#define TITLE_BAR_H     (UIF_H + 4)               /* 标题栏高（24 号=28，16 号=20）*/
#define FILE_ROW_H      UIF_ROW_H                 /* 列表行高（字高 + 4 间距）*/
#define FILE_ROW_Y(i)   ((uint16_t)(TITLE_BAR_H + 2 + (i) * FILE_ROW_H)) /* 标题栏下起 */
#define FILE_LIST_MAX   ((240 - (TITLE_BAR_H + 2)) / FILE_ROW_H) /* 行数随行高自适应 */
#define UI_ID_W         (3 * UIF_ASC_STEP)        /* 行首编号块宽：3 位 ASCII（24 号 39 / 16 号 27）*/
#define UI_NAME_X       (UI_ID_W + 2)             /* 文件名起点（编号块右侧 + 2px 间隙）*/

/* font_mgr 的 height 字段双重语义：显示层按像素高（页数=height/8），寻址层
 * 按字节步进（ASCII: (code-0x20)*height；GB2312: index*height*2）。
 * 适配器约定：height=UIF_H（像素高，显示层正确）；read 收到的 addr 按 font_mgr
 * 步进公式反推字符序号，再查内置表取点阵：
 *   ASCII: addr = 1 + (code-0x20)*UIF_H → idx=(addr-1)/UIF_H
 *   GB2312: addr = GB2312_INDEX(code)*UIF_H*2 → 网格序号=addr/(UIF_H*2) → 还原 GB2312 码查表 */
static uint8_t uif_asc_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* font_mgr 传的 len=height（页格式字节数<height 时截短），页格式实际需
     * UIF_ASC_BYTES——忽略 len 拷全量（调用方 font_data[128] 放得下，V2.2 实测
     * 下 8 行乱码的根因即 len 不足残留旧数据） */
    (void)len;
    uint32_t idx = (addr - 1) / UIF_H;
    if (idx >= UIF_ASC_N) { memset(buf, 0, UIF_ASC_BYTES); return 1; }
    memcpy(buf, uif_asc_bits[idx], UIF_ASC_BYTES);
    return 0;
}

static uint8_t uif_chs_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* 同 uif_asc_read：len 是 font_mgr 公式（height*2），页格式需 UIF_CHS_BYTES */
    (void)len;
    uint32_t idx = addr / (UIF_H * 2);                 /* 94×94 网格序号 */
    uint8_t hi = (uint8_t)(0xA1 + idx / 94);
    uint8_t lo = (uint8_t)(0xA1 + idx % 94);
    const uint8_t *p = uif_chs(hi, lo);
    if (!p) { memset(buf, 0, UIF_CHS_BYTES); return 1; }
    memcpy(buf, p, UIF_CHS_BYTES);
    return 0;
}

static font_mgr_t s_font_ascii = {          /* 文件名/数值：ASCII 半宽×UIF_H 高 */
    .encoding  = FONT_MGR_ENCODING_ASCII,
    .width = UIF_ASC_W, .height = UIF_H,
    .base_addr = 1,                           /* 哨兵：uif_asc_read 减 1 起数 */
    .read = uif_asc_read,
};
static font_mgr_t s_font_chs = {             /* 汉字：GB2312 UIF_H（仅 UI 用字） */
    .encoding  = FONT_MGR_ENCODING_GB2312,
    .width = UIF_CHS_W, .height = UIF_H,
    .base_addr = 0,
    .read = uif_chs_read,
};

/* ==================== 字体比对演示页（V2.2.4）====================
 * 同屏同渲染三档字体：每行 16 号（f16，原 SD 字库）/ 20 号（f20，折中档）/
 * 24 号（f24，等线渲染）各画同一句 UI 文案，行内垂直居中——所见即三套字库
 * blit 到真屏的差异。P1 长按进入，再按确认/长按退出回 P1。
 * 三字库由 gen_font_demo.py 合并生成（前缀 F16_/F20_/F24_ 隔离符号）。
 * 演示页绕过 font_mgr 直取点阵 blit（area_pixel），无 font_data[128] 中转。 */

#include "ui_font_demo.h"

typedef struct {
    const uint8_t * (*glyph)(uint8_t hi, uint8_t lo);   /* 汉字查找 */
    const uint8_t * (*asc)(uint8_t c);                  /* ASCII 查找 */
    uint8_t chs_w, chs_h, asc_w;                        /* 点阵尺寸 */
} demo_font_t;

static const demo_font_t s_demo16 = { f16_chs, f16_asc, F16_CHS_W, F16_CHS_H, F16_ASC_W };
static const demo_font_t s_demo20 = { f20_chs, f20_asc, F20_CHS_W, F20_CHS_H, F20_ASC_W };
static const demo_font_t s_demo24 = { f24_chs, f24_asc, F24_CHS_W, F24_CHS_H, F24_ASC_W };

/* GBK 串透明 blit：x0/y0 顶左起，max_w 宽度上限（整字超限即停），space 字距。
 * 缺字跳过仍计步进。返回已画总宽。 */
static uint16_t demo_blit(const demo_font_t *f, const uint8_t *s,
                          uint16_t x0, uint16_t y0, uint16_t max_w, uint8_t space,
                          svc_display_tft_color_t fg)
{
    uint16_t x = x0;
    while (*s) {
        const uint8_t *bm;
        uint8_t w;
        if (*s >= 0x80) { bm = f->glyph(s[0], s[1]); w = f->chs_w; }
        else            { bm = f->asc(*s);           w = f->asc_w; }
        if (x + w > x0 + max_w) break;               /* 整字超限截断 */
        if (bm) {
            svc_display_tft_area_t a = {x, y0, w, f->chs_h};
            svc_display_tft_area_pixel(&a, bm, fg,
                                       (svc_display_tft_color_t)SVC_DISPLAY_TFT_TRANSPARENT);
        }
        x = (uint16_t)(x + w + space);
        s += (*s >= 0x80) ? 2 : 1;
    }
    return (uint16_t)(x - space - x0);
}

/* 演示页文案（GBK 显式字节，§13.8）。★字符必须在字库收录集内（UI 用字 40 字
 * + →）——「字体比对」「脱机烧录器」等含集外字会显示空白，标题用 ASCII 代替 */
static const uint8_t D_ROW1[]   = {0xC9,0xD5,0xC2,0xBC,0xD6,0xD0,0x00};              /* 烧录中 */
static const uint8_t D_ROW2[]   = {0xB3,0xCC,0xD0,0xF2,0xC9,0xD5,0xC2,0xBC,0x00};    /* 程序烧录 */
static const uint8_t D_ROW3[]   = {0xD0,0xA3,0xD1,0xE9,0xCD,0xA8,0xB9,0xFD,0x00};    /* 校验通过 */
static const uint8_t D_ROW4[]   = {0xB0,0xB4,0xC8,0xB7,0xC8,0xCF,0xB7,0xB5,0xBB,0xD8,0x00}; /* 按确认返回 */

/* 满行 ASCII 演示（三档同屏，各铺满全行宽，字距 0）：
 *   16 号 30 字 × 8px  = 240px（恰好一行 30 个——验证题）
 *   20 号 24 字 × 10px = 240px
 *   24 号 20 字 × 12px = 240px */
static const uint8_t D_ASC30[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123";  /* 30 字符 */
static const uint8_t D_ASC24[]  = "ABCDEFGHIJKLMNOPQRSTUVWX";        /* 24 字符 */
static const uint8_t D_ASC20[]  = "ABCDEFGHIJKLMNOPQRST";            /* 20 字符 */

/* 一行三列比对：16(灰) | 20(黄) | 24(白)，同一文案垂直居中（行高 32）。
 * 列布局：16 号 x=2..76 / 缝 79 / 20 号 x=82..156 / 缝 159 / 24 号 x=162..237 */
#define DEMO_ROW_H  32
static void demo_row(const uint8_t *txt, uint16_t y)
{
    demo_blit(&s_demo16, txt, 2,  (uint16_t)(y + (DEMO_ROW_H - F16_CHS_H) / 2),
              74, 1, DEV_LCD_TFT_GRAY);
    demo_blit(&s_demo20, txt, 82, (uint16_t)(y + (DEMO_ROW_H - F20_CHS_H) / 2),
              74, 1, DEV_LCD_TFT_YELLOW);
    demo_blit(&s_demo24, txt, 162,(uint16_t)(y + (DEMO_ROW_H - F24_CHS_H) / 2),
              74, 1, DEV_LCD_TFT_WHITE);
}

static void draw_font_demo(void)
{
    /* 黑底整屏 */
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);

    /* 标题栏（青条 24px）：左「16|20|24」图例，右「press: exit」 */
    {
        svc_display_tft_area_t bar = {0, 0, DEV_LCD_TFT_WIDTH, 24};
        svc_display_tft_area_fill_all(&bar, DEV_LCD_TFT_CYAN);
        svc_display_tft_rect_t t = {{4, 0, 100, 24}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&t, (const uint8_t *)"16|20|24", &s_font_ascii,
                                  SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
        svc_display_tft_rect_t h = {{104, 0, 132, 24}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&h, (const uint8_t *)"press: exit", &s_font_ascii,
                                  SVC_DISPLAY_TFT_ALIGN_RIGHT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
    }

    /* 满行 ASCII 三连（字距 0 各铺满 240px；颜色与比对列对应）：
     * y=26 16 号白 / y=48 20 号黄 / y=70 24 号白 */
    demo_blit(&s_demo16, D_ASC30, 0, 26, 240, 0, DEV_LCD_TFT_WHITE);
    demo_blit(&s_demo20, D_ASC24, 0, 48, 240, 0, DEV_LCD_TFT_YELLOW);
    demo_blit(&s_demo24, D_ASC20, 0, 70, 240, 0, DEV_LCD_TFT_WHITE);

    /* 两条中缝分隔虚线（仅比对行区 y=104..236） */
    for (uint16_t y = 104; y < 237; y += 2) {
        dev_lcd_tft_set_pixel(app_main_get_tft(), 79, y, DEV_LCD_TFT_CYAN);
        dev_lcd_tft_set_pixel(app_main_get_tft(), 159, y, DEV_LCD_TFT_CYAN);
    }

    /* 比对行 4 行 × 32px（y=104..232） */
    demo_row(D_ROW1, 104);
    demo_row(D_ROW2, 136);
    demo_row(D_ROW3, 168);
    demo_row(D_ROW4, 200);
}

/* ==================== 页面状态 ==================== */

typedef enum { PAGE_FILES = 0, PAGE_BURN, PAGE_FONT_DEMO } page_t;
static page_t s_page = PAGE_FILES;

/* 烧录页阶段：0=待机(按确认开始) 1=烧录中 2=成功 3=失败 */
static uint8_t s_burn_phase;

/* 脏标记：只在页面/内容变化时重画（这块屏在持续全屏重写下会进异常状态，
 * §13.16 实测 —— 静态页面画一次就停，与 v3 单次绘制的成功模式一致） */
static uint8_t s_dirty = 1;           /* 1=需要重画当前页 */
static uint8_t s_last_progress;       /* P3 进度去重（回调频繁，只在变化时刷） */

/* 文件列表（SD 扫描）；容量上限本地定义（原 ui_view_files.h 已不依赖） */
#define UI_FILES_MAX 10
static uint8_t  s_names[UI_FILES_MAX][40];
static const uint8_t *s_ptrs[UI_FILES_MAX];
static uint32_t s_image_ids[UI_FILES_MAX];       /* v5 镜像编号（0=无；列表行首显示）*/
static uint16_t s_count, s_sel;

/* 烧录页数据（.opfp 解析结果） */
static app_flash_info_t s_info;

/* ==================== P1 文件列表画面（app 层业务，2026-08-16 自 modules 迁入）====================
 * 标题「文件选择」青底黑字（16×16 汉字）+ 逐行 .opfp 文件名（8×16 ASCII），
 * 选中行白底反白 + 红「→」标记（与文字留 7px 空隙）。中文串为显式 GBK 字节（§13.8）。
 *
 * 局部刷新（§13.16 教训落地）：换选文件时只重画受影响的两个列表行
 * （旧行去高亮 + 新行加高亮，各 240×20 区域，~3ms），不整屏 clear —— 无可见刷屏。
 */
static const uint8_t S_TITLE[] = {0xCE,0xC4,0xBC,0xFE,0xD1,0xA1,0xD4,0xF1,0x00};  /* 文件选择 */
static const uint8_t S_NONE[]  = {0xCE,0xDE,0xCE,0xC4,0xBC,0xFE,0x00};            /* 无文件 */
static const uint8_t S_ARROW[] = {0xA1,0xFA,0x00};                                /* → */

/* 烧录页中文串（GBK） */
static const uint8_t S_BURN_TITLE[] = {0xB3,0xCC,0xD0,0xF2,0xC9,0xD5,0xC2,0xBC,0x00};              /* 程序烧录 */
static const uint8_t S_CHIP[]       = {0xD0,0xBE,0xC6,0xAC,0x00};                                   /* 芯片 */
static const uint8_t S_FW[]         = {0xB9,0xCC,0xBC,0xFE,0x00};                                   /* 固件 */
static const uint8_t S_COUNT[]      = {0xB4,0xCE,0xCA,0xFD,0x00};                                   /* 次数 */
static const uint8_t S_ROLL[]       = {0xB9,0xF6,0xC2,0xEB,0x00};                                   /* 滚码 */
static const uint8_t S_MAXCNT[]     = {0xCF,0xDE,0xD6,0xC6,0x00};                                   /* 限制 */
static const uint8_t S_BURNING[]    = {0xC9,0xD5,0xC2,0xBC,0xD6,0xD0,0x00};                         /* 烧录中 */
static const uint8_t S_VERIFY[]     = {0xD0,0xA3,0xD1,0xE9,0xD6,0xD0,0x00};                         /* 校验中 */
static const uint8_t S_VERIFY_OK[]  = {0xD0,0xA3,0xD1,0xE9,0xCD,0xA8,0xB9,0xFD,0x00};               /* 校验通过 */
static const uint8_t S_BURN_FAIL[]  = {0xC9,0xD5,0xC2,0xBC,0xCA,0xA7,0xB0,0xDC,0x00};               /* 烧录失败 */
static const uint8_t S_VERIFY_FAIL[]={0xD0,0xA3,0xD1,0xE9,0xCA,0xA7,0xB0,0xDC,0x00};                /* 校验失败 */
static const uint8_t S_PRESS_GO[]   = {0xB0,0xB4,0xC8,0xB7,0xC8,0xCF,0xBF,0xAA,0xCA,0xBC,0x00};     /* 按确认开始 */
static const uint8_t S_PRESS_BACK[] = {0xB0,0xB4,0xC8,0xB7,0xC8,0xCF,0xB7,0xB5,0xBB,0xD8,0x00};     /* 按确认返回 */

/** 画一个列表行（含高亮/普通两种状态）。只写该行的 240×FILE_ROW_H 区域。
 *  image_id>0 时行首画编号块（黄底黑字 3 位「001」），名字右移 x=28 起。 */
static void draw_file_row(const uint8_t *name, int is_sel, uint16_t y, uint32_t image_id)
{
    /* 行底色：选中白 / 普通黑 */
    svc_display_tft_area_t row = {0, y, DEV_LCD_TFT_WIDTH, FILE_ROW_H};
    svc_display_tft_area_fill_all(&row, is_sel ? DEV_LCD_TFT_WHITE : DEV_LCD_TFT_BLACK);

    if (image_id > 0) {
        /* 编号块（黄底黑字——青底白字实测对比不足看不清；选中/未选中同色，
         * 编号是文件属性不随选态变）。块宽=3 位 ASCII 数字宽 */
        char v[8];
        uint16_t id = (uint16_t)(image_id > 999 ? 999 : image_id);
        snprintf(v, sizeof(v), "%03u", (unsigned)id);
        svc_display_tft_rect_t idr = {{0, (uint16_t)(y + 2), UI_ID_W, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&idr, (const uint8_t *)v, &s_font_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_YELLOW, 1);
    } else if (is_sel) {
        /* 「→」标记（红字，x=2 起），与文字（x=UI_NAME_X 起）留空隙 */
        svc_display_tft_rect_t mk = {{2, (uint16_t)(y + 2), (uint16_t)(UIF_CHS_W + 4), UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&mk, S_ARROW, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_RED, DEV_LCD_TFT_WHITE, 1);
    }

    /* 文件名（x=UI_NAME_X 起：编号块右侧 + 2px 间隙，V1.8.2 曾与块重叠显拥挤；
     * 超宽截断加 "..."）*/
    {
        const uint8_t *show = name;
        uint8_t trunc[40];
        uint32_t w = font_mgr_get_width(&s_font_ascii, show, 1);
        uint16_t avail = (uint16_t)(DEV_LCD_TFT_WIDTH - UI_NAME_X - 4);
        if (w > avail && w > 3 * UIF_ASC_STEP) {
            uint16_t max_chars = (uint16_t)((avail - 3 * UIF_ASC_STEP) / UIF_ASC_STEP);
            uint16_t n = 0;
            while (show[n] && n < max_chars && n < sizeof(trunc) - 4) n++;
            memcpy(trunc, show, n);
            trunc[n] = '.'; trunc[n+1] = '.'; trunc[n+2] = '.'; trunc[n+3] = 0;
            show = trunc;
        }
        svc_display_tft_rect_t r = {{UI_NAME_X, (uint16_t)(y + 2), avail, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, show, &s_font_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  is_sel ? DEV_LCD_TFT_BLACK : DEV_LCD_TFT_WHITE,
                                  is_sel ? DEV_LCD_TFT_WHITE : DEV_LCD_TFT_BLACK, 1);
    }
}

/** 标题栏右侧的序号「当前/总数」（如 1/10）。位置固定 x=188..236，青底黑字。
 *  换选时只刷这个 48×UIF_H 小块（局部刷新）。count=0 时不显示。 */
static void draw_filelist_index(uint16_t sel, uint16_t count)
{
    char v[12];

    if (count == 0) return;
    snprintf(v, sizeof(v), "%u/%u", (unsigned)(sel + 1), (unsigned)count);

    /* 青底小块（与标题栏同色，盖掉旧数字）+ 黑字 */
    svc_display_tft_area_t bg = {188, 2, 48, UIF_TXT_H};
    svc_display_tft_area_fill_all(&bg, DEV_LCD_TFT_CYAN);
    svc_display_tft_rect_t r = {{188, 2, 48, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
    svc_display_tft_rect_text(&r, (const uint8_t *)v, &s_font_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                              DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
}

/** 整页画文件列表（进页/重扫后用；此后换选走 update_filelist_sel 局部刷新） */
static void draw_filelist(const uint8_t *const files[], uint16_t count, uint16_t selected)
{
    /* 黑底整屏 */
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);

    /* ---- 标题栏：青底，「文件选择」黑字 + 右侧序号 ---- */
    {
        svc_display_tft_area_t bar = {0, 0, DEV_LCD_TFT_WIDTH, TITLE_BAR_H};
        svc_display_tft_area_fill_all(&bar, DEV_LCD_TFT_CYAN);
        svc_display_tft_rect_t r = {{4, 2, 176, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, S_TITLE, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
    }
    draw_filelist_index(selected, count);

    if (count == 0) {
        svc_display_tft_rect_t r = {{0, 60, DEV_LCD_TFT_WIDTH, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, S_NONE, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_GRAY, DEV_LCD_TFT_BLACK, 1);
        return;
    }
    if (selected >= count) selected = 0;

    /* ---- 文件列表 ---- */
    for (uint16_t i = 0; i < count && i < FILE_LIST_MAX; i++)
        draw_file_row(files[i], i == selected, FILE_ROW_Y(i),
                      i < UI_FILES_MAX ? s_image_ids[i] : 0);
}

/** 换选：只重画旧/新两行 + 标题栏序号小块（局部刷新，无整屏闪） */
static void update_filelist_sel(const uint8_t *const files[], uint16_t count,
                                uint16_t old_sel, uint16_t new_sel)
{
    if (old_sel < count && old_sel < FILE_LIST_MAX)
        draw_file_row(files[old_sel], 0, FILE_ROW_Y(old_sel),
                      old_sel < UI_FILES_MAX ? s_image_ids[old_sel] : 0);
    if (new_sel < count && new_sel < FILE_LIST_MAX)
        draw_file_row(files[new_sel], 1, FILE_ROW_Y(new_sel),
                      new_sel < UI_FILES_MAX ? s_image_ids[new_sel] : 0);
    draw_filelist_index(new_sel, count);          /* 序号随选变化（46×16 小块） */
}

/* ==================== P2 烧录页（标题「程序烧录」+ 参数 + 进度条 + 状态/结果）====================
 * 布局（240×240，V1.8.1 参数 7 行重排）：
 *   y=0..23   标题栏「程序烧录」（青底黑字）
 *   y=25..40  烧录名称（黄字，v5 编号前缀）
 *   y=43..138 关键参数 7 行（芯片/固件/地址/保护/滚码/次数/已烧，行距 14，字高 16 叠 2px 视觉可接受）
 *   y=141..168 进度条（28px，条上状态 + 百分比）
 *   y=178..193 「按确认返回」提示（结果出来后）
 *   y=194..209 失败码 ERR n（失败时）
 * v4 文件：滚码/次数行显示「—/—」（无 PGCF 段）
 * 进度回调高频触发：draw_burn_page 按 phase 只重画变化区（进度条/状态行），其余首画一次。
 */

/* 画一行参数「标签: 值」。值字体按内容自动选：首字节>=0x80（GBK 中文）用中文字体，
 * 纯 ASCII 用 ASCII 字体 —— 「保护: 开/关」的值是中文，此前误用 ASCII 字体导致乱码。 */
static void draw_burn_row(uint16_t y, const uint8_t *label, const char *value)
{
    svc_display_tft_rect_t r;
    font_mgr_t *vfont = (value && (uint8_t)value[0] >= 0x80) ? &s_font_chs : &s_font_ascii;

    r.area.x = 4; r.area.y = y;
    r.area.width = 44; r.area.height = UIF_TXT_H;
    r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;
    svc_display_tft_rect_text(&r, label, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    r.area.x = 68; r.area.width = (uint16_t)(DEV_LCD_TFT_WIDTH - 68 - 4);
    svc_display_tft_rect_text(&r, (const uint8_t *)value, vfont, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              value && (uint8_t)value[0] >= 0x80 ? DEV_LCD_TFT_WHITE : DEV_LCD_TFT_WHITE,
                              DEV_LCD_TFT_BLACK, 1);
}

/* ==================== 进度条 + 条上状态（V1.3.0 重构，增量填充防闪烁）====================
 * 布局：x=8..231，y=116..145（30px 高）。条内左侧为状态文字，右侧 60px 为百分比。
 * 状态融入进度条：
 *   phase 0 待机：黑条 +「按确认开始」（灰）
 *   phase 1 烧录中：青色填充从左推进 + 条上「烧录中」黄字重画 + 百分比小块刷新
 *   phase 2 成功：整条变绿 +「烧录成功」白字（一次性，无闪感）
 *   phase 3 失败：整条变红 +「烧录失败」白字
 */
#define BAR_X       8
#define BAR_Y       198
#define BAR_W       ((uint16_t)(DEV_LCD_TFT_WIDTH - 16))
#define BAR_H       28
#define BAR_INNER_W ((uint16_t)(BAR_W - 2))
#define BAR_FILL_W(p)  ((uint16_t)((uint32_t)BAR_INNER_W * (p) / 100))
#define BAR_NUM_X   (uint16_t)(BAR_X + BAR_W - 64)
#define BAR_NUM_W   56

/* 整条重画：白框 + 黑底 + 按 percent 填充（进页用一次） */
static void draw_burn_bar(uint8_t percent)
{
    svc_display_tft_rect_t frame = {{BAR_X, BAR_Y, BAR_W, BAR_H}, 1, SVC_DISPLAY_TFT_EDGE_ALL};
    svc_display_tft_rect_fill_all(&frame, DEV_LCD_TFT_WHITE, DEV_LCD_TFT_BLACK);

    if (percent > 0 && BAR_FILL_W(percent) > 0) {
        svc_display_tft_area_t fa = {(uint16_t)(BAR_X + 1), (uint16_t)(BAR_Y + 1),
                                     BAR_FILL_W(percent), (uint16_t)(BAR_H - 2)};
        svc_display_tft_area_fill_all(&fa, DEV_LCD_TFT_CYAN);
    }
}

/* 增量推进：只填 [old, new) 对应的条形段（无闪烁） */
static void burn_bar_advance(uint8_t old_p, uint8_t new_p)
{
    if (new_p <= old_p) return;
    uint16_t w0 = BAR_FILL_W(old_p);
    uint16_t w1 = BAR_FILL_W(new_p);
    if (w1 <= w0) return;

    svc_display_tft_area_t fa = {(uint16_t)(BAR_X + 1 + w0), (uint16_t)(BAR_Y + 1),
                                 (uint16_t)(w1 - w0), (uint16_t)(BAR_H - 2)};
    svc_display_tft_area_fill_all(&fa, DEV_LCD_TFT_CYAN);
}

/* 透明背景文字 blit：只画文字前景像素，完全不碰底图。
 * ★进度条上的文字必须用本函数 —— rect_text 写字前会用 bg 铺满整个矩形，
 * 会把已推进的青色填充整带抹掉（V1.4.2 及之前反复出现的"中空条/双进度"根因）。
 * 白色前景在黑底（未填充区）和青底（已填充区）上都清晰。 */
static void bar_text_transparent(const uint8_t *txt, font_mgr_t *font,
                                 uint16_t x, uint16_t y, uint16_t h,
                                 svc_display_tft_color_t fg)
{
    uint8_t fdata[128];
    uint16_t cx = x;
    const uint8_t *p = txt;

    while (*p) {
        uint32_t code = font_mgr_get_code(font, p);
        uint32_t cw = font_mgr_get_pixel_data(font, code, fdata);
        if (cw) {
            svc_display_tft_area_t ca = {cx, y, (uint8_t)cw, (uint8_t)h};
            svc_display_tft_area_pixel(&ca, fdata, fg,
                                       (svc_display_tft_color_t)SVC_DISPLAY_TFT_TRANSPARENT);
        }
        cx = (uint16_t)(cx + cw + 1);
        p = font_mgr_get_next(font, p);
    }
}

/* 条上状态文字（phase 变化时调用；烧录中随填充推进重画——透明 blit 无副作用）
 * phase：0=待机「按确认开始」 1=烧录中 2=成功（绿） 3=失败（红） 4=校验中 5=校验失败（红）*/
static void draw_burn_bar_status(uint8_t phase)
{
    const uint8_t *txt = S_PRESS_GO;
    svc_display_tft_color_t fg = DEV_LCD_TFT_GRAY;

    if (phase == 1 || phase == 4) {
        /* 烧录中/校验中：透明 blit 白字 —— 填充推进从字底下穿过，字始终可见 */
        fg = DEV_LCD_TFT_WHITE;
        txt = (phase == 4) ? S_VERIFY : S_BURNING;
    } else if (phase == 2 || phase == 3 || phase == 5) {
        /* 结果：整条变绿/红 + 白字（2=烧录成功即校验通过 / 3=烧录失败 / 5=校验失败） */
        svc_display_tft_area_t fa = {(uint16_t)(BAR_X + 1), (uint16_t)(BAR_Y + 1),
                                     BAR_INNER_W, (uint16_t)(BAR_H - 2)};
        svc_display_tft_area_fill_all(&fa, (phase == 2) ? DEV_LCD_TFT_GREEN : DEV_LCD_TFT_RED);
        fg = DEV_LCD_TFT_WHITE;
        txt = (phase == 2) ? S_VERIFY_OK : (phase == 5 ? S_VERIFY_FAIL : S_BURN_FAIL);
    }

    /* 透明 blit：不碰底图（结果 phase 已整条铺色，同样无碍）。
     * 垂直居中于条内：(BAR_H-字高)/2 偏移 */
    bar_text_transparent(txt, &s_font_chs,
                         (uint16_t)(BAR_X + 4), (uint16_t)(BAR_Y + (BAR_H - UIF_H) / 2),
                         UIF_H, fg);
}

/* 只刷条上右侧百分比数字（高频，小块无闪感） */
static void draw_burn_percent(uint8_t percent)
{
    static uint8_t last_shown = 0xFF;

    if (percent == last_shown) return;
    last_shown = percent;

    {
        char v[8];
        snprintf(v, sizeof(v), "%3u%%", (unsigned)percent);

        /* 数字区清底：按填充右缘把小块"劈成两半"——填充已到的左段清青，
         * 未到的右段清黑 —— 颜色与进度条实际状态逐像素一致（V1.4.4 修：
         * 此前整块单色，填充右缘穿过数字区时（~85-95% 段）出现快结束时
         * 数字带与主条进度不一致）。 */
        {
            uint16_t fill_w = BAR_FILL_W(percent);          /* 填充右缘相对条内起点 */
            uint16_t edge_x = (uint16_t)(BAR_X + 1 + fill_w);   /* 填充右缘绝对 x */
            uint16_t num_x1 = (uint16_t)(BAR_NUM_X + BAR_NUM_W);
            uint16_t y0 = (uint16_t)(BAR_Y + (BAR_H - UIF_H) / 2);

            if (edge_x <= BAR_NUM_X) {
                /* 填充未到数字区：整块黑 */
                svc_display_tft_area_t bg = {BAR_NUM_X, y0, BAR_NUM_W, UIF_H};
                svc_display_tft_area_fill_all(&bg, DEV_LCD_TFT_BLACK);
            } else if (edge_x >= num_x1) {
                /* 填充已越过数字区：整块青 */
                svc_display_tft_area_t bg = {BAR_NUM_X, y0, BAR_NUM_W, UIF_H};
                svc_display_tft_area_fill_all(&bg, DEV_LCD_TFT_CYAN);
            } else {
                /* 填充右缘在数字区中间：劈两半 */
                uint16_t left_w = (uint16_t)(edge_x - BAR_NUM_X);
                uint16_t right_w = (uint16_t)(num_x1 - edge_x);
                svc_display_tft_area_t l = {BAR_NUM_X, y0, left_w, UIF_H};
                svc_display_tft_area_fill_all(&l, DEV_LCD_TFT_CYAN);
                svc_display_tft_area_t r2 = {edge_x, y0, right_w, UIF_H};
                svc_display_tft_area_fill_all(&r2, DEV_LCD_TFT_BLACK);
            }
        }
        bar_text_transparent((const uint8_t *)v, &s_font_ascii,
                             BAR_NUM_X, (uint16_t)(BAR_Y + (BAR_H - UIF_H) / 2),
                             UIF_H, DEV_LCD_TFT_WHITE);
    }
}

/** 整页画烧录页（进页时一次；此后进度/状态走局部更新）
 *  布局（V1.3.0）：
 *    y=0..23   标题栏「程序烧录」（青底黑字）
 *    y=28..44  烧录名称（黄字大号位，v4 显示名优先回退文件名）★第二行
 *    y=40..115 参数 5 行（芯片/固件/地址/保护/次数，行距 15；值字体按内容自动选）
 *    y=116..145 进度条（30px，条上状态：待机/烧录中/校验中/绿校验通过/红失败 + 右侧百分比）
 *    y=156..171 「按确认返回」提示（结果出来后）
 */
static void draw_burn_page(const app_flash_info_t *info, uint8_t phase, uint8_t percent)
{
    char v[24];

    /* 黑底整屏 */
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);

    /* 标题栏「程序烧录」 */
    {
        svc_display_tft_area_t bar = {0, 0, DEV_LCD_TFT_WIDTH, TITLE_BAR_H};
        svc_display_tft_area_fill_all(&bar, DEV_LCD_TFT_CYAN);
        svc_display_tft_rect_t r = {{4, 2, DEV_LCD_TFT_WIDTH - 8, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, S_BURN_TITLE, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
    }

    /* 第二行：烧录名称（黄字；v4 显示名优先回退文件名；超宽截断）。
     * 与 PC 端输入完全一致——不加编号前缀（编号在列表行首看即可，V1.8.4） */
    {
        const uint8_t *show = (const uint8_t *)(info->disp_name[0] ? info->disp_name
                                                                   : info->filename);
        uint8_t trunc[40];
        uint32_t w = font_mgr_get_width(&s_font_ascii, show, 1);
        if (w > DEV_LCD_TFT_WIDTH - 8 && w > 3 * UIF_ASC_STEP) {
            uint16_t max_chars = (uint16_t)((DEV_LCD_TFT_WIDTH - 8 - 3 * UIF_ASC_STEP) / UIF_ASC_STEP);
            uint16_t n = 0;
            while (show[n] && n < max_chars && n < sizeof(trunc) - 4) n++;
            memcpy(trunc, show, n);
            trunc[n] = '.'; trunc[n+1] = '.'; trunc[n+2] = '.'; trunc[n+3] = 0;
            show = trunc;
        }
        svc_display_tft_rect_t r = {{4, 30, (uint16_t)(DEV_LCD_TFT_WIDTH - 8), UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, show, &s_font_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_YELLOW, DEV_LCD_TFT_BLACK, 1);
    }

    /* 参数 7 行（行距 14，y=43 起；值字体按内容自动选）：
     * 芯片/固件/地址/保护 + v5 新增：滚码（地址@起始）/次数上限/已烧次数 */
    draw_burn_row(58, S_CHIP, info->chip_model);
    if (info->fw_size >= 1024u && (info->fw_size % 1024u) == 0)
        snprintf(v, sizeof(v), "%lu KB", (unsigned long)(info->fw_size / 1024u));
    else
        snprintf(v, sizeof(v), "%lu B", (unsigned long)info->fw_size);
    draw_burn_row(86, S_FW, v);
    snprintf(v, sizeof(v), "0x%08lX", (unsigned long)info->flash_start);
    /* 地址/保护行砍（240px 放不下 7×24 参数行；信息在 PC 端可查）*/

    /* 滚码行：显示本次将写入的序列号（起始 + 已烧次数×步进，与 run2 计算一致） */
    if (info->serial_on) {
        uint32_t sn = info->serial_start + info->burn_count * info->serial_step;
        snprintf(v, sizeof(v), "%lu@0x%06lX", (unsigned long)sn,
                 (unsigned long)info->serial_addr & 0xFFFFFF);
    } else {
        snprintf(v, sizeof(v), "%s", "-");       /* ASCII '-'（未启用/无段）*/
    }
    draw_burn_row(114, S_ROLL, v);
    if (info->maxcnt_on)
        snprintf(v, sizeof(v), "%lu", (unsigned long)info->max_burn_count);
    else
        snprintf(v, sizeof(v), "%s", "-");
    draw_burn_row(142, S_MAXCNT, v);
    /* 已烧次数：达上限时值红显（按确认 run2 也会硬拒，码 13） */
    snprintf(v, sizeof(v), "%lu", (unsigned long)info->burn_count);
    draw_burn_row(170, S_COUNT, v);
    if (app_flash_count_exceeded(info)) {
        svc_display_tft_rect_t r = {{68, (uint16_t)(170 + 2),
                                     (uint16_t)(DEV_LCD_TFT_WIDTH - 50 - 4), UIF_TXT_H},
                                    0, SVC_DISPLAY_TFT_EDGE_NO};
        char vb[16];
        snprintf(vb, sizeof(vb), "%lu/%lu!", (unsigned long)info->burn_count,
                 (unsigned long)info->max_burn_count);
        svc_display_tft_rect_text(&r, (const uint8_t *)vb, &s_font_ascii,
                                  SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_RED, DEV_LCD_TFT_BLACK, 1);
    }

    /* 进度条（含条上状态） */
    draw_burn_bar(percent);
    draw_burn_bar_status(phase);
    if (phase == 1)
        draw_burn_percent(percent);

    /* 结果出来后：提示「按确认返回」（V1.8.1 下移让位参数 7 行） */
    if (phase >= 2) {
        svc_display_tft_rect_t r = {{0, 178, DEV_LCD_TFT_WIDTH, UIF_TXT_H}, 0, SVC_DISPLAY_TFT_EDGE_NO};
        svc_display_tft_rect_text(&r, S_PRESS_BACK, &s_font_chs, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_GRAY, DEV_LCD_TFT_BLACK, 1);
    }
}

/* ==================== SD 扫描 .opfp ==================== */

/* 列表显示名缓存：v4 .opfp 的显示名优先，v3/未设置回退文件名。
 * s_disp 与 s_names 一一对应；draw_file_row 直接用 s_ptrs 指向的串。 */
static uint8_t  s_disp_names[UI_FILES_MAX][20];

/* 判 .opfp 是否 v5（打开失败/v4 及更老/坏 magic → 0）。列表准入判据（V1.8.2）：
 * 只认 v5；v4/v3 等旧版一律视为不合法文件不显示（PC 工具已统一产 v5）。 */
static int opfp_is_v5(const char *fname)
{
    FIL fp;
    FRESULT fr;
    UINT br;
    opfp_header_t hdr;
    uint8_t enchead[4];
    char path[64];

    snprintf(path, sizeof(path), "0:%s", fname);
    fr = f_open(&fp, path, FA_READ);
    if (fr != FR_OK) return 0;
    /* V2.1：加密文件（ENC1 头）——读密文头后解密前 112B 判版本 */
    fr = f_read(&fp, enchead, 4, &br);
    if (fr == FR_OK && br == 4 && memcmp(enchead, "ENC1", 4) == 0) {
        uint8_t hdr16[16];
        uint32_t enc_nonce;
        fr = f_read(&fp, hdr16 + 4, 12, &br);          /* 头余 12B（nonce+保留）*/
        if (fr == FR_OK && br == 12) {
            enc_nonce = ((uint32_t)hdr16[4] << 24) | ((uint32_t)hdr16[5] << 16)
                        | ((uint32_t)hdr16[6] << 8) | (uint32_t)hdr16[7];
            /* 解密逻辑偏移 0 起 112B（读物理 16..128）*/
            uint8_t hdrbuf[sizeof(opfp_header_t)];
            fr = f_read(&fp, hdrbuf, sizeof(hdrbuf), &br);
            f_close(&fp);
            if (fr == FR_OK && br == sizeof(hdrbuf)) {
                app_sec_enc_crypt(hdrbuf, sizeof(hdrbuf), 0, enc_nonce);
                memcpy(&hdr, hdrbuf, sizeof(hdr));
                return (hdr.magic == OPFP_MAGIC && hdr.version >= 5) ? 1 : 0;
            }
        }
        f_close(&fp);
        return 0;
    }
    f_lseek(&fp, 0);
    fr = f_read(&fp, &hdr, sizeof(hdr), &br);
    f_close(&fp);
    return (fr == FR_OK && br == sizeof(hdr)
            && hdr.magic == OPFP_MAGIC && hdr.version >= 5) ? 1 : 0;
}

/* 读一个 .opfp 的 v4 显示名 + v5 镜像编号（v3/无名称/打开失败 → 空串，调用方跳过）
 * v5：名称段后 PGCF，取 image_id（magic/flags 校验，坏段编号=0 不致命）
 * V2.1：加密文件（ENC1）先解密头部 160B（header+名+PGCF 段）再解析 */
static void read_list_info(const char *fname, char *out, size_t out_sz, uint32_t *image_id)
{
    FIL fp;
    FRESULT fr;
    UINT br;
    opfp_header_t hdr;
    uint8_t hdrbuf[128 + 32];      /* header 112 + 名 16 + PGCF 32（一次解密读全）*/
    uint8_t *base = hdrbuf;
    char path[64];

    out[0] = 0;
    *image_id = 0;
    snprintf(path, sizeof(path), "0:%s", fname);
    fr = f_open(&fp, path, FA_READ);
    if (fr != FR_OK) return;

    {
        uint8_t enchead[16];
        uint32_t enc_nonce = 0;
        int is_enc = 0;
        fr = f_read(&fp, enchead, 16, &br);
        if (fr == FR_OK && br == 16 && memcmp(enchead, "ENC1", 4) == 0) {
            is_enc = 1;
            enc_nonce = ((uint32_t)enchead[4] << 24) | ((uint32_t)enchead[5] << 16)
                        | ((uint32_t)enchead[6] << 8) | (uint32_t)enchead[7];
        }
        if (is_enc) {
            fr = f_read(&fp, hdrbuf, sizeof(hdrbuf), &br);
            f_close(&fp);
            if (fr != FR_OK || br != sizeof(hdrbuf)) return;
            app_sec_enc_crypt(hdrbuf, sizeof(hdrbuf), 0, enc_nonce);
        } else {
            f_lseek(&fp, 0);
            fr = f_read(&fp, hdrbuf, sizeof(hdrbuf), &br);
            f_close(&fp);
            if (fr != FR_OK || br < sizeof(opfp_header_t)) return;
        }
    }

    memcpy(&hdr, base, sizeof(hdr));
    if (hdr.magic == OPFP_MAGIC && hdr.version >= 4) {
        opfp_name_t nm;
        memcpy(&nm, base + sizeof(hdr), sizeof(nm));
        {
            size_t k = 0;
            while (k < OPFP_NAME_MAX && k < out_sz - 1
                   && nm.name[k] >= 0x20 && nm.name[k] <= 0x7E) {
                out[k] = (char)nm.name[k];
                k++;
            }
            out[k] = 0;
        }
        if (hdr.version >= 5) {
            opfp_pgcf_t pg;
            memcpy(&pg, base + sizeof(hdr) + sizeof(opfp_name_t), sizeof(pg));
            if (pg.magic == OPFP_PGCF_MAGIC
                && (pg.flags & OPFP_PGCF_FLAG_IMAGE_ID))
                *image_id = pg.image_id;
        }
    }
}

static uint16_t scan_opfp(void)
{
    DIR d; FILINFO fi; FRESULT fr;
    uint16_t n = 0;

    fr = f_opendir(&d, "0:/");
    if (fr != FR_OK) return 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0' && n < UI_FILES_MAX) {
        size_t len = strlen(fi.fname);
        if (len >= 5 && len < sizeof(s_names[0]) && !(fi.fattrib & AM_DIR)) {
            const char *ext = fi.fname + len - 5;
            if (toupper((unsigned char)ext[0]) == '.' &&
                toupper((unsigned char)ext[1]) == 'O' &&
                toupper((unsigned char)ext[2]) == 'P' &&
                toupper((unsigned char)ext[3]) == 'F' &&
                toupper((unsigned char)ext[4]) == 'P') {
                /* 只认 v5（V1.8.2）：v4/v3 旧版一律不合法不显示（PC 工具统一产 v5）。
                 * 显示名可留空（PC GUI 允许）→ 回退显示文件名（去 .opfp 扩展名） */
                {
                    uint32_t id = 0;
                    read_list_info(fi.fname, (char *)s_disp_names[n], sizeof(s_disp_names[n]),
                                   &id);
                    if (!opfp_is_v5(fi.fname)) {
                        continue;                   /* 旧版/坏文件：不进列表 */
                    }
                    if (s_disp_names[n][0] == 0) {
                        size_t bl = len - 5;        /* 去 ".opfp" */
                        if (bl >= sizeof(s_disp_names[n])) bl = sizeof(s_disp_names[n]) - 1;
                        memcpy(s_disp_names[n], fi.fname, bl);
                        s_disp_names[n][bl] = 0;
                    }
                    s_image_ids[n] = id;
                    memcpy(s_names[n], fi.fname, len + 1);
                    s_ptrs[n] = s_disp_names[n];
                    n++;
                }
            }
        }
    }
    f_closedir(&d);

    /* 按编号升序排（V1.8.3）：FAT 目录序在覆盖/删除后就乱——产线按编号找文件，
     * 列表必须编号有序。插入排序（≤10 项）四数组同步换。 */
    for (uint16_t i = 1; i < n; i++) {
        uint32_t key_id = s_image_ids[i];
        uint8_t key_nm[40], key_dn[20];
        memcpy(key_nm, s_names[i], sizeof(key_nm));
        memcpy(key_dn, s_disp_names[i], sizeof(key_dn));
        uint16_t j = i;
        while (j > 0 && s_image_ids[j - 1] > key_id) {
            s_image_ids[j] = s_image_ids[j - 1];
            memcpy(s_names[j], s_names[j - 1], sizeof(s_names[0]));
            memcpy(s_disp_names[j], s_disp_names[j - 1], sizeof(s_disp_names[0]));
            j--;
        }
        s_image_ids[j] = key_id;
        memcpy(s_names[j], key_nm, sizeof(key_nm));
        memcpy(s_disp_names[j], key_dn, sizeof(key_dn));
    }
    /* s_ptrs 重指（排序中换了内容，指针数组按新序重建） */
    for (uint16_t i = 0; i < n; i++)
        s_ptrs[i] = s_disp_names[i];
    return n;
}

/* ==================== 烧录进度回调：增量刷进度条 + 只刷数字 ====================
 * 进度语义（V1.6.0）：0~95 编程 / 96~99 回读校验 / 100 完成 —— ≥96 时条上
 * 文字切「校验中」，操作者能看见验证环节在跑。 */

static void on_progress(uint8_t percent)
{
    /* 烧录是同步阻塞的（app_usb_poll 在主循环）——这里捎带消化 USB 环，
     * 让烧录期间 PC 发来的传输帧能及时收到 BUSY 拒绝（环不溢出、PC 不干等） */
    app_usb_poll();

    if (percent != s_last_progress) {
        /* 进度条：只填新增段（防闪烁）；随后透明 blit 补画状态字（不碰底图，
         * 填充从字底穿过）+ 劈半清底刷数字（V1.4.4：颜色与填充逐像素一致） */
        burn_bar_advance(s_last_progress, percent);
        draw_burn_bar_status((percent >= 96) ? 4 : 1);   /* 4=校验中 / 1=烧录中 */
        draw_burn_percent(percent);

        s_last_progress = percent;
        dev_lcd_tft_refresh(app_main_get_tft());
    }
}

/* ==================== 对外接口 ==================== */

void app_ui_init(void)
{
    /* 按键实例/初始化已统一在 app_main.c（app_main_get_btn_ok/up/down），此处不再 init */

    s_count = scan_opfp();
    s_sel = 0;
    s_page = PAGE_FILES;
    gen_log_info("UI: %u opfp file(s) on SD\n", (unsigned)s_count);
}

void app_ui_poll(void)
{
    uint8_t ev = button_event();

    /* ---- 事件 → 状态推进（变化才置脏）----
     * 两页流程：P1 文件列表 --确认--> P2 烧录页 --确认(待机)--> 执行烧录 --确认(结束)--> 回 P1
     * PA15 = 确认键（短按）；长按 = P1 进字体比对演示页（V2.2.4，调试用）。 */

    if (s_page == PAGE_FONT_DEMO) {
        /* 任意按键（短按/长按）退出回 P1 */
        if (ev == BTN_SHORT || ev == BTN_LONG) {
            s_page = PAGE_FILES;
            s_dirty = 1;
        }
    } else if (s_page == PAGE_FILES) {
        /* UP/DOWN 选文件 */
        int8_t ud = updown_event();
        int dir = 0;
        if (ud == 0)      dir = -1;
        else if (ud == 1) dir = +1;

        if (dir != 0 && s_count > 1) {
            uint16_t old = s_sel;
            if (dir < 0)
                s_sel = (s_sel == 0) ? (uint16_t)(s_count - 1) : (uint16_t)(s_sel - 1);
            else
                s_sel = (uint16_t)((s_sel + 1) % s_count);
            if (s_sel != old) {
                update_filelist_sel(s_ptrs, s_count, old, s_sel);
                dev_lcd_tft_refresh(app_main_get_tft());
                gen_log_info("UI: sel %u/%u %s\n", (unsigned)(s_sel + 1), (unsigned)s_count,
                             (const char *)s_ptrs[s_sel]);
            }
        }

        /* 确认（短按）：解析选中文件 → 进烧录页（待机） */
        if (ev == BTN_SHORT && s_count > 0) {
            char path[56];
            snprintf(path, sizeof(path), "0:%s", s_names[s_sel]);   /* 路径用真文件名（s_ptrs 可能是显示名）*/
            if (app_flash_parse(path, &s_info) == 0) {
                s_page = PAGE_BURN;
                s_burn_phase = 0;               /* 待机：按确认开始 */
                s_dirty = 1;
                gen_log_info("UI: -> burn page %s chip=%s fw=%lu\n", s_info.filename,
                             s_info.chip_model, (unsigned long)s_info.fw_size);
            } else {
                gen_log_err("UI: parse %s FAIL\n", path);
            }
        }

        /* 长按：进字体比对演示页（V2.2.4） */
        if (ev == BTN_LONG) {
            s_page = PAGE_FONT_DEMO;
            s_dirty = 1;
            gen_log_info("UI: -> font demo page\n");
        }
    } else {   /* PAGE_BURN */
        if (ev == BTN_SHORT) {
            if (s_burn_phase == 0) {            /* 待机 → 执行烧录 */
                char path[56];
                int r;
                snprintf(path, sizeof(path), "0:%s", s_names[s_sel]);   /* 路径用真文件名（s_ptrs 可能是显示名）*/

                s_burn_phase = 1;               /* 烧录中 */
                s_last_progress = 0;
                draw_burn_bar(0);
                draw_burn_bar_status(1);       /* 条上「烧录中」（此后只刷数字） */
                draw_burn_percent(0);
                dev_lcd_tft_refresh(app_main_get_tft());

                gen_log_info("UI: burn start\n");
                app_usb_set_busy(1);                 /* 烧录中：USB 传输帧忙拒 */
                r = app_flash_run2(path, on_progress);
                app_usb_set_busy(0);
                /* 结果 phase：0=成功（含回读校验通过）/ 12=校验失败 / 其他=烧录失败 */
                s_burn_phase = (r == 0) ? 2 : ((r == 12) ? 5 : 3);
                gen_log_info("UI: burn %s (%d)\n", r ? "FAIL" : "OK", r);

                /* 成功后刷新「次数」/「滚码」行（V1.12.0：计数已在 flash 层按
                 * nonce 键 +1，UI 只重查显示；滚码行=下一号 = 起始+新计数×步进） */
                if (r == 0) {
                    char v[16];
                    uint32_t burned = app_count_get(app_flash_count_key());
                    snprintf(v, sizeof(v), "%lu", (unsigned long)burned);
                    draw_burn_row(170, S_COUNT, v);
                    if (s_info.serial_on) {
                        uint32_t sn = s_info.serial_start + burned * s_info.serial_step;
                        snprintf(v, sizeof(v), "%lu@0x%06lX", (unsigned long)sn,
                                 (unsigned long)s_info.serial_addr & 0xFFFFFF);
                        draw_burn_row(114, S_ROLL, v);
                    }
                }

                /* 结果反馈：LED（成功绿/失败红）+ 蜂鸣器（成功 2.7kHz 短响1声 /
                 * 失败 2.7kHz 急促3连响），r 码见 app_flash.h（11=文件坏 12=校验不一致）*/
                app_main_burn_feedback(r == 0);

                /* 结果帧：不整页重画 —— 条整体变绿/红 + 条上结果文字 + 失败码 + 返回提示。
                 * （整页 clear 会全屏闪；此处 90% 画面未变，局部换即可）
                 * 2=绿「校验通过」 3=红「烧录失败」 5=红「校验失败」 */
                draw_burn_bar_status(s_burn_phase);
                {
                    if (r != 0) {
                        /* 失败码（ASCII，进度条下方）：产线据此查 app_flash.h 码表 */
                        char ec[24];
                        snprintf(ec, sizeof(ec), "ERR %d", r);
                        svc_display_tft_rect_t r3 = {{0, 200, DEV_LCD_TFT_WIDTH, UIF_TXT_H + 4}, 0, SVC_DISPLAY_TFT_EDGE_NO};
                        svc_display_tft_rect_text(&r3, (const uint8_t *)ec, &s_font_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                                  DEV_LCD_TFT_RED, DEV_LCD_TFT_BLACK, 1);
                    }
                    (void)0;
                }
                dev_lcd_tft_refresh(app_main_get_tft());
                return;
            } else if (s_burn_phase >= 2) {     /* 结束 → 回列表 */
                s_page = PAGE_FILES;
                s_count = scan_opfp();           /* 重扫（卡可能换过） */
                s_sel = 0;
                s_dirty = 1;
                app_main_feedback_idle();        /* LED 恢复待机（红亮绿灭）*/
            }
        }
    }

    /* ---- 渲染：只在脏时重画（静态页面画一次就停，见 §13.16 屏异常教训）---- */
    if (!s_dirty) return;
    s_dirty = 0;

    if (s_page == PAGE_FILES)
        draw_filelist(s_ptrs, s_count, s_sel);
    else if (s_page == PAGE_FONT_DEMO)
        draw_font_demo();
    else
        draw_burn_page(&s_info, s_burn_phase, 0);

    dev_lcd_tft_refresh(app_main_get_tft());
}
