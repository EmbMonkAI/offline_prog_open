#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flash_and_check.py — AI 开发闭环:命令行编译 → JLink 烧录 → 串口日志自动验证

用法:
    python flash_and_check.py                    # 全流程:编译+烧录+验证
    python flash_and_check.py --skip-build       # 跳过编译(固件没改时)
    python flash_and_check.py --skip-flash       # 只编译,不烧录
    python flash_and_check.py --expect "TFT init OK" --expect "SD mount OK"
    python flash_and_check.py --serial COM7      # 指定串口(默认自动探测)
    python flash_and_check.py --list-ports       # 只列串口

验证规则(全部满足才判 PASS):
    1. UV4 编译返回 0 Error(build.log 里 "0 Error")
    2. JLink 烧录输出含 "O.K." / "Writing complete" 且无 "FAILED/Cannot connect"
    3. 串口日志里出现每一条 --expect 模式(默认至少要求出现 "gen_log" 类标记行)
    4. 串口日志里不得出现 --forbid 模式(默认: ERROR FAIL assert HardFault)

退出码:0 = 全过;非 0 = 失败(1=编译, 2=烧录, 3=串口/验证, 4=参数/环境)
AI 调用方按退出码 + stdout 的 [STEP]/[PASS]/[FAIL] 标记自行判断。
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial  # pyserial
    import serial.tools.list_ports
except ImportError:
    print("[FAIL] pyserial 未安装: pip install pyserial")
    sys.exit(4)

# ---------------- 路径与环境(按本工程实测布局写死,可用参数覆盖) ----------------
REPO = Path(__file__).resolve().parents[2]          # offline_prog/
UV4 = r"C:\Keil_v5\UV4\UV4.exe"
UVPORJX = REPO / "stm32f401_proj" / "MDK-ARM" / "stm32f401_proj.uvprojx"
BUILD_LOG = REPO / "stm32f401_proj" / "MDK-ARM" / "build.log"
JLINK = r"C:\Program Files\SEGGER\JLink_V876\JLink.exe"
AXF = REPO / "stm32f401_proj" / "MDK-ARM" / "stm32f401_proj" / "stm32f401_proj.axf"
MCU = "STM32F401RB"          # JLink 设备名(F401RB,64KB SRAM 实装)
IFACE = "swd"
SPEED = 4000

BAUD = 115200
SERIAL_TIMEOUT_S = 20        # 串口等 expect 的总时长
DEFAULT_EXPECT = [           # 固件启动日志里应出现的标记(按 main 启动序列)
    r"TFT",                  # TFT init OK / TFT driver vX —— 有任何 TFT 日志即算
]
DEFAULT_FORBID = [
    r"\bERROR\b", r"\bFAIL", r"HardFault", r"assert",
]


# ---------------- 工具 ----------------
def log(tag, msg=""):
    print(f"[{tag}] {msg}", flush=True)


def run_cmd(cmd, timeout_s):
    """运行外部命令,返回 (returncode, stdout+stderr 合并文本)。"""
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout_s)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


# ---------------- 步骤 1:UV4 编译 ----------------
def do_build():
    log("STEP", "1/3 Keil UV4 命令行编译")
    if not Path(UV4).exists():
        log("FAIL", f"UV4.exe 不存在: {UV4}")
        return False
    log("INFO", f"project: {UVPORJX}")
    rc, _ = run_cmd([UV4, "-b", str(UVPORJX), "-j0", "-o", str(BUILD_LOG)], timeout_s=600)
    # UV4 -b 的进程退出码不可靠,以 build.log 内容为准(0 Error + 含 linking/Program Size)
    if not BUILD_LOG.exists():
        log("FAIL", "build.log 未生成")
        return False
    text = BUILD_LOG.read_text(encoding="utf-8", errors="replace")
    tail = "\n".join(text.splitlines()[-15:])
    log("BUILD-LOG-TAIL", "\n" + tail)
    ok = ("0 Error" in text) or ("0 Error" in tail)
    if not ok:
        log("FAIL", "编译有错误,见 build.log")
        return False
    log("PASS", "编译通过 (0 Error)")
    return True


