# main.py - 完整版 Display Stream Server
import sys
import os
import json
import mss
import cv2
import numpy as np
import ctypes
from ctypes import wintypes
import time
import socket
import threading
import argparse
import winreg
import zmq # 引入 ZMQ
import logging # 引入 logging

# --- 配置日志 ---
logger = logging.getLogger(__name__)
if not logger.handlers:
    handler = logging.StreamHandler()
    formatter = logging.Formatter('[%(levelname)s] %(message)s')
    handler.setFormatter(formatter)
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
# ---

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QLabel, QSystemTrayIcon,
    QMenu, QComboBox, QSpinBox, QGroupBox, QFormLayout,
    QCheckBox, QVBoxLayout, QHBoxLayout, QWidget, QPushButton,
    QSizePolicy, QMessageBox
)
from PyQt6.QtGui import QAction, QIcon, QPixmap, QImage, QCursor
from PyQt6.QtCore import QTimer, Qt, pyqtSignal, QObject

# ===== 配置 =====
CONFIG_FILE = "config.json"
DEFAULT_CONFIG = {
    "monitor_idx": 4,
    "target_resolution": [160, 128],
    "jpeg_quality": 10,
    "zmq_port": 5555,
    "tcp_port": 5655,
    "target_fps": 30,
    "encoding_format": "jpg",
    "png_compression": 6,
    "webp_quality": 10
}

def parse_args():
    """解析启动参数"""
    parser = argparse.ArgumentParser()
    parser.add_argument('--silent', '-s', nargs='?', const=0, type=int,
                       help='静默启动模式，可选延迟秒数')
    return parser.parse_args()

class ConfigManager:
    def __init__(self):
        self.config = DEFAULT_CONFIG.copy()
        self.load()

    def load(self):
        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                    self.config.update(json.load(f))
            except:
                pass

    def save(self):
        try:
            with open(CONFIG_FILE, 'w', encoding='utf-8') as f:
                json.dump(self.config, f, indent=4)
        except:
            pass

    def update_and_save(self, key, value):
        self.config[key] = value
        self.save()

class ConnectionChecker(QObject):
    """TCP 连接检测器"""
    connected = pyqtSignal(bool)
    bandwidth_update = pyqtSignal(float)

    def __init__(self, config_manager):
        super().__init__()
        self.config_mgr = config_manager
        self.port = self.config_mgr.config['tcp_port']
        self.running = False
        self.clients = {}
        self.client_sockets = {}
        self.lock = threading.Lock()

    def start_checking(self):
        self.running = True
        self.thread = threading.Thread(target=self._check_loop, daemon=True)
        self.thread.start()

    def stop_checking(self):
        self.running = False

    def _check_loop(self):
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server_socket.bind(('0.0.0.0', self.port))
            server_socket.listen(5)
            server_socket.settimeout(1.0)
        except Exception as e:
            logger.error(f"TCP 服务器启动失败: {e}")
            return

        while self.running:
            try:
                conn, addr = server_socket.accept()
                with self.lock:
                    self.clients[addr] = time.time()
                    self.client_sockets[addr] = conn

                try:
                    handshake = conn.recv(1024).decode()
                    if handshake == "client_connected":
                        conn.send(b"connected")
                except:
                    pass

                hb_thread = threading.Thread(target=self._heartbeat_check, args=(addr, conn), daemon=True)
                hb_thread.start()

            except socket.timeout:
                continue
            except Exception as e:
                logger.error(f"TCP 服务器错误: {e}")
                break

        server_socket.close()

    def _heartbeat_check(self, addr, conn):
        while self.running and addr in self.clients:
            try:
                conn.settimeout(5.0)
                data = conn.recv(1024)
                if data == b"hb":
                    with self.lock:
                        self.clients[addr] = time.time()
                else:
                    break
            except:
                with self.lock:
                    if addr in self.clients:
                        del self.clients[addr]
                        if addr in self.client_sockets:
                            del self.client_sockets[addr]
                break

        try:
            conn.close()
        except:
            pass

    def get_connected_count(self):
        """获取有效连接数"""
        with self.lock:
            current_time = time.time()
            expired_addrs = [
                addr for addr, last_time in self.clients.items()
                if current_time - last_time > 8.0
            ]
            for addr in expired_addrs:
                del self.clients[addr]
                if addr in self.client_sockets:
                    del self.client_sockets[addr]
            return len(self.clients)

