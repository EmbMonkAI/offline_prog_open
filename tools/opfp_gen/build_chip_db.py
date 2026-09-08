#!/usr/bin/env python3
"""
build_chip_db.py — 从 probe-rs 芯片 YAML 提取脱机烧录器芯片库 (chips.json)

流程：扫描 probe_rs_targets/*.yaml → 过滤 ARM Cortex-M → 对每个芯片
  复刻 probe-rs 的 assemble_from_raw_with_data（BKPT header / load_address-4 /
  stack-buffer 布局）→ 剥掉 BKPT header、按 code_start 重算地址，适配成
  MCU 端 program_target_t 字段 → 输出 chips.json。

详见同目录 DEVELOPMENT.md「assemble 映射」一节。

用法： python build_chip_db.py
"""

import os, sys, json, base64, struct, glob, re, hashlib

try:
    import yaml
except ImportError:
    sys.exit("缺少依赖 PyYAML，请: pip install pyyaml")


# ---------------------------------------------------------------------------
# YAML loader：吞掉 probe-rs 的自定义 tag (!Nvm/!Ram/!Arm/!v1 ...)，
# 把 tag 名记到 __kind__，其余按普通 mapping/sequence/scalar 解析。
# ---------------------------------------------------------------------------
class ChipLoader(yaml.SafeLoader):
    pass

def _tagged(loader, tag_suffix, node):
    if isinstance(node, yaml.MappingNode):
        d = loader.construct_mapping(node, deep=True)
        d["__kind__"] = tag_suffix
        return d
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node, deep=True)
    return loader.construct_scalar(node)

ChipLoader.add_multi_constructor("!", _tagged)


# ---------------------------------------------------------------------------
# 厂家 / 系列识别
# ---------------------------------------------------------------------------
VENDOR_MAP = [
    # ST / 国产 Cortex-M
    ("STM32",   "ST"),
    ("CH32",    "沁恒WCH"), ("CH5", "沁恒WCH"), ("CH6", "沁恒WCH"),
    ("GD32",    "兆易GD"),
    ("AT32",    "雅特力"),
    ("HT32",    "合泰"),
    ("HK32",    "航顺HK"),
    ("HC32",    "华大HC"),
    ("MM32",    "MindMotion"),
    ("CW32",    "CellWise"),
    ("PY32",    "Puya"),
    ("CIU",     "芯海CIU"),
    ("AIR",     "Luft"),
    ("ASR",     "翱捷ASR"),
    # Renesas
    ("R7",      "Renesas"), ("RA", "Renesas"), ("RX", "Renesas"),
    ("RL",      "Renesas"), ("RZ", "Renesas"), ("RH", "Renesas"),
    # NXP
    ("LPC",     "NXP"), ("MIMX", "NXP"), ("KE", "NXP"), ("Kinetis", "NXP"),
    ("HCS",     "NXP"), ("S32K", "NXP"),
    # TI
    ("LM",      "TI"), ("TM", "TI"), ("MSP", "TI"), ("MSPM0", "TI"),
    ("CC13",    "TI"), ("CC26", "TI"), ("CC23", "TI"), ("CC27", "TI"),
    # 英飞凌 (含 Cypress / Infineon)
    ("CY",      "英飞凌"), ("CYB", "英飞凌"), ("PSoC", "英飞凌"),
    ("XMC",     "英飞凌"), ("TLE", "英飞凌"),
    # Microchip (含 Atmel)
    ("ATSAM",   "Microchip"), ("ATmega", "Microchip"), ("ATtiny", "Microchip"),
    ("SAMD",    "Microchip"), ("SAME", "Microchip"), ("SAML", "Microchip"),
    ("PIC",     "Microchip"), ("MEC", "Microchip"),
    # 其他
    ("EFM32",   "SiliconLabs"), ("EFR32", "SiliconLabs"),
    ("nRF",     "Nordic"),
    ("RP2040",  "RaspberryPi"), ("RP2350", "RaspberryPi"),
    ("MAX",     "Maxim"),
    ("MB",      "富士通"),    # FM3 (MB9BF...)
    ("ADuCM",   "ADI"),
]

ARM_CORE_PREFIXES = ("armv6m", "armv7m", "armv7em", "armv8m", "cortex")


def detect_vendor(name):
    for prefix, vendor in VENDOR_MAP:
        if name.startswith(prefix):
            return vendor
    return "其他"


