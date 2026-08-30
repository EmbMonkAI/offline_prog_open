#ifndef __APP_MAIN_H
#define __APP_MAIN_H

/**
 * @file app_main.h
 * @brief 应用层入口
 *
 * 在 main() 的初始化段调用 app_main_init()，在主循环调用 app_main_loop()。
 */

/* ==================== 固件版本 ====================
 * 每次功能性修改递增（major.功能不兼容重构 / minor.新功能 / patch.修复）。
 * 上电时串口打印（app_main_init 开头）；排查"烧的是不是新固件"一眼可辨。
 * 版本史：
 *   V1.0.0  2026-08-16  TFT 三键脱机烧录全流程（文件选择→确认→烧录进度/结果）
 *   V1.1.0  2026-08-16  .opfp v4 显示名（列表/烧录页优先显示短名）
 *   V1.1.1  2026-08-16  修 v4 显示名文件的确认键路径 bug
 *   V1.2.0  2026-08-16  进度条增量填充防闪烁 + 固件版本号打印
 *   V1.2.1  2026-08-16  烧录中只刷百分比数字小块；结果帧局部换（不再整页刷）
 *   V1.3.0  2026-08-16  烧录页改版：第二行烧录名称；修「保护」值乱码（中文字体）；
 *                       状态并入进度条（成功整条绿/失败整条红）
 *   V1.4.0  2026-08-16  文件列表只认 v4（带显示名）——v3 旧文件不再显示（不兼容）
 *   V1.4.1  2026-08-16  文件选择标题栏右侧显示「当前/总数」序号（如 1/10）
 *   V1.4.2  2026-08-16  修进度条中空：文字/数字重画的底色跟随填充状态（青底黑字）
 *   V1.4.3  2026-08-16  根治进度条"双进度"：条上文字改透明 blit（不再用 rect_text
 *                       铺底色），填充从字底穿过，进度视觉连续
 *   V1.4.4  2026-08-16  数字区清底按填充右缘劈两半（青|黑），修快结束时
 *                       （填充右缘穿过数字区 ~85-95% 段）进度不一致
 *   V1.4.5  2026-08-28  TFT 硬件装配从 test_tft_hw.c 移入 app_main.c（分层归位，
 *                       test/=测试 sim/=PC模拟 硬件配置集中 app 层），功能等价
 *   V1.5.0  2026-08-28  PCB010-V3.0 板级适配：LCD(CS=PB0/DC=PC4/RST=PC5/BL=PC7)、
 *                       蜂鸣器 PC6(TIM3_CH1)、按键 SW_OK=PB14/UP=PA0/DOWN=PB13
 *                       （装配统一 app_main.c）、SWD 输出 SWDIO=PB7/SWCLK=PB6/RST=PB5、
 *                       日志改 USART2(PA2/PA3,115200)、杂项 PC0/PC1/PB1/PB2/PB10/
 *                       PB12/PB15 上拉输出（不兼容 V2 板）
 *   V1.5.1  2026-08-28  烧录结果反馈：LED（成功绿/失败红）+ 蜂鸣器（成功 2.7kHz
 *                       短响1声 / 失败 2.7kHz 连续短响3声），判断点在 app_ui 烧录结束处
 *   V1.5.2  2026-08-28  日志口切回 USART1（PA9/PA10；原 V1.5.0 曾切 USART2，实测改回）
 *   V1.6.0  2026-08-28  烧录可靠性闭环：.opfp 头 CRC32 校验（坏文件拒烧，码11）+
 *                       烧后回读验证（read_mem 逐块比对，目标重新上锁前，码12）+
 *                       编程/校验失败自动重试一次；失败码显示在结果页（ERR n）
 *   V1.7.0  2026-8-28  烧录计数持久化：内部 flash Sector4 顶部 append-only 记录
 *                       （256槽/压缩重写），烧录页新增「次数」行，成功后 +1 刷新
 *   V1.7.1  2026-8-29  count 修复：残留 flash 错误标志（PGAERR）堵死 HAL 入口——
 *                       清标志挪入解锁窗口内 + DCache 刷缓存 + 写后回读校验
 *   V1.8.0  2026-8-29  .opfp v5 适配：编程配置段 PGCF(28B) 解析（滚码/次数/
 *                       加密占位/镜像编号）；列表认 v5 文件 + 行首「NNN」编号块
 *                       （青底白字，多文件快速定位）；烧录页名称前缀编号
 *   V1.8.1  2026-8-29  修空显示名 v5 文件整文件消失（回退文件名进列表）；
 *                       烧录页参数 5→7 行：新增滚码（地址@起始）/限制（次数上限），
 *                       布局重排（名称 y25 / 参数 y43..138 / 进度条 y141..168）
 *   V1.8.2  2026-8-29  ①编号块 23→26px（3 位 ASCII 24px 被裁成「00」）；
 *                       ②PC 发送文件名强制 NNN- 前缀（编号唯一化防同名覆盖）；
 *                       ③只认 v5：v4/v3 旧文件列表不显示 + parse/run2 拒绝
 *   V1.8.3  2026-8-29  ①编号块青底白字→黄底黑字（对比不足看不清）；②文件名
 *                       x24→28（编号块 0..25，间隙 2px 不再拥挤）；③列表按编号
 *                       升序（FAT 目录序覆盖/删除后乱）；④USB 收同编号文件先删
 *                       旧（dedup_image_id：设备每号唯一，重发=更新该号镜像）
 *   V1.8.4  2026-8-29  烧录页名称行去编号前缀——与 PC 端输入的显示名完全一致
 *                       （编号只在列表行首显示）
  *   V2.1.3  2026-8-30  CMD_GET_VER(0x01)：PC 连接轮询查固件版本，连接显示
 *                       「设备已连接 V2.0.0 COMx」（旧固件回退仅 COM 号）
 *   V2.0.0  2026-8-30  【架构变更】加密下沉设备端：PC 生成明文 v6（零密码学），
 *                       厂家受控 USB 导入；烧录器接收后用 K_dev=AES(DEV_SALT,UID)
 *                       整文件加密落盘（ENC1+密文+CMAC）；烧录走 encf 透明解密层；
 *                       明文/CMAC 坏文件拒烧（码 4/17）。（V1.9~V1.13 版本史见
 *                       git 工作区历史/DEVELOPMENT.md：滚码·次数执行、nonce 授权、
 *                       账本证书、计数迁回 flash、v7 PC 加密等中间路线）
 */
