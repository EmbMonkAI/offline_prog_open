/**
 * @file app_sec.c
 * @brief AES-128/CMAC/CTR/UID/账本 实现（见 app_sec.h 设计说明）
 *
 * AES：经典 256B S-box 实现，仅加密方向（CMAC 与 CTR 都只要加密）。
 * 代码量 ~1.5KB，F401 软实现每块 ~µs 级，账本 56B 每次烧录一次，无压力。
 * 主密钥编译期注入（SEC_KEY_SRC），见下方 master_key()。
 */

#include "app_sec.h"
#include "main.h"          /* SysTick（nonce 生成）*/
#include <string.h>
#include <stdio.h>

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "sec"
#include "gen_log.h"

/* ===================== V2.1 设备端全文件加密（口令+UID 双因子）=====================
 * 密钥来源：用户口令字符串（PC 随 BEGIN 帧下发，≤32B）+ 本机 UID。
 *   K0 = 口令字节循环填充 16B（每字节 ^ 位置，短口令混淆）
 *   K1 = AES(K0, K0)（口令加强）
 *   K_dev = AES(K1, UID||0^4)（设备绑定——换设备解不开）
 * 口令空 = 默认口令 "DONECHIP"。换口令 = 已落盘密文作废（重新导入即可）。 */
#define KEYSTR_DEFAULT "DONECHIP"
static char     s_keystr[33];          /* 当前口令（BEGIN 帧设置，EOF 加密用）*/

/* 口令持久化：Sector5 基部 0x08020000（独立 16KB 扇区——固件 Sector0-3、
 * 计数 Sector4，互不干扰）。布局：magic "KEYS"(4B)+len(1B)+口令(≤32B)+0xFF。
 * 上电恢复——否则「导入时口令 A、重启后按 OK 时口令归默认」→ CMAC 错位。 */
#define KEYSTORE_ADDR  0x08020000u
#define KEYSTORE_MAGIC 0x5359454Bu   /* "KEYS" 小端读出 */

typedef struct {
    uint32_t magic;
    uint8_t  len;
    char     ks[32];
} keystore_t;

void app_sec_keystr_init(void)
{
    const keystore_t *st = (const void *)KEYSTORE_ADDR;
    if (st->magic == KEYSTORE_MAGIC && st->len > 0 && st->len <= 32) {
        memcpy(s_keystr, st->ks, st->len);
        s_keystr[st->len] = 0;
        gen_log_info("sec: keystr restored (%lu B) from flash\n",
                     (unsigned long)st->len);
    } else {
        strncpy(s_keystr, KEYSTR_DEFAULT, sizeof(s_keystr) - 1);
        s_keystr[sizeof(s_keystr) - 1] = 0;
        gen_log_info("sec: keystr default (flash empty)\n");
    }
}

void app_sec_set_keystr(const char *ks, uint32_t len)
{
    const keystore_t *st = (const void *)KEYSTORE_ADDR;
    int same;

    if (!ks || len == 0 || len > 32) {
        strncpy(s_keystr, KEYSTR_DEFAULT, sizeof(s_keystr) - 1);
        s_keystr[sizeof(s_keystr) - 1] = 0;
        return;
    }
    memcpy(s_keystr, ks, len);
    s_keystr[len] = 0;

    /* 与 flash 记录相同则不写（省擦写）；不同则擦 Sector5 重烧（含口令变更）*/
    same = (st->magic == KEYSTORE_MAGIC && st->len == (uint8_t)len
            && memcmp(st->ks, ks, len) == 0);
    if (!same) {
        FLASH_EraseInitTypeDef ei;
        uint32_t err = 0;
        uint8_t img[40];
        uint32_t w;
        HAL_StatusTypeDef hst;

        memset(img, 0xFF, sizeof(img));
        memcpy(img, &(uint32_t){KEYSTORE_MAGIC}, 4);
        img[4] = (uint8_t)len;
        memcpy(img + 5, ks, len);

        HAL_FLASH_Unlock();
        if (st->magic == KEYSTORE_MAGIC) {          /* 旧记录：先擦 */
            ei.TypeErase    = FLASH_TYPEERASE_SECTORS;
            ei.Banks        = FLASH_BANK_1;
            ei.Sector       = FLASH_SECTOR_5;
            ei.NbSectors    = 1;
            ei.VoltageRange = FLASH_VOLTAGE_RANGE_2;
            hst = HAL_FLASHEx_Erase(&ei, &err);
            if (hst != HAL_OK) {
                HAL_FLASH_Lock();
                gen_log_err("sec: keystore erase FAIL\n");
                return;
            }
        }
        for (w = 0; w < sizeof(img) / 4u; w++) {    /* 逐字（0xFF 字跳过）*/
            uint32_t v;
            memcpy(&v, img + w * 4, 4);
            if (v != 0xFFFFFFFFu)
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  KEYSTORE_ADDR + w * 4u, v);
        }
        HAL_FLASH_Lock();
        FLASH_FlushCaches();
        gen_log_info("sec: keystr stored (%lu B)\n", (unsigned long)len);
    }
}

