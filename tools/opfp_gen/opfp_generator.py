#!/usr/bin/env python3
"""
opfp_generator.py — OPFP 工程文件生成器 v5（产线四区布局）

芯片数据来自同目录 chips.json（probe-rs 提取，1196 款 ARM Cortex-M）。
界面四区：①芯片信息（厂家→型号两级下拉，导入固件后显示 FLASH 占用/校验和）
②编程配置（滚码=递增序列号、编程次数上限、加密占位）③文件导入导出
（导入原始固件 / 导出 .opfp / 导入 .opfp 回填）④日志（操作留痕）。

.opfp v5 = v4（112B header + 16B 显示名）+ 28B 编程配置段（PGCF）+ algo + fw。
MCU 固件 V1.7.1 只认 v4 —— v5 文件暂不在烧录器列表显示，MCU 适配是下个
固件里程碑（见 DEVELOPMENT.md §15）。

用法： python opfp_generator.py
"""

import sys, os, json, struct, zlib, datetime

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen" if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") else "")

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QFormLayout, QPushButton, QLineEdit, QComboBox, QCheckBox,
    QFileDialog, QLabel, QMessageBox, QGroupBox, QProgressDialog,
    QGridLayout, QPlainTextEdit, QCompleter, QSpinBox, QInputDialog,
)
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QTextCharFormat, QFont

from opfp_transfer import TransferWorker
import usbproto

OPFP_MAGIC, OPFP_VERSION, OPFP_FLAG_RDP = 0x50464F4C, 6, 0x0001
# V2.0.0：PC 零密码学——明文 v6（固件明文+PGCF nonce）；加密由烧录器
# 接收时做（K=AES(SALT,UID) 整文件加密落盘），见 DEVELOPMENT.md §15.8

PGCF_MAGIC = 0x50474346          # "PGCF" 编程配置段标识
PGCF_FLAG_SERIAL   = 0x00000001  # 滚码启用
PGCF_FLAG_MAXCNT   = 0x00000002  # 编程次数上限启用
PGCF_FLAG_CRYPT    = 0x00000004  # 加密启用（占位，算法未定）
PGCF_FLAG_IMAGE_ID = 0x00000008  # 镜像编号已设置（image_id>0 时置位，冗余校验）
PGCF_LEN = 32                    # 段长：24B(初) + 4B image_id + 4B auth_nonce

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- 加载芯片库 ----
with open(os.path.join(HERE, "chips.json"), encoding="utf-8") as _f:
    _DB = json.load(_f)
CHIPS = _DB["chips"]      # {name: {vendor, family, flash_start, flash_size, page_size, algo_id, ...}}
ALGOS = _DB["algos"]      # {algo_id: {algo_start, algo_code, algo_init, ...}}

LOG_OK, LOG_ERR, LOG_WARN = "ok", "err", "warn"


def pgcf_len_of(data):
    """v5 编程配置段长判别：32（现版 +auth_nonce）/ 28（+image_id）/ 24（初版）/
    0（v4 无段）。用 header 自带 fw_size+algo_size 反推总长唯一确定——不能
    用 len 阈值；v6 文件尾还有 48B 账本，总长先扣掉再比对。"""
    version = struct.unpack_from("<H", data, 4)[0]
    if version < 5:
        return 0
    algo_size = struct.unpack_from("<I", data, 28)[0]
    fw_size = struct.unpack_from("<I", data, 16)[0]
    total = len(data)   # V2.0：明文 v6 无尾段（曾 v6/v7 有 56B 证书，已废）
    for plen in (PGCF_LEN, 28, 24):
        if total == 128 + plen + algo_size + fw_size:
            return plen
    return 24   # 兜底：按初版处理


def human_size(n):
    if n >= 1024 * 1024:
        return "%.1f MB" % (n / 1024 / 1024)
    return "%d KB" % (n / 1024)


def human_size_bytes(n):
    if n >= 1024 * 1024:
        return "%.1f MB (%d bytes)" % (n / 1024 / 1024, n)
    if n >= 1024:
        return "%.1f KB (%d bytes)" % (n / 1024, n)
    return "%d bytes" % n


