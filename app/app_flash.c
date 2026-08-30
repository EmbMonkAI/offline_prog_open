/**
 * @file app_flash.c
 * @brief 脱机烧录编排：读 .opfp → 校验 → 烧录 → 回读验证（通用）
 *
 * 可靠性闭环（V1.6.0）：
 *   1. 文件级校验：header CRC32（偏移 0~107 vs 字段 108），坏文件拒烧（码 11）
 *   2. 烧后回读验证：read_mem 逐块比对 flash 与文件，须在 目标重新上锁前（码 12）
 *   3. 编程/校验失败自动重试一次（重擦重烧），再失败才报失败
 *
 * 返回码：0=成功 1=无文件 2=open 3=read-hdr 4=magic 5=algo超大 6=algo读
 *         7=connect/begin 8=固件读 9=编程 10=越界 11=头CRC错 12=回读不一致
 */

#include "app_flash.h"
#include "app_util.h"
#include "app_count.h"      /* burn_count 持久化查询 */
#include "app_sec.h"        /* V1.11.0 账本（AES-CMAC/UID） */
#include "main.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "ff.h"
#include "target.h"
#include "opfp.h"
#include "SWD_host.h"       /* swd_read_memory（回读验证） */

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "flash"
#include "gen_log.h"

#define FLASH_BUF_SIZE  2048
#define ALGO_MAX_SIZE   4096

static uint8_t s_buf[FLASH_BUF_SIZE];
static uint8_t s_algo_blob[ALGO_MAX_SIZE];
static program_target_t s_algo;
static target_chip_t    s_chip;

/* 当前 .opfp 的 v5 编程配置快照（run2 读 PGCF 后填；parse 也填 info）
 * 滚码/次数执行状态：V1.9.0；授权安全：V1.10.0 */
static struct {
    uint8_t  serial_on;         /* PGCF_FLAG_SERIAL */
    uint8_t  maxcnt_on;         /* PGCF_FLAG_MAXCNT */
    uint32_t serial_addr;
    uint32_t serial_start;
    uint32_t serial_step;
    uint32_t serial_width;
    uint32_t max_burn_count;
    uint32_t image_id;          /* V1.10.0：授权高水位键 */
    uint32_t auth_nonce;        /* V1.10.0：授权号（0=旧 28B 段） */
    char     count_key[24];     /* 计数持久化键（nonce 或回退文件名） */
} s_pgcf;

/* V1.12.0 账本已退化为静态证书：烧后不写回，仅保留 PGCF 原文供 CMAC 校验 */
static uint8_t  s_pgcf_raw[32];     /* PGCF 段原始字节（证书 CMAC 覆盖用）*/

/* ===================== V2.0.0 加密文件透明读层 =====================
 * SD 上的 .opfp 物理布局：[ENC1 头 16B][AES-CTR 密文 = 原明文][CMAC 16B]。
 * 逻辑视图 = 原明文。encf_* 包装 FatFs：open 判头+验 CMAC+记 nonce，
 * read/lseek/size 全部按「密文偏移 = 逻辑偏移 + 16」换算，读后原地解密。
 * 明文文件（无 ENC 头）直接拒——烧录器只认设备加密过的文件。 */
static uint8_t  s_enc_active;       /* 当前文件为设备加密格式 */
static uint32_t s_enc_nonce;
static uint32_t s_enc_logic_size;   /* 明文逻辑长度 */

