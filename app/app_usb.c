/**
 * @file app_usb.c
 * @brief USB CDC 文件传输协议层（Phase 3）—— 见 app_usb.h
 */

#include "app_usb.h"
#include "app_util.h"        /* app_util_crc32（传输校验） */
#include "app_sec.h"         /* V1.11.4：导入即绑定 UID（账本重封） */
#include "opfp.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "ff.h"              /* FatFs：f_open/f_write/f_close */

/* CDC 发送（CubeMX 生成，usbd_cdc_if.c）。用 extern 避开跨目录 include 路径问题。*/
extern uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len);

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "usb"
#include "gen_log.h"

/* ===================== 协议常量 ===================== */
#define SYNC0          0x55U
#define SYNC1          0xAAU
#define T_BEGIN        0x01U
#define T_DATA         0x02U
#define T_EOF          0x03U
#define T_CMD          0x10U
#define T_ACK          0x81U
/* #define T_STATUS    0x90U  Phase 5 */

#define AT_BEGIN       0x01U
#define AT_DATA        0x02U
#define AT_EOF         0x03U

/* ACK status 码 */
#define STAT_OK        0x00U
#define STAT_BAD       0xF0U   /* 帧格式错 */
#define STAT_OPENFAIL  0xF1U   /* f_open 失败 */
#define STAT_WRFAIL    0xF3U   /* f_write 失败 */
#define STAT_CRC       0xF4U   /* EOF 校验：CRC/长度不符 */
#define STAT_BUSY      0xF5U   /* 烧录进行中，拒绝传输（PC 等待后重试） */
#define STAT_DEVBOUND  0xF6U   /* 文件账本绑定其他设备——整传输拒绝（V1.11.4） */

#define DATA_CHUNK_MAX 512U    /* DATA 帧数据上限（对齐 SD 扇区）*/
#define PAYLOAD_MAX    (4U + DATA_CHUNK_MAX + 16U)  /* seq(4)+data+余量 */
#define USB_FNAME_MAX  64U

/* ===================== 环形缓冲（SPSC：ISR 写，主循环读）===================== */
#define RX_RING_SIZE   2048U                       /* 2 的幂 */
#define RX_RING_MASK   (RX_RING_SIZE - 1U)
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static uint8_t  s_rx_ring[RX_RING_SIZE];
static volatile uint32_t s_rx_drop;                /* 溢出丢字节计数（诊断用）*/

/* ===================== 帧解析状态 ===================== */
typedef enum { PS_SYNC0, PS_SYNC1, PS_TYPE, PS_LEN0, PS_LEN1, PS_PAYLOAD } pstate_t;
static pstate_t s_ps;
static uint8_t  s_ptype;
static uint16_t s_plen;
static uint16_t s_phave;
static uint8_t  s_payload[PAYLOAD_MAX];

/* ===================== 传输状态 ===================== */
static FIL      s_fil;
static char     s_fname[USB_FNAME_MAX];
static char     s_fname_bak[USB_FNAME_MAX];   /* 备份名（FatFs f_open 会触碰 s_fname 首字节，用备份保命）*/
static uint32_t s_expect_size, s_expect_crc;
static uint32_t s_got, s_crc, s_seq;
static uint8_t  s_file_open;
static volatile uint8_t s_busy;                      /* 烧录中：拒绝传输帧 */

/* ===================== CRC32（共享实现，见 app_util.c）===================== */
#define crc32_update app_util_crc32

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void     wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

/* ===================== 发送（主循环上下文）===================== */
static uint8_t tx_frame(uint8_t type, const uint8_t *pl, uint16_t len)
{
    static uint8_t f[PAYLOAD_MAX + 8];
    f[0] = SYNC0; f[1] = SYNC1; f[2] = type; f[3] = (uint8_t)(len & 0xFFU); f[4] = (uint8_t)(len >> 8);
    if (len) memcpy(f + 5, pl, len);
    return CDC_Transmit_FS(f, (uint16_t)(5 + len));   /* DMA 关闭，同步拷进 FIFO，返回后 f 可复用 */
}

static void ack(uint8_t atype, uint32_t status)
{
    uint8_t pl[5];
    pl[0] = atype; wr32(pl + 1, status);
    tx_frame(T_ACK, pl, 5);
}

static void ack_eof(uint32_t status, uint32_t crc)
{
    uint8_t pl[9];
    pl[0] = AT_EOF; wr32(pl + 1, status); wr32(pl + 5, crc);
    tx_frame(T_ACK, pl, 9);
}