class OPFPGenerator(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("OPFP 工程文件生成器 v5 — 产线版 (%d 型号)" % len(CHIPS))
        self.setMinimumSize(960, 600)
        self.selected_chip = None      # 当前选中型号名
        self.current_algo = None       # 对应的算法 dict
        self.fw_data = None            # 已导入固件字节（None=未导入）
        self.fw_path = ""              # 固件路径（生成时重读，保持与磁盘一致）
        self._auto_output = True       # 输出路径是否自动生成（未手动改）
        self._worker = None            # USB 传输线程（持有引用防 GC）
        self._test_mode = False        # 测试模式：多候选弹窗静默取第一
        self._build_ui()
        self._fill_vendors()

        # 设备连接轮询：2s 周期 find_port（VID/PID 探测 CDC 口）。
        # 只在状态翻转时打日志（避免刷屏）；btn_send 随连接态启停。
        self._link_port = None
        self._link_timer = QTimer(self)
        self._link_timer.timeout.connect(self._poll_link)
        self._link_timer.start(2000)
        self._poll_link()

    # ==================== UI ====================
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        grid = QGridLayout(central)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setSpacing(6)

        # ---- ① 芯片信息区（左上）----
        grid.addWidget(self._build_chip_group(), 0, 0)
        # ---- ② 编程配置区（右上）----
        grid.addWidget(self._build_config_group(), 0, 1)
        # ---- ③ 文件导入导出区（左下）----
        grid.addWidget(self._build_file_group(), 1, 0)
        # ---- ④ 日志区（右下，占满剩余）----
        grid.addWidget(self._build_log_group(), 1, 1)

        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(0, 3)
        grid.setRowStretch(1, 2)

        self.statusBar().showMessage("就绪 — 先选厂家，再选型号")
        self.log("OPFP 生成器 v5 启动 — 芯片库 %d 型号（%d 厂家）" %
                 (len(CHIPS), len({c['vendor'] for c in CHIPS.values()})))

    # ---- ① 芯片信息 ----
    def _build_chip_group(self):
        grp = QGroupBox("① 芯片信息")
        outer = QVBoxLayout(grp)

        # 两级选型：厂家 → 型号
        sel = QFormLayout()
        self.cmb_vendor = QComboBox()
        self.cmb_vendor.currentIndexChanged.connect(self._on_vendor_changed)
        sel.addRow("厂家:", self.cmb_vendor)

        self.cmb_part = QComboBox()
        self.cmb_part.setEditable(True)          # 1193 款型号：可输入过滤 + 补全
        self.cmb_part.setInsertPolicy(QComboBox.NoInsert)
        self.cmb_part.completer().setCompletionMode(QCompleter.PopupCompletion)
        self.cmb_part.currentIndexChanged.connect(self._on_part_selected)
        self.cmb_part.editTextChanged.connect(self._on_part_filter)
        sel.addRow("型号:", self.cmb_part)
        outer.addLayout(sel)

        # 选中后自动填充（只读展示）
        info = QFormLayout()
        self.lbl_flash_start = QLabel("—")
        self.lbl_page_size = QLabel("—")
        self.lbl_flash_size = QLabel("—")
        self.lbl_flash_size.setStyleSheet("color:#0066cc;font-weight:bold;")
        self.lbl_usage = QLabel("未导入固件")
        self.lbl_usage.setStyleSheet("color: gray;")
        self.lbl_fw_crc = QLabel("—")
        self.lbl_fw_crc.setStyleSheet("color: gray;")
        info.addRow("Flash 起始:", self.lbl_flash_start)
        info.addRow("页/扇区:", self.lbl_page_size)
        info.addRow("Flash 总容量:", self.lbl_flash_size)
        info.addRow("FLASH 占用:", self.lbl_usage)
        info.addRow("FLASH 校验和:", self.lbl_fw_crc)
        outer.addLayout(info)

        self.lbl_algo_info = QLabel("未选择型号")
        self.lbl_algo_info.setFont(QFont("Consolas", 9))
        self.lbl_algo_info.setWordWrap(True)
        self.lbl_algo_info.setStyleSheet("color: gray;")
        outer.addWidget(self.lbl_algo_info)
        return grp

    # ---- ② 编程配置 ----
    def _build_config_group(self):
        grp = QGroupBox("② 编程配置")
        outer = QVBoxLayout(grp)

        # 镜像编号：烧录器多文件时按编号快速定位（仅存 .opfp 内部，不改文件名）。
        # V1.8.1 起 1~9999、默认 1——导出的文件永远带编号（id=0 语义上=不显示）
        box_id = QGroupBox("镜像编号 — SD 卡多文件时按编号快速定位")
        il = QHBoxLayout(box_id)
        self.spin_image_id = QSpinBox()
        self.spin_image_id.setRange(1, 9999)
        self.spin_image_id.setValue(1)
        il.addWidget(QLabel("编号:")); il.addWidget(self.spin_image_id)
        il.addStretch()
        outer.addWidget(box_id)

        # 滚码（递增序列号）
        box_serial = QGroupBox("滚码 — 递增序列号（每次烧录序号 = 上次 + 步进，写入指定 flash 地址）")
        ser = QFormLayout(box_serial)
        self.chk_serial = QCheckBox("启用滚码")
        self.chk_serial.toggled.connect(self._on_config_changed)
        ser.addRow(self.chk_serial)

        row1 = QHBoxLayout()
        self.edit_serial_addr = QLineEdit("0x0800FC00")
        self.edit_serial_addr.setPlaceholderText("如 0x0800FC00")
        row1.addWidget(QLabel("写入地址:")); row1.addWidget(self.edit_serial_addr)
        self.cmb_serial_width = QComboBox()
        self.cmb_serial_width.addItems(["1 字节", "2 字节", "4 字节"])
        self.cmb_serial_width.setCurrentIndex(2)
        row1.addWidget(QLabel("宽度:")); row1.addWidget(self.cmb_serial_width)
        ser.addRow(row1)

        row2 = QHBoxLayout()
        self.spin_serial_start = QSpinBox()
        self.spin_serial_start.setRange(0, 0x7FFFFFFF)
        self.spin_serial_start.setValue(1)
        self.spin_serial_step = QSpinBox()
        self.spin_serial_step.setRange(1, 255)
        self.spin_serial_step.setValue(1)
        row2.addWidget(QLabel("起始值:")); row2.addWidget(self.spin_serial_start)
        row2.addWidget(QLabel("步进:")); row2.addWidget(self.spin_serial_step)
        ser.addRow(row2)
        outer.addWidget(box_serial)

        # 编程次数
        box_count = QGroupBox("编程次数 — 最大烧录次数限制（超限烧录器拒烧）")
        cl = QFormLayout(box_count)
        self.chk_maxcnt = QCheckBox("启用次数限制")
        self.chk_maxcnt.toggled.connect(self._on_config_changed)
        cl.addRow(self.chk_maxcnt)
        row3 = QHBoxLayout()
        self.spin_maxcnt = QSpinBox()
        self.spin_maxcnt.setRange(1, 1000000)
        self.spin_maxcnt.setValue(100)
        row3.addWidget(QLabel("最大次数:")); row3.addWidget(self.spin_maxcnt)
        cl.addRow(row3)
        outer.addWidget(box_count)

        box_crypt = QGroupBox("设备端加密口令 — K = f(口令, 设备UID)，加密在烧录器内完成")
        cl2 = QFormLayout(box_crypt)
        self.edit_keystr = QLineEdit()
        self.edit_keystr.setPlaceholderText("口令字符串（≤32 字符；空 = 设备用默认口令）")
        cl2.addRow("口令:", self.edit_keystr)
        self.lbl_crypt = QLabel("● 口令随发送下发，SD 落盘即密文")
        self.lbl_crypt.setStyleSheet("color:#007800;font-weight:bold;")
        cl2.addRow(self.lbl_crypt)
        outer.addWidget(box_crypt)
        outer.addStretch()
        return grp

    # ---- ③ 文件导入导出 ----
    def _build_file_group(self):
        grp = QGroupBox("③ 文件导入导出")
        outer = QVBoxLayout(grp)

        # 设备连接状态行：定时轮询 CDC 口，「发送到设备」仅连接时可用
        dl = QHBoxLayout()
        self.lbl_link = QLabel("● 未连接")
        self.lbl_link.setStyleSheet("color: gray; font-weight: bold;")
        dl.addWidget(self.lbl_link)
        dl.addStretch()
        outer.addLayout(dl)

        # 显示名（v4）
        nl = QHBoxLayout()
        self.edit_disp_name = QLineEdit()
        self.edit_disp_name.setMaxLength(16)
        self.edit_disp_name.setPlaceholderText("显示名（烧录器 TFT 上，≤16 英文字符，可留空）")
        self.edit_disp_name.textChanged.connect(self._on_disp_name_edited)
        nl.addWidget(QLabel("显示名:")); nl.addWidget(self.edit_disp_name)
        outer.addLayout(nl)

        # 固件路径
        fl = QHBoxLayout()
        self.edit_fw_path = QLineEdit()
        self.edit_fw_path.setPlaceholderText("原始烧录文件 (.bin)...")
        self.edit_fw_path.textChanged.connect(self._on_fw_path_changed)
        btn_fw = QPushButton("导入原始固件...")
        btn_fw.clicked.connect(self._browse_firmware)
        fl.addWidget(self.edit_fw_path); fl.addWidget(btn_fw)
        outer.addLayout(fl)

        # 按钮行
        btns = QHBoxLayout()
        self.btn_import_opfp = QPushButton("导入 .opfp...")
        self.btn_import_opfp.clicked.connect(self._import_opfp)
        self.btn_generate = QPushButton("导出 .opfp (v5)")
        self.btn_generate.setMinimumHeight(36)
        self.btn_generate.setStyleSheet("font-size:13px;font-weight:bold;")
        self.btn_generate.clicked.connect(self._generate)
        self.btn_send = QPushButton("发送到设备 (USB)")
        self.btn_send.clicked.connect(self._send_to_device)
        self.btn_send.setEnabled(False)          # 仅设备连接时可用（_poll_link 控制）
        btns.addWidget(self.btn_import_opfp)
        btns.addWidget(self.btn_generate)
        btns.addWidget(self.btn_send)
        outer.addLayout(btns)

        # 输出路径
        ol = QHBoxLayout()
        self.edit_out_path = QLineEdit()
        self.edit_out_path.setPlaceholderText("输出 .opfp 路径...")
        self.edit_out_path.textChanged.connect(self._on_out_edited)
        btn_out = QPushButton("浏览...")
        btn_out.clicked.connect(self._browse_output)
        ol.addWidget(self.edit_out_path); ol.addWidget(btn_out)
        outer.addLayout(ol)
        return grp

    # ---- ④ 日志 ----
    def _build_log_group(self):
        grp = QGroupBox("④ 操作日志")
        outer = QVBoxLayout(grp)
        self.txt_log = QPlainTextEdit()
        self.txt_log.setReadOnly(True)
        self.txt_log.setMaximumBlockCount(2000)   # 限长防内存涨
        f = QFont("Consolas", 9)
        self.txt_log.setFont(f)
        outer.addWidget(self.txt_log)
        return grp

    def _poll_link(self):
        """探测脱机烧录器 CDC 口：绿=已连接(带 COM 号) / 灰=未连接。
        「发送到设备」仅连接时可用；插拔沿打一条日志（不刷屏）。"""
        try:
            port = usbproto.find_port()
        except Exception:
            port = None
        if port != self._link_port:
            self._link_port = port
            if port:
                ver = usbproto.query_version(port)     # V2.1.3+ 固件回版本；旧固件回退
                self.lbl_link.setText("● 设备已连接 %s" % (ver or ""))
                self.lbl_link.setStyleSheet("color:#007800;font-weight:bold;")
                self.log("烧录器已连接 %s(%s) — 发送到设备已可用" %
                         (ver + " " if ver else "", port), LOG_OK)
            else:
                self.lbl_link.setText("● 未连接")
                self.lbl_link.setStyleSheet("color:gray;font-weight:bold;")
                self.log("烧录器未连接 — 等待 USB 接入", LOG_WARN)
        self.btn_send.setEnabled(bool(self._link_port))

    def log(self, msg, kind=None):
        """操作留痕：[HH:MM:SS] 消息；ok=绿 err=红 warn=黄"""
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        fmt = QTextCharFormat()
        if kind == LOG_OK:
            fmt.setForeground(QColor("#007800"))
        elif kind == LOG_ERR:
            fmt.setForeground(QColor("#C00000"))
        elif kind == LOG_WARN:
            fmt.setForeground(QColor("#B06000"))
        self.txt_log.setCurrentCharFormat(fmt)
        self.txt_log.appendPlainText("[%s] %s" % (ts, msg))
        self.txt_log.setCurrentCharFormat(QTextCharFormat())  # 复位

    # ==================== 选型 ====================
    def _fill_vendors(self):
        vendors = sorted({c["vendor"] for c in CHIPS.values()})
        for v in vendors:
            n = sum(1 for c in CHIPS.values() if c["vendor"] == v)
            self.cmb_vendor.addItem("%s  (%d 型号)" % (v, n), userData=v)
        self.cmb_vendor.setCurrentIndex(0)

    def _on_vendor_changed(self):
        vendor = self.cmb_vendor.currentData()
        if not vendor:
            return
        self.cmb_part.blockSignals(True)
        self.cmb_part.clear()
        parts = [name for name, c in CHIPS.items() if c["vendor"] == vendor]
        for name in sorted(parts):
            c = CHIPS[name]
            self.cmb_part.addItem("%s  (%s)" % (name, human_size(c["flash_size"])), userData=name)
        self.cmb_part.setCurrentIndex(0)
        self.cmb_part.blockSignals(False)
        self._on_part_selected()
        self.log("厂家已选: %s（%d 型号）" % (vendor, len(parts)))

    def _on_part_filter(self, text):
        """型号框输入过滤：不可匹配项灰显不可选（Qt6 无行隐藏 API，用禁用）"""
        combo = self.cmb_part
        model = combo.model()
        text = (text or "").strip().lower()
        for i in range(model.rowCount()):
            name = (combo.itemData(i) or "").lower()
            visible = (not text) or (text in name)
            item = model.item(i)
            if item is not None:
                item.setEnabled(visible)
        combo.view().update()

    def _on_part_selected(self):
        pname = self.cmb_part.currentData()
        if not pname or pname not in CHIPS:
            return
        c = CHIPS[pname]
        self.selected_chip = pname
        self.current_algo = ALGOS[c["algo_id"]]
        a = self.current_algo

        self.lbl_flash_start.setText("0x%08X" % c["flash_start"])
        self.lbl_page_size.setText("0x%X  (%d B)" % (c["page_size"], c["page_size"]))
        self.lbl_flash_size.setText("%s  (0x%X)" % (human_size(c["flash_size"]), c["flash_size"]))
        self.lbl_algo_info.setText(
            "%s\nalgo: %s  load=0x%08X  blob=%d B\n"
            "init=0x%08X  erase=0x%08X  prog=0x%08X  buf=%d B"
            % (pname, c.get("algo_name", "?"),
               a["algo_start"], len(a["algo_code"]) * 4,
               a["algo_init"] or 0, a["algo_erase_sector"], a["algo_program_page"],
               a["algo_program_buf_sz"]))
        self.lbl_algo_info.setStyleSheet("color:#0066cc;")
        self.statusBar().showMessage("已选: %s" % pname)
        self.log("型号已选: %s — flash %s @ 0x%08X, page %d B" %
                 (pname, human_size(c["flash_size"]), c["flash_start"], c["page_size"]))
        self._set_output_auto()
        self._refresh_fw_info()

    def _on_config_changed(self):
        pass   # V1.13.0：加密强制启用，无开关状态变化

    # ==================== 固件导入 / 占用显示 ====================
    def _on_fw_path_changed(self, text):
        path = text.strip()
        if path and os.path.isfile(path):
            self._load_firmware(path)
        elif not path:
            self.fw_data = None
            self.fw_path = ""
            self._refresh_fw_info()

    def _browse_firmware(self):
        path, _ = QFileDialog.getOpenFileName(self, "选择固件", "", "Binary (*.bin);;All (*.*)")
        if path:
            self.edit_fw_path.setText(path)   # textChanged → _load_firmware
            self._auto_output = True
            self._set_output_auto()

    def _load_firmware(self, path):
        try:
            with open(path, "rb") as f:
                self.fw_data = f.read()
            self.fw_path = path
            self.log("固件已导入: %s — %s" % (os.path.basename(path),
                                             human_size_bytes(len(self.fw_data))), LOG_OK)
        except OSError as e:
            self.fw_data = None
            self.log("固件导入失败: %s" % e, LOG_ERR)
        self._refresh_fw_info()
        self._set_output_auto()

    def _refresh_fw_info(self):
        """FLASH 占用 / 校验和实时显示（选型或导入后调用）"""
        if not self.selected_chip:
            self.lbl_usage.setText("未选型号")
            return
        c = CHIPS[self.selected_chip]
        if not self.fw_data:
            self.lbl_usage.setText("未导入固件（总容量 %s）" % human_size(c["flash_size"]))
            self.lbl_usage.setStyleSheet("color: gray;")
            self.lbl_fw_crc.setText("—")
            self.lbl_fw_crc.setStyleSheet("color: gray;")
            return
        n = len(self.fw_data)
        pct = n * 100.0 / c["flash_size"]
        over = n > c["flash_size"]
        self.lbl_usage.setText("%s / %s  (%.1f%%)" %
                               (human_size_bytes(n), human_size_bytes(c["flash_size"]), pct))
        self.lbl_usage.setStyleSheet("color:#C00000;font-weight:bold;" if over else "color:#007800;font-weight:bold;")
        crc = zlib.crc32(self.fw_data) & 0xFFFFFFFF
        self.lbl_fw_crc.setText("0x%08X" % crc)
        self.lbl_fw_crc.setStyleSheet("color:#0066cc;")
        if over:
            self.log("固件 %s 超出 flash 容量 %s — 导出将被拒绝" %
                     (human_size_bytes(n), human_size_bytes(c["flash_size"])), LOG_ERR)

    # ==================== 显示名 / 输出路径 ====================
    def _on_disp_name_edited(self, text):
        """显示名只允许可打印 ASCII（烧录器字库仅 ASCII）；非法字符即时反馈"""
        clean = "".join(ch for ch in text if 0x20 <= ord(ch) <= 0x7E)
        if clean != text:
            pos = self.edit_disp_name.cursorPosition()
            self.edit_disp_name.setText(clean)
            self.edit_disp_name.setCursorPosition(max(0, pos - 1))

    def _on_out_edited(self):
        self._auto_output = False

    def _set_output_auto(self):
        """输出路径 = 固件名-芯片型号.opfp（仅当未被手动修改时）"""
        if not self._auto_output:
            return
        fw = self.fw_path or self.edit_fw_path.text().strip()
        if not fw:
            return
        base = os.path.splitext(fw)[0]
        chip = (self.selected_chip or "").lower()
        out = (base + "-" + chip + ".opfp") if chip else (base + ".opfp")
        self.edit_out_path.blockSignals(True)
        self.edit_out_path.setText(out)
        self.edit_out_path.blockSignals(False)

    def _browse_output(self):
        path, _ = QFileDialog.getSaveFileName(self, "保存 .opfp", "", "OPFP (*.opfp)")
        if path:
            if not path.endswith(".opfp"):
                path += ".opfp"
            self._auto_output = False
            self.edit_out_path.setText(path)

    # ==================== v5 编程配置段 ====================
    def _collect_pgcf(self):
        """从 UI 收集编程配置 → (flags, serial_addr, width, step, start, maxcnt,
        image_id, auth_nonce)。约束违规抛 ValueError。
        auth_nonce：每次「导出/发送」生成新授权号（计数按它存设备端）——
        重新导出 = 新授权（计数从 0），重发同一字节（SD 拷贝/USB 重传）计数延续。"""
        flags = 0
        serial_addr, width, step, start, maxcnt, image_id = 0, 1, 1, 0, 0, 0
        if not self.selected_chip:
            raise ValueError("请先选择芯片型号")

        image_id = self.spin_image_id.value()   # 1~9999（默认 1，永有编号）
        auth_nonce = struct.unpack("<I", os.urandom(4))[0] or 1   # 0 保留给旧段

        if self.chk_serial.isChecked():
            c = CHIPS[self.selected_chip]
            try:
                serial_addr = int(self.edit_serial_addr.text(), 0)
            except ValueError:
                raise ValueError("滚码写入地址格式无效（支持 0x 前缀）")
            width = (1, 2, 4)[self.cmb_serial_width.currentIndex()]
            step = self.spin_serial_step.value()
            start = self.spin_serial_start.value()
            fs, fe = c["flash_start"], c["flash_start"] + c["flash_size"]
            if not (fs <= serial_addr and serial_addr + width <= fe):
                raise ValueError("滚码地址 0x%08X(+%dB) 超出 flash [%08X, %08X)" %
                                 (serial_addr, width, fs, fe))
            if serial_addr % width:
                raise ValueError("滚码地址 0x%08X 未按宽度 %d 字节对齐" % (serial_addr, width))
            flags |= PGCF_FLAG_SERIAL
            self.log("滚码启用: 写 0x%08X (%dB) 起=%d 步进=%d" % (serial_addr, width, start, step))

        if self.chk_maxcnt.isChecked():
            maxcnt = self.spin_maxcnt.value()
            if maxcnt < 1:
                raise ValueError("最大烧录次数须 ≥ 1")
            flags |= PGCF_FLAG_MAXCNT
            self.log("次数限制启用: 最多 %d 次" % maxcnt)

        flags |= PGCF_FLAG_IMAGE_ID              # 编号永有（1~9999）
        return flags, serial_addr, width, step, start, maxcnt, image_id, auth_nonce

    def _pack_pgcf(self, flags, serial_addr, width, step, start, maxcnt, image_id, auth_nonce):
        """32B 编程配置段（见 DEVELOPMENT.md §15）"""
        return struct.pack("<I I I B B H I I I I",
                           PGCF_MAGIC, flags, serial_addr, width, step, 0,
                           start, maxcnt, image_id, auth_nonce)

    def _apply_pgcf_to_ui(self, cfg):
        """导入 .opfp 时把 v5 配置段回填 UI（v4 无段 → 全关）。
        auth_nonce 不回填——再导出会生成新的（导入文件≠延续授权）"""
        flags, serial_addr, width, step, start, maxcnt, image_id, auth_nonce = cfg
        self.spin_image_id.setValue(image_id)
        if auth_nonce:
            self.log("该文件授权号 0x%08X（重新导出将生成新授权，计数从 0 起）" % auth_nonce)
        self.chk_serial.setChecked(bool(flags & PGCF_FLAG_SERIAL))
        self.edit_serial_addr.setText("0x%08X" % serial_addr)
        self.cmb_serial_width.setCurrentIndex({1: 0, 2: 1, 4: 2}.get(width, 2))
        self.spin_serial_start.setValue(start)
        self.spin_serial_step.setValue(step if step > 0 else 1)
        self.chk_maxcnt.setChecked(bool(flags & PGCF_FLAG_MAXCNT))
        self.spin_maxcnt.setValue(maxcnt if maxcnt > 0 else 100)

    # ==================== 构建 .opfp 字节（落盘 / USB 发送共用）================
    def _build_opfp_bytes(self):
        """构建完整 .opfp v5（112B header + 16B 显示名 + 28B 配置段 + algo + fw）。
        选型/固件无效或越界时抛 ValueError。"""
        if not self.selected_chip or not self.current_algo:
            raise ValueError("请先选择芯片型号")
        if not self.fw_path or not os.path.isfile(self.fw_path):
            raise ValueError("请先导入有效的固件文件")

        c = CHIPS[self.selected_chip]
        a = self.current_algo
        with open(self.fw_path, "rb") as f:
            fw_data = f.read()
        if len(fw_data) > c["flash_size"]:
            raise ValueError("固件 %s 大于目标 flash %s" %
                             (human_size_bytes(len(fw_data)), human_size_bytes(c["flash_size"])))
        self.fw_data = fw_data   # 占用/校验和显示同步最新

        flags_cfg, serial_addr, width, step, start, maxcnt, image_id, auth_nonce = self._collect_pgcf()
        self.log("授权号 0x%08X（本次导出）" % auth_nonce)

        algo_blob = struct.pack("<%dI" % len(a["algo_code"]), *a["algo_code"])
        rdp_type = c.get("rdp_type", 0)
        flags = OPFP_FLAG_RDP if rdp_type != 0 else 0

        # Header = v3 布局 112 B（version=5）
        header = struct.pack("<I H H", OPFP_MAGIC, OPFP_VERSION, flags)
        header += struct.pack("<I I I I", c["flash_start"], c["page_size"], len(fw_data), c["flash_size"])
        header += struct.pack("<I I I I I I I I I I I I",
            a["algo_start"], len(algo_blob),
            a["algo_init"] or 0, a["algo_uninit"] or 0,
            a["algo_erase_chip"] or 0, a["algo_erase_sector"], a["algo_program_page"],
            a["algo_breakpoint"], a["algo_static_base"], a["algo_stack_pointer"],
            a["algo_program_buffer"], a["algo_program_buf_sz"])
        header += struct.pack("<H H", rdp_type, 0)
        header += struct.pack("<I I I I I I I I", 0, 0, 0, 0, 0, 0, 0, 0)  # rdp_*：MCU 内置，不传
        crc = zlib.crc32(header) & 0xFFFFFFFF
        header += struct.pack("<I", crc)
        assert len(header) == 112, len(header)

        # v4 显示名（16B 定长，\0 填充；不参与 CRC）
        disp = self.edit_disp_name.text().strip().encode("ascii", "ignore")[:16]
        name_field = disp.ljust(16, b"\x00")

        # v5 编程配置段（32B）
        pgcf = self._pack_pgcf(flags_cfg, serial_addr, width, step, start, maxcnt,
                               image_id, auth_nonce)

        # V2.0.0：明文 v6 直接返回——烧录器接收时整文件加密（设备端 K=UID 派生）。
        # PC 零密码学：本工具无密钥无算法，反编译无所得（导入须厂家受控完成）。
        self._ledger_ok = True
        self.lbl_crypt.setText("● 明文 v6 已生成 — 发送时设备端加密")
        self.lbl_crypt.setStyleSheet("color:#007800;font-weight:bold;")
        return header + name_field + pgcf + algo_blob + fw_data

    # ==================== 导出 ====================
    def _generate(self):
        out_path = self.edit_out_path.text().strip()
        if not out_path:
            QMessageBox.warning(self, "错误", "请指定输出路径"); return
        try:
            data = self._build_opfp_bytes()
        except ValueError as e:
            self.log("导出失败: %s" % e, LOG_ERR)
            QMessageBox.warning(self, "错误", str(e)); return

        with open(out_path, "wb") as f:
            f.write(data)

        c = CHIPS[self.selected_chip]
        a = self.current_algo
        algo_size = len(a["algo_code"]) * 4
        fw_size = len(data) - 112 - 16 - PGCF_LEN - algo_size
        crc = struct.unpack("<I", data[108:112])[0]
        self.log("导出成功: %s（total %s, algo %dB, fw %dB, CRC32 0x%08X）" %
                 (out_path, human_size_bytes(len(data)), algo_size, fw_size, crc), LOG_OK)
        self.statusBar().showMessage("OK! %s" % os.path.basename(out_path), 10000)
        QMessageBox.information(self, "成功",
            ".opfp v5 已生成:\n%s\n\n芯片: %s\nFlash: %s @ 0x%08X\nFirmware: %s\n"
            "CRC32: 0x%08X\n滚码: %s  次数限制: %s\n\n"
            "注意: 烧录器固件 V1.7.1 暂只认 v4 —— v5 需配套 MCU 固件升级（开发中）。"
            % (out_path, self.selected_chip, human_size(c["flash_size"]), c["flash_start"],
               human_size_bytes(fw_size), crc,
               "开" if self.chk_serial.isChecked() else "关",
               ("%d 次" % self.spin_maxcnt.value()) if self.chk_maxcnt.isChecked() else "关"))

    # ==================== 导入 .opfp（回填）================
    def _import_opfp(self):
        path, _ = QFileDialog.getOpenFileName(self, "导入 .opfp", "", "OPFP (*.opfp)")
        if not path:
            return
        self._import_opfp_file(path)

    def _import_opfp_file(self, path):
        """导入 .opfp 解析回填（_import_opfp 去掉对话框的直通版，测试也用）"""
        try:
            with open(path, "rb") as f:
                data = f.read()
            magic, version, flags = struct.unpack_from("<I H H", data, 0)
            if magic != OPFP_MAGIC:
                raise ValueError("magic 不符（0x%08X）— 不是 .opfp 文件" % magic)
            if version < 3 or version > OPFP_VERSION:
                raise ValueError("不支持的版本 v%d" % version)
            crc_stored = struct.unpack_from("<I", data, 108)[0]
            crc_calc = zlib.crc32(data[:108]) & 0xFFFFFFFF
            if crc_stored != crc_calc:
                raise ValueError("header CRC32 不符（存 0x%08X 算 0x%08X）— 文件损坏" %
                                 (crc_stored, crc_calc))
            flash_start, page_size, fw_size, flash_size = struct.unpack_from("<I I I I", data, 8)
            algo_size = struct.unpack_from("<I", data, 28)[0]

            # 反查芯片：按 flash_start/flash_size/page_size 匹配（algo 入口兜底）
            chip_name = self._match_chip(flash_start, flash_size, page_size, data,
                                         interactive=not self._test_mode)
            if not chip_name:
                raise ValueError("芯片库中无匹配型号（flash=%s @0x%08X page=%d）— "
                                 "请手动选型后重试" % (human_size(flash_size), flash_start, page_size))

            # 回填：选型
            c = CHIPS[chip_name]
            for i in range(self.cmb_vendor.count()):
                if self.cmb_vendor.itemData(i) == c["vendor"]:
                    self.cmb_vendor.setCurrentIndex(i); break
            self._on_vendor_changed()
            for i in range(self.cmb_part.count()):
                if self.cmb_part.itemData(i) == chip_name:
                    self.cmb_part.setCurrentIndex(i); break
            self._on_part_selected()

            # 显示名（v4+）
            if version >= 4:
                name = data[112:128].rstrip(b"\x00").decode("ascii", "ignore")
                self.edit_disp_name.setText(name)

            # 编程配置段（v5；v4 无 → 全关）。段长 32/28/24 三档兼容
            if version >= 5:
                (pg_magic, pg_flags, saddr, swidth, sstep, _rsv, sstart, maxcnt) = \
                    struct.unpack_from("<I I I B B H I I", data, 128)
                if pg_magic != PGCF_MAGIC:
                    raise ValueError("配置段 magic 不符（0x%08X）— 文件损坏" % pg_magic)
                plen = pgcf_len_of(data)
                image_id = struct.unpack_from("<I", data, 128 + 24)[0] if plen >= 28 else 0
                auth_nonce = struct.unpack_from("<I", data, 128 + 28)[0] if plen >= 32 else 0
                self._apply_pgcf_to_ui((pg_flags, saddr, swidth, sstep, sstart, maxcnt,
                                        image_id, auth_nonce))
            else:
                self._apply_pgcf_to_ui((0, 0, 4, 1, 0, 0, 0, 0))

            # V2.0.0：明文 v6（无证书尾）——直接切固件段
            algo_end = 128 + pgcf_len_of(data)
            fw = data[algo_end + algo_size:]
            ledger_info = ""
            tmp = os.path.join(HERE, "_imported_fw.bin")
            with open(tmp, "wb") as f:
                f.write(fw)
            self.edit_fw_path.setText(tmp)   # textChanged → _load_firmware
            self._auto_output = True
            base = os.path.splitext(path)[0]
            self.edit_out_path.setText(base + "-reexport.opfp")
            self.log("导入 .opfp 成功: %s（v%d, 芯片 %s, fw %s, %s%s）" %
                     (os.path.basename(path), version, chip_name,
                      human_size_bytes(len(fw)),
                      "含编程配置" if version >= 5 else "无配置段(v4)",
                      ledger_info), LOG_OK)
            self.statusBar().showMessage("已导入: %s — 可改配置后重新导出" % os.path.basename(path))
        except (ValueError, struct.error, OSError) as e:
            self.log("导入 .opfp 失败: %s" % e, LOG_ERR)
            if not self._test_mode:
                QMessageBox.warning(self, "导入失败", str(e))

    def _match_chip(self, flash_start, flash_size, page_size, data, interactive=True):
        """按 flash 三参数 + algo blob 字节级匹配（同三参数的多型号靠 blob 区分；
        仍多候选（共享同一 algo，如 F401RB/F401CB）时 interactive=True 弹窗让用户
        确认，False（测试）取字典序第一）"""
        algo_start, algo_size = struct.unpack_from("<I I", data, 24)
        off = 128 + pgcf_len_of(data)
        blob = data[off: off + algo_size]
        exact = []
        for n, c in CHIPS.items():
            if (c["flash_start"] == flash_start
                    and c["flash_size"] == flash_size
                    and c["page_size"] == page_size):
                a = ALGOS[c["algo_id"]]
                a_blob = struct.pack("<%dI" % len(a["algo_code"]), *a["algo_code"])
                if a_blob == blob:
                    exact.append(n)
        if len(exact) == 1:
            return exact[0]
        if not exact:   # blob 不匹配（异库生成？）——退回三参数唯一匹配
            same3 = [n for n, c in CHIPS.items()
                     if c["flash_start"] == flash_start
                     and c["flash_size"] == flash_size
                     and c["page_size"] == page_size]
            if len(same3) == 1:
                return same3[0]
            return None
        # 多个同 blob 型号（如 F401RB/F401CB 同 flash 同 algo）：.opfp 不含型号名，
        # 无法唯一还原 —— 弹窗让用户确认（预选第一项；烧录结果等价，仅显示名）
        exact = sorted(exact)
        if not interactive:
            return exact[0]
        sel, ok = QInputDialog.getItem(
            self, "芯片型号确认",
            ".opfp 与多个型号参数完全一致（同 flash/算法），请确认实际型号:",
            exact, 0, False)
        return sel if ok else exact[0]

    # ==================== 发送到设备（USB CDC）================
    def _send_filename(self):
        """发给 MCU、落 SD 根目录的文件名（app_flash 按 *.opfp 查找）。
        V1.8.2 起强制编号前缀「NNN-」：编号唯一化——同固件/同输出路径的两次发送
        不再互相覆盖（此前文件名取自输出路径，重名即 FA_CREATE_ALWAYS 覆盖），
        且 SD 卡目录天然按编号排序。"""
        out = self.edit_out_path.text().strip()
        base = None
        if out:
            b = os.path.basename(out)
            if b.lower().endswith(".opfp"):
                base = b[:-5]
        if not base:
            fw = os.path.splitext(os.path.basename(self.fw_path or "fw.bin"))[0] or "fw"
            chip = (self.selected_chip or "fw").lower()
            base = "%s-%s" % (fw, chip)
        return "%03u-%s.opfp" % (self.spin_image_id.value(), base)

    def _send_to_device(self):
        try:
            data = self._build_opfp_bytes()
        except ValueError as e:
            self.log("发送取消: %s" % e, LOG_ERR)
            QMessageBox.warning(self, "错误", str(e)); return
        filename = self._send_filename()
        self.log("发送 %s — 设备上同编号(%d)旧文件将被替换" %
                 (filename, self.spin_image_id.value()))

        self.btn_generate.setEnabled(False)
        self.btn_send.setEnabled(False)
        self._worker = TransferWorker(data, filename, self, keystr=self.edit_keystr.text().strip())
        self._dlg = QProgressDialog("发送 %s (%d B) 到设备..." % (filename, len(data)),
                                    "取消", 0, 100, self)
        self._dlg.setWindowTitle("USB 传输")
        self._dlg.setWindowModality(Qt.WindowModal)
        self._dlg.setMinimumDuration(0)
        self._dlg.setValue(0)
        self._worker.progress.connect(self._dlg.setValue)
        self._worker.log.connect(self._on_transfer_log)
        self._worker.done.connect(self._on_transfer_done)
        self._dlg.canceled.connect(self._worker.cancel)
        self._worker.start()
        self.log("USB 发送开始: %s (%d B)" % (filename, len(data)))

    def _on_transfer_log(self, msg):
        self.statusBar().showMessage(msg)
        self.log("USB: %s" % msg)

    def _on_transfer_done(self, ok, msg):
        self._dlg.close()
        self.btn_generate.setEnabled(True)
        self.btn_send.setEnabled(bool(self._link_port))   # 随连接态（可能已拔线）
        self.statusBar().showMessage(msg, 10000)
        self.log("USB 发送完成: %s" % msg, LOG_OK if ok else LOG_ERR)
        if ok:
            QMessageBox.information(self, "发送完成",
                msg + "\n\n.opfp 已写入 SD 卡根目录。\n"
                "（注意: v5 文件需配套新版 MCU 固件才会在烧录器列表显示）")
        else:
            QMessageBox.warning(self, "发送失败", msg)
        self._worker = None


if __name__ == "__main__":
    try:
        app = QApplication(sys.argv)
        app.setStyle("Fusion")
        window = OPFPGenerator()
        window.show()
        sys.exit(app.exec())
    except Exception as e:
        import traceback
        traceback.print_exc()
        input("\n[ERROR] %s\nPress Enter to exit..." % e)