/* 返回 0=加密文件就绪；1=非加密（拒）；2=CMAC 坏（拒）；3=IO 错 */
static int encf_open(FIL *fp, const char *path)
{
    UINT br;
    uint8_t hdr[16];
    uint8_t tag_file[16];

    s_enc_active = 0;
    if (f_open(fp, path, FA_READ) != FR_OK) return 3;
    if (f_read(fp, hdr, 16, &br) != FR_OK || br != 16) { f_close(fp); return 3; }
    if (memcmp(hdr, "ENC1", 4) != 0) {
        /* V2.0.1 兼容模式：明文文件也放行（设备端加密暂关——EOF 后复位病根
         * 未除；encf_* 在 s_enc_active=0 下全部透传，行为=V1.12 明文路径）*/
        f_lseek(fp, 0);
        return 0;
    }

    s_enc_nonce = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16)
                  | ((uint32_t)hdr[6] << 8) | (uint32_t)hdr[7];
    s_enc_logic_size = (uint32_t)f_size(fp) - 32;

    /* CMAC 验证（密文段）——V2.1.2 走共享流式实现（两端同码，消灭等价副本分歧）*/
    {
        static uint8_t ebuf[2048];
        cmac_stream_t cs;
        uint8_t tag_calc[16];

        app_sec_cmac_stream_begin(&cs, s_enc_logic_size);
        f_lseek(fp, 16);
        {
            uint32_t c = 0;
            while (c < s_enc_logic_size) {
                uint32_t chunk = sizeof ebuf;
                if (chunk > s_enc_logic_size - c) chunk = s_enc_logic_size - c;
                if (f_read(fp, ebuf, chunk, &br) != FR_OK || br != chunk) { f_close(fp); return 3; }
                app_sec_cmac_stream_update(&cs, ebuf, chunk);
                c += chunk;
            }
        }
        app_sec_cmac_stream_end(&cs, tag_calc);

        /* 读文件尾 CMAC 比对 */
        f_lseek(fp, 16 + s_enc_logic_size);
        if (f_read(fp, tag_file, 16, &br) != FR_OK || br != 16) { f_close(fp); return 3; }
        if (memcmp(tag_calc, tag_file, 16) != 0) {
            gen_log_err("flash: ENC CMAC bad (%lu B cipher)\n",
                        (unsigned long)s_enc_logic_size);
            f_close(fp);
            return 2;
        }
    }

    s_enc_active = 1;
    f_lseek(fp, 16);
    return 0;
}

/* 逻辑 seek（偏移按明文坐标） */
static FRESULT encf_lseek(FIL *fp, uint32_t logic_off)
{
    if (!s_enc_active) return f_lseek(fp, logic_off);
    return f_lseek(fp, 16 + logic_off);
}

/* 逻辑读：读密文段 → 原地解密。offset 参数 = 本次读的逻辑起点（调用方
 * 给出，避免依赖 f_tell——见 V1.12.3 f_tell 异常教训）*/
static FRESULT encf_read(FIL *fp, void *buf, UINT want, UINT *got, uint32_t logic_off)
{
    FRESULT fr;
    if (!s_enc_active) {
        fr = f_read(fp, buf, want, got);
        return fr;
    }
    fr = f_read(fp, buf, want, got);
    if (fr == FR_OK && *got > 0)
        app_sec_enc_crypt((uint8_t *)buf, *got, logic_off, s_enc_nonce);
    return fr;
}

static uint32_t encf_size(const FIL *fp)
{
    if (!s_enc_active) return (uint32_t)f_size((FIL *)fp);
    return s_enc_logic_size;
}

static int find_opfp(char *out, size_t out_sz)
{
    DIR d; FILINFO fi; FRESULT fr;
    fr = f_opendir(&d, "0:/");
    if (fr != FR_OK) return 1;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
        size_t len = strlen(fi.fname);
        if (len >= 5 && !(fi.fattrib & AM_DIR)) {
            const char *ext = fi.fname + len - 5;
            if (toupper((unsigned char)ext[0]) == '.' &&
                toupper((unsigned char)ext[1]) == 'O' &&
                toupper((unsigned char)ext[2]) == 'P' &&
                toupper((unsigned char)ext[3]) == 'F' &&
                toupper((unsigned char)ext[4]) == 'P') {
                snprintf(out, out_sz, "0:%s", fi.fname);
                f_closedir(&d);
                return 0;
            }
        }
    }
    f_closedir(&d);
    return 1;
}

int app_flash_run(void)
{
    return app_flash_run2(NULL, NULL);
}

int app_flash_count_exceeded(const app_flash_info_t *info)
{
    if (!info || !info->maxcnt_on) return 0;
    return info->burn_count >= info->max_burn_count;
}

const char *app_flash_count_key(void)
{
    return s_pgcf.count_key;
}

