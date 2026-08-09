#ifndef DEV_FONT_H
#define DEV_FONT_H

/**
 * @file dev_font.h
 * @brief 字体数据读取设备驱动（设备层）
 *
 * 提供从文件系统读取字体点阵数据的能力，供 font_mgr 模块注入使用。
 * 字体文件名固定为 "kp_font_lib.bin"，支持多个候选路径自动搜索。
 *
 * 使用流程：
 *   1. dev_font_init()   — 搜索并打开字体文件
 *   2. dev_font_read()   — 按地址读取点阵数据（注入给 font_mgr_t.read）
 *   3. dev_font_deinit() — 关闭字体文件
 */
#include <stdint.h>

/**
 * @brief 初始化字体数据读取模块
 *
 * 在多个候选路径中搜索字体文件 "kp_font_lib.bin" 并打开。
 * 失败时不会中止程序，上层可通过 read 返回值感知。
 *
 * @return 0=成功，-1=字体文件未找到
 */
int dev_font_init(void);

/**
 * @brief 从字体文件中读取指定地址的数据
 *
 * 通常作为 font_mgr_t.read 函数指针的实现，调用方无需直接调用。
 *
 * @param addr 字体数据在文件中的偏移地址（字节）
 * @param buf  输出缓冲区
 * @param len  读取长度（字节）
 * @return 0=成功，1=文件未打开
 */
uint8_t dev_font_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief 关闭字体文件，释放资源
 */
void dev_font_deinit(void);

#endif /* DEV_FONT_H */
