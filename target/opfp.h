#ifndef __OPFP_H
#define __OPFP_H

/**
 * @file opfp.h
 * @brief OPFP 工程文件格式 v5（v4 + 编程配置段）
 *
 * Header = 文件标识 + 芯片参数 + algo 描述 + RDP 参数 + CRC（= v3 的 112B）
 * v4 在 112B 后追加 16B 显示名；v5 再追加 28B 编程配置段（opfp_pgcf_t，见下），
 * 之后是 algo blob + firmware。
 * v3/v4 解析：version<5 无 PGCF 段，algo 直接跟在（v4:名称段 / v3:header）之后。
 */

#include <stdint.h>

#define OPFP_MAGIC      0x50464F4Cu
#define OPFP_VERSION    7              /* v7: 固件段加密；6=证书；5=PGCF；4=显示名；3=flash_size */

/* v7：固件段 AES-CTR 加密（强制，无开关——基础安全）。
 * 密钥 K_fw = AES(主密钥, nonce_be||"FW\0\0\0\0\0\0\0\0\0")（域分隔于账本密钥），
 * IV = nonce_be||seq=1_be（与账本同构）。文件布局：
 *   header(112) + 名(16) + PGCF(32) + algo(明文) + 固件(密文) + 证书(56)
 * algo 不加密（probe-rs 公开数据）。证书 fw_crc 字段 = 固件密文的 CRC32。*/

#pragma pack(push, 1)
typedef struct {
    /* 文件标识 */
    uint32_t magic;                   /* 0:  0x50464F4C */
    uint16_t version;                  /* 4:  3 */
    uint16_t flags;                    /* 6:  预留 */

    /* 芯片参数 */
    uint32_t flash_start;              /* 8:  0x08000000 */
    uint32_t page_size;                /* 12: 擦除扇区大小 */
    uint32_t fw_size;                  /* 16: 固件字节数 */
    uint32_t flash_size;               /* 20: flash 总大小（v3 新增）*/

    /* algo 描述 */
    uint32_t algo_start;               /* 24: 目标 RAM 下载地址 */
    uint32_t algo_size;                /* 28: algo blob 字节数 */
    uint32_t algo_init;                /* 32 */
    uint32_t algo_uninit;              /* 36 */
    uint32_t algo_erase_chip;          /* 40 */
    uint32_t algo_erase_sector;        /* 44 */
    uint32_t algo_program_page;        /* 48 */
    uint32_t algo_breakpoint;          /* 52 */
    uint32_t algo_static_base;         /* 56 */
    uint32_t algo_stack_pointer;       /* 60 */
    uint32_t algo_program_buffer;      /* 64 */
    uint32_t algo_program_buf_sz;      /* 68 */

    /* RDP 参数（数据化，不同芯片族不同）*/
    uint16_t rdp_type;                 /* 72: 0=none, 1=F1式, 2=F4式 */
    uint16_t rdp_reserved;             /* 74 */
    uint32_t rdp_fpec_base;            /* 76: F1=0x40022000, F4=0x40023C00 */
    uint32_t rdp_key1;                 /* 80: FLASH KEYR key1 */
    uint32_t rdp_key2;                 /* 84: FLASH KEYR key2 */
    uint32_t rdp_optkey1;              /* 88: 选项字节 KEYR key1 (F1同key1, F4不同) */
    uint32_t rdp_optkey2;              /* 92: 选项字节 KEYR key2 */
    uint32_t rdp_ob_addr;              /* 96: F1=选项字节RDP地址(0x1FFFF800); F4=OPTCR地址(fpec+0x14) */
    uint32_t rdp_ob_val;               /* 100: F1=0xA5(解锁值); F4=0xAA(OPTCR RDP Level0) */
    uint32_t rdp_sr_bsy_mask;          /* 104: SR BSY 位掩码 (F1=bit0, F4=bit16) */

    /* 校验 */
    uint32_t crc32;                    /* 108: CRC(偏移 0~107) */
} opfp_header_t;                       /* sizeof = 112 */

