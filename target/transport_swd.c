/**
 * @file transport_swd.c
 * @brief SWD 传输层 —— 把 dap/SWD_host 薄包装成 transport_ops_t
 *
 * 不改 SWD_host 内部，只做签名适配。未来加 UART ISP 时实现一份 uart_ops 即可。
 */

#include "target.h"
#include "SWD_host.h"

static uint8_t swd_connect(void)
{
    return swd_init_debug();
}

static uint8_t swd_disconnect(void)
{
    swd_set_target_state_hw(RUN);
    return 1;
}

static uint8_t swd_write_mem(uint32_t addr, const uint8_t *data, uint32_t size)
{
    return swd_write_memory(addr, (uint8_t *)data, size);
}

static uint8_t swd_read_mem(uint32_t addr, uint8_t *data, uint32_t size)
{
    return swd_read_memory(addr, data, size);
}

const transport_ops_t swd_ops = {
    .connect    = swd_connect,
    .disconnect = swd_disconnect,
    .write_mem  = swd_write_mem,
    .read_mem   = swd_read_mem,
};
