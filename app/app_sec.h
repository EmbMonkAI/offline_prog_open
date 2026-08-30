#ifndef __APP_SEC_H
#define __APP_SEC_H

/**
 * @file app_sec.h
 * @brief 安全原语：AES-128 单块 + CMAC(RFC4493) + CTR 模式 + F401 UID + 烧录账本
 *
 * V1.11.0 安全闭环：
 *   计数随文件（账本）+ AES-CMAC 认证（防手改/比对定位）+ MCU UID 绑定
 *   （防拷到另一台烧录器）+ 单调流水号（防回滚旧账本）。
 *
 * 主密钥：编译期注入 app_sec.c 的 SEC_KEY_SRC 宏（gen_key.py 产出片段抄入），
 * 与 PC 端 opfp_sec.py 的 secret.key 一致（均不入 git）。每文件工作密钥 =
 * AES(主密钥, auth_nonce)。
 */

#include <stdint.h>
#include "opfp.h"

/* F401 96bit UID（ST 出厂唯一 ID：X/Y/WAF 坐标）*/
void     app_sec_get_uid(uint8_t uid[12]);

/* AES-128 单块加密（16B in/out，key 16B）——CMAC/CTR/工作密钥派生的底座 */
void     app_sec_aes128(uint8_t out[16], const uint8_t in[16], const uint8_t key[16]);

/* CMAC（RFC4493，AES-128）：tag 16B。用于账本认证 */
void     app_sec_cmac(uint8_t tag[16], const uint8_t *data, uint32_t len,
                      const uint8_t key[16]);

/* ===================== V2.0.0 设备端全文件加密（SD 落盘密文）=====================
 * 架构：PC 生成明文 .opfp（零密码学）→ USB 传输（厂家受控环境）→ 设备收完
 * 用本机密钥加密后写 SD——使用者拿到 SD 只有本设备可解的密文；PC 软件反编译
 * 无任何密钥/算法。使用者无原始固件 → 无法自造有价值的 .opfp。
 *
 * SD 上密文文件布局（在明文 .opfp 外再包一层）：
 *   [0..3]  "ENC1" magic
 *   [4..7]  enc_nonce（设备随机生成，进 IV；每次导入不同）
 *   [8..15] 保留 0
 *   [16..]  密文 = AES-CTR(K_dev, 明文.opfp 全部字节)
 *   [尾16]  CMAC(K_dev, 密文)  —— 使用者改 SD 密文即拒烧
 *
 * K_dev = AES(SALT, UID[12]||0^4)（SALT 编译期常量；UID 设备唯一 → 换设备解不开）
 * IV = enc_nonce_be || "EN" || 0^2，块号从 1 起 LE（与 fw_crypt 风格一致）。
 * 文件名不变（NNN-xxx.opfp）——列表/编号/去重逻辑全复用。 */

#define ENC_MAGIC  0x31434E45u          /* "ENC1" 小端读出 */

/* V2.1：设置加密口令（PC BEGIN 帧下发，≤32B；空=默认）。持久化 Sector5，
 * 换口令=旧密文作废。app_usb_init 时调 keystr_init 恢复（跨重启一致）。 */
void app_sec_set_keystr(const char *ks, uint32_t len);
void app_sec_keystr_init(void);

/* 设备密钥派生：K0=口令混淆 → K1=AES(K0,K0) → K_dev=AES(K1,UID||0^4)。 */
int app_sec_dev_key(uint8_t key[16]);

/* 全文件 CTR 加解密（对称）：offset 为该 buf 在密文数据段内的字节偏移
 * （须 16 对齐——调用方按块保证），块号 = 1 + (offset+i)/16 续算。 */
void app_sec_enc_crypt(uint8_t *buf, uint32_t len, uint32_t offset, uint32_t enc_nonce);

/* 全文件 CMAC：覆盖密文数据段（data 指针 + len）。 */
void app_sec_enc_cmac(uint8_t tag[16], const uint8_t *ct_data, uint32_t len);

/* V2.1.2 流式 CMAC（两端共用一份实现——加密端与验证端各自内联的
 * "等价"副本算出不同结果（实测 calc 分歧），单一实现消灭分歧）：
 *   cmac_stream_begin(total_len)  → 开始
 *   cmac_stream_update(buf, len)  → 顺序喂密文（任意长度分块）
 *   cmac_stream_end(tag)          → 出 16B tag */
typedef struct {
    uint8_t  k[16], k1[16], k2[16];
    uint8_t  tagc[16], m[16], last[16];
    uint32_t lastlen;
    uint32_t total;      /* 总密文长 */
    uint32_t fed;        /* 已喂字节数 */
} cmac_stream_t;

void app_sec_cmac_stream_begin(cmac_stream_t *cs, uint32_t total_len);
void app_sec_cmac_stream_update(cmac_stream_t *cs, const uint8_t *data, uint32_t len);
void app_sec_cmac_stream_end(cmac_stream_t *cs, uint8_t tag[16]);

/* 生成随机 enc_nonce（UID + SysTick + 计数器混合，非密码学级但每次导入不同）。 */
uint32_t app_sec_enc_nonce_gen(void);

#endif /* __APP_SEC_H */