/**
 * v4 显示名（紧跟 header 之后，16 字节定长）：
 *   烧录器 TFT 上显示的短名（可打印 ASCII，\0 填充；全 \0 = 未设置，回退显示文件名）。
 *   与 .opfp 文件名解耦：文件名便于 PC 端管理（可长），显示名便于屏上识别（≤16）。
 *   不参与 CRC（CRC 只覆盖 112B 的 v3 段）。
 */
#define OPFP_NAME_MAX   16
typedef struct {
    uint8_t name[OPFP_NAME_MAX];       /* 112: 显示名，\0 结尾/填充 */
} opfp_name_t;                         /* sizeof = 16 */

/**
 * v5 编程配置段（PGCF，紧跟显示名之后）：
 *   滚码（递增序列号）+ 次数上限 + 加密占位 + 镜像编号 + 授权号。
 * 段长演进：24B(初版) → 28B(+image_id) → 32B(+auth_nonce)。
 * 段长判别：f_size - 128 - algo_size - fw_size 唯一确定（不可用 len 阈值）。
 * 段不参与 CRC（CRC 只覆盖 112B header）——段损坏靠 magic 校验拒烧。
 *
 * auth_nonce（V1.10.0 授权安全）：PC 每次「导出/生成」产生新授权号。
 * 烧录计数按 nonce 键存设备内部 flash——同一文件字节（SD 拷贝/重发）计数
 * 延续，仅 PC 重新导出才重新计数；配合按 image_id 的授权高水位拒旧nonce文件。
 */
#define OPFP_PGCF_MAGIC        0x50474346u   /* "PGCF" */
#define OPFP_PGCF_FLAG_SERIAL   0x00000001u  /* 滚码启用 */
#define OPFP_PGCF_FLAG_MAXCNT   0x00000002u  /* 次数上限启用 */
#define OPFP_PGCF_FLAG_CRYPT    0x00000004u  /* 加密启用（占位） */
#define OPFP_PGCF_FLAG_IMAGE_ID 0x00000008u  /* 镜像编号已设置 */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                    /* 128: 0x50474346 */
    uint32_t flags;                    /* 132: 见上 */
    uint32_t serial_addr;              /* 136: 滚码写入 flash 绝对地址 */
    uint8_t  serial_width;             /* 140: 1/2/4 字节 */
    uint8_t  serial_step;              /* 141: 步进 */
    uint16_t reserved;                 /* 142: 0 */
    uint32_t serial_start;             /* 144: 起始序列号 */
    uint32_t max_burn_count;           /* 148: 最大烧录次数（0=不限） */
    uint32_t image_id;                 /* 152: 镜像编号 1~9999（0=未设置） */
    uint32_t auth_nonce;               /* 156: 授权号（0=旧 28B 段，计数回退按文件名） */
} opfp_pgcf_t;                         /* sizeof = 32 */
#pragma pack(pop)

/**
 * v6 烧录账本（LEDG，文件尾 56B，每次烧录成功后由设备重写）：
 *   计数随文件 + AES-CMAC 认证 + MCU UID 绑定 + 单调流水号防回滚。
 * 文件布局：header 112 + 名 16 + PGCF 32 + algo + fw + [账本 56]。
 *
 * 明文（32B，AES-CTR 加密，计数器=1，IV=nonce|流水号）：
 *   burned(4) 流水号 seq(4) auth_nonce(4) 预留(4) uid[3](12) fw_crc(4)
 * 密文 32B + CMAC 16B（覆盖 IV+密文）。CMAC 密钥 = AES(主密钥, nonce)。
 *
 * 安全性质：
 *   - 改任何密文字节 → CMAC 崩 → 拒烧（防比对定位+手改计数）
 *   - UID 不匹配 → 拒烧（防拷到另一台烧录器）
 *   - seq < 设备记录的最大流水号 → 拒烧（防回滚旧账本备份）
 *   - 主密钥存设备 flash 与 PC 授权工具（不入库不随工具分发）
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  iv[8];                    /* IV 低 8B = nonce(4)+seq(4)；CTR 高 8B=0 */
    uint8_t  ct[32];                   /* 明文 32B 的 AES-CTR 密文 */
    uint8_t  cmac[16];                 /* AES-CMAC(K_file, iv||ct) */
} opfp_ledger_t;                       /* sizeof = 56 */
#pragma pack(pop)

#endif /* __OPFP_H */