class CaptureWorker(QObject):
    frame_ready = pyqtSignal(np.ndarray)
    status_update = pyqtSignal(str)
    bandwidth_update = pyqtSignal(float)
    send_fps_update = pyqtSignal(float)

    def __init__(self, config_manager, conn_checker):
        super().__init__()
        self.config_mgr = config_manager
        self.conn_checker = conn_checker
        self.running = False
        self.capture_enabled = False
        self.current_monitor = None
        self.capture_lock = threading.Lock()
        self.last_bytes = 0
        self.last_time = time.time()
        self.frame_count = 0
        self.frame_start_time = time.time()

        # 帧变化检测
        self.last_frame_hash = None
        self.frame_skip_count = 0

        # 动态帧率
        self.current_fps = self.config_mgr.config['target_fps']
        self.target_interval = 1.0 / self.current_fps

        # --- CurveZMQ 密钥生成与存储 ---
        self.zmq_secret_key_file = "server_secret.key"
        self.zmq_public_key, self.zmq_secret_key = self._load_or_generate_curve_keys()

    def _load_or_generate_curve_keys(self):
        """加载或生成 CurveZMQ 密钥对"""
        if os.path.exists(self.zmq_secret_key_file):
            logger.info("Loading existing ZMQ secret key from file.")
            try:
                with open(self.zmq_secret_key_file, 'rb') as f:
                    secret_key = f.read()
                    # 从私钥推导出公钥
                    public_key = zmq.curve_public(secret_key)
                    return public_key, secret_key
            except Exception as e:
                logger.error(f"Failed to load ZMQ secret key: {e}. Generating new pair.")

        logger.info("Generating new ZMQ Curve key pair.")
        public_key, secret_key = zmq.curve_keypair()

        # 保存私钥（二进制格式）
        try:
            with open(self.zmq_secret_key_file, 'wb') as f:
                f.write(secret_key)
            # Windows 下没有 chmod，但可以提示用户注意文件安全
            logger.info(f"ZMQ secret key saved to {self.zmq_secret_key_file}. Please ensure it's secure.")
        except Exception as e:
            logger.error(f"Failed to save ZMQ secret key: {e}")

        # 保存公钥到文件 (可选，方便客户端获取)
        try:
            with open("server_public.key", 'wb') as f:
                f.write(public_key)
            logger.info(f"ZMQ public key saved to server_public.key.")
        except Exception as e:
            logger.error(f"Failed to save ZMQ public key: {e}")

        return public_key, secret_key


    def start_worker(self):
        if self.running:
            return
        self.running = True
        self.thread = threading.Thread(target=self._worker_loop, daemon=True)
        self.thread.start()

    def stop_worker(self):
        self.running = False

    def set_capture_state(self, capture_enabled, monitor_idx=None):
        with self.capture_lock:
            self.capture_enabled = capture_enabled
            if monitor_idx is not None:
                self.current_monitor = monitor_idx

    def _worker_loop(self):
        import zmq
        zmq_ctx = zmq.Context()
        video_sock = zmq_ctx.socket(zmq.PUB)
        video_sock.setsockopt(zmq.SNDHWM, 2)

        # --- 启用 Curve 加密 ---
        video_sock.setsockopt(zmq.CURVE_SERVER, 1) # 启用 Curve 服务器模式
        video_sock.setsockopt(zmq.CURVE_SECRETKEY, self.zmq_secret_key) # 设置服务器私钥

        bind_address = f"tcp://*:{self.config_mgr.config['zmq_port']}"
        video_sock.bind(bind_address)
        logger.info(f"ZMQ PUB socket bound to {bind_address} with Curve encryption enabled.")

        next_frame_time = time.perf_counter()

        while self.running:
            current_config_fps = self.config_mgr.config['target_fps']
            if current_config_fps != self.current_fps:
                self.current_fps = current_config_fps
                self.target_interval = 1.0 / self.current_fps
                logger.info(f"帧率已更新为: {self.current_fps} FPS")

            current_time = time.perf_counter()
            sleep_time = next_frame_time - current_time
            if sleep_time > 0:
                time.sleep(max(sleep_time, 0.001))

            next_frame_time += self.target_interval

            with self.capture_lock:
                should_capture = self.capture_enabled or self.conn_checker.get_connected_count() > 0
                should_send = self.conn_checker.get_connected_count() > 0
                monitor_idx = self.current_monitor or self.config_mgr.config['monitor_idx']

            if not should_capture and not should_send:
                continue

            try:
                with mss.mss() as sct:
                    monitors = sct.monitors
                    if monitor_idx >= len(monitors):
                        self.status_update.emit("错误: 显示器索引无效")
                        continue

                    mon = monitors[monitor_idx]
                    screenshot = sct.grab(mon)
                    img_rgb = np.frombuffer(screenshot.rgb, dtype=np.uint8).reshape(
                        (mon["height"], mon["width"], 3)
                    )
                    img_bgr = cv2.cvtColor(img_rgb, cv2.COLOR_RGB2BGR)

                    # 绘制鼠标指针
                    mx, my = self.get_mouse_pos()
                    mouse_x = mx - mon["left"]
                    mouse_y = my - mon["top"]
                    if 0 <= mouse_x < mon["width"] and 0 <= mouse_y < mon["height"]:
                        img_bgr = self.draw_mouse_cursor(img_bgr, mouse_x, mouse_y)

                    # 网络发送
                    if should_send:
                        # 帧变化检测
                        frame_hash = hash(img_bgr.tobytes())
                        should_send_frame = True

                        if frame_hash == self.last_frame_hash:
                            self.frame_skip_count += 1
                            should_send_frame = False
                        else:
                            self.last_frame_hash = frame_hash
                            self.frame_skip_count = 0

                        # 每10帧强制发送一次
                        if self.frame_skip_count >= 10:
                            should_send_frame = True
                            self.frame_skip_count = 0

                        if should_send_frame:
                            small_bgr = cv2.resize(img_bgr, self.config_mgr.config['target_resolution'], interpolation=cv2.INTER_AREA)

                            encoding_format = self.config_mgr.config['encoding_format']
                            if encoding_format == 'jpg':
                                quality = self.config_mgr.config['jpeg_quality'] * 10
                                success, encoded_data = cv2.imencode('.jpg', small_bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
                            elif encoding_format == 'png':
                                compression = self.config_mgr.config['png_compression']
                                success, encoded_data = cv2.imencode('.png', small_bgr, [cv2.IMWRITE_PNG_COMPRESSION, compression])
                            elif encoding_format == 'webp':
                                quality = self.config_mgr.config['webp_quality'] * 10
                                success, encoded_data = cv2.imencode('.webp', small_bgr, [cv2.IMWRITE_WEBP_QUALITY, quality])
                            else:
                                quality = self.config_mgr.config['jpeg_quality'] * 10
                                success, encoded_data = cv2.imencode('.jpg', small_bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])

                            if success:
                                try:
                                    format_prefix = encoding_format.encode()
                                    video_sock.send_multipart([format_prefix, encoded_data.tobytes()])

                                    # 带宽统计
                                    now = time.time()
                                    if now - self.last_time > 1.0:
                                        mbps = (self.last_bytes * 8) / (1e6 * (now - self.last_time))
                                        self.bandwidth_update.emit(mbps)
                                        self.last_bytes = 0
                                        self.last_time = now
                                    self.last_bytes += len(encoded_data)

                                    # 发送帧率统计
                                    self.frame_count += 1
                                    if time.time() - self.frame_start_time >= 1.0:
                                        send_fps = self.frame_count / (time.time() - self.frame_start_time)
                                        self.send_fps_update.emit(send_fps)
                                        self.frame_count = 0
                                        self.frame_start_time = time.time()

                                except zmq.Again:
                                    pass

                    # 发送帧（用于预览和网络）
                    self.frame_ready.emit(img_bgr.copy())

                    if should_send:
                        self.status_update.emit("状态: 捕获中")

            except Exception as e:
                self.status_update.emit(f"错误: {str(e)}")

        zmq_ctx.term()

    def get_mouse_pos(self):
        class POINT(ctypes.Structure):
            _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]
        pt = POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
        return pt.x, pt.y

    def draw_mouse_cursor(self, img, x, y):
        """
        在图像上绘制鼠标指针
        
        Args:
            img: OpenCV图像 (BGR格式)
            x, y: 鼠标位置坐标
            
        Returns:
            绘制了鼠标指针的图像
        """
        # 计算鼠标指针大小（基于图像尺寸自适应）
        h, w = img.shape[:2]
        base_size = min(w, h) // 50  # 基础大小约为图像宽度/100
        cursor_size = max(16, base_size)  # 最小8像素
        
        # 定义鼠标指针的点集 (经典的箭头形状)
        # 使用相对坐标，然后按实际位置进行偏移
        points = np.array([
            [0, 0],      # 尖端
            [cursor_size//2, cursor_size],   # 右侧转折
            [cursor_size//4, cursor_size],   # 中间转折
            [cursor_size//4, cursor_size*2], # 底部
            [0, cursor_size*2],              # 底部尖端
            [-cursor_size//4, cursor_size],  # 左侧转折
            [-cursor_size//2, cursor_size],  # 左侧
        ], dtype=np.int32)
        
        # 将相对坐标转换为绝对坐标
        points[:, 0] += x
        points[:, 1] += y
        
        # 检查是否超出边界
        if (points[:, 0].min() < 0 or points[:, 0].max() >= w or 
            points[:, 1].min() < 0 or points[:, 1].max() >= h):
            return img  # 如果超出边界则不绘制
        
        # 计算区域平均亮度以确定颜色
        roi_size = min(cursor_size * 3, x, y, w-x, h-y)
        if roi_size > 0:
            roi = img[max(0, y-roi_size):min(h, y+roi_size), max(0, x-roi_size):min(w, x+roi_size)]
            avg_brightness = np.mean(roi)
            color = (0, 0, 0) if avg_brightness > 128 else (255, 255, 255)
        else:
            color = (255, 255, 255)
        
        # 绘制填充的鼠标指针
        cv2.fillPoly(img, [points], color=color)
        
        # 添加边框以增强对比度
        border_color = (255 - color[0], 255 - color[1], 255 - color[2])  # 反色边框
        cv2.polylines(img, [points], True, border_color, 1)
        
        return img

class PreviewLabel(QLabel):
    def __init__(self):
        super().__init__()
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(320, 256)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.setStyleSheet("background-color: palette(base); border: 1px solid palette(mid);")

    def set_frame(self, frame):
        if frame is None:
            return

        h, w = frame.shape[:2]
        label_w = self.width()
        label_h = self.height()

        if label_w <= 0 or label_h <= 0:
            return

        scale = min(label_w / w, label_h / h)
        new_w = int(w * scale)
        new_h = int(h * scale)

        resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
        bytes_per_line = 3 * new_w
        qimg = QImage(resized.data, new_w, new_h, bytes_per_line, QImage.Format.Format_BGR888)
        pixmap = QPixmap.fromImage(qimg)
        self.setPixmap(pixmap)

class MainWindow(QMainWindow):
    def __init__(self, silent_mode=False, startup_delay=0):
        super().__init__()
        self.config_mgr = ConfigManager()
        self.silent_mode = silent_mode
        self.startup_delay = startup_delay

        # 设置窗口图标（与托盘相同）
        self.set_window_icon()
        self.setWindowTitle("Display Stream Server")
        self.setMinimumSize(500, 700)

        # 中央控件
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(10, 10, 10, 10)
        main_layout.setSpacing(10)

        # 预览区域
        self.preview_label = PreviewLabel()
        main_layout.addWidget(self.preview_label)

        # 控制面板
        control_group = QGroupBox("配置")
        control_layout = QFormLayout()
        control_layout.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)

        # 显示器选择
        self.monitor_combo = QComboBox()
        self.refresh_monitors()
        self.monitor_combo.currentIndexChanged.connect(self.on_monitor_changed)
        control_layout.addRow("显示器:", self.monitor_combo)

        # FPS
        self.fps_spin = QSpinBox()
        self.fps_spin.setRange(1, 60)
        self.fps_spin.setValue(self.config_mgr.config['target_fps'])
        self.fps_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('target_fps', v))
        control_layout.addRow("目标FPS:", self.fps_spin)

        # 编码格式选择
        self.encoding_combo = QComboBox()
        self.encoding_combo.addItems(['jpg', 'png', 'webp'])
        self.encoding_combo.setCurrentText(self.config_mgr.config['encoding_format'])
        self.encoding_combo.currentTextChanged.connect(self.on_encoding_changed)
        control_layout.addRow("编码格式:", self.encoding_combo)

        # 创建格式特定参数的标签和控件
        # JPG 质量标签和控件
        self.jpg_label = QLabel("JPG质量(1-10):")
        self.jpg_quality_spin = QSpinBox()
        self.jpg_quality_spin.setRange(1, 10)
        self.jpg_quality_spin.setValue(self.config_mgr.config['jpeg_quality'])
        self.jpg_quality_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('jpeg_quality', v))
        control_layout.addRow(self.jpg_label, self.jpg_quality_spin)

        # PNG 压缩标签和控件
        self.png_label = QLabel("PNG压缩(0-9):")
        self.png_compression_spin = QSpinBox()
        self.png_compression_spin.setRange(0, 9)
        self.png_compression_spin.setValue(self.config_mgr.config['png_compression'])
        self.png_compression_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('png_compression', v))
        control_layout.addRow(self.png_label, self.png_compression_spin)

        # WebP 质量标签和控件
        self.webp_label = QLabel("WebP质量(1-10):")
        self.webp_quality_spin = QSpinBox()
        self.webp_quality_spin.setRange(1, 10)
        self.webp_quality_spin.setValue(self.config_mgr.config['webp_quality'])
        self.webp_quality_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('webp_quality', v))
        control_layout.addRow(self.webp_label, self.webp_quality_spin)

        # 端口配置
        port_layout = QHBoxLayout()
        self.zmq_port_spin = QSpinBox()
        self.zmq_port_spin.setRange(1000, 65535)
        self.zmq_port_spin.setValue(self.config_mgr.config['zmq_port'])
        self.zmq_port_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('zmq_port', v))

        self.tcp_port_spin = QSpinBox()
        self.tcp_port_spin.setRange(1000, 65535)
        self.tcp_port_spin.setValue(self.config_mgr.config['tcp_port'])
        self.tcp_port_spin.valueChanged.connect(lambda v: self.config_mgr.update_and_save('tcp_port', v))

        port_layout.addWidget(QLabel("ZMQ端口:"))
        port_layout.addWidget(self.zmq_port_spin)
        port_layout.addWidget(QLabel("TCP端口:"))
        port_layout.addWidget(self.tcp_port_spin)
        port_layout.addStretch()
        control_layout.addRow("", port_layout)

        # 开关布局
        switch_layout = QHBoxLayout()
        self.preview_checkbox = QCheckBox("启用预览")
        self.preview_checkbox.setChecked(True)
        self.preview_checkbox.stateChanged.connect(self.on_preview_toggled)

        self.autostart_checkbox = QCheckBox("开机自动启动")
        self.autostart_checkbox.stateChanged.connect(self.toggle_autostart)

        switch_layout.addWidget(self.preview_checkbox)
        switch_layout.addWidget(self.autostart_checkbox)
        switch_layout.addStretch()
        control_layout.addRow("", switch_layout)

        control_group.setLayout(control_layout)
        main_layout.addWidget(control_group)

        # 状态栏（右下角）
        status_layout = QHBoxLayout()
        status_layout.addStretch()
        self.status_label = QLabel("状态: 初始化中")
        self.conn_bandwidth_label = QLabel("连接: 无 | 带宽: -- Mbps | 发送: -- FPS")
        status_layout.addWidget(self.status_label)
        status_layout.addWidget(self.conn_bandwidth_label)
        main_layout.addLayout(status_layout)

        # 状态变量
        self.current_conn_count = 0
        self.current_bandwidth = "--"
        self.current_send_fps = "--"
        self.update_status_text()

        # 连接检测器
        self.conn_checker = ConnectionChecker(self.config_mgr)
        self.conn_checker.start_checking()

        # 工作线程
        self.worker = CaptureWorker(self.config_mgr, self.conn_checker)
        self.worker.frame_ready.connect(self.on_frame_received)
        self.worker.status_update.connect(self.update_status)
        self.worker.bandwidth_update.connect(self.update_bandwidth)
        self.worker.send_fps_update.connect(self.update_send_fps)
        self.worker.start_worker()

        # 连接状态定时器
        self.conn_timer = QTimer()
        self.conn_timer.timeout.connect(self.update_connection_status)
        self.conn_timer.start(2000)

        # 初始状态
        self.capture_active = True
        self.worker.set_capture_state(True)

        # 检查自启状态
        self.check_autostart_status()

        # 托盘图标
        self.create_tray_icon()
        self.setup_tray()

        # 更新 UI 可见性
        self.update_encoding_ui_visibility()

        self.status_label.setText("状态: 监听中")

    def update_encoding_ui_visibility(self):
        """根据选择的编码格式更新 UI 控件可见性"""
        encoding = self.encoding_combo.currentText()

        # 隐藏所有格式特定控件
        self.jpg_label.setVisible(False)
        self.jpg_quality_spin.setVisible(False)
        self.png_label.setVisible(False)
        self.png_compression_spin.setVisible(False)
        self.webp_label.setVisible(False)
        self.webp_quality_spin.setVisible(False)

        # 显示对应格式的控件
        if encoding == 'jpg':
            self.jpg_label.setVisible(True)
            self.jpg_quality_spin.setVisible(True)
        elif encoding == 'png':
            self.png_label.setVisible(True)
            self.png_compression_spin.setVisible(True)
        elif encoding == 'webp':
            self.webp_label.setVisible(True)
            self.webp_quality_spin.setVisible(True)

    def on_encoding_changed(self, encoding):
        """编码格式改变"""
        self.config_mgr.update_and_save('encoding_format', encoding)
        self.update_encoding_ui_visibility()

    def update_send_fps(self, fps):
        """更新发送帧率"""
        self.current_send_fps = f"{fps:.1f}"
        self.update_status_text()

    def update_connection_status(self):
        count = self.conn_checker.get_connected_count()
        self.current_conn_count = count
        self.update_status_text()

    def update_bandwidth(self, mbps):
        self.current_bandwidth = f"{mbps:.1f}" if mbps is not None else "--"
        self.update_status_text()

    def update_status_text(self):
        if self.current_conn_count > 0:
            text = f"连接: 已连接({self.current_conn_count}) | 带宽: {self.current_bandwidth} Mbps | 发送: {self.current_send_fps} FPS"
        else:
            self.on_preview_toggled(self.preview_checkbox.isChecked())
            text = f"连接: 无 | 带宽: -- Mbps | 发送: {self.current_send_fps} FPS"
        self.conn_bandwidth_label.setText(text)

    def set_window_icon(self):
        """设置窗口图标（与托盘相同）"""
        icon = QIcon.fromTheme("video-display")
        if icon.isNull():
            icon = QIcon.fromTheme("computer")
        if icon.isNull():
            icon = QIcon.fromTheme("application-x-executable")
        if icon.isNull():
            import io
            from PIL import Image, ImageDraw
            try:
                img = Image.new('RGBA', (16, 16), (255, 255, 255, 0))
                draw = ImageDraw.Draw(img)
                draw.rectangle([2, 2, 14, 14], outline=(0, 0, 0), width=1)
                draw.rectangle([4, 4, 12, 12], fill=(100, 150, 255))

                buf = io.BytesIO()
                img.save(buf, format='PNG')
                buf.seek(0)

                qimg = QImage()
                qimg.loadFromData(buf.getvalue())
                pixmap = QPixmap.fromImage(qimg)
                icon = QIcon(pixmap)
            except:
                icon = QIcon()

        self.setWindowIcon(icon)

    def setup_tray(self):
        """设置托盘行为"""
        if self.tray_icon:
            self.tray_icon.show()
            self.tray_icon.activated.connect(self.on_tray_activated)

    def on_tray_activated(self, reason):
        """托盘点击事件"""
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.show_window()

    def resizeEvent(self, event):
        if hasattr(self, '_current_frame') and self.preview_checkbox.isChecked():
            self.preview_label.set_frame(self._current_frame)
        super().resizeEvent(event)

    def on_frame_received(self, frame):
        self._current_frame = frame
        if self.preview_checkbox.isChecked():
            self.preview_label.set_frame(frame)

    def on_preview_toggled(self, state):
        enabled = bool(state)
        if enabled:
            self.update_status("状态: 预览已启用")
        else:
            self.update_status("状态: 监听中")
            self.preview_label.clear()

    def on_monitor_changed(self):
        monitor_idx = self.monitor_combo.currentData()
        self.config_mgr.update_and_save('monitor_idx', monitor_idx)
        self.worker.set_capture_state(self.capture_active, monitor_idx)

    def update_status(self, text):
        self.status_label.setText(text)

    def refresh_monitors(self):
        """改进的显示器检测（带重试）"""
        self.monitor_combo.clear()
        max_retries = 5
        for attempt in range(max_retries):
            try:
                with mss.mss() as sct:
                    if len(sct.monitors) > 1:
                        for i, mon in enumerate(sct.monitors[1:], 1):
                            name = f"显示器 {i} ({mon['width']}×{mon['height']})"
                            self.monitor_combo.addItem(name, i)
                        break
                    else:
                        if attempt < max_retries - 1:
                            time.sleep(2)
            except Exception as e:
                if attempt < max_retries - 1:
                    time.sleep(2)

        saved_idx = self.config_mgr.config['monitor_idx']
        if saved_idx < len(sct.monitors):
            self.monitor_combo.setCurrentIndex(saved_idx - 1)

    def get_current_exe_path(self):
        """获取当前执行文件的正确路径"""
        if getattr(sys, 'frozen', False):
            # PyInstaller 打包后的路径
            return sys.executable
        else:
            # 开发模式：返回当前脚本路径
            return os.path.abspath(__file__)

    def validate_autostart_path(self):
        """验证并修复自启路径（在启动时调用）"""
        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Run",
                            0, winreg.KEY_READ)
            try:
                stored_path, _ = winreg.QueryValueEx(key, "VirtualDisplaySender")
                current_path = self.get_current_exe_path()

                # 检查存储的路径是否还有效
                stored_exe = stored_path.strip('"').split()[0]  # 移除引号并取第一个参数

                if stored_exe != current_path and os.path.exists(current_path):
                    # 路径已改变，更新注册表
                    logger.info(f"检测到路径变化: {stored_exe} -> {current_path}")

                    # 重新设置正确的路径
                    winreg.CloseKey(key)
                    key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                    r"Software\Microsoft\Windows\CurrentVersion\Run",
                                    0, winreg.KEY_WRITE)

                    new_command = f'"{current_path}" --silent 5'
                    winreg.SetValueEx(key, "VirtualDisplaySender", 0, winreg.REG_SZ, new_command)
                    logger.info(f"自启路径已更新: {new_command}")

            except FileNotFoundError:
                # 没有设置自启，无需处理
                pass
            finally:
                winreg.CloseKey(key)
        except Exception as e:
            logger.error(f"验证自启路径失败: {e}")

    def toggle_autostart(self, state):
        """切换开机自启（带5秒延迟）"""
        # 检查是否为打包版本
        if state == Qt.CheckState.Checked.value and not getattr(sys, 'frozen', False):
            # 不是打包版本，报错并不设置自启
            from PyQt6.QtWidgets import QMessageBox
            msg_box = QMessageBox()
            msg_box.setIcon(QMessageBox.Icon.Warning)
            msg_box.setWindowTitle("警告")
            msg_box.setText("只有打包的EXE版本才支持开机自启功能。")
            msg_box.exec()
            
            # 取消勾选
            self.autostart_checkbox.blockSignals(True)
            self.autostart_checkbox.setChecked(False)
            self.autostart_checkbox.blockSignals(False)
            return

        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Run",
                            0, winreg.KEY_WRITE)

            current_exe_path = self.get_current_exe_path()

            if state == Qt.CheckState.Checked.value:
                # 开机自启带5秒延迟
                command = f'"{current_exe_path}" --silent 5'
                winreg.SetValueEx(key, "VirtualDisplaySender", 0, winreg.REG_SZ, command)
                logger.info(f"开机自启已启用: {command}")
            else:
                try:
                    winreg.DeleteValue(key, "VirtualDisplaySender")
                    logger.info("开机自启已禁用")
                except FileNotFoundError:
                    pass
            winreg.CloseKey(key)
        except Exception as e:
            logger.error(f"设置开机自启失败: {e}")
            self.autostart_checkbox.blockSignals(True)
            self.autostart_checkbox.setChecked(not bool(state))
            self.autostart_checkbox.blockSignals(False)

    def check_autostart_status(self):
        """检查当前开机自启状态"""
        # 先验证路径
        self.validate_autostart_path()

        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Run",
                            0, winreg.KEY_READ)
            try:
                value, _ = winreg.QueryValueEx(key, "VirtualDisplaySender")
                
                # 如果当前不是打包版本，直接移除自启项
                if not getattr(sys, 'frozen', False):
                    winreg.CloseKey(key)
                    # 以写权限重新打开并删除
                    key_write = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                    r"Software\Microsoft\Windows\CurrentVersion\Run",
                                    0, winreg.KEY_WRITE)
                    try:
                        winreg.DeleteValue(key_write, "VirtualDisplaySender")
                        logger.info("已移除开机自启项（非EXE版本）")
                    except FileNotFoundError:
                        pass
                    winreg.CloseKey(key_write)
                    self.autostart_checkbox.setChecked(False)
                else:
                    self.autostart_checkbox.setChecked(True)
                    
            except FileNotFoundError:
                self.autostart_checkbox.setChecked(False)
            finally:
                winreg.CloseKey(key)
        except Exception as e:
            logger.error(f"检查自启状态失败: {e}")
            self.autostart_checkbox.setChecked(False)

    def create_tray_icon(self):
        """创建托盘图标（使用与窗口相同的图标）"""
        self.tray_icon = QSystemTrayIcon(self)

        window_icon = self.windowIcon()
        if not window_icon.isNull():
            self.tray_icon.setIcon(window_icon)
        else:
            icon = QIcon.fromTheme("video-display")
            if icon.isNull():
                icon = QIcon.fromTheme("computer")
            self.tray_icon.setIcon(icon)

        # 创建菜单
        menu = QMenu()
        show_action = QAction("显示窗口", self)
        show_action.triggered.connect(self.show_window)
        quit_action = QAction("退出", self)
        quit_action.triggered.connect(self.quit_app)
        menu.addAction(show_action)
        menu.addAction(quit_action)
        self.tray_icon.setContextMenu(menu)

    def show_window(self):
        """显示窗口"""
        self.showNormal()
        self.raise_()
        self.activateWindow()
        # 窗口显示时启用捕获
        self.capture_active = True
        self.worker.set_capture_state(True)

    def closeEvent(self, event):
        """窗口关闭事件"""
        event.ignore()  # 隐藏窗口到托盘
        self.hide()
        # 窗口隐藏时禁用预览，但仍保持捕获（如果网络连接存在）
        self.preview_checkbox.setChecked(False)
        self.preview_label.clear()
        # 保持网络捕获，如果存在连接
        if self.conn_checker.get_connected_count() > 0:
            self.worker.set_capture_state(True)
        else:
            self.worker.set_capture_state(False)

    def quit_app(self):
        """退出程序"""
        self.conn_timer.stop()
        self.worker.stop_worker()
        self.conn_checker.stop_checking()
        if self.tray_icon:
            self.tray_icon.hide()
        QApplication.quit()

