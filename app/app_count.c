/**
 * @file app_count.c
 * @brief 烧录计数持久化实现（内部 flash append-only，见 app_count.h 设计说明）
 */

#include "app_count.h"
#include "app_util.h"        /* CRC（记录有效性） */
#include "main.h"            /* HAL flash API */
#include <string.h>

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "count"
#include "gen_log.h"

/* ---- 存储区：Sector4 顶部 4KB（0x0801F000），256 条 × 16B ---- */
#define COUNT_BASE        0x0801F000u
#define COUNT_SLOT_SIZE   16u
#define COUNT_SLOT_N      256u                      /* 4KB / 16B */
#define COUNT_MAGIC       0x434E5431u               /* "CNT1" */

/* 记录：4B magic + 4B name_hash + 4B count + 4B crc（crc 覆盖前 12B） */
typedef struct {
    uint32_t magic;
    uint32_t hash;
    uint32_t count;
    uint32_t crc;
} count_rec_t;

/* ---- 文件名散列（FNV-1a，简单稳定，不追求密码学） ---- */
static uint32_t name_hash(const char *name)
{
    uint32_t h = 0x811C9DC5u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 0x01000193u;
    }
    return h;
}

static int rec_valid(const count_rec_t *r)
{
    return (r->magic == COUNT_MAGIC)
        && (r->crc == app_util_crc32(0, (const uint8_t *)r, 12));
}

static int rec_empty(const count_rec_t *r)
{
    /* 空槽 = 全 0xFF（擦除态） */
    const uint8_t *p = (const uint8_t *)r;
    for (uint32_t i = 0; i < sizeof(*r); i++)
        if (p[i] != 0xFF) return 0;
    return 1;
}

/** 扫描：返回 {该 hash 最新记录的 count, 首个空槽下标}。
 *  empty=N 表示无空槽（写满，需压缩）。 */
static uint32_t scan_latest(uint32_t hash, uint32_t *empty_slot)
{
    const count_rec_t *base = (const count_rec_t *)COUNT_BASE;
    uint32_t latest = 0;
    uint32_t i;

    for (i = 0; i < COUNT_SLOT_N; i++) {
        if (rec_empty(&base[i])) break;              /* 之后全空 */
        if (!rec_valid(&base[i])) continue;          /* 坏记录跳过（掉电半写） */
        if (base[i].hash == hash && base[i].count > latest)
            latest = base[i].count;
    }
    *empty_slot = i;                                /* i==N 表示满 */
    return latest;
}

uint32_t app_count_get(const char *filename)
{
    uint32_t empty;
    if (!filename) return 0;
    return scan_latest(name_hash(filename), &empty);
}

/* 编程前清 flash 粘滞错误标志（F4 errata：标志写 1 清除，外部 flashloader
 * ——Keil/JLink 下载——常残留 PGAERR 等，堵死 HAL_FLASH_Program 入口）*/