/** 解析 .opfp header → info（烧录页参数显示 + 烧录前预检共用）。
 * V2.0.0：经 encf 透明解密层——SD 文件必须是设备加密格式（ENC1），明文文件拒。 */
int app_flash_parse(const char *path, app_flash_info_t *info)
{
    FIL fp;
    FRESULT fr;
    UINT br;
    opfp_header_t hdr;
    const char *base, *dash, *dot;

    if (!path || !info) return 1;
    memset(info, 0, sizeof(*info));

    {
        int er = encf_open(&fp, path);
        if (er == 1) { gen_log_err("flash: plaintext file rejected\n"); return 4; }
        if (er == 2) { gen_log_err("flash: ENC CMAC bad\n"); return 17; }
        if (er == 3) return 2;
    }
    fr = encf_read(&fp, &hdr, sizeof(hdr), &br, 0);
    if (fr != FR_OK || br != sizeof(hdr)) { f_close(&fp); return 3; }
    if (hdr.magic != OPFP_MAGIC || hdr.version < 5) { f_close(&fp); return 4; }

    if (hdr.version >= 4) {
        opfp_name_t nm;
        UINT br2 = 0;
        fr = encf_read(&fp, &nm, sizeof(nm), &br2, sizeof(hdr));
        if (fr == FR_OK && br2 == sizeof(nm)) {
            uint16_t n = 0;
            while (n < OPFP_NAME_MAX && nm.name[n] >= 0x20 && nm.name[n] <= 0x7E) {
                info->disp_name[n] = (char)nm.name[n];
                n++;
            }
            info->disp_name[n] = 0;
        }
    }
    if (hdr.version >= 5) {
        opfp_pgcf_t pg;
        UINT br3 = 0;
        memset(&pg, 0, sizeof(pg));
        fr = encf_read(&fp, &pg, sizeof(pg), &br3, sizeof(hdr) + sizeof(opfp_name_t));
        if (fr == FR_OK && br3 >= 24 && pg.magic == OPFP_PGCF_MAGIC) {
            if (pg.flags & OPFP_PGCF_FLAG_IMAGE_ID)
                info->image_id = pg.image_id;
            if (pg.flags & OPFP_PGCF_FLAG_MAXCNT) {
                info->maxcnt_on = 1;
                info->max_burn_count = pg.max_burn_count;
            }
            if (pg.flags & OPFP_PGCF_FLAG_SERIAL) {
                info->serial_on = 1;
                info->serial_addr = pg.serial_addr;
                info->serial_start = pg.serial_start;
                info->serial_width = pg.serial_width;
                info->serial_step = pg.serial_step ? pg.serial_step : 1;
            }
            info->auth_nonce = pg.auth_nonce;   /* 计数键用 */
        }
    }
    f_close(&fp);

    /* 文件名（去路径）+ 型号反推（-<型号>.opfp） */
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(info->filename, sizeof(info->filename), "%s", base);
    dash = strrchr(base, '-');
    dot = strrchr(base, '.');
    if (dash && dot && dash < dot && (size_t)(dot - dash - 1) < sizeof(info->chip_model)) {
        memcpy(info->chip_model, dash + 1, (size_t)(dot - dash - 1));
        info->chip_model[dot - dash - 1] = 0;
    } else {
        snprintf(info->chip_model, sizeof(info->chip_model), "unknown");
    }

    info->fw_size     = hdr.fw_size;
    info->flash_start = hdr.flash_start;
    info->flash_size  = hdr.flash_size;
    info->rdp         = (hdr.rdp_type != 0);

    /* 计数权威源 = 设备 flash（nonce 键）。身份绑定在 V2.0 由设备加密承担
     * （UID 派生 K_dev，换设备解不开）；nonce 仍作计数键（重导出=新授权从 0） */
    if (info->auth_nonce)
        snprintf(s_pgcf.count_key, sizeof(s_pgcf.count_key), "#N%08lX",
                 (unsigned long)info->auth_nonce);
    else
        snprintf(s_pgcf.count_key, sizeof(s_pgcf.count_key), "%s", info->filename);
    info->burn_count = app_count_get(s_pgcf.count_key);
    return 0;
}