/* ===================== 帧处理 ===================== */

/* 编号唯一化（V1.8.3）：新文件名形如「NNN-xxx.opfp」（NNN=镜像编号，PC 端保证）。
 * 写入前删 SD 根目录所有同编号、不同名的 .opfp —— 设备上每号只留最新一份，
 * 「重发同号镜像」语义自然（PC 重生成编号 1 再发送 = 更新 1 号镜像）。
 * 非 NNN- 前缀文件名（异常来源）不清理，只防同名覆盖（CREATE_ALWAYS）。 */
static void dedup_image_id(const char *new_fname)
{
    DIR d;
    FILINFO fi;
    char pre[5];

    /* 前缀 = 前三字符 + '-'（"001-"）；不足 4 字符或无 '-' 视为无编号，不清理 */
    if (new_fname[0] < '0' || new_fname[0] > '9' ||
        new_fname[1] < '0' || new_fname[1] > '9' ||
        new_fname[2] < '0' || new_fname[2] > '9' || new_fname[3] != '-') {
        return;
    }
    memcpy(pre, new_fname, 4);
    pre[4] = 0;

    if (f_opendir(&d, "0:/") != FR_OK) return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
        const char *fn = fi.fname;
        size_t len = strlen(fn);
        if (len >= 5 && !(fi.fattrib & AM_DIR)
            && strncmp(fn, pre, 4) == 0 && strcmp(fn, new_fname) != 0) {
            char path[80];
            snprintf(path, sizeof(path), "0:%s", fn);
            if (f_unlink(path) == FR_OK)
                gen_log_info("usb: dedup: rm '%s' (same id as '%s')\n", fn, new_fname);
        }
    }
    f_closedir(&d);
}

static void on_begin(const uint8_t *pl, uint16_t len)
{
    uint8_t nl;
    uint32_t size, crc;
    char path[USB_FNAME_MAX + 2];
    FRESULT fr;

    if (len < 1U) { ack(AT_BEGIN, STAT_BAD); return; }
    nl = pl[0];
    if (nl == 0U || nl > USB_FNAME_MAX - 1U || len < (uint16_t)(1U + nl + 8U)) {
        ack(AT_BEGIN, STAT_BAD); return;
    }
    memcpy(s_fname, pl + 1, nl); s_fname[nl] = '\0';
    memcpy(s_fname_bak, pl + 1, nl); s_fname_bak[nl] = '\0';   /* V2.0.1 备份名 */
    size = rd32(pl + 1 + nl);
    crc  = rd32(pl + 1 + nl + 4);

    s_expect_size = size; s_expect_crc = crc;
    s_got = 0; s_crc = 0; s_seq = 0; s_file_open = 0;

    /* dedup 在 on_eof 做；f_open 后 s_fname 可能失效，后续一律用备份名 */
    snprintf(path, sizeof(path), "0:%s", s_fname);
    fr = f_open(&s_fil, (const TCHAR *)path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        gen_log_err("usb: BEGIN open '%s' FAIL (%d)\n", s_fname_bak, (int)fr);
        ack(AT_BEGIN, STAT_OPENFAIL);
        return;
    }
    s_file_open = 1;
    gen_log_info("usb: BEGIN '%s' size=%lu crc=%08lX\n", s_fname_bak,
                 (unsigned long)size, (unsigned long)crc);
    ack(AT_BEGIN, STAT_OK);
}

static void on_data(const uint8_t *pl, uint16_t len)
{
    uint32_t seq;
    UINT bw = 0;
    FRESULT fr;

    if (!s_file_open || len < 4U) { return; }   /* 未开文件/异常：丢弃（停等下 PC 会超时）*/
    seq = rd32(pl);
    fr = f_write(&s_fil, pl + 4, len - 4U, &bw);
    if (fr != FR_OK || bw != (UINT)(len - 4U)) {
        gen_log_err("usb: DATA seq=%lu write FAIL fr=%d bw=%u/%u\n",
                    (unsigned long)seq, (int)fr, (unsigned)bw, (unsigned)(len - 4U));
        f_close(&s_fil); s_file_open = 0;
        ack(AT_DATA, 0xDEAD0000U | (uint32_t)fr);   /* 高位标记 + FatFs FR 码，PC 解码打印 */
        return;
    }
    s_crc = crc32_update(s_crc, pl + 4, len - 4U);
    s_got += (len - 4U);
    s_seq++;
    ack(AT_DATA, seq);                           /* 回显 seq */
}