def main():
    args = parse_args()

    # 确保工作目录正确
    if getattr(sys, 'frozen', False):
        # PyInstaller 打包版本
        application_path = os.path.dirname(sys.executable)
    else:
        # 开发版本
        application_path = os.path.dirname(os.path.abspath(__file__))
    os.chdir(application_path)

    app = QApplication(sys.argv)
    app.setApplicationName("VirtualDisplaySender")

    # 设置应用图标
    window_icon = QIcon.fromTheme("video-display")
    if window_icon.isNull():
        window_icon = QIcon.fromTheme("computer")
    if not window_icon.isNull():
        app.setWindowIcon(window_icon)

    # 处理启动参数
    if args.silent is not None:
        startup_delay = args.silent
        if startup_delay > 0:
            logger.info(f"等待 {startup_delay} 秒后启动...")
            time.sleep(startup_delay)

        window = MainWindow(silent_mode=True, startup_delay=startup_delay)
        window.hide()  # 静默启动，只显示托盘
        window.preview_checkbox.setChecked(False)
        window.preview_label.clear()
        if window.conn_checker.get_connected_count() > 0:
            window.worker.set_capture_state(True)
        else:
            window.worker.set_capture_state(False)
        logger.info("Display Stream Server started in silent mode")
    else:
        window = MainWindow(silent_mode=False)
        window.show()

    sys.exit(app.exec())

if __name__ == "__main__":
    main()