static void flash_err_flags_clear(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

static int app_count_set(uint32_t hash, uint32_t value);

int app_count_inc_raw(const char *key, uint32_t value)
{
    /* 高水位写入：仅当 value > 现值才追加记录（单调不回退，不浪费槽位） */
    uint32_t empty;
    if (!key) return 1;
    if (value <= scan_latest(name_hash(key), &empty))
        return 0;                       /* 不比现值大：无需写 */
    return app_count_set(name_hash(key), value);
}

/* 内部：写一条 {hash, value} 记录（inc=cur+1 与 raw=value 共用追加/压缩路径） */
static int app_count_set(uint32_t hash, uint32_t value)
{
    const count_rec_t *base = (const count_rec_t *)COUNT_BASE;
    uint32_t empty;
    count_rec_t rec = {
        .magic = COUNT_MAGIC,
        .hash  = hash,
        .count = value,
    };
    rec.crc = app_util_crc32(0, (const uint8_t *)&rec, 12);
    HAL_StatusTypeDef st;
    scan_latest(hash, &empty);

    /* ---- 满了：压缩重写（每 hash 只留最新值，然后从槽 0 重记） ---- */
    if (empty >= COUNT_SLOT_N) {
        FLASH_EraseInitTypeDef ei;
        uint32_t err = 0;
        uint32_t seen[64];                          /* 最多 64 个不同文件 */
        uint32_t nseen = 0;

        gen_log_info("count: compacting...\n");

        /* 收集每个 hash 的最新值 */
        memset(seen, 0, sizeof(seen));
        uint32_t vals[64];
        for (uint32_t i = 0; i < COUNT_SLOT_N && nseen < 64; i++) {
            if (rec_empty(&base[i])) break;
            if (!rec_valid(&base[i])) continue;
            uint32_t j;
            for (j = 0; j < nseen; j++)
                if (seen[j] == base[i].hash) {
                    if (base[i].count > vals[j]) vals[j] = base[i].count;
                    break;
                }
            if (j == nseen) { seen[nseen] = base[i].hash; vals[nseen] = base[i].count; nseen++; }
        }

        /* 擦 Sector4（固件在 Sector0-3，不受影响） */
        HAL_FLASH_Unlock();
        ei.TypeErase    = FLASH_TYPEERASE_SECTORS;
        ei.Banks        = FLASH_BANK_1;
        ei.Sector       = FLASH_SECTOR_4;
        ei.NbSectors    = 1;
        ei.VoltageRange = FLASH_VOLTAGE_RANGE_2;    /* x32 编程对应 2.4-2.7V 档（VTref 实测 2.45V）*/
        st = HAL_FLASHEx_Erase(&ei, &err);
        HAL_FLASH_Lock();
        if (st != HAL_OK) {
            gen_log_err("count: erase FAIL\n");
            return 1;
        }

        /* 重写（不含本次 +1 的记录，下面统一追加；x32 字编程，同追加路径） */
        for (uint32_t j = 0; j < nseen; j++) {
            count_rec_t r = { .magic = COUNT_MAGIC, .hash = seen[j], .count = vals[j] };
            r.crc = app_util_crc32(0, (const uint8_t *)&r, 12);
            const uint32_t *p32 = (const uint32_t *)&r;
            HAL_FLASH_Unlock();
            flash_err_flags_clear();                  /* 同上：解锁窗口内清残留标志 */
            for (uint32_t w = 0; w < sizeof(r) / 4u; w++)
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, COUNT_BASE + j * COUNT_SLOT_SIZE + w * 4u, p32[w]);
            HAL_FLASH_Lock();
        }
        FLASH_FlushCaches();   /* F4 DCache 不随编程失效：刷缓存，否则读回旧值 */
        empty = nseen;
        /* 重取最新值（压缩后 cur 可能已被上面收集逻辑更新过 —— vals 已含最新，
         * 本函数开头算的 cur 是扫描值，一致；直接用 cur+1 落在 empty 槽） */
    }

    /* ---- 追加一条（x32 字编程 ×4；32F4 flash 只能 1→0，追加到空槽安全。
     * 不用 x64 双字：RM0090 并行度-电压表 x64 需 VDD≥2.7V，本板 JLink 实测
     * VTref≈2.45V —— x64 实测报 PGA|PGS(0xA)；x32 在 2.4V+ 全程合法） ---- */
    {
        const uint32_t *p32 = (const uint32_t *)&rec;
        uint32_t addr = COUNT_BASE + empty * COUNT_SLOT_SIZE;
        uint32_t sr0, cr0;                            /* 编程前现场（HAL 会清标志） */

        HAL_FLASH_Unlock();
        flash_err_flags_clear();                      /* 必须在解锁窗口内：LOCK 下 SR 写无效 */
        sr0 = FLASH->SR; cr0 = FLASH->CR;
        for (uint32_t w = 0; w < sizeof(rec) / 4u; w++) {
            st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + w * 4u, p32[w]);
            if (st != HAL_OK) break;
        }
        HAL_FLASH_Lock();
        if (st != HAL_OK) {
            /* 解码：st 1=错误标志 3=BSY超时；halerr位 0x02=PGS序列 0x04=PGP并行度
             * 0x08=PGA对齐 0x10=WRP写保护；sr_pre=编程前现场，sr_post=失败后现场 */
            gen_log_err("count: program FAIL st=%d halerr=0x%lX sr_pre=0x%lX cr_pre=0x%lX sr_post=0x%lX @0x%08lX\n",
                        (int)st, (unsigned long)HAL_FLASH_GetError(),
                        (unsigned long)sr0, (unsigned long)cr0,
                        (unsigned long)FLASH->SR, (unsigned long)addr);
            return 2;
        }
        FLASH_FlushCaches();   /* F4 DCache 不随编程失效：刷缓存，否则读回旧值 */
        /* 回读验证：真读到新记录才算数（串口日志可辨：编程失败/成功） */
        if (memcmp((const void *)(COUNT_BASE + empty * COUNT_SLOT_SIZE), &rec, sizeof(rec)) != 0) {
            gen_log_err("count: readback FAIL @slot %lu\n", (unsigned long)empty);
            return 3;
        }
    }

    gen_log_info("count: h%08lX -> %lu\n", (unsigned long)hash, (unsigned long)value);
    return 0;
}

int app_count_inc(const char *filename)
{
    if (!filename) return 1;
    uint32_t hash = name_hash(filename);
    uint32_t empty;
    uint32_t cur = scan_latest(hash, &empty);
    return app_count_set(hash, cur + 1);
}