/* V2.0.0 设备端加密：传输完成且 CRC OK 时，若收的是 .opfp——
 * 整文件加密重写落盘（读明文 → AES-CTR(K_dev) 加密 → 新文件头 ENC1 +
 * 密文 + CMAC 重写）。SD 上只存本设备可解的密文：拷出 SD/换设备均无效；
 * PC 软件零密码学（反编译无密钥），导入由厂家受控完成（明文仅存在于
 * USB 线上一瞬）。非 .opfp 文件不动。
 * 返回 0=正常（含跳过），1=加密失败（文件保留明文，烧录时会因无 ENC 头拒绝）。 */
#define ENC_BUF_SZ 2048
static int encrypt_file_on_recv(void)
{
    FIL fp;
    /* V2.1.1 修【EOF 后复位】：buf 从栈改 static——本函数栈深 ~2.7KB（FIL 558B
     * + buf 2048B + 局部）叠在 on_eof→dispatch→app_usb_poll 调用链上撞栈底，
     * 尾部日志/close 时越界致复位。static 化后栈深 <300B（病根案卷任务#23）。 */
    static uint8_t buf[ENC_BUF_SZ];
    FRESULT fr;
    UINT br, bw;
    char path[USB_FNAME_MAX + 4];
    size_t len = strlen(s_fname_bak);               /* 备份名（s_fname[0] 会被 f_open 清零）*/
    uint32_t total, enc_nonce;
    uint8_t hdr16[16];

    if (len < 5 || strcasecmp(s_fname_bak + len - 5, ".opfp") != 0) {
        gen_log_info("usb: enc skip (not .opfp: '%s')\n", s_fname_bak);
        return 0;                                   /* 非烧录文件不碰 */
    }

    snprintf(path, sizeof(path), "0:%s", s_fname_bak);
    fr = f_open(&fp, path, FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        gen_log_err("usb: enc open FAIL (%d)\n", (int)fr);
        return 0;
    }

    total = (uint32_t)f_size(&fp);
    if (total < sizeof(opfp_header_t)) {
        gen_log_err("usb: enc skip (too small %lu)\n", (unsigned long)total);
        f_close(&fp); return 0;
    }

    /* 快速判已是密文（重发幂等：已是 ENC1 直接过——理论不会发生，防御） */
    f_lseek(&fp, 0);
    if (f_read(&fp, hdr16, 4, &br) == FR_OK && br == 4
        && memcmp(hdr16, "ENC1", 4) == 0) {
        gen_log_info("usb: already encrypted, skip\n");
        f_close(&fp);
        return 0;
    }

    enc_nonce = app_sec_enc_nonce_gen();

    /* 原地重写：先把明文整体后移 16B？FAT 不支持插入——改写法：
     * 从文件头开始，把明文块【倒序】加密回写偏移+16 处，最后写文件头。
     * 倒序避免覆盖未读数据。CMAC 需要全密文——先就地加密完成并流式算 CMAC
     * 不可行（CMAC 顺序处理），分两遍：一遍加密倒序搬移，一遍顺序算 CMAC。 */

    /* 遍 1：倒序块搬移+加密（密文写到 off+16，头 16B 留给 ENC 头） */
    {
        uint32_t done = 0;
        while (done < total) {
            uint32_t chunk = ENC_BUF_SZ;
            if (chunk > total - done) chunk = total - done;
            uint32_t src = total - done - chunk;    /* 本块明文起点 */
            f_lseek(&fp, src);
            if (f_read(&fp, buf, chunk, &br) != FR_OK || br != chunk) {
                gen_log_err("usb: enc read FAIL @%lu\n", (unsigned long)src);
                f_close(&fp);
                return 1;
            }
            app_sec_enc_crypt(buf, chunk, src, enc_nonce);
            f_lseek(&fp, src + 16);
            if (f_write(&fp, buf, chunk, &bw) != FR_OK || bw != chunk) {
                gen_log_err("usb: enc write FAIL @%lu\n", (unsigned long)src);
                f_close(&fp);
                return 1;
            }
            done += chunk;
        }
        /* 此时文件布局：[0..16)=旧明文残留（遍 2 覆盖），[16..16+total)=密文。
         * 文件长度 = 16+total（首块写到 16 起，若原 total<2048 则文件尾在
         * 16+total 处——由倒序写自然形成）。*/
    }

    /* 遍 2：顺序算密文 CMAC——V2.1.2 走共享流式实现（与验证端同一份代码）*/
    {
        cmac_stream_t cs;
        uint8_t tagc[16];
        uint32_t cmclen = total;

        app_sec_cmac_stream_begin(&cs, cmclen);
        f_lseek(&fp, 16);
        {
            uint32_t c = 0;
            while (c < cmclen) {
                uint32_t chunk = ENC_BUF_SZ;
                if (chunk > cmclen - c) chunk = cmclen - c;
                if (f_read(&fp, buf, chunk, &br) != FR_OK || br != chunk) {
                    f_close(&fp); return 1;
                }
                app_sec_cmac_stream_update(&cs, buf, chunk);
                c += chunk;
            }
        }
        app_sec_cmac_stream_end(&cs, tagc);

        /* CMAC 追加到密文尾（文件总长 = 16 + total + 16） */
        f_lseek(&fp, 16 + cmclen);
        if (f_write(&fp, tagc, 16, &bw) != FR_OK || bw != 16) { f_close(&fp); return 1; }
        if (f_sync(&fp) != FR_OK) { f_close(&fp); return 1; }

        /* 写 ENC1 文件头（V2.1.2 补——重构 CMAC 时被误删：文件头残留明文
         * 前 16B，验证端当明文解析 → 16B 后全乱 + header CRC mismatch）*/
        memset(hdr16, 0, 16);
        memcpy(hdr16, "ENC1", 4);
        hdr16[4] = (uint8_t)(enc_nonce >> 24); hdr16[5] = (uint8_t)(enc_nonce >> 16);
        hdr16[6] = (uint8_t)(enc_nonce >> 8);  hdr16[7] = (uint8_t)(enc_nonce);
        f_lseek(&fp, 0);
        if (f_write(&fp, hdr16, 16, &bw) != FR_OK || bw != 16) { f_close(&fp); return 1; }
        if (f_sync(&fp) != FR_OK) { f_close(&fp); return 1; }

    }

    gen_log_info("usb: encrypted on-device (%lu B -> %lu B, nonce %08lX)\n",
                 (unsigned long)total, (unsigned long)(total + 32),
                 (unsigned long)enc_nonce);
    f_close(&fp);
    return 0;
}

