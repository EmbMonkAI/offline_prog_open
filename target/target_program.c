/**
 * @file target_program.c
 * @brief 通用烧录流程（RDP 参数从芯片对象取，方案 B 数据化）
 */

#include "target.h"
#include "SWD_host.h"

extern void delaymS(uint32_t ms);

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "tgt"
#include "gen_log.h"

static uint32_t s_programmed;

error_t target_program_begin(target_session_t *s, uint32_t fw_size)
{
    const target_chip_t     *chip = s->chip;
    const program_target_t  *algo = chip->algo;
    uint32_t off;

    s_programmed = 0;

    /* 0. 越界校验：固件不得大于 flash 总大小（防擦除越过 flash 末尾）*/
    if (fw_size > chip->flash_size) {
        gen_log_err("tgt: fw %lu > flash %lu\n",
                    (unsigned long)fw_size, (unsigned long)chip->flash_size);
        return ERROR_SIZE;
    }
    gen_log_info("tgt: page=%u bufsz=%u\n", (unsigned)chip->page_size, (unsigned)algo->program_buffer_size);

    /* 1. 连接 */
    if (!s->ops->connect()) {
        gen_log_err("tgt: connect FAIL\n");
        return ERROR_RESET;
    }

    /* 2. RDP 检测 + 解锁（参数从 chip->rdp 取）*/
    if (chip->rdp.type != 0) {
        if (app_rdp_is_protected(&chip->rdp)) {
            gen_log_info("tgt: read-protected, unlocking (mass-erase)...\n");
            if (app_rdp_unlock(&chip->rdp) != 0) {
                gen_log_err("tgt: RDP unlock FAIL\n");
                return ERROR_UNLOCK;
            }
            /* RDP 解锁会 mass-erase + AIRCR 复位，等目标恢复 */
            delaymS(100);
        }
    }

    /* 3. halt + 下载 algo + init */
    gen_log_info("tgt: halt+algo...\n");
    if (!swd_set_target_state_hw(RESET_PROGRAM)) {
        gen_log_err("tgt: RESET_PROGRAM FAIL\n");
        return ERROR_RESET;
    }
    gen_log_info("tgt: download algo %lu B...\n", (unsigned long)algo->algo_size);
    if (!swd_write_memory(algo->algo_start, (uint8_t *)algo->algo_blob, algo->algo_size)) {
        gen_log_err("tgt: algo download FAIL\n");
        return ERROR_ALGO_DL;
    }
    gen_log_info("tgt: algo init...\n");
    if (!swd_flash_syscall_exec(&algo->sys_call_s, algo->init, chip->flash_start, 0, 0, 0)) {
        gen_log_err("tgt: algo init FAIL\n");
        return ERROR_INIT;
    }

    /* 4. 擦除 */
    gen_log_info("tgt: erasing %u B (page=%u)...\n", (unsigned)fw_size, (unsigned)chip->page_size);
    for (off = 0; off < fw_size; off += chip->page_size) {
        if (!swd_flash_syscall_exec(&algo->sys_call_s, algo->erase_sector, chip->flash_start + off, 0, 0, 0))
            return ERROR_ERASE_SECTOR;
    }

    /* 擦除验证：抽几点读 flash 看是否 0xFF（排查 erase 假成功）*/
    {
        static const uint32_t chk_off[] = {0x0000, 0x3400, 0x8000};
        uint8_t buf[16]; int i, j;
        for (i = 0; i < 3; i++) {
            uint32_t a = chip->flash_start + chk_off[i];
            if (a < chip->flash_start + fw_size + chip->page_size) {
                if (swd_read_memory(a, buf, 16)) {
                    uint8_t m = 0; for (j = 0; j < 16; j++) m |= buf[j];
                    gen_log_info("tgt: erase chk @0x%08lX =0x%02X %s\n",
                                 (unsigned long)a, m, (m == 0xFF) ? "OK" : "NOT-ERASED!");
                }
            }
        }
    }

    return ERROR_SUCCESS;
}

error_t target_program_page(target_session_t *s, const uint8_t *data, uint32_t len)
{
    const target_chip_t     *chip = s->chip;
    const program_target_t  *algo = chip->algo;
    uint32_t addr = chip->flash_start + s_programmed;
    uint32_t remaining = len;

    gen_log_info("tgt: prog @0x%08lX len=%u\n", (unsigned long)addr, (unsigned)len);
    while (remaining > 0) {
        uint32_t chunk = (remaining > algo->program_buffer_size) ? algo->program_buffer_size : remaining;
        if (!swd_write_memory(algo->program_buffer, (uint8_t *)data, chunk)) return ERROR_ALGO_DATA_SEQ;
        if (!swd_flash_syscall_exec(&algo->sys_call_s, algo->program_page,
                                     addr, algo->program_buffer_size, algo->program_buffer, 0))
            return ERROR_WRITE;
        addr += chunk; data += chunk; remaining -= chunk;
    }

    s_programmed += len;
    return ERROR_SUCCESS;
}

void target_program_end(target_session_t *s)
{
    s->ops->disconnect();
    gen_log_info("tgt: DONE (%u B)\n", (unsigned)s_programmed);
}
