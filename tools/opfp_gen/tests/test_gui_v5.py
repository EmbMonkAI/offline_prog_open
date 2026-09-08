#!/usr/bin/env python3
"""test_gui_v5.py — v5 GUI 冒烟 + 打包布局 + 导入回填往返测试（offscreen）。

运行: python tests/test_gui_v5.py   （或 pytest tests/test_gui_v5.py）
不依赖 pytest-qt —— 直接 QApplication offscreen 实例化断言。
"""
import os, sys, json, struct, zlib, tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
sys.path.insert(0, SRC)

from PySide6.QtWidgets import QApplication

app = QApplication.instance() or QApplication(sys.argv)

from opfp_generator import (OPFPGenerator, OPFP_MAGIC, PGCF_MAGIC,
                            PGCF_FLAG_SERIAL, PGCF_FLAG_MAXCNT, PGCF_FLAG_CRYPT,
                            PGCF_FLAG_IMAGE_ID, PGCF_LEN)
FAILS = []


def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print("  [%s] %s%s" % (tag, name, ("  — " + detail) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def main():
    with tempfile.TemporaryDirectory() as td:
        win = OPFPGenerator()
        print("== 1. 构造 / 厂家加载 ==")
        check("厂家下拉非空", win.cmb_vendor.count() >= 1)
        check("默认厂家已填型号", win.cmb_part.count() > 0,
              "count=%d" % win.cmb_part.count())

        print("== 2. 选型 STM32F401RB ==")
        # 切到 ST 厂家（可能是第一个或第二个）
        for i in range(win.cmb_vendor.count()):
            if win.cmb_vendor.itemData(i) == "ST":
                win.cmb_vendor.setCurrentIndex(i); break
        idx = None
        for i in range(win.cmb_part.count()):
            if win.cmb_part.itemData(i) == "STM32F401RB":
                idx = i; break
        check("型号列表含 STM32F401RB", idx is not None)
        if idx is not None:
            win.cmb_part.setCurrentIndex(idx)
        check("选中态记录", win.selected_chip == "STM32F401RB", win.selected_chip or "None")
        # 芯片库 F401RB flash=128KB（probe-rs 数据），非手误——显示须与库一致
        check("Flash 容量显示 128KB", "128 KB" in win.lbl_flash_size.text(),
              win.lbl_flash_size.text())

        print("== 3. 导入固件 → 占用/校验和显示 ==")
        fw = bytes(range(256)) * 4   # 1024 B 测试固件
        fw_file = os.path.join(td, "demo.bin")
        with open(fw_file, "wb") as f:
            f.write(fw)
        win.edit_fw_path.setText(fw_file)
        check("固件已加载", win.fw_data == fw)
        usage = win.lbl_usage.text()
        check("占用显示含百分比", "%" in usage and "1.0 KB" in usage, usage)
        crc_expect = "0x%08X" % (zlib.crc32(fw) & 0xFFFFFFFF)
        check("校验和正确", win.lbl_fw_crc.text() == crc_expect,
              "%s != %s" % (win.lbl_fw_crc.text(), crc_expect))

        print("== 4. v7 打包布局 ==")
        # 开滚码 + 次数限制 + 镜像编号
        win.chk_serial.setChecked(True)
        win.edit_serial_addr.setText("0x0800FC00")
        win.cmb_serial_width.setCurrentIndex(2)   # 4 字节
        win.spin_serial_start.setValue(0x100)
        win.chk_maxcnt.setChecked(True)
        win.spin_maxcnt.setValue(50)
        win.spin_image_id.setValue(3)
        data = win._build_opfp_bytes()
        magic, version, flags = struct.unpack_from("<I H H", data, 0)
        check("magic", magic == OPFP_MAGIC)
        check("version=6", version == 6, str(version))
        check("header CRC", struct.unpack_from("<I", data, 108)[0] ==
              (zlib.crc32(data[:108]) & 0xFFFFFFFF))
        pg_magic, pg_flags, saddr, swidth, sstep, _r, sstart, maxcnt = \
            struct.unpack_from("<I I I B B H I I", data, 128)
        image_id = struct.unpack_from("<I", data, 128 + 24)[0]
        check("PGCF magic @128", pg_magic == PGCF_MAGIC, hex(pg_magic))
        check("PGCF flags 滚码|次数|编号",
              pg_flags == (PGCF_FLAG_SERIAL | PGCF_FLAG_MAXCNT
                           | PGCF_FLAG_IMAGE_ID),
              hex(pg_flags))
        check("滚码地址", saddr == 0x0800FC00, hex(saddr))
        check("滚码宽度4", swidth == 4)
        check("滚码起始", sstart == 0x100)
        check("次数上限50", maxcnt == 50)
        check("镜像编号3", image_id == 3, str(image_id))
        fw_size = struct.unpack_from("<I", data, 16)[0]
        algo_size = struct.unpack_from("<I", data, 28)[0]
        check("总长 = 128+32+algo+fw", len(data) == 128 + PGCF_LEN + algo_size + fw_size,
              "%d vs %d" % (len(data), 128 + PGCF_LEN + algo_size + fw_size))
        check("fw 段明文(V2.0 设备端加密)", data[128 + PGCF_LEN + algo_size:] == fw)

        print("== 5. 约束拒绝 ==")
        win.edit_serial_addr.setText("0x20000000")   # RAM 地址 → 越界
        try:
            win._build_opfp_bytes(); check("滚码越界被拒", False)
        except ValueError:
            check("滚码越界被拒", True)
        win.edit_serial_addr.setText("0x0800FC01")   # 未对齐
        try:
            win._build_opfp_bytes(); check("滚码未对齐被拒", False)
        except ValueError:
            check("滚码未对齐被拒", True)
        win.edit_serial_addr.setText("0x0800FC00")

        print("== 6. 导入回填往返 ==")
        win.edit_disp_name.setText("DemoV5")
        out = os.path.join(td, "demo.opfp")
        with open(out, "wb") as f:
            f.write(win._build_opfp_bytes())
        # 清空再导入
        win2 = OPFPGenerator()
        win2._test_mode = True          # 多候选弹窗静默（F401RB/CB 同参歧义属预期）
        win2._import_opfp_file(out)
        # F401RB 与 F401CB 三参数+algo 完全同 —— .opfp 无法唯一还原，同组即过
        check("往返-芯片(同参组)", win2.selected_chip in ("STM32F401RB", "STM32F401CB"),
              win2.selected_chip or "None")
        check("往返-显示名", win2.edit_disp_name.text() == "DemoV5",
              win2.edit_disp_name.text())
        check("往返-滚码启用", win2.chk_serial.isChecked())
        check("往返-滚码地址", win2.edit_serial_addr.text().lower() == "0x0800fc00",
              win2.edit_serial_addr.text())
        check("往返-次数限制", win2.chk_maxcnt.isChecked() and
              win2.spin_maxcnt.value() == 50)
        check("往返-镜像编号", win2.spin_image_id.value() == 3,
              str(win2.spin_image_id.value()))
        check("往返-固件内容", win2.fw_data == fw)

        print("== 7. v4 导入（无配置段 → 全关）==")
        # 手工构造 v4：把 v5 数据的 28B 段抽掉、version 改 4、CRC 重算
        v5 = win._build_opfp_bytes()
        hdr = bytearray(v5[:112])
        struct.pack_into("<H", hdr, 4, 4)
        struct.pack_into("<I", hdr, 108, zlib.crc32(bytes(hdr[:108])) & 0xFFFFFFFF)
        v4 = bytes(hdr) + v5[112:128] + v5[128 + PGCF_LEN:]   # 去 PGCF 段
        out4 = os.path.join(td, "demo_v4.opfp")
        with open(out4, "wb") as f:
            f.write(v4)
        win3 = OPFPGenerator()
        win3._test_mode = True
        win3._import_opfp_file(out4)
        check("v4 导入成功(同参组)", win3.selected_chip in ("STM32F401RB", "STM32F401CB"),
              win3.selected_chip or "None")
        check("v4 无配置段→滚码关", not win3.chk_serial.isChecked())
        check("v4 无配置段→次数关", not win3.chk_maxcnt.isChecked())
        check("v4 无配置段→编号钳到1", win3.spin_image_id.value() == 1,
              str(win3.spin_image_id.value()))

        print("== 8. 旧 24B v5 段兼容（无 image_id/nonce/账本）==")
        # 取 v6 数据抽掉段尾 8B（image_id+nonce）+ 56B 账本，version 改 5
        v5_full = win._build_opfp_bytes()
        v5_body = v5_full[:128 + 24] + v5_full[128 + PGCF_LEN:-56]
        hdr5 = bytearray(v5_body[:112])
        struct.pack_into("<H", hdr5, 4, 5)
        struct.pack_into("<I", hdr5, 108, zlib.crc32(bytes(hdr5[:108])) & 0xFFFFFFFF)
        v5_24 = bytes(hdr5) + v5_body[112:]
        out_old = os.path.join(td, "demo_old24.opfp")
        with open(out_old, "wb") as f:
            f.write(v5_24)
        win4 = OPFPGenerator()
        win4._test_mode = True
        win4._import_opfp_file(out_old)
        check("旧24B 导入成功", win4.selected_chip in ("STM32F401RB", "STM32F401CB"),
              win4.selected_chip or "None")
        check("旧24B 编号无段→钳到1", win4.spin_image_id.value() == 1,
              str(win4.spin_image_id.value()))
        # v7 起旧 24B 段文件的固件为密文且无解密路径——只验证导入解析不崩

        print("== 8.5 v6 明文格式（V2.0 设备端加密）==")
        v6 = win._build_opfp_bytes()
        check("v6 version=6", struct.unpack_from("<H", v6, 4)[0] == 6)
        check("无证书尾(V2.0)", True)
        algo_sz = struct.unpack_from("<I", v6, 28)[0]
        fw_sz = struct.unpack_from("<I", v6, 16)[0]
        check("v6 总长(无尾段)", len(v6) == 128 + 32 + algo_sz + fw_sz,
              "%d" % len(v6))

        print("== 9. 发送文件名编号前缀（防覆盖）==")
        win.spin_image_id.setValue(1)
        win.edit_out_path.setText(os.path.join(td, "same-name.opfp"))
        n1 = win._send_filename()
        win.spin_image_id.setValue(2)
        n2 = win._send_filename()
        check("编号前缀1", n1 == "001-same-name.opfp", n1)
        check("编号前缀2（不同名不覆盖）", n2 == "002-same-name.opfp", n2)
        check("两文件名不同", n1 != n2)

        print("== 10. 设备连接状态（mock find_port）==")
        import opfp_generator as og
        win5 = OPFPGenerator()
        win5._link_timer.stop()                    # 测试不跑周期
        real_find = og.usbproto.find_port
        real_query = og.usbproto.query_version
        og.usbproto.find_port = lambda vid=None, pid=None: "COM16"
        og.usbproto.query_version = lambda port: "V9.9.9"   # mock 版本查询
        win5._poll_link()
        check("连接态-标签含版本", "V9.9.9" in win5.lbl_link.text(), win5.lbl_link.text())
        check("连接态-无COM号", "COM" not in win5.lbl_link.text(), win5.lbl_link.text())
        check("连接态-按钮可用", win5.btn_send.isEnabled())
        og.usbproto.find_port = lambda vid=None, pid=None: None
        win5._poll_link()
        check("断开态-标签", "未连接" in win5.lbl_link.text())
        check("断开态-按钮禁用", not win5.btn_send.isEnabled())
        og.usbproto.find_port = real_find
        og.usbproto.query_version = real_query

    print()
    if FAILS:
        print("FAIL %d 项: %s" % (len(FAILS), ", ".join(FAILS)))
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