static void on_eof(void)
{
    uint8_t ok;
    if (s_file_open) { f_close(&s_fil); s_file_open = 0; }
    ok = (s_got == s_expect_size) && (s_crc == s_expect_crc);
    gen_log_info("usb: EOF got=%lu/%lu crc=%08lX/%08lX %s\n",
                 (unsigned long)s_got, (unsigned long)s_expect_size,
                 (unsigned long)s_crc, (unsigned long)s_expect_crc, ok ? "OK" : "MISMATCH");
    /* 传输成功：清同编号旧文件 + 设备端加密落盘（口令+UID 密钥，BEGIN 已解析）*/
    if (ok && s_fname_bak[0]) {
        dedup_image_id(s_fname_bak);
        if (encrypt_file_on_recv() != 0)
            gen_log_err("usb: encrypt-on-recv FAIL (file left plaintext)\n");
    }
    ack_eof(ok ? STAT_OK : STAT_CRC, s_crc);
}

static void on_cmd(const uint8_t *pl, uint16_t len);   /* 下文定义 */

static void dispatch(uint8_t type, const uint8_t *pl, uint16_t len)
{
    /* 烧录中：文件传输帧一律忙拒（SD 被 FATFS 占用非可重入；PC 收到 BUSY
     * 后等烧录结束再重试，而不是等 3s 超时 ×N）。CMD 不拒（保留控制通道）。*/
    if (s_busy && (type == T_BEGIN || type == T_DATA || type == T_EOF)) {
        uint8_t at = (type == T_BEGIN) ? AT_BEGIN : (type == T_DATA) ? AT_DATA : AT_EOF;
        ack(at, STAT_BUSY);
        return;
    }

    switch (type) {
        case T_BEGIN: on_begin(pl, len); break;
        case T_DATA:  on_data(pl, len);  break;
        case T_EOF:   on_eof();          break;
        case T_CMD:   on_cmd(pl, len);   break;
        default:      break;             /* 未知类型：忽略 */
    }
}

/* ===================== CMD 子命令（0x10）=====================
 * V2.1.3：CMD_GET_VER(0x01) —— PC 连接轮询时查询固件版本，回 16B：
 *   [cmd=0x01][版本 12B ASCII（FW_VERSION_STR \0 填充）][保留 3B]
 * PC 端「设备已连接 Vx.x.x」显示用。未知子命令回 BAD。 */
