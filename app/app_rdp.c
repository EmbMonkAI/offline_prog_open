/**
 * @file app_rdp.c
 * @brief 目标读保护(RDP)处理 — 开源版桩实现
 *
 * 开源版保留完整接口（app_rdp.h），不包含读保护自动解锁的实现。
 * 普通芯片（未设读保护）烧录不受任何影响；目标若开启读保护，
 * 本模块返回检测成功但不执行解锁，烧录流程将以明确错误码终止。
 *
 * 完整的 RDP 自动解锁（F1 选项字节 / F4 OPTCR 数据化方案）在闭源版本中维护，
 * 原理与实现过程见配套课程与视频讲解——欢迎在视频评论区讨论你的芯片型号。
 */

#include "app_rdp.h"
#include <stdint.h>

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "rdp"
#include "gen_log.h"

/* 检查目标是否设了读保护。
 * 开源版：不执行实际的 RDP 状态探测，保守报告"未检测到保护"，
 * 让后续烧录步骤以标准的连接/擦除错误自然暴露（而非在此拦截）。 */
int app_rdp_is_protected(const rdp_params_t *p)
{
    (void)p;
    return 0;
}

/* 解除读保护（闭源版会整片擦除目标并解锁）。
 * 开源版：恒返回失败，调用方据此终止烧录并给出明确提示。 */
int app_rdp_unlock(const rdp_params_t *p)
{
    (void)p;
    gen_log_info("rdp: open-source build: RDP unlock not included\n");
    gen_log_info("rdp: see video comments for protected-chip discussion\n");
    return 1;
}