# ---------------- 步骤 2:JLink 烧录 ----------------
def do_flash():
    log("STEP", "2/3 JLink 烧录")
    if not Path(JLINK).exists():
        log("FAIL", f"JLink.exe 不存在: {JLINK}")
        return False
    if not AXF.exists():
        log("FAIL", f"固件产物不存在: {AXF}")
        return False
    # 注意:烧完只做 reset+halt,不 go —— CPU 停在复位向量,
    # 等串口打开后由 do_serial_check 里再执行 go,保证启动日志一条不漏。
    script = "\n".join([
        "r",                    # reset
        "h",                    # halt
        "loadfile " + str(AXF),
        "r",                    # 复位并停在复位向量(halt),不放行
        "qc",                   # 退出连接(CPU 保持 halted)
    ]) + "\n"
    try:
        p = subprocess.run([JLINK, "-device", MCU, "-if", IFACE, "-speed", str(SPEED),
                            "-autoconnect", "1", "-exitonerror", "1"],
                           input=script, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=60)
    except subprocess.TimeoutExpired:
        log("FAIL", "JLink 60s 超时(探针没插 / 设备名不对 / 连接卡死)")
        return False
    out = (p.stdout or "") + (p.stderr or "")
    log("JLINK-OUT-TAIL", "\n" + "\n".join(out.splitlines()[-12:]))
    bad = re.search(r"FAILED|Cannot connect|Error while|failed", out, re.I)
    # JLink "O.K." 出现且无失败字样即认为烧录成功(loadfile 成功回显 O.K.)
    if bad or "O.K." not in " ".join(out.splitlines()[-12:]):
        log("FAIL", "烧录失败(详见 JLINK-OUT-TAIL)")
        return False
    log("PASS", "烧录完成(CPU 保持 halt,待串口就绪后放行)")
    return True


def jlink_release_cpu():
    """第二次连 JLink 只做 go —— 在串口已打开后放行 CPU,启动日志零丢失。"""
    try:
        p = subprocess.run([JLINK, "-device", MCU, "-if", IFACE, "-speed", str(SPEED),
                            "-autoconnect", "1"],
                           input="g\nqc\n", capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
        return p.returncode == 0
    except subprocess.TimeoutExpired:
        return False


# ---------------- 步骤 3:串口验证 ----------------
def pick_serial_port():
    ports = list(serial.tools.list_ports.comports())
    dbg = [p for p in ports if re.search(r"J-?Link|STM32|ST-?Link|CP210|CH340|CH910|USB 串|UART", str(p.description), re.I)]
    cand = dbg or ports
    if not cand:
        return None
    return cand[0].device


flashed_halted = False   # do_flash 成功后置 True:串口就绪后需 jlink go 放行


def do_serial_check(port, expects, forbids):
    log("STEP", "3/3 串口日志验证")
    if not port:
        port = pick_serial_port()
    if not port:
        log("FAIL", "未找到串口(板子没插 / 驱动未装?)用 --list-ports 查看")
        return False
    log("INFO", f"port={port} baud={BAUD} 等待窗口 {SERIAL_TIMEOUT_S}s")
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except serial.SerialException as e:
        log("FAIL", f"打开串口失败: {e}")
        return False
    lines, buf = [], bytearray()
    t0 = time.time()
    with ser:
        # 串口就绪后才放行 CPU(配套 do_flash 的"烧完保持 halt"):
        # 固件启动日志从第一条起完整落入本窗口,expect 不漏收。
        if flashed_halted:
            log("INFO", "串口就绪,放行 CPU (JLink go)")
            jlink_release_cpu()
        # --skip-flash 模式下固件已在跑,只能收打开时刻之后的日志
        while time.time() - t0 < SERIAL_TIMEOUT_S:
            n = ser.in_waiting
            if n:
                buf.extend(ser.read(n))
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    try:
                        s = line.decode("utf-8", "replace").rstrip("\r")
                    except Exception:
                        s = ""
                    if s.strip():
                        print(f"    | {s}", flush=True)
                        lines.append(s)
            else:
                time.sleep(0.05)
    text = "\n".join(lines)
    ok = True
    for pat in expects:
        hit = re.search(pat, text)
        log("PASS" if hit else "FAIL", f"expect {pat!r} " + ("命中" if hit else "未出现"))
        ok = ok and bool(hit)
    for pat in forbids:
        hit = re.search(pat, text)
        log("PASS" if not hit else "FAIL", f"forbid {pat!r} " + ("未出现" if not hit else "出现!"))
        ok = ok and not hit
    return ok


# ---------------- main ----------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-flash", action="store_true")
    r_group = ap.add_argument_group("串口重启方式")
    ap.add_argument("--serial", help="指定串口,如 COM7(默认自动探测)")
    capture = ap.add_argument_group("验证规则")
    ap.add_argument("--expect", action="append", default=[],
                    help="串口必须出现的正则(可多次;默认要求 TFT 标记)")
    ap.add_argument("--forbid", action="append", default=[],
                    help="串口不得出现的正则(可多次;默认 ERROR/FAIL/HardFault)")
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("--list-ports" if False else "--dummy", help=argparse.SUPPRESS)  # 占位
    a = ap.parse_args()

    if a.list_ports:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device}  {p.description}")
        return 0

    expects = a.expect or DEFAULT_EXPECT
    forbids = a.forbid or DEFAULT_FORBID

    if not a.skip_build:
        if not do_build():
            return 1
    else:
        log("STEP", "1/3 跳过编译(--skip-build)")

    global flashed_halted
    if not a.skip_flash:
        if not do_flash():
            return 2
        flashed_halted = True
    else:
        log("STEP", "2/3 脱离烧录(--skip-flash)")

    if not do_serial_check(a.serial, expects, forbids):
        return 3
    log("DONE", "编译→烧录→串口验证 全链路 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