/* ===================== F401 UID ===================== */
/* RM0368: 96-bit UID @ 0x1FFF7A10（X/Y/WAF 各 16bit 打包为 3×32bit）*/
void app_sec_get_uid(uint8_t uid[12])
{
    const uint32_t *p = (const uint32_t *)0x1FFF7A10u;
    for (int i = 0; i < 3; i++) {
        uid[i * 4 + 0] = (uint8_t)(p[i] >> 24);
        uid[i * 4 + 1] = (uint8_t)(p[i] >> 16);
        uid[i * 4 + 2] = (uint8_t)(p[i] >> 8);
        uid[i * 4 + 3] = (uint8_t)(p[i]);
    }
}

/* ===================== AES-128（加密方向）===================== */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xt(uint8_t x)            /* GF(2^8) 乘 2 */
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void key_expand(uint8_t rk[176], const uint8_t key[16])
{
    memcpy(rk, key, 16);
    for (uint32_t i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {              /* RotWord + SubWord + Rcon */
            uint8_t tmp = t[0];
            t[0] = SBOX[t[1]];
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[tmp];
            uint8_t rc = 1;
            for (uint32_t r = 1; r < i / 16; r++) rc = xt(rc);
            t[0] ^= rc;
        }
        rk[i + 0] = rk[i - 16 + 0] ^ t[0];
        rk[i + 1] = rk[i - 16 + 1] ^ t[1];
        rk[i + 2] = rk[i - 16 + 2] ^ t[2];
        rk[i + 3] = rk[i - 16 + 3] ^ t[3];
    }
}

