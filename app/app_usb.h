/**
 * @file app_usb.h
 * @brief USB CDC 文件传输协议层（Phase 3）
 *
 * 数据流：PC → CDC bulk OUT → CDC_Receive_FS(IRQ) → 环形缓冲 → app_usb_poll(主循环)
 *         → 帧解析 → f_write 写 SD → ACK 经 CDC bulk IN 回 PC。
 *
 * 协议帧（CDC 流式，帧头同步 0x55 0xAA）：
 *   [0x55][0xAA][type:u8][len:u16 LE][payload:len]
 *   PC→MCU: BEGIN(0x01)=name_len:u8,name[],size:u32,crc32:u32
 *           DATA (0x02)=seq:u32,bytes[≤512]
 *           EOF  (0x03)=空
 *           CMD  (0x10)=cmd:u8        (保留，Phase 5 中止等)
 *   MCU→PC: ACK  (0x81)=ack_type:u8,status:u32
 *           STATUS(0x90)=...          (Phase 5 进度)
 *   停等协议：PC 发一帧等一个 ACK 再发下一帧（简单可靠，环不会溢出）。
 */

#ifndef __APP_USB_H
#define __APP_USB_H

#include <stdint.h>

/* 初始化（FATFS 挂载后、主循环前调用）。复位解析状态。*/
void    app_usb_init(void);

/* 主循环调用：消化环形缓冲字节，驱动协议状态机（开文件/写SD/EOF校验/回ACK）。*/
void    app_usb_poll(void);

/* 烧录互斥（app_ui 在烧录前后调用）：
 * busy=1 期间收到 BEGIN/DATA/EOF 一律回 STAT_BUSY(0xF5) 拒绝——PC 端立即
 * 得知忙而非等超时；数据也不落 SD（烧录中 SD 正被读，FATFS 非可重入）。*/
void    app_usb_set_busy(uint8_t busy);

/* CDC 接收回调（CDC_Receive_FS 在 IRQ 上下文调用）：字节压入环形缓冲。
 * 必须极快——只 memcpy+推指针，禁做 FATFS/解析/日志（FATFS 非可重入，解析耗时）。*/
void    app_usb_rx_push(const uint8_t *data, uint32_t len);

#endif /* __APP_USB_H */