#define CMD_GET_VER   0x01U
static void on_cmd(const uint8_t *pl, uint16_t len)
{
    uint8_t sub;

    if (len < 1U) { ack(0x00U, STAT_BAD); return; }
    sub = pl[0];
    switch (sub) {
        case CMD_GET_VER: {
            /* 版本串经 app_main.h 的 FW_VERSION_STR（"V2.0.0" 形态）*/
            extern const char *app_fw_version_str(void);
            uint8_t rpl[16];
            memset(rpl, 0, sizeof(rpl));
            rpl[0] = CMD_GET_VER;
            {
                const char *v = app_fw_version_str();
                uint8_t n = 0;
                while (v[n] && n < 11) { rpl[1 + n] = (uint8_t)v[n]; n++; }
            }
            tx_frame(T_ACK, rpl, sizeof(rpl));
            break;
        }
        default:
            ack(0x00U, STAT_BAD);
            break;
    }
}

/* ===================== 环消费 + 帧解析（主循环）===================== */
static int16_t ring_get(void)
{
    uint16_t h, t;
    h = s_rx_head; t = s_rx_tail;
    if (h == t) return -1;
    {
        uint8_t b = s_rx_ring[t];
        s_rx_tail = (uint16_t)((t + 1U) & RX_RING_MASK);
        return b;
    }
}

void app_usb_poll(void)
{
    int16_t bv;
    int budget = RX_RING_SIZE;   /* 单次最多消化整环，防止异常帧死循环 */

    if (s_rx_drop) {                             /* 诊断：偶发丢字节（停等下应为 0）*/
        uint32_t d = s_rx_drop; s_rx_drop = 0;
        gen_log_err("usb: rx ring dropped %lu bytes\n", (unsigned long)d);
    }

    while (budget-- > 0 && (bv = ring_get()) >= 0) {
        uint8_t b = (uint8_t)bv;
        switch (s_ps) {
            case PS_SYNC0:
                if (b == SYNC0) s_ps = PS_SYNC1;
                break;
            case PS_SYNC1:
                if (b == SYNC1) { s_ps = PS_TYPE; s_phave = 0; s_plen = 0; }
                else if (b == SYNC0) { /* 连续 0x55，留在 SYNC1 */ }
                else s_ps = PS_SYNC0;
                break;
            case PS_TYPE:
                s_ptype = b; s_ps = PS_LEN0;
                break;
            case PS_LEN0:
                s_plen = b; s_ps = PS_LEN1;
                break;
            case PS_LEN1:
                s_plen |= (uint16_t)(b << 8);
                if (s_plen > PAYLOAD_MAX) {
                    gen_log_err("usb: frame len %u > max, resync\n", s_plen);
                    s_ps = PS_SYNC0;             /* 超长：丢帧重同步 */
                } else if (s_plen == 0U) {
                    dispatch(s_ptype, s_payload, 0);
                    s_ps = PS_SYNC0;
                } else {
                    s_ps = PS_PAYLOAD;
                }
                break;
            case PS_PAYLOAD:
                s_payload[s_phave++] = b;
                if (s_phave == s_plen) {
                    dispatch(s_ptype, s_payload, s_plen);
                    s_ps = PS_SYNC0;
                }
                break;
        }
    }
}

/* ===================== 环生产（IRQ 上下文，CDC_Receive_FS 调用）===================== */
void app_usb_rx_push(const uint8_t *data, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((s_rx_head + 1U) & RX_RING_MASK);
        if (next == s_rx_tail) { s_rx_drop++; break; }   /* 满：丢（停等下不会发生）*/
        s_rx_ring[s_rx_head] = data[i];
        s_rx_head = next;
    }
}

void app_usb_init(void)
{
    s_rx_head = 0; s_rx_tail = 0; s_rx_drop = 0;
    s_ps = PS_SYNC0; s_phave = 0; s_plen = 0; s_ptype = 0;
    s_file_open = 0; s_got = 0; s_crc = 0; s_seq = 0;
    s_expect_size = 0; s_expect_crc = 0;
    s_busy = 0;
    app_sec_keystr_init();      /* V2.1：恢复持久化口令（跨重启密钥一致）*/
    gen_log_info("usb: ready (CDC rx ring %uB)\n", RX_RING_SIZE);

}

void app_usb_set_busy(uint8_t busy)
{
    s_busy = busy;
    if (busy) {
        /* 若传输进行到一半被打断（DATA 收了一半）：关掉半开文件，EOF 时 PC
         * 已收到 BUSY，会整传输重试（BEGIN 重新 f_open CREATE_ALWAYS 幂等） */
        if (s_file_open) { f_close(&s_fil); s_file_open = 0; }
    }
}
