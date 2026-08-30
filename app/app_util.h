#ifndef __APP_UTIL_H
#define __APP_UTIL_H

#include <stdint.h>

/**
 * @file app_util.h
 * @brief app 层公共小工具（跨模块共享，避免互相 include）
 */

/* CRC32（与 PC 端 zlib.crc32 / .opfp 生成器一致：poly 0xEDB88320 反射，初值 0）。
 * 用法：crc = app_util_crc32(crc, data, len) —— 可链式续算多段。
 * app_usb.c 的 USB 传输校验与 app_flash.c 的 .opfp 头校验共用本实现。 */
uint32_t app_util_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* __APP_UTIL_H */
