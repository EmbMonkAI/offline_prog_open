"""USB CDC 文件传输协议 —— PC 端单一份实现（对应 MCU app_usb.c）。

协议规范见 DEVELOPMENT.md §14。修改协议时：同步本模块 + MCU app_usb.c 的宏
（T_*/STAT_*/DATA_CHUNK_MAX 等）。tests/test_cdc_file.py 是该协议的独立参照实现
（已硬件验证 PASS），与本文逻辑一致；若改协议也请同步它。
"""
import struct
import serial
from serial.tools import list_ports

# ST / STM32 Virtual ComPort（usbd_desc.c: USBD_VID=0x0483, USBD_PID_FS=0x5740）
VID, PID = 0x0483, 0x5740

# ---- 帧类型（与 app_usb.c T_* 一致）----
T_BEGIN, T_DATA, T_EOF, T_CMD = 0x01, 0x02, 0x03, 0x10
T_ACK = 0x81
# ---- ACK ack_type ----
AT_BEGIN, AT_DATA, AT_EOF = 0x01, 0x02, 0x03
# ---- ACK status / 标记 ----
STAT_OK = 0x00
STAT_BUSY = 0xF5             # MCU 烧录中：忙拒，等待后重试（V1.7.0+ 固件）
STAT_DEVBOUND = 0xF6         # 文件账本绑定其他设备：整传输拒绝，不重试（V1.11.4+）
WRITEFAIL_MARK = 0xDEAD0000   # DATA 写失败：status = WRITEFAIL_MARK | FatFs FR 码

# FatFs FR_* 名（DATA 写失败解码用）
FR_NAMES = {0: "OK", 1: "DISK_ERR", 2: "INT_ERR", 3: "NOT_READY", 6: "INVALID_OBJECT",
            7: "DENIED", 10: "WRITE_PROTECTED", 13: "NO_FILESYSTEM", 15: "TIMEOUT",
            19: "INVALID_PARAMETER"}

CHUNK = 512   # DATA 帧数据上限（对齐 SD 扇区；与 MCU DATA_CHUNK_MAX 一致）

# ---- CMD 子命令（T_CMD=0x10 载荷首字节；V2.1.3+ 固件）----
CMD_GET_VER = 0x01    # 查固件版本：回 ACK 载荷 [cmd][版本 ASCII ≤11B NUL 填充]


def query_version(port):
    """连上设备后查询固件版本（CMD_GET_VER）→ 返回 "V2.0.0" 形态字符串。
    失败（旧固件无此命令/超时）返回 ""——调用方回退只显示 COM 号。"""
    try:
        with serial.Serial(port, 115200, timeout=1.5) as s:
            s.reset_input_buffer()
            s.write(frame(T_CMD, bytes([CMD_GET_VER])))
            t, p = recv_frame(s, timeout=1.5)
            if t == T_ACK and len(p) >= 2 and p[0] == CMD_GET_VER:
                ver = p[1:12].split(chr(0).encode())[0].decode("ascii", "ignore").strip()
                if ver:
                    return ver
    except Exception:
        pass
    return ""


def find_port(vid=VID, pid=PID):
    """按 VID/PID 找 CDC 虚拟串口，返回设备名（如 'COM16'）或 None。"""
    for p in list_ports.comports():
        if p.vid == vid and p.pid == pid:
            return p.device
    return None


def frame(t, payload=b""):
    """成帧：[0x55][0xAA][type][len:u16 LE][payload]。"""
    return bytes([0x55, 0xAA, t]) + struct.pack("<H", len(payload)) + payload


def recv_exactly(s, n, timeout=3.0):
    """从串口精确读 n 字节；超时抛 TimeoutError（EOF 后设备加密需 ~8s，调用方传大超时）。"""
    s.timeout = timeout
    buf = b""
    while len(buf) < n:
        chunk = s.read(n - len(buf))
        if not chunk:
            raise TimeoutError("读 %d 字节超时（已得 %d）" % (n, len(buf)))
        buf += chunk
    return buf


def recv_frame(s, timeout=3.0):
    """读一帧：5 字节头（含同步+type+len）+ len 字节载荷。返回 (type, payload)。"""
    hdr = recv_exactly(s, 5, timeout=timeout)
    if hdr[0] != 0x55 or hdr[1] != 0xAA:
        raise ValueError("帧头同步失配: %r" % hdr[:2])
    t = hdr[2]
    ln = struct.unpack("<H", hdr[3:5])[0]
    pl = recv_exactly(s, ln, timeout=timeout) if ln else b""
    return t, pl


def decode_data_ack(status):
    """DATA ACK 的 status 解码：正常返回 seq(int)；写失败返回 'FR_xxx(n)' 字符串。"""
    if (status & 0xFFFF0000) == WRITEFAIL_MARK:
        fr = status & 0xFFFF
        return "FR_%s(%d)" % (FR_NAMES.get(fr, "?"), fr)
    return status