def detect_family(name):
    """STM32F401RB → STM32F4 ；CH32F103C8 → CH32F1 ；nRF52832 → nRF52"""
    m = re.match(r"^([A-Za-z]+\d+[A-Za-z]\d)", name)      # STM32F4 / CH32F1 / GD32F1
    if m:
        return m.group(1).upper()
    m = re.match(r"^([A-Za-z]+\d[A-Za-z]?\d+)", name)      # nRF52 / RP2040
    if m:
        return m.group(1).upper()
    m = re.match(r"^([A-Za-z]+\d+)", name)                 # STM32 / 兜底
    if m:
        return m.group(1).upper()
    return name[:8].upper()


def is_arm_core(type_str):
    type_str = (type_str or "").lower()
    return any(type_str.startswith(p) for p in ARM_CORE_PREFIXES)


def align_up(x, a):
    return ((x + a - 1) // a) * a


# ---------------------------------------------------------------------------
# 核心：复刻 probe-rs FlashAlgorithm::assemble_from_raw_with_data
# 参考：probe-rs/probe-rs/src/flashing/flash_algorithm.rs L260-461
# ---------------------------------------------------------------------------
def assemble_algo(raw, ram_start, ram_end):
    """返回适配 program_target_t 的字段 dict（已剥 BKPT header）。"""

    # 1. raw.instructions (base64) → u32 小端 words（FLM 原始代码，不含 header）
    raw_bytes = base64.b64decode(raw["instructions"])
    pad = (-len(raw_bytes)) % 4
    if pad:
        raw_bytes += b"\x00" * pad
    code_words = list(struct.unpack("<%dI" % (len(raw_bytes) // 4), raw_bytes))

    # 2. ARM Cortex-M BKPT header（probe-rs assemble 会加在 blob 前）
    #    保留在 blob 开头作「返回陷阱」（见 DEVELOPMENT.md 第5节）；用它还原 code_start
    HEADER_SIZE = 4                                  # 0xBE00BE00，1 word

    # 3. addr_load（算法下载基址，含 header 位置）
    load_addr = raw.get("load_address")
    if load_addr is not None:
        addr_load = load_addr - HEADER_SIZE
    else:
        addr_load = ram_start                        # PIC：放到 RAM 起始

    code_start = addr_load + HEADER_SIZE             # = 原 FLM load_address
    code_size_bytes = len(code_words) * 4
    STACK_ALIGN = 8
    code_end = align_up(code_start + code_size_bytes, STACK_ALIGN)

    # 4. data / stack / page_buffers 布局（与 probe-rs 一致）
    data_load_addr = raw.get("data_load_address", code_end)
    stack_size = raw.get("stack_size") or 512
    page_size = raw["flash_properties"]["page_size"]
    ram_for_data = ram_end - data_load_addr

    if code_end + stack_size > data_load_addr:       # stack 与 data 同区
        if stack_size > ram_for_data:
            raise ValueError("no room for stack")
        ram_for_data -= stack_size

    double = ram_for_data >= 2 * page_size
    if not double and ram_for_data < page_size:
        raise ValueError("no room for page buffer")

    if code_end + stack_size <= data_load_addr:
        stack_bottom = code_end
    else:
        page_count = 2 if double else 1
        stack_bottom = align_up(data_load_addr + page_count * page_size, STACK_ALIGN)
    stack_top = stack_bottom + stack_size

    page_buffers = ([data_load_addr, data_load_addr + page_size]
                    if double else [data_load_addr])

    # 5. 适配 program_target_t —— 关键：保留 BKPT header 在 blob 开头作「返回陷阱」。
    #    本工程 swd_flash_syscall_exec 靠 LR=breakpoint 命中 BKPT 指令 halt（无 FPMBP
    #    软件断点），故 blob 必须含 BKPT header，且 breakpoint 指向它（addr_load）。
    #    剥 header 会让 breakpoint 落在普通代码上，CPU 返回后永不 halt（卡死）。
    BKPT_HEADER = 0xBE00BE00   # 两条 BKPT #0（ARM Cortex-M Thumb）

    def off2abs(v):
        return None if v is None else (code_start + v)

    return {
        "algo_start":          addr_load,            # blob（含 BKPT header）下载地址
        "algo_code":           [BKPT_HEADER] + code_words,
        "algo_init":           off2abs(raw.get("pc_init")),       # = code_start + offset
        "algo_uninit":         off2abs(raw.get("pc_uninit")),
        "algo_erase_chip":     off2abs(raw.get("pc_erase_all")),
        "algo_erase_sector":   off2abs(raw.get("pc_erase_sector")),
        "algo_program_page":   off2abs(raw.get("pc_program_page")),
        "algo_breakpoint":     addr_load | 1,        # 指向 BKPT header（Thumb）
        "algo_static_base":    code_start + raw["data_section_offset"],
        "algo_stack_pointer":  stack_top,
        "algo_program_buffer": page_buffers[0],
        "algo_program_buf_sz": min(page_size, 0x80),  # ≤128B：大 sz 触发算法慢路径
    }


# ---------------------------------------------------------------------------
# 解析单个 ChipFamily YAML
# ---------------------------------------------------------------------------
def parse_chip_family(path):
    with open(path, "r", encoding="utf-8") as f:
        fam = yaml.load(f, Loader=ChipLoader)
    if not isinstance(fam, dict):
        return []

    algos_by_name = {a["name"]: a for a in fam.get("flash_algorithms", []) or []}
    out = []

    for chip in fam.get("variants", []) or []:
        name = chip.get("name")
        if not name:
            continue

        # 仅保留 ARM Cortex-M（跳过 RISC-V / Xtensa —— SWD 烧不了）
        cores = chip.get("cores", []) or []
        arm_core = next((c for c in cores if is_arm_core(c.get("type", ""))), None)
        if arm_core is None:
            continue
        core_name = arm_core.get("name", "main")

        mmap = chip.get("memory_map", []) or []

        # 主 flash region：排除 is_alias 别名区，优先 boot=true，否则取最大
        def is_nvm(r):
            return r.get("__kind__") in ("Nvm", "Flash")

        nvm = [r for r in mmap if isinstance(r, dict) and is_nvm(r) and not r.get("is_alias")]
        if not nvm:  # fallback：仅别名区时退回使用
            nvm = [r for r in mmap if isinstance(r, dict) and is_nvm(r)]
        if not nvm:
            continue
        flash_region = next((r for r in nvm if (r.get("access") or {}).get("boot")), None)
        if flash_region is None:
            flash_region = max(nvm, key=lambda r: r["range"]["end"] - r["range"]["start"])
        rng = flash_region["range"]
        flash_start = int(rng["start"])
        flash_size = int(rng["end"]) - int(rng["start"])

        # RAM region（算法加载区）：优先当前 core 可访问、且可执行（execute != false）的。
        # 关键：必须跳过 execute:false 的区域（如 STM32F4 CCMRAM @0x10000000）。
        # STM32F4 的 CCM 只挂 CPU D-bus，I-code 取指访问不到——把 PIC 算法下到 CCM，
        # CPU 取第一条指令即 bus fault → HardFault 死循环 → 永不 halt（表现为 algo init FAIL）。
        def ram_exec_ok(r):
            return (r.get("access") or {}).get("execute", True) is not False
        rams = [r for r in mmap if isinstance(r, dict) and r.get("__kind__") == "Ram"
                and core_name in (r.get("cores") or []) and ram_exec_ok(r)]
        if not rams:
            rams = [r for r in mmap if isinstance(r, dict) and r.get("__kind__") == "Ram"
                    and ram_exec_ok(r)]
        if not rams:  # 兜底：该 core 没有任何可执行 RAM 时退回全量（极少见）
            rams = [r for r in mmap if isinstance(r, dict) and r.get("__kind__") == "Ram"]
        if not rams:
            continue
        ram = rams[0]
        ram_start = int(ram["range"]["start"])
        ram_end = int(ram["range"]["end"])

        # 选主 flash 算法（跳过 otp / opt / 选项字节）
        algo_name = None
        for n in chip.get("flash_algorithms", []) or []:
            ln = (n or "").lower()
            if any(k in ln for k in ("otp", "opt", "option", "ee", "nvdata")):
                continue
            if n in algos_by_name:
                algo_name = n
                break
        if algo_name is None:
            continue
        raw_algo = algos_by_name[algo_name]

        # assemble
        try:
            asm = assemble_algo(raw_algo, ram_start, ram_end)
        except Exception as e:
            print("  WARN: %-20s assemble skip (%s)" % (name, e))
            continue

        # page_size 字段（擦除扇区步进）= sectors[0].size；编程页 = page_size
        sectors = raw_algo["flash_properties"].get("sectors") or []
        erase_size = int(sectors[0]["size"]) if sectors else int(raw_algo["flash_properties"]["page_size"])

        entry = {
            "vendor":  detect_vendor(name),
            "family":  detect_family(name),
            "algo_name": algo_name,
            "flash_start": flash_start,
            "flash_size":  flash_size,
            "page_size":   erase_size,
            "ram_start":   ram_start,
            "ram_size":    ram_end - ram_start,
        }
        entry.update(asm)
        # F1 系列 Med-density 用 Keil F1 算法（probe-rs F1 在 CH32F103 上慢，Keil 兼容快）
        if entry["family"] in ("STM32F1", "CH32F1") and entry["flash_size"] <= 128 * 1024:
            entry.update(KEIL_F1_ALGO)
            entry["rdp_type"] = 1           # F1 式 RDP（参数 MCU 内置，PC 只标族）
            entry["page_size"] = 0x400      # Keil F1 按 1KB 物理页擦除（Med-density）
        out.append((name, entry))
    return out


# ---------------------------------------------------------------------------
# 结构校验
# ---------------------------------------------------------------------------
def validate(chips):
    for name, c in chips.items():
        assert c["algo_erase_sector"] is not None, "%s: no erase_sector" % name
        assert c["algo_program_page"] is not None, "%s: no program_page" % name
        assert c["flash_size"] > 0, "%s: bad flash_size" % name
        assert c["page_size"] > 0, "%s: bad page_size" % name
        assert len(c["algo_code"]) > 0, "%s: empty algo_code" % name
        ram_lo, ram_hi = c["ram_start"], c["ram_start"] + c["ram_size"]
        for fld in ("algo_init", "algo_erase_sector", "algo_program_page"):
            v = c[fld]
            if v is not None:
                assert ram_lo <= v < ram_hi, "%s: %s=%#x out of RAM" % (name, fld, v)
        assert c["algo_start"] >= ram_lo and c["algo_start"] < ram_hi, "%s: algo_start out of RAM" % name
        # program_buffer / stack 在 RAM 内
        assert ram_lo <= c["algo_program_buffer"] < ram_hi, "%s: program_buffer out of RAM" % name
        assert ram_lo <= c["algo_stack_pointer"] <= ram_hi, "%s: stack out of RAM" % name


# Keil F1 算法（Med-density，已验证烧 CH32F103/STM32F103 快；probe-rs F1 在沁恒芯片上慢）
# 无 header 风格：algo_code[0]=0xE00ABE00，小端 halfword 0xBE00=BKPT，breakpoint=algo_start|1
KEIL_F1_ALGO = {
    "algo_start": 0x20000000,
    "algo_code": [
        0xE00ABE00, 0x062D780D, 0x24084068, 0xD3000040, 0x1E644058, 0x1C49D1FA, 0x2A001E52, 0x4770D1F2,
        0x4603B510, 0x04C00CD8, 0x444C4C7A, 0x20006020, 0x60204C79, 0x60604879, 0x60604879, 0x62604877,
        0x62604877, 0x69C04620, 0x0004F000, 0xF245B940, 0x4C745055, 0x20066020, 0xF6406060, 0x60A070FF,
        0xBD102000, 0x486C4601, 0xF0406900, 0x4A6A0080, 0x20006110, 0x48684770, 0xF0406900, 0x49660004,
        0x46086108, 0xF0406900, 0x61080040, 0xF64AE003, 0x496420AA, 0x48606008, 0xF00068C0, 0x28000001,
        0x485DD1F5, 0xF0206900, 0x495B0004, 0x20006108, 0xB5084770, 0x20004601, 0x48579000, 0xF0406900,
        0x4A550002, 0x20016110, 0xBF009000, 0x61414852, 0xF0406900, 0x4A500040, 0xE0036110, 0x20AAF64A,
        0x60104A50, 0x68C0484C, 0x0001F000, 0xD1F52800, 0x0000F89D, 0xB2C01E40, 0x28009000, 0x4846D1E6,
        0xF0206900, 0x4A440002, 0x20006110, 0xB5F0BD08, 0x460D4604, 0x46232608, 0x60086828, 0x40024842,
        0x2200F442, 0x6102483C, 0x483BBF00, 0xF00068C0, 0x28000001, 0xF422D1F9, 0xBF002200, 0x6018C901,
        0x6058C901, 0x6098C901, 0x60D8C901, 0x2280F442, 0x61024831, 0xBF003310, 0x68C0482F, 0x0001F000,
        0xD1F92800, 0xB2C01E70, 0xD1E71E06, 0x2280F422, 0x007FF024, 0x61784F28, 0x0240F042, 0x61024638,
        0x0240F022, 0x4824BF00, 0xF00068C0, 0x28000001, 0x4821D1F9, 0xF00068C0, 0xB1600014, 0x68C0481E,
        0x0014F040, 0x60F84F1C, 0x20FFF240, 0x46384002, 0x20016102, 0x2000BDF0, 0xE92DE7FC, 0x460641F8,
        0x4615460F, 0x0800F04F, 0xF1079600, 0xF3C0007F, 0x481118C7, 0xF4446904, 0x61043480, 0x4622BF00,
        0x98004629, 0xFF93F7FF, 0x2001B110, 0x81F8E8BD, 0x30809800, 0x35809000, 0x0001F1A8, 0xF1B0B2C0,
        0xD1EC0800, 0x20FFF240, 0x48034004, 0x20006104, 0x0000E7EC, 0x00000004, 0x40022000, 0x45670123,
        0xCDEF89AB, 0x40003000, 0x000102FF, 0x00000000, 0x00000000,
    ],
    "algo_init": 0x20000021, "algo_uninit": 0x20000065,
    "algo_erase_chip": 0x20000077, "algo_erase_sector": 0x200000B3,
    "algo_program_page": 0x200001BB,
    "algo_breakpoint": 0x20000001, "algo_static_base": 0x20000C00,
    "algo_stack_pointer": 0x20001000,
    "algo_program_buffer": 0x20000080, "algo_program_buf_sz": 128,
}

# 算法字段（assemble 产物）——去重时抽出，共享给同算法的芯片
ALGO_KEYS = (
    "algo_start", "algo_code", "algo_init", "algo_uninit", "algo_erase_chip",
    "algo_erase_sector", "algo_program_page", "algo_breakpoint", "algo_static_base",
    "algo_stack_pointer", "algo_program_buffer", "algo_program_buf_sz",
)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.join(here, "probe_rs_targets")
    files = sorted(glob.glob(os.path.join(src_dir, "*.yaml")))
    if not files:
        sys.exit("找不到 YAML，确认 probe_rs_targets/ 存在")

    chips_full = {}
    skipped_files = []
    for f in files:
        try:
            for name, entry in parse_chip_family(f):
                chips_full[name] = entry
        except Exception as e:
            skipped_files.append((os.path.basename(f), str(e)))

    # 校验（完整 entry）
    try:
        validate(chips_full)
    except AssertionError as e:
        print("校验失败: %s" % e)
        sys.exit(1)

    # 去重：同 assemble 结果的算法只存一份，芯片只引用 algo_id
    algos = {}
    chips = {}
    for name, entry in chips_full.items():
        algo = {k: entry[k] for k in ALGO_KEYS}
        ah = hashlib.md5(json.dumps(algo, sort_keys=True).encode()).hexdigest()[:12]
        algos.setdefault(ah, algo)
        meta = {k: v for k, v in entry.items() if k not in ALGO_KEYS}
        meta["algo_id"] = ah
        chips[name] = meta

    out = os.path.join(here, "chips.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"chips": chips, "algos": algos}, f, ensure_ascii=False, indent=1)

    # 统计
    vendors = {}
    for c in chips.values():
        vendors[c["vendor"]] = vendors.get(c["vendor"], 0) + 1

    print("=== 生成完成 ===")
    print("芯片总数: %d   独立算法: %d（去重率 %.0f%%）"
          % (len(chips), len(algos), 100 * (1 - len(algos) / max(1, len(chips)))))
    print("厂家分布:")
    for v in sorted(vendors, key=lambda k: -vendors[k]):
        print("  %-14s %d" % (v, vendors[v]))
    if skipped_files:
        print("跳过 %d 个 YAML（解析异常）:" % len(skipped_files))
        for fn, err in skipped_files[:20]:
            print("  %s: %s" % (fn, err))
    sz = os.path.getsize(out)
    print("输出: %s (%.1f KB)" % (out, sz / 1024))


if __name__ == "__main__":
    main()
