#!/usr/bin/env python3
"""CDC loopback 测试 —— Phase 2。

找 VID:PID=0483:5740 的 ST CDC 虚拟串口，发若干测试串（含二进制），校验 MCU
原样回发是否一致。MCU 侧需烧入 Phase2 回环固件（usbd_cdc_if.c CDC_Receive_FS 回发）。

用法:
    pip install pyserial
    python test_cdc_loopback.py
"""
import sys
from serial.tools import list_ports
import serial

VID, PID = 0x0483, 0x5740   # ST / STM32 Virtual ComPort（见 usbd_desc.c USBD_PID_FS=22336=0x5740）
BAUD = 115200               # CDC 虚拟串口波特率对 USB 无意义，pyserial 需要一个值


def find_port():
    """按 VID/PID 找 CDC 设备的 COM 口；找不到返回 None。"""
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


def main():
    port = find_port()
    if not port:
        print("FAIL: 未找到 VID:PID=%04X:%04X 的 CDC 设备" % (VID, PID))
        print("      确认 MCU 已烧 Phase2 固件、USB 已枚举。")
        sys.exit(1)
    print("found COM port:", port)

    s = serial.Serial(port, BAUD, timeout=1.0)
    s.dtr = True   # 某些 Windows 终端流需要 DTR 才开通 bulk 管道

    # 测试用例：短串 / 长串(满包 64B) / 二进制(含 0x00、0xFF)
    tests = [b"hello",
             b"0123456789ABCDEF",
             b"\x00\x01\x02\xFF\xFE",
             b"A" * 64]

    all_ok = True
    for t in tests:
        s.reset_input_buffer()
        s.write(t)
        echo = s.read(len(t))            # 1s 内读回同样长度
        passed = (echo == t)
        all_ok = all_ok and passed
        tag = "PASS" if passed else "FAIL"
        print("%s  sent=%r" % (tag, t))
        if not passed:
            print("     echo=%r (len sent=%d got=%d)" % (echo, len(t), len(echo)))

    s.close()
    print("\n=== %s ===" % ("ALL PASS — CDC 双向管道 OK" if all_ok else "SOME FAILED"))
    sys.exit(0 if all_ok else 2)


if __name__ == "__main__":
    main()