#define FW_VERSION_MAJOR  2
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0
#define _STR(x)  _STR2(x)
#define _STR2(x) #x
#define FW_VERSION_STR    "V" _STR(FW_VERSION_MAJOR) "." _STR(FW_VERSION_MINOR) "." _STR(FW_VERSION_PATCH)

/* 初始化应用（装配 LED 设备等） */
void app_main_init(void);

/* 主循环业务（必须非阻塞，由 main 的 while(1) 周期性调用） */
void app_main_loop(void);

/* 烧录结果反馈：ok!=0 成功（绿灯+2.7kHz 短响1声）；ok==0 失败（红灯+2.7kHz 急促3连响）
 * 在烧录结束判断点调用（阻塞 ~120/260ms，与 UI 结果帧同步无妨） */
void app_main_burn_feedback(int ok);

/* 恢复待机 LED（红亮绿灭；回文件列表/新一次烧录前调） */
void app_main_feedback_idle(void);

/* TFT 设备指针（app_main_init 装配；app_ui 等模块渲染用） */
struct dev_lcd_tft_dev;
struct dev_lcd_tft_dev *app_main_get_tft(void);

/* 按键设备实例（app_main_init 已初始化；app_ui 只读，经 getter 获取）
 * SW_OK=PB14 / SW_UP=PA0 / SW_DOWN=PB13（PCB010-V3.0） */
#include "dev_button.h"   /* dev_button_dev_t（匿名 struct，无法前向声明） */
dev_button_dev_t *app_main_get_btn_ok(void);
dev_button_dev_t *app_main_get_btn_up(void);
dev_button_dev_t *app_main_get_btn_down(void);

#endif /* __APP_MAIN_H */