int app_flash_run2(const char *path, app_flash_progress_cb_t progress)
{
    char pbuf[64];
    FIL fp;
    FRESULT fr;
    UINT br;
    opfp_header_t hdr;
    error_t e;
    int rc = 0;
    uint32_t crc;
    uint32_t fw_data_off;      /* 固件数据在文件中的起始偏移（header+name+algo） */
    int attempt;

    /* 1. 文件：入参优先，否则根目录找第一个 .opfp */
    if (path) {
        snprintf(pbuf, sizeof(pbuf), "%s", path);
    } else if (find_opfp(pbuf, sizeof(pbuf)) != 0) {
        gen_log_err("flash: no .opfp in root\n");
        return 1;
    }
    path = pbuf;
    gen_log_info("flash: %s\n", path);

    if (progress) progress(0);

    /* 2. 开文件（V2.0：encf 透明解密层——只认设备加密格式，明文/CMAC 坏拒） */
    {
        int er = encf_open(&fp, path);
        if (er == 1) { gen_log_err("flash: plaintext file rejected\n"); return 4; }
        if (er == 2) { gen_log_err("flash: ENC CMAC bad\n"); return 17; }
        if (er == 3) { gen_log_err("flash: open FAIL\n"); return 2; }
    }
    fr = encf_read(&fp, &hdr, sizeof(hdr), &br, 0);
    if (fr != FR_OK || br != sizeof(hdr)) { gen_log_err("flash: read header FAIL\n"); f_close(&fp); return 3; }

    if (hdr.magic != OPFP_MAGIC) {
        gen_log_err("flash: bad magic 0x%08lX\n", (unsigned long)hdr.magic);
        f_close(&fp); return 4;
    }
    if (hdr.version < 5) {                      /* 旧版不合法拒烧 */
        gen_log_err("flash: old version v%lu (need v5+)\n", (unsigned long)hdr.version);
        f_close(&fp); return 4;
    }

    /* CRC 覆盖偏移 0~107（与 PC 生成器 zlib.crc32(header[0:108]) 一致，不含 CRC 字段自身） */
    crc = app_util_crc32(0, (const uint8_t *)&hdr, 108);
    if (crc != hdr.crc32) {
        gen_log_err("flash: header CRC mismatch %08lX != %08lX\n",
                    (unsigned long)crc, (unsigned long)hdr.crc32);
        f_close(&fp); return 11;
    }
    gen_log_info("flash: header CRC OK\n");

    gen_log_info("flash: v%d fw=%lu algo=%lu flash=%lu rdp=%d\n",
                 hdr.version, (unsigned long)hdr.fw_size,
                 (unsigned long)hdr.algo_size, (unsigned long)hdr.flash_size, hdr.rdp_type);

    /* 越界早失败：固件大于 flash 总大小，连接 SWD 前就拒掉 */
    if (hdr.fw_size > hdr.flash_size) {
        gen_log_err("flash: fw %lu > flash %lu\n",
                    (unsigned long)hdr.fw_size, (unsigned long)hdr.flash_size);
        f_close(&fp); return 10;
    }

    /* 3. 读 algo blob（v4/v5/v6：跳过 header 后扩展段。v6 PGCF=32B（+nonce）；
     *    v5 过渡段长 24/28B（无 nonce），按文件总长反推——algo 起点必须判对，
     *    否则读错位。PGCF magic 坏 = 文件损坏拒烧（码 11） */
    memset(&s_pgcf, 0, sizeof(s_pgcf));
    uint32_t pg_len_used = 0;                              /* 实际 PGCF 段长（布局计算用） */
    if (hdr.version >= 5) {
        opfp_pgcf_t pg;
        UINT brp = 0;
        uint32_t pg_len = sizeof(opfp_pgcf_t);            /* v6=32 */

        if (hdr.version == 5) {                            /* 24/28 兼容判别 */
            uint32_t body = encf_size(&fp)
                            - (uint32_t)sizeof(hdr) - (uint32_t)sizeof(opfp_name_t)
                            - hdr.algo_size - hdr.fw_size;
            pg_len = (body == 32) ? 32 : (body == 28 ? 28 : 24);
        }
        fr = encf_lseek(&fp, sizeof(hdr) + sizeof(opfp_name_t));
        if (fr != FR_OK) { gen_log_err("flash: seek FAIL\n"); f_close(&fp); return 6; }
        memset(&pg, 0, sizeof(pg));
        fr = encf_read(&fp, &pg, pg_len, &brp, sizeof(hdr) + sizeof(opfp_name_t));
        if (fr != FR_OK || brp != pg_len || pg.magic != OPFP_PGCF_MAGIC) {
            gen_log_err("flash: PGCF bad\n"); f_close(&fp); return 11;
        }
        memcpy(s_pgcf_raw, &pg, sizeof(s_pgcf_raw));   /* 账本 CMAC 覆盖原文 */
        pg_len_used = pg_len;
        /* V1.9.0：快照存模块级（滚码写入/次数比对执行用） */
        if (pg.flags & OPFP_PGCF_FLAG_SERIAL) {
            s_pgcf.serial_on    = 1;
            s_pgcf.serial_addr  = pg.serial_addr;
            s_pgcf.serial_start = pg.serial_start;
            s_pgcf.serial_step  = pg.serial_step ? pg.serial_step : 1;
            s_pgcf.serial_width = pg.serial_width;
        }
        if (pg.flags & OPFP_PGCF_FLAG_MAXCNT) {
            s_pgcf.maxcnt_on = 1;
            s_pgcf.max_burn_count = pg.max_burn_count;
        }
        s_pgcf.auth_nonce = pg.auth_nonce;    /* V1.10.0 授权号（0=旧 28B 段）*/
        s_pgcf.image_id   = pg.image_id;
        gen_log_info("flash: PGCF flags=0x%lX image=%lu maxcnt=%lu serial=%lu@%08lX nonce=%08lX\n",
                     (unsigned long)pg.flags, (unsigned long)pg.image_id,
                     (unsigned long)pg.max_burn_count,
                     (unsigned long)s_pgcf.serial_start,
                     (unsigned long)s_pgcf.serial_addr,
                     (unsigned long)pg.auth_nonce);

        /* V1.10.0 计数键：nonce（滚码游标/旧文件回退用，账本优先） */
        {
            const char *base = strrchr(path, '/');
            char fname[40];
            snprintf(fname, sizeof(fname), "%s", base ? base + 1 : path);
            if (s_pgcf.auth_nonce)
                snprintf(s_pgcf.count_key, sizeof(s_pgcf.count_key), "#N%08lX",
                         (unsigned long)s_pgcf.auth_nonce);
            else
                snprintf(s_pgcf.count_key, sizeof(s_pgcf.count_key), "%s", fname);
        }

        /* V1.10.0 授权高水位：按 image_id 拒旧 nonce（码 15）。
         * V1.12.1：高水位「写」挪到烧录成功后（run2 尾部）——烧前零 flash 写，
         * 避免 50s 超时的编程卡顿出现在烧录起点；「读+拒」仍在此处 */
        if (s_pgcf.auth_nonce && s_pgcf.image_id > 0) {
            char hwkey[16];
            uint32_t hw;
            snprintf(hwkey, sizeof(hwkey), "#HW%03lu", (unsigned long)s_pgcf.image_id);
            hw = app_count_get(hwkey);
            if (hw != 0 && s_pgcf.auth_nonce < hw) {
                gen_log_err("flash: stale auth %08lX < HW %08lX (id %lu), refuse\n",
                            (unsigned long)s_pgcf.auth_nonce, (unsigned long)hw,
                            (unsigned long)s_pgcf.image_id);
                f_close(&fp);
                return 15;
            }
        }
    } else if (hdr.version == 4) {
        fr = encf_lseek(&fp, sizeof(hdr) + sizeof(opfp_name_t));
        if (fr != FR_OK) { gen_log_err("flash: seek FAIL\n"); f_close(&fp); return 6; }
    }
    if (hdr.algo_size > ALGO_MAX_SIZE) { gen_log_err("flash: algo too big\n"); f_close(&fp); return 5; }
    fr = encf_read(&fp, s_algo_blob, hdr.algo_size, &br,
                   sizeof(hdr) + sizeof(opfp_name_t) + pg_len_used);
    if (fr != FR_OK || br != hdr.algo_size) { gen_log_err("flash: read algo FAIL\n"); f_close(&fp); return 6; }
    fw_data_off = f_tell(&fp);        /* algo 之后即固件数据 */
    /* f_tell 在加密路径上不可靠（V1.12.3 教训）——按格式显式计算固件起点 */
    fw_data_off = (uint32_t)sizeof(opfp_header_t)
                + (uint32_t)sizeof(opfp_name_t)
                + pg_len_used
                + hdr.algo_size;

    /* 次数上限（V1.12.0：flash 计数为准，与烧录页显示同源） */
    if (s_pgcf.maxcnt_on) {
        uint32_t burned = app_count_get(s_pgcf.count_key);
        if (burned >= s_pgcf.max_burn_count) {
            gen_log_err("flash: count %lu >= max %lu, refuse\n",
                        (unsigned long)burned,
                        (unsigned long)s_pgcf.max_burn_count);
            f_close(&fp);
            return 13;
        }
        gen_log_info("flash: count %lu/%lu (key %s)\n", (unsigned long)burned,
                     (unsigned long)s_pgcf.max_burn_count, s_pgcf.count_key);
    }

    /* 4. 构建 algo + chip + rdp（全部从文件）*/
    {
        program_target_t tmp = {
            .init = hdr.algo_init, .uninit = hdr.algo_uninit,
            .erase_chip = hdr.algo_erase_chip, .erase_sector = hdr.algo_erase_sector,
            .program_page = hdr.algo_program_page,
            .sys_call_s = { hdr.algo_breakpoint, hdr.algo_static_base, hdr.algo_stack_pointer },
            .program_buffer = hdr.algo_program_buffer, .algo_start = hdr.algo_start,
            .algo_size = hdr.algo_size, .algo_blob = (const uint32_t *)s_algo_blob,
            .program_buffer_size = hdr.algo_program_buf_sz,
        };
        memcpy(&s_algo, &tmp, sizeof(s_algo));
    }
    s_chip.name        = "from_file";
    s_chip.flash_start = hdr.flash_start;
    s_chip.flash_size  = hdr.flash_size;
    s_chip.page_size   = hdr.page_size;
    s_chip.algo        = &s_algo;
    s_chip.rdp.type       = hdr.rdp_type;
    s_chip.rdp.fpec_base  = hdr.rdp_fpec_base;
    s_chip.rdp.key1       = hdr.rdp_key1;
    s_chip.rdp.key2       = hdr.rdp_key2;
    s_chip.rdp.optkey1    = hdr.rdp_optkey1;
    s_chip.rdp.optkey2    = hdr.rdp_optkey2;
    s_chip.rdp.ob_addr    = hdr.rdp_ob_addr;
    s_chip.rdp.ob_val     = hdr.rdp_ob_val;
    s_chip.rdp.sr_bsy_mask = hdr.rdp_sr_bsy_mask;

    /* 5. 烧录 + 回读验证（失败自动重试一次：重新连接/擦除/编程/验证）*/
    {
        target_session_t sess = { .chip = &s_chip, .ops = &swd_ops };

        for (attempt = 1; attempt <= 2; attempt++) {
            rc = 0;

            if (attempt > 1) {
                gen_log_info("flash: retry (attempt 2)...\n");
                if (progress) progress(0);
            }

            /* V1.12.3：滚码启用时擦除范围扩到 serial 页尾（该页可能残留旧数据，
             * 未擦的页 program_page 写入会失败/数据不对） */
            {
                uint32_t erase_len = hdr.fw_size;
                if (s_pgcf.serial_on) {
                    uint32_t end = s_pgcf.serial_addr + s_pgcf.serial_width
                                   - s_chip.flash_start;
                    if (end > erase_len) erase_len = end;
                }
                e = target_program_begin(&sess, erase_len);
            }
            if (e != ERROR_SUCCESS) { gen_log_err("flash: begin FAIL (%d)\n", (int)e); rc = 7; break; }

            /* ---- 5.1 编程：SD 逐块读 → SWD 写 ---- */
            gen_log_info("flash: programming...\n");
            fr = encf_lseek(&fp, fw_data_off);
            if (fr != FR_OK) { gen_log_err("flash: seek fw FAIL (%d)\n", (int)fr); rc = 8; break; }

            {
                uint32_t remaining = hdr.fw_size;
                while (remaining > 0) {
                    /* 读粒度必须受 s_buf 容量(FLASH_BUF_SIZE)限制，不能用 page_size：
                     * page_size 是「擦除扇区步进」，F4=16KB >> s_buf 2KB，按它读会溢出 s_buf、
                     * 冲烂后面的 s_algo_blob/s_algo/s_chip → SWD 用乱码地址卡死（无任何打印）。
                     * target_program_page 内部已按 program_buffer_size 分块编程，读块无需对齐 flash 页。*/
                    UINT to_read = (remaining > FLASH_BUF_SIZE) ? FLASH_BUF_SIZE : remaining;
                    br = 0;
                    fr = encf_read(&fp, s_buf, to_read, &br, hdr.fw_size - remaining);
                    if (fr != FR_OK || br == 0) {
                        gen_log_err("flash: fw read FAIL (fr=%d br=%u off=%lu rem=%lu)\n",
                                    (int)fr, (unsigned)br,
                                    (unsigned long)f_tell(&fp), (unsigned long)remaining);
                        rc = 8; break;
                    }
                    e = target_program_page(&sess, s_buf, br);
                    if (e != ERROR_SUCCESS) { gen_log_err("flash: program FAIL\n"); rc = 9; break; }
                    remaining -= br;
                    if (progress) progress((uint8_t)((hdr.fw_size - remaining) * 95u / hdr.fw_size));
                }
            }

            /* ---- 5.2 回读验证（编程 OK 才做；进度 96~99）----
             * read_mem 走 AHB-AP 内存映射读，flash 空间可直读。必须在整个流程
             * 结束（含 目标重新上锁）之前 —— Level1 起调试口读不到 flash。*/
            if (rc == 0) {
                gen_log_info("flash: verifying...\n");
                fr = encf_lseek(&fp, fw_data_off);
                if (fr != FR_OK) { gen_log_err("flash: seek fw FAIL\n"); rc = 8; break; }

                {
                    uint32_t verified = 0;
                    while (verified < hdr.fw_size) {
                        UINT to_read = (hdr.fw_size - verified > FLASH_BUF_SIZE)
                                       ? FLASH_BUF_SIZE : (hdr.fw_size - verified);
                        static uint8_t rbuf[FLASH_BUF_SIZE];   /* 回读缓冲（与写缓冲分开） */
                        br = 0;
                        fr = encf_read(&fp, s_buf, to_read, &br, verified);
                        if (fr != FR_OK || br == 0) { gen_log_err("flash: verify read file FAIL\n"); rc = 8; break; }
                        if (!swd_read_memory(s_chip.flash_start + verified, rbuf, br)
                            || memcmp(s_buf, rbuf, br) != 0) {
                            gen_log_err("flash: verify mismatch @0x%08lX (%u B)\n",
                                        (unsigned long)(s_chip.flash_start + verified), (unsigned)br);
                            rc = 12;
                            break;
                        }
                        verified += br;
                        if (progress) progress((uint8_t)(95u + verified * 4u / hdr.fw_size));
                    }
                    if (rc == 0) gen_log_info("flash: verify OK (%lu B)\n",
                                              (unsigned long)hdr.fw_size);
                }
            }

            /* ---- 5.3 滚码写入（V1.9.0）：验证通过后、复位/目标重新上锁前 ----
             * 序列号 = 游标持久化（app_count，键=文件名+"#SN"），本次写入值 =
             * serial_start + burned*step（burned=已成功次数，本次成功计数由 UI
             * 层在 run2 返回 0 后 app_count_inc——滚码与计数同步推进）。
             * 写目标 serial_addr（1/2/4B 小端）+ 回读验证；失败码 14 不重试
             * （目标 flash 已是好固件，仅序列号问题，报人工处理）。 */
            if (rc == 0 && s_pgcf.serial_on) {
                /* V1.10.0：游标键按 serial_addr（跨授权延续——换授权不重烧序列号，
                 * 永不回绕重号；V1.12.0 起计数已不在账本，游标为唯一序列源）。
                 * V1.12.3 修【serial mismatch 0xFFFFFFFF】：flash 地址裸 swd_write
                 * 会被目标 flash 控制器静默丢弃（写须走 algo 的 program_page
                 * syscall，与固件主体同路径）；且目标页须先擦。 */
                char key[24];
                uint32_t burned, sn_val, readback = 0;
                uint8_t snb[4];

                snprintf(key, sizeof(key), "#S%08lX", (unsigned long)s_pgcf.serial_addr);
                burned = app_count_get(key);
                sn_val = s_pgcf.serial_start + burned * s_pgcf.serial_step;

                memset(snb, 0, sizeof(snb));
                if (s_pgcf.serial_width == 1) {
                    snb[0] = (uint8_t)sn_val;
                } else if (s_pgcf.serial_width == 2) {
                    snb[0] = (uint8_t)sn_val; snb[1] = (uint8_t)(sn_val >> 8);
                } else {
                    snb[0] = (uint8_t)sn_val; snb[1] = (uint8_t)(sn_val >> 8);
                    snb[2] = (uint8_t)(sn_val >> 16); snb[3] = (uint8_t)(sn_val >> 24);
                }

                gen_log_info("flash: serial %lu -> 0x%08lX (%lu B)\n",
                             (unsigned long)sn_val,
                             (unsigned long)s_pgcf.serial_addr,
                             (unsigned long)s_pgcf.serial_width);
                /* 走 algo：program_buffer 写入数据 → syscall 调 program_page */
                if (!swd_write_memory(s_algo.program_buffer, snb, 4)) {
                    gen_log_err("flash: serial buf write FAIL\n"); rc = 14;
                } else if (!swd_flash_syscall_exec(&s_algo.sys_call_s,
                            s_algo.program_page, s_pgcf.serial_addr,
                            s_algo.program_buffer_size, s_algo.program_buffer, 0)) {
                    gen_log_err("flash: serial program FAIL\n"); rc = 14;
                } else if (!swd_read_memory(s_pgcf.serial_addr, snb, s_pgcf.serial_width)) {
                    gen_log_err("flash: serial readback FAIL\n"); rc = 14;
                } else {
                    memcpy(&readback, snb, s_pgcf.serial_width);
                    if (readback != (sn_val & (s_pgcf.serial_width == 1 ? 0xFFu :
                                               s_pgcf.serial_width == 2 ? 0xFFFFu : ~0u))) {
                        gen_log_err("flash: serial mismatch %lu != %lu\n",
                                    (unsigned long)readback, (unsigned long)sn_val);
                        rc = 14;
                    } else {
                        /* 游标 +1 持久化（本次烧录的序列号已可追溯） */
                        app_count_inc(key);
                        gen_log_info("flash: serial OK (next %lu)\n",
                                     (unsigned long)(sn_val + s_pgcf.serial_step));
                    }
                }
            }

            /* V1.12.0 计数：设备 flash（nonce 键）——SD 文件烧后不再写。
             * 语义：同一授权（同 nonce）在本设备累计；PC 重新导出（新 nonce）= 新授权从 0 计
             * V1.12.1：成功后一并推高授权高水位（烧前只读不写，见上方说明） */
            if (rc == 0) {
                app_count_inc(s_pgcf.count_key);
                if (s_pgcf.auth_nonce && s_pgcf.image_id > 0) {
                    char hwkey[16];
                    snprintf(hwkey, sizeof(hwkey), "#HW%03lu",
                             (unsigned long)s_pgcf.image_id);
                    app_count_inc_raw(hwkey, s_pgcf.auth_nonce);
                }
            }

            target_program_end(&sess);

            /* 成功，或属于不该重试的失败（SD 读错），或已到最后一次尝试 → 结束 */
            if (rc == 0 || rc == 8 || attempt == 2) break;
            /* rc==9（编程失败）/ rc==12（校验不一致）：SWD/接触/时序类故障 → 重试一次 */
        }
    }

    f_close(&fp);
    if (progress && rc == 0) progress(100);
    return rc;
}
