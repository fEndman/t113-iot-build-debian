# services/zmq_stream_service.py
"""ZeroMQ 流媒体服务 - 负责网络连接、数据接收及预解码（支持多格式）"""
import zmq
import socket
import time
import threading
from collections import deque
from PyQt6.QtCore import QObject, pyqtSignal, Qt, QTimer
from PyQt6.QtGui import QPixmap, QImage
from PIL import Image
from io import BytesIO
import logging

logger = logging.getLogger(__name__)

# --- 从 config 导入服务端公钥路径 ---
from config import DESKTOP_STREAM_PUB_KEY_PATH
# --- 生成客户端密钥对 ---
CLIENT_CURVE_PUBLIC_KEY, CLIENT_CURVE_SECRET_KEY = zmq.curve_keypair()

class ZMQStreamService(QObject):
    frame_decoded = pyqtSignal(QPixmap)
    connection_status_changed = pyqtSignal(bool, str)

    def __init__(self, tcp_ip, tcp_port, zmq_port, target_size=(160, 128)):
        super().__init__()
        self.tcp_ip = tcp_ip
        self.tcp_port = tcp_port
        self.zmq_port = zmq_port
        self.target_width, self.target_height = target_size

        self.tcp_socket = None
        self.tcp_lock = threading.Lock()
        self.zmq_ctx = None
        self.zmq_sock = None
        self.zmq_poller = None
        self.zmq_lock = threading.Lock()

        self.service_condition = threading.Condition(threading.RLock())
        self._running = False
        self._tcp_connected = False
        self._zmq_initialized = False

        self.tcp_thread = None
        self.zmq_decode_thread = None

        self.frame_count = 0
        self.fps_start_time = time.time()
        self.stats_timer = None

    def _initialize_zmq_resources(self):
        try:
            self.zmq_ctx = zmq.Context()
            self.zmq_sock = self.zmq_ctx.socket(zmq.SUB)

            # --- 加载服务端公钥 ---
            server_public_key = None
            try:
                with open(DESKTOP_STREAM_PUB_KEY_PATH, 'rb') as f:
                    server_public_key = f.read()
                logger.info(f"ZMQ Curve: Loaded server public key from {DESKTOP_STREAM_PUB_KEY_PATH}")
            except FileNotFoundError:
                logger.error(f"ZMQ Curve: Server public key file not found: {DESKTOP_STREAM_PUB_KEY_PATH}")
                raise FileNotFoundError(f"Server public key file '{DESKTOP_STREAM_PUB_KEY_PATH}' not found.")
            except Exception as e:
                logger.error(f"ZMQ Curve: Failed to read server public key: {e}")
                raise e

            # --- 启用 Curve 加密 ---
            # 设置服务端公钥（用于验证服务端身份）
            self.zmq_sock.setsockopt(zmq.CURVE_SERVERKEY, server_public_key)
            # 设置客户端密钥对（用于加密和认证）
            self.zmq_sock.setsockopt(zmq.CURVE_PUBLICKEY, CLIENT_CURVE_PUBLIC_KEY)
            self.zmq_sock.setsockopt(zmq.CURVE_SECRETKEY, CLIENT_CURVE_SECRET_KEY)
            # ---

            self.zmq_sock.connect(f"tcp://{self.tcp_ip}:{self.zmq_port}")
            self.zmq_sock.setsockopt(zmq.SUBSCRIBE, b"")
            self.zmq_sock.setsockopt(zmq.RCVHWM, 2)
            self.zmq_poller = zmq.Poller()
            self.zmq_poller.register(self.zmq_sock, zmq.POLLIN)
            self._zmq_initialized = True
            logger.info("ZMQ resources initialized with Curve encryption.")
            return True
        except Exception as e:
            logger.error(f"Failed to initialize ZMQ resources with Curve encryption: {e}")
            self._cleanup_zmq_resources()
            return False

    def _cleanup_zmq_resources(self):
        with self.zmq_lock:
            if self.zmq_sock:
                try:
                    if self.zmq_poller:
                        self.zmq_poller.unregister(self.zmq_sock)
                    self.zmq_sock.close(linger=0)
                    self.zmq_sock = None
                except Exception as e:
                    logger.warning(f"Error closing ZMQ socket: {e}")
            if self.zmq_ctx:
                try:
                    self.zmq_ctx.term()
                    self.zmq_ctx = None
                except Exception as e:
                    logger.warning(f"Error terminating ZMQ context: {e}")
            if self.zmq_poller:
                self.zmq_poller = None
        self._zmq_initialized = False
        logger.debug("ZMQ resources cleaned up.")

    def connect(self):
        with self.service_condition:
            if self._running:
                logger.warning("ZMQStreamService: Already running, ignoring connect request.")
                return

            logger.info(f"Attempting to connect to {self.tcp_ip}:{self.tcp_port} (TCP) and {self.tcp_ip}:{self.zmq_port} (ZMQ)")
            self._running = True
            self._tcp_connected = False
            self._zmq_initialized = False
            self.service_condition.notify_all()

        self.connection_status_changed.emit(False, "Connecting...")

        if not self._initialize_zmq_resources():
            self._cleanup_and_notify(status_message="ZMQ Init Error")
            return

        self.zmq_decode_thread = threading.Thread(target=self._zmq_receive_decode_loop, daemon=True)
        self.zmq_decode_thread.start()
        logger.info("ZMQ receive+decode thread started.")

        self.tcp_thread = threading.Thread(target=self._tcp_control_loop, daemon=True)
        self.tcp_thread.start()
        logger.info("TCP control thread started.")

        self.stats_timer = QTimer()
        self.stats_timer.timeout.connect(self._log_stats)
        self.stats_timer.start(10000)
        logger.info("Stats timer started.")

    def disconnect(self):
        logger.info("Disconnecting...")

        if self.stats_timer:
            self.stats_timer.stop()
            self.stats_timer.deleteLater()
            self.stats_timer = None
            logger.info("Stats timer stopped and scheduled for deletion.")

        with self.service_condition:
            was_running = self._running
            self._running = False
            self.service_condition.notify_all()

        self._force_close_sockets()
        self._cleanup_and_notify(status_message="Disconnected" if was_running else "Already disconnected")

    def _force_close_sockets(self):
        with self.tcp_lock:
            if self.tcp_socket:
                try: self.tcp_socket.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                try: self.tcp_socket.close()
                except OSError: pass
                self.tcp_socket = None

    def _cleanup_and_notify(self, status_message="Disconnected"):
        self._cleanup()
        self.connection_status_changed.emit(False, status_message)
        logger.info(f"{status_message}")

    def _cleanup(self):
        if self.tcp_thread and self.tcp_thread.is_alive():
            self.tcp_thread.join(timeout=2.0)
        if self.zmq_decode_thread and self.zmq_decode_thread.is_alive():
            self.zmq_decode_thread.join(timeout=2.0)

        self._cleanup_zmq_resources()

    def _tcp_control_loop(self):
        heartbeat_interval = 1.0
        reconnect_interval = 5.0

        while True:
            with self.service_condition:
                if not self._running:
                    break

            if not self._tcp_connect():
                with self.service_condition:
                    if not self._running:
                        break
                time.sleep(reconnect_interval)
                continue

            while True:
                with self.service_condition:
                    if not self._running or not self._tcp_connected:
                        break

                time.sleep(heartbeat_interval)

                with self.service_condition:
                    if not self._running or not self._tcp_connected:
                        break

                try:
                    with self.tcp_lock:
                        if not self.tcp_socket:
                            break
                        self.tcp_socket.send(b"hb")
                    with self.service_condition:
                        self._last_heartbeat_time = time.time()
                        if self._running and self._tcp_connected:
                            self.connection_status_changed.emit(True, "Streaming...")
                except Exception as e:
                    logger.warning(f"Heartbeat failed: {e}")
                    break

            with self.service_condition:
                if not self._running:
                    break
                self._tcp_disconnect()
        logger.debug("TCP control loop ended.")

    def _tcp_connect(self):
        try:
            with self.service_condition:
                if not self._running:
                    return False

            with self.tcp_lock:
                if self.tcp_socket:
                    try: self.tcp_socket.close()
                    except: pass
                    self.tcp_socket = None

                self.tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.tcp_socket.settimeout(3.0)
                self.tcp_socket.connect((self.tcp_ip, self.tcp_port))
                self.tcp_socket.send(b"client_connected")
                response = self.tcp_socket.recv(1024)

            logger.info(f"TCP control connection established: {response.decode()}")
            with self.service_condition:
                self._tcp_connected = True
            return True
        except Exception as e:
            logger.warning(f"TCP connection failed: {e}")
            self._tcp_disconnect()
            with self.service_condition:
                if self._running:
                    self.connection_status_changed.emit(False, f"TCP Conn Error: {e}")
            return False

    def _tcp_disconnect(self):
        with self.tcp_lock:
            if self.tcp_socket:
                try: self.tcp_socket.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                try: self.tcp_socket.close()
                except OSError: pass
                self.tcp_socket = None
        with self.service_condition:
            self._tcp_connected = False

    def _zmq_receive_decode_loop(self):
        poll_timeout = 1000
        batch_size = 5
        zmq_reinit_needed = False

        logger.debug("ZMQ receive+decode loop started.")
        while True:
            with self.service_condition:
                if not self._running:
                    logger.debug("ZMQ loop exiting due to stop signal.")
                    break
                if zmq_reinit_needed:
                    logger.info("ZMQ loop detected re-init flag, attempting re-initialization...")
                    self._cleanup_zmq_resources()
                    if self._initialize_zmq_resources():
                        logger.info("ZMQ re-initialization successful.")
                        zmq_reinit_needed = False
                        continue
                    else:
                        logger.error("ZMQ re-initialization failed.")
                        break

            try:
                with self.zmq_lock:
                    if not self._zmq_initialized or not self.zmq_sock or not self.zmq_poller:
                        logger.debug("ZMQ resources invalid, setting re-init flag.")
                        zmq_reinit_needed = True
                        continue
                    received_messages = []
                    socks = dict(self.zmq_poller.poll(poll_timeout))
                    while self.zmq_sock in socks and socks[self.zmq_sock] == zmq.POLLIN:
                        try:
                            format_bytes, data = self.zmq_sock.recv_multipart(flags=zmq.NOBLOCK)
                            format_hint = format_bytes.decode()
                            received_messages.append((format_hint, data))
                            socks = dict(self.zmq_poller.poll(0))
                        except zmq.Again:
                            break
                        except Exception as e:
                            logger.warning(f"Error receiving ZMQ multipart: {e}")
                            zmq_reinit_needed = True
                            break

                    if not zmq_reinit_needed:
                        for format_hint, data in received_messages:
                            pixmap = self._decode_image_to_pixmap(data, format_hint)
                            if pixmap:
                                self.frame_decoded.emit(pixmap)
                                with self.service_condition:
                                    self.frame_count += 1
            except Exception as e:
                logger.error(f"Unexpected error in ZMQ receive+decode loop: {e}")
                zmq_reinit_needed = True
        logger.debug("ZMQ receive+decode loop ended.")

    def _decode_image_to_pixmap(self, image_data, format_hint):
        try:
            img = Image.open(BytesIO(image_data))
            if img.mode != "RGB":
                img = img.convert("RGB")

            qimg = QImage(
                img.tobytes(),
                img.width,
                img.height,
                QImage.Format.Format_RGB888
            )
            pixmap = QPixmap.fromImage(qimg)
            scaled = pixmap.scaled(
                self.target_width, self.target_height,
                Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.TransformationMode.FastTransformation
            )
            return scaled
        except Exception as e:
            logger.warning(f"Decode error for format {format_hint}: {e}")
            return None

    def _log_stats(self):
        if not self._running:
            logger.debug("Stats timer fired, but service is inactive. Skipping log.")
            return

        current_time = time.time()
        duration = current_time - self.fps_start_time
        recv_fps = self.frame_count / duration if duration > 0 else 0

        logger.info(f"ZMQ Service Recv FPS: {recv_fps:.1f}")

        self.frame_count = 0
        self.fps_start_time = current_time

    def get_current_fps(self):
        with self.service_condition:
            current_time = time.time()
            duration = current_time - self.fps_start_time
            fps = self.frame_count / duration if duration > 0 else 0
        return fps

    def __del__(self):
        if self._running:
            logger.warning("ZMQStreamService: Destructor called while still running! Calling disconnect.")
            self.disconnect()