void app_sec_aes128(uint8_t out[16], const uint8_t in[16], const uint8_t key[16])
{
    uint8_t rk[176];
    uint8_t s[16];

    key_expand(rk, key);
    for (uint32_t i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];

    for (uint32_t rnd = 1; rnd <= 10; rnd++) {
        /* SubBytes */
        for (uint32_t i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        /* ShiftRows（状态按列主序：s[col*4+row]）*/
        uint8_t t;
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;     t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
        /* MixColumns（末轮跳过）*/
        if (rnd < 10) {
            for (uint32_t c = 0; c < 4; c++) {
                uint8_t *p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                p[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                p[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                p[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        }
        /* AddRoundKey */
        for (uint32_t i = 0; i < 16; i++) s[i] ^= rk[rnd * 16 + i];
    }
    memcpy(out, s, 16);
}

/* ===================== CMAC（RFC4493）===================== */
static void dbl(uint8_t d[16], const uint8_t s[16])   /* <<1 with R128 */
{
    uint8_t carry = 0, next;
    for (int i = 15; i >= 0; i--) {
        next = (uint8_t)(s[i] >> 7);
        d[i] = (uint8_t)(s[i] << 1) | carry;
        carry = next;
    }
    d[15] ^= (uint8_t)(carry ? 0x87 : 0);
}

void app_sec_cmac(uint8_t tag[16], const uint8_t *data, uint32_t len,
                  const uint8_t key[16])
{
    uint8_t k1[16], k2[16], l[16], m[16];
    uint32_t n = (len + 15) / 16;

    app_sec_aes128(l, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", key);
    dbl(k1, l);
    dbl(k2, k1);

    memset(tag, 0, 16);
    if (n == 0) {
        memcpy(m, data, len);
        m[len] = 0x80;
        for (uint32_t i = len + 1; i < 16; i++) m[i] = 0;
        for (uint32_t i = 0; i < 16; i++) m[i] ^= k2[i];
    } else {
        for (uint32_t b = 0; b < n; b++) {
            uint32_t off = b * 16;
            uint32_t rem = len - off;
            if (b == n - 1 && rem < 16) {          /* 末块不整 */
                memset(m, 0, 16);
                memcpy(m, data + off, rem);
                m[rem] = 0x80;
                for (uint32_t i = 0; i < 16; i++) m[i] ^= k2[i];
            } else {
                for (uint32_t i = 0; i < 16; i++) m[i] = data[off + i];
                if (b == n - 1)
                    for (uint32_t i = 0; i < 16; i++) m[i] ^= k1[i];
            }
            for (uint32_t i = 0; i < 16; i++) m[i] ^= tag[i];
            app_sec_aes128(tag, m, key);
        }
    }
}

/* ===================== 旧 PC 端密码学（V1.10~V1.13 路线）已删 =====================
 * V2.0.0 架构：PC 零密码学，SEC_KEY_SRC/master_key/账本证书/固件段加密整链移除。
 * 现役密钥仅 DEV_SALT（设备端整文件加密，见下）。 */

/* ===================== V2.0.0 设备端全文件加密实现 ===================== */
static uint32_t s_enc_nonce_ctr = 0;

int app_sec_dev_key(uint8_t key[16])
{
    /* V2.1：K0=口令混淆填充 → K1=AES(K0,K0) → K_dev=AES(K1,UID||0^4) */
    uint8_t k0[16], k1[16], uid[12], in[16];
    uint32_t klen = strlen(s_keystr);
    uint32_t i;

    if (klen == 0) {
        strncpy(s_keystr, KEYSTR_DEFAULT, sizeof(s_keystr) - 1);
        s_keystr[sizeof(s_keystr) - 1] = 0;
        klen = strlen(s_keystr);
    }
    for (i = 0; i < 16; i++)
        k0[i] = (uint8_t)s_keystr[i % klen] ^ (uint8_t)i;
    app_sec_aes128(k1, k0, k0);
    app_sec_get_uid(uid);
    memcpy(in, uid, 12);
    in[12] = 0; in[13] = 0; in[14] = 0; in[15] = 0;
    app_sec_aes128(key, in, k1);
    return 1;
}

void app_sec_enc_crypt(uint8_t *buf, uint32_t len, uint32_t offset, uint32_t enc_nonce)
{
    uint8_t kdev[16], ctr[16], ks[16];
    uint32_t off = 0;

    if (!app_sec_dev_key(kdev)) return;

    while (off < len) {
        uint32_t blk = 1 + (offset + off) / 16;
        memset(ctr, 0, 16);
        ctr[0] = (uint8_t)(enc_nonce >> 24); ctr[1] = (uint8_t)(enc_nonce >> 16);
        ctr[2] = (uint8_t)(enc_nonce >> 8);  ctr[3] = (uint8_t)(enc_nonce);
        ctr[4] = 'E'; ctr[5] = 'N';
        ctr[14] = (uint8_t)((blk >> 8) & 0xFF);
        ctr[15] = (uint8_t)(blk & 0xFF);
        app_sec_aes128(ks, ctr, kdev);
        uint32_t n = (len - off > 16) ? 16 : len - off;
        for (uint32_t i = 0; i < n; i++)
            buf[off + i] ^= ks[i];
        off += n;
    }
}

void app_sec_enc_cmac(uint8_t tag[16], const uint8_t *ct_data, uint32_t len)
{
    uint8_t kdev[16];
    if (app_sec_dev_key(kdev))
        app_sec_cmac(tag, ct_data, len, kdev);
    else
        memset(tag, 0, 16);
}

uint32_t app_sec_enc_nonce_gen(void)
{
    /* UID 内容 + SysTick + 调用计数 + DEREF：每次导入不同即可（非密码学级） */
    uint32_t t = HAL_GetTick();
    const uint32_t *uid = (const uint32_t *)0x1FFF7A10u;
    s_enc_nonce_ctr++;
    uint32_t n = t ^ uid[0] ^ (uid[2] << 16) ^ (s_enc_nonce_ctr * 2654435761u);
    if (n == 0) n = 1;
    return n;
}


/* ===================== V2.1.2: streaming CMAC (single shared impl) =====================
 * encrypt-side & verify-side previously each had an "equivalent" inline copy
 * that computed DIFFERENT tags on identical data (measured). One implementation
 * kills the divergence. */
void app_sec_cmac_stream_begin(cmac_stream_t *cs, uint32_t total_len)
{
    uint8_t l[16], zero[16];
    memset(cs, 0, sizeof(*cs));
    cs->total = total_len;
    if (!app_sec_dev_key(cs->k)) return;
    memset(zero, 0, 16);
    app_sec_aes128(l, zero, cs->k);
    {
        uint8_t carry = 0;
        for (int i = 15; i >= 0; i--) {
            uint8_t nx = (uint8_t)(l[i] >> 7);
            cs->k1[i] = (uint8_t)(l[i] << 1) | carry; carry = nx;
        }
        if (carry) cs->k1[15] ^= 0x87;
    }
    {
        uint8_t carry = 0;
        for (int i = 15; i >= 0; i--) {
            uint8_t nx = (uint8_t)(cs->k1[i] >> 7);
            cs->k2[i] = (uint8_t)(cs->k1[i] << 1) | carry; carry = nx;
        }
        if (carry) cs->k2[15] ^= 0x87;
    }
}

void app_sec_cmac_stream_update(cmac_stream_t *cs, const uint8_t *data, uint32_t len)
{
    uint32_t i = 0;
    if (cs->lastlen == 0xFFFF) { /* 未 begin */ }
    while (i < len) {
        uint32_t remaining_total = cs->total - cs->fed;   /* 含本 chunk 未消费部分 */
        if (remaining_total > 16) {
            /* 还没到末块：整 16B 块入 CBC-MAC 链。若本 chunk 剩余不足 16，
             * 先暂存 last（作为"疑似末块前块"），下次 update 到位后消费 */
            if (len - i >= 16) {
                for (int j = 0; j < 16; j++) cs->m[j] = data[i + j] ^ cs->tagc[j];
                app_sec_aes128(cs->tagc, cs->m, cs->k);
                i += 16;
            } else {
                /* 不足 16 且后面还有数据：暂存（正常不会走到——调用方 2KB 对齐；
                 * 防御：当作待续块存 last，标记 lastlen 为 pending */
                uint32_t n = len - i;
                memcpy(cs->last, data + i, n);
                cs->lastlen = 0x8000 | n;      /* pending 标记 */
                i += n;
            }
        } else {
            /* 末 16B（或不足）：完整暂存到 last（跨 chunk 拼接）*/
            uint32_t want = (remaining_total < 16) ? remaining_total : 16;
            uint32_t n = len - i;
            if (n > want) n = want;
            if (cs->lastlen & 0x8000) {        /* 有 pending 前块：先消费它 */
                uint32_t plen = cs->lastlen & 0x7FFF;
                /* pending 块 + 本次数据拼成整块（调用方保证到 total 前 ≥16）*/
                uint8_t blk[16];
                uint32_t take = 16 - plen;
                if (take > n) take = n;
                memcpy(blk, cs->last, plen);
                memcpy(blk + plen, data + i, take);
                if (plen + take == 16) {
                    for (int j = 0; j < 16; j++) cs->m[j] = blk[j] ^ cs->tagc[j];
                    app_sec_aes128(cs->tagc, cs->m, cs->k);
                    i += take;
                    cs->lastlen = 0;
                    remaining_total = cs->total - cs->fed - i + 0;  /* 供下轮 */
                    continue;
                } else { /* 仍不足（尾块未到齐） */
                    memcpy(cs->last, blk, plen + take);
                    cs->lastlen = 0x8000 | (plen + take);
                    i += take;
                    continue;
                }
            }
            if ((cs->lastlen & 0x7FFF) == 0 || cs->lastlen == 0)
                { memcpy(cs->last + 0, data + i, n); cs->lastlen = n; }
            else
                { memcpy(cs->last + (cs->lastlen & 0x7FFF), data + i, n); cs->lastlen += n; }
            i += n;
        }
    }
    cs->fed += len;
}

void app_sec_cmac_stream_end(cmac_stream_t *cs, uint8_t tag[16])
{
    if (cs->total == 0) {
        memset(cs->last, 0, 16);
        cs->last[0] = 0x80;
        cs->lastlen = 16;
        memcpy(cs->k1, cs->k2, 16);
    }
    cs->lastlen &= 0x7FFF;
    if (cs->lastlen == 16) {
        for (int j = 0; j < 16; j++) cs->m[j] = cs->last[j] ^ cs->k1[j] ^ cs->tagc[j];
    } else {
        cs->last[cs->lastlen] = 0x80;
        for (int j = 0; j < 16; j++) cs->m[j] = cs->last[j] ^ cs->k2[j] ^ cs->tagc[j];
    }
    app_sec_aes128(cs->tagc, cs->m, cs->k);
    memcpy(tag, cs->tagc, 16);
}
