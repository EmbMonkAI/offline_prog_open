"""USB CDC 文件传输 worker（QThread）—— GUI 后台把 .opfp 字节发给 MCU。

复用 usbproto 协议（对应 MCU app_usb.c）。停等式 BEGIN/DATA/EOF + 端到端 CRC32。
在独立线程跑，避免阻塞 GUI；用 Signal 上报进度/结果。

重试：整条 BEGIN→DATA→EOF 失败时整体重试（FA_CREATE_ALWAYS 幂等，安全重发），
最多 MAX_RETRIES 次。掩盖 SDIO 写偶发 FR_DISK_ERR（降速未完全消除；根治用 SDIO DMA）。
重试后仍经端到端 CRC 校验，数据可信。

信号：
  progress(int 0..100)   发送进度百分比
  log(str)               状态/诊断文本（显示到状态栏）
  done(bool ok, str msg) 完成（ok=True 成功）
"""
import time
import struct
import zlib

from PySide6.QtCore import QThread, Signal
import serial

import usbproto
from usbproto import (T_BEGIN, T_DATA, T_EOF, T_ACK,
                      AT_BEGIN, AT_DATA, AT_EOF, STAT_OK, STAT_BUSY, STAT_DEVBOUND,
                      CHUNK, frame, recv_frame, find_port, decode_data_ack)

MAX_RETRIES = 3
RETRY_DELAY = 0.3   # 重试间隔（秒）
BUSY_WAIT = 2.0     # MCU 忙（烧录中）单次等待（秒）；烧录最长 ~30s，靠重试上限兜底


class TransferError(Exception):
    """可重试的传输错误。"""

class McuBusy(Exception):
    """MCU 烧录中忙拒（STAT_BUSY）——不算失败，等待后原阶段重试。"""

class _NoRetry(Exception):
    """终态错误（如 STAT_DEVBOUND）：done 已发，run 直接返回不重试。"""


class TransferWorker(QThread):
    progress = Signal(int)
    log = Signal(str)
    done = Signal(bool, str)

    def __init__(self, data, filename, parent=None, keystr=""):
        super().__init__(parent)
        self.data = bytes(data)
        self.filename = filename
        self.keystr = keystr or ""      # V2.1：设备端加密口令（用户字符串，随 BEGIN 明文下发）
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def run(self):
        """QThread 入口。整体重试（TransferError）；烧录忙（McuBusy）不计次等待。"""
        try:
            port = find_port()
            if not port:
                self.done.emit(False, "未找到 USB CDC 设备（VID:PID=%04X:%04X）— "
                                      "确认 MCU 已烧 Phase3+ 固件并枚举" % (usbproto.VID, usbproto.PID))
                return
            self.log.emit("COM 口: %s" % port)

            last_err = "未知错误"
            attempt = 0
            while attempt < MAX_RETRIES:
                attempt += 1
                if self._cancel:
                    self.done.emit(False, "已取消"); return
                try:
                    with serial.Serial(port, 115200, timeout=3.0) as s:
                        s.dtr = True
                        time.sleep(0.1)
                        # 烧录中忙拒：原阶段等待重试（不计 attempt；取消可退出）
                        while True:
                            try:
                                self._phase_begin(s)
                                break
                            except McuBusy:
                                if self._cancel:
                                    self.done.emit(False, "已取消"); return
                                self.log.emit("MCU 烧录中，等待 %.0fs..." % BUSY_WAIT)
                                time.sleep(BUSY_WAIT)
                        self._phase_data(s)
                        self._phase_eof(s)   # 成功时由它发 done(True)
                    return
                except _NoRetry:
                    return                          # 终态：done 已在 _phase_eof 发出
                except TransferError as e:
                    last_err = str(e)
                    if self._cancel:
                        self.done.emit(False, "已取消"); return
                    self.log.emit("第 %d/%d 次失败：%s" % (attempt, MAX_RETRIES, last_err))
                    if attempt < MAX_RETRIES:
                        self.progress.emit(0)
                        time.sleep(RETRY_DELAY)
            self.done.emit(False, "重试 %d 次仍失败：%s" % (MAX_RETRIES, last_err))
        except Exception as e:
            self.done.emit(False, "传输异常: %s" % e)

    # ---- 三阶段（失败抛 TransferError，由 run 重试）----
    def _phase_begin(self, s):
        crc = zlib.crc32(self.data) & 0xFFFFFFFF
        name = self.filename.encode()
        if len(name) > 63:
            raise TransferError("文件名过长(>63): %s" % self.filename)
        key = (self.keystr or "").encode()
        if len(key) > 32:
            raise TransferError("加密口令过长(>32)")
        # V2.1 帧：nl + name + size(4) + crc(4) + klen(1) + key —— 老固件
        # （无 klen 字段）按 len 检查会判 BAD，天然版本门禁
        s.write(frame(T_BEGIN, bytes([len(name)]) + name +
                      struct.pack("<II", len(self.data), crc) +
                      bytes([len(key)]) + key))
        t, p = recv_frame(s)
        if not (t == T_ACK and len(p) >= 5 and p[0] == AT_BEGIN):
            raise TransferError("BEGIN ACK 异常")
        st = struct.unpack("<I", p[1:5])[0]
        if st == STAT_BUSY:
            raise McuBusy("BEGIN")
        if st != STAT_OK:
            raise TransferError("MCU 打开文件失败 status=0x%02X（检查 SD 卡）" % st)
        self.log.emit("已开 %s，%d B" % (self.filename, len(self.data)))

    def _phase_data(self, s):
        total = len(self.data)
        seq = 0
        off = 0
        while off < total:
            if self._cancel:
                raise TransferError("已取消")
            chunk = self.data[off:off + CHUNK]
            s.write(frame(T_DATA, struct.pack("<I", seq) + chunk))
            t, p = recv_frame(s)
            if not (t == T_ACK and len(p) >= 5 and p[0] == AT_DATA):
                raise TransferError("DATA ACK 异常 seq=%d" % seq)
            got = struct.unpack("<I", p[1:5])[0]
            if got != seq:
                dec = decode_data_ack(got)
                if isinstance(dec, str):
                    raise TransferError("MCU 写失败 seq=%d: %s" % (seq, dec))
                raise TransferError("DATA seq 不符 发=%d 收=%s" % (seq, dec))
            off += len(chunk)
            seq += 1
            self.progress.emit(int(off * 100 / total))

    def _phase_eof(self, s):
        s.write(frame(T_EOF))
        # V2.1：EOF 后设备整文件加密+CMAC（34KB 约 5~8s），ACK 前无任何回包
        # —— 用 20s 超时兜住（加密期间 PC 干等是预期行为）
        t, p = recv_frame(s, timeout=20.0)
        if not (t == T_ACK and len(p) >= 9 and p[0] == AT_EOF):
            raise TransferError("EOF ACK 异常")
        st = struct.unpack("<I", p[1:5])[0]
        mcu_crc = struct.unpack("<I", p[5:9])[0]
        if st == STAT_DEVBOUND:
            # V1.11.4：文件账本绑定其他设备——MCU 已删文件，重试无意义
            self.done.emit(False, "该文件已绑定其他烧录器（账本 UID 不符），"
                                  "设备已拒绝并删除 —— 请在本机重新导出后再发送")
            raise _NoRetry()
        if st != STAT_OK:
            raise TransferError("CRC 校验不符（MCU 实算 0x%08X）" % mcu_crc)
        self.progress.emit(100)
        self.done.emit(True, "传输完成，CRC 校验通过 (0x%08X)；文件已绑定本设备" % mcu_crc)
