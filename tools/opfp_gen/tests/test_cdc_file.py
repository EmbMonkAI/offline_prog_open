#!/usr/bin/env python3
"""CDC 文件传输测试 —— Phase 3。

向 MCU 发一个测试文件（BEGIN/DATA/EOF），逐帧核对 ACK，末尾校验 CRC。
验证：PC→CDC→MCU→写 SD 的协议链路 + 端到端 CRC。
MCU 侧需烧 Phase3 固件（app_usb.c）。可选终极验证：测完拔 SD 卡，在 PC 上读
usbtest.bin 比对内容。

用法: python test_cdc_file.py
"""
import sys
import struct
import zlib
import time
from serial.tools import list_ports
import serial

VID, PID = 0x0483, 0x5740
T_BEGIN, T_DATA, T_EOF, T_ACK = 0x01, 0x02, 0x03, 0x81
AT_BEGIN, AT_DATA, AT_EOF = 0x01, 0x02, 0x03
CHUNK = 512
FILENAME = "usbtest.bin"


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


def frame(t, payload=b""):
    return bytes([0x55, 0xAA, t]) + struct.pack("<H", len(payload)) + payload


def recv_exactly(s, n, timeout=3.0):
    s.timeout = timeout
    buf = b""
    while len(buf) < n:
        chunk = s.read(n - len(buf))
        if not chunk:
            raise TimeoutError("读 %d 字节超时（已得 %d）" % (n, len(buf)))
        buf += chunk
    return buf


def recv_frame(s):
    hdr = recv_exactly(s, 5)
    if hdr[0] != 0x55 or hdr[1] != 0xAA:
        raise ValueError("帧头同步失配: %r" % hdr[:2])
    t = hdr[2]
    ln = struct.unpack("<H", hdr[3:5])[0]
    pl = recv_exactly(s, ln) if ln else b""
    return t, pl


def main():
    port = find_port()
    if not port:
        print("FAIL: 未找到 VID:PID=%04X:%04X 的 CDC 设备" % (VID, PID))
        sys.exit(1)
    print("found:", port)
    s = serial.Serial(port, 115200, timeout=3.0)
    s.dtr = True
    time.sleep(0.1)

    # 4000 字节（非整除 512，顺带测末尾短块）；确定性内容便于比对
    data = bytes((i * 7 + 3) & 0xFF for i in range(4000))
    crc = zlib.crc32(data) & 0xFFFFFFFF
    name = FILENAME.encode()

    # ---- BEGIN ----
    pl = bytes([len(name)]) + name + struct.pack("<II", len(data), crc)
    s.write(frame(T_BEGIN, pl))
    t, p = recv_frame(s)
    assert t == T_ACK and p[0] == AT_BEGIN, "BEGIN ack 异常: t=%d p=%r" % (t, p)
    assert struct.unpack("<I", p[1:5])[0] == 0, "BEGIN status!=0（f_open 失败？检查 SD 卡/挂载）"
    print("BEGIN ok  (%s, %d B, crc=%08X)" % (FILENAME, len(data), crc))

    # ---- DATA ----
    seq = 0
    off = 0
    while off < len(data):
        chunk = data[off:off + CHUNK]
        s.write(frame(T_DATA, struct.pack("<I", seq) + chunk))
        t, p = recv_frame(s)
        assert t == T_ACK and p[0] == AT_DATA, "DATA ack 异常: seq=%d t=%d" % (seq, t)
        got = struct.unpack("<I", p[1:5])[0]
        if got != seq:
            if (got & 0xFFFF0000) == 0xDEAD0000:   # MCU 写失败回传的 FatFs FR 码
                fr = got & 0xFFFF
                names = {0:"OK",1:"DISK_ERR",2:"INT_ERR",3:"NOT_READY",6:"INVALID_OBJECT",
                         7:"DENIED",10:"WRITE_PROTECTED",13:"NO_FILESYSTEM",
                         15:"TIMEOUT",19:"INVALID_PARAMETER"}
                raise AssertionError("DATA seq=%d 写失败: FatFs FR_%s(%d)"
                                     % (seq, names.get(fr, "?"), fr))
            raise AssertionError("DATA seq 不符: 发=%d 收=%d" % (seq, got))
        off += len(chunk)
        seq += 1
    print("DATA ok  (%d 帧)" % seq)

    # ---- EOF ----
    s.write(frame(T_EOF))
    t, p = recv_frame(s)
    assert t == T_ACK and p[0] == AT_EOF, "EOF ack 异常: t=%d" % t
    status = struct.unpack("<I", p[1:5])[0]
    got_crc = struct.unpack("<I", p[5:9])[0]
    s.close()

    if status == 0 and got_crc == crc:
        print("EOF ok   (MCU crc=%08X 与 PC 一致)" % got_crc)
        print("\n=== PASS — %d B 经 CDC 写入 SD，CRC 校验通过 ===" % len(data))
        print("(可选) 拔 SD 卡在 PC 读 %s 比对内容，终极确认。" % FILENAME)
        sys.exit(0)
    else:
        print("\n=== FAIL — status=%d mcu_crc=%08X pc_crc=%08X ===" % (status, got_crc, crc))
        sys.exit(2)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, ValueError, TimeoutError) as e:
        print("\n=== FAIL — %s ===" % e)
        sys.exit(2)
