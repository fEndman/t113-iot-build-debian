# ui/panels/desktop_stream_panel.py
import sys
import time
import threading
from collections import deque
from PyQt6.QtWidgets import QLabel, QWidget, QVBoxLayout
from PyQt6.QtGui import QPixmap, QImage
from PyQt6.QtCore import QTimer, Qt, QThread, pyqtSignal
import logging
from ui.base import BasePanel
from config import (
    DESKTOP_STREAM_IP,
    DESKTOP_STREAM_TCP_PORT,
    DESKTOP_STREAM_ZMQ_PORT,
    DESKTOP_STREAM_BUFFER_SIZE,
    DESKTOP_STREAM_TARGET_FPS,
    BRIGHTNESS_MIN
)
from services.zmq_stream_service import ZMQStreamService

logger = logging.getLogger(__name__)

class DesktopStreamPanel(BasePanel):
    def __init__(self, state_manager=None):
        super().__init__()
        self.state_manager = state_manager
        self.init_ui()
        self.client = None
        self.render_buffer = deque(maxlen=DESKTOP_STREAM_BUFFER_SIZE)
        self.render_timer = None
        self.render_interval_ms = int(1000 / DESKTOP_STREAM_TARGET_FPS)
        self.active = False
        self.rendered_frame_count = 0
        self.render_fps_start_time = time.time()
        self.stats_timer = None
        # Overlay for connection status
        self.status_overlay = QLabel()
        overlay_font = self.status_overlay.font()
        overlay_font.setPointSize(8)
        self.status_overlay.setFont(overlay_font)
        self.status_overlay.setStyleSheet("color: white; background-color: rgba(0, 0, 0, 180); border: 1px solid white;")
        self.status_overlay.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.status_overlay.setParent(self)
        self.status_overlay.setGeometry(8, 36, 144, 56)
        self.status_overlay.hide()
        # Cache for connection status
        self._last_conn_status_shown = None
        # Black Screen Detection
        self._last_black_screen_state = False
        self._black_detection_sample_step = 8
        self._black_detection_threshold = 10
        self._black_detection_ratio = 0.95
        self._black_detection_frame_counter = 0 # Counter for interval detection
        self._black_detection_interval = 5 # Check every N frames

    def init_ui(self):
        layout = QVBoxLayout()
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.video_label = QLabel()
        self.video_label.setFixedSize(160, 128)
        self.video_label.setStyleSheet("background-color: black;")
        self.video_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.video_label)
        self.setLayout(layout)
        self.setFixedSize(160, 128)

    def _get_backlight_manager(self):
        if self.state_manager and hasattr(self.state_manager, 'backlight_manager'):
            return self.state_manager.backlight_manager
        return None

    def _check_if_frame_is_black(self, qimage):
        width = qimage.width()
        height = qimage.height()
        step = self._black_detection_sample_step
        threshold = self._black_detection_threshold
        required_pixels = int((width // step) * (height // step) * self._black_detection_ratio)

        black_pixel_count = 0
        total_samples = 0

        for x in range(0, width, step):
            for y in range(0, height, step):
                pixel_color = qimage.pixelColor(x, y)
                if (pixel_color.red() <= threshold and
                    pixel_color.green() <= threshold and
                    pixel_color.blue() <= threshold):
                    black_pixel_count += 1
                total_samples += 1
                if black_pixel_count >= required_pixels:
                    return True

        return black_pixel_count >= required_pixels

    def on_enter(self):
        logger.info("Activated, initializing resources.")
        self.active = True
        self.video_label.clear()
        self.render_buffer.clear()
        self.rendered_frame_count = 0
        self.render_fps_start_time = time.time()
        self.buffer_len_count = 0
        self._last_conn_status_shown = None
        self._last_black_screen_state = False
        self._black_detection_frame_counter = 0 # Reset counter when panel becomes active
        backlight_mgr = self._get_backlight_manager()
        if backlight_mgr:
            backlight_mgr.request_keep_screen_on()
        self.client = ZMQStreamService(
            tcp_ip=DESKTOP_STREAM_IP,
            tcp_port=DESKTOP_STREAM_TCP_PORT,
            zmq_port=DESKTOP_STREAM_ZMQ_PORT,
            target_size=(160, 128)
        )
        # Connect signals from the client service to local slots
        self.client.frame_decoded.connect(self.on_decoded_pixmap)
        self.client.connection_status_changed.connect(self.on_connection_status_change) # Connect the client's signal to this panel's slot
        self.client.connect()
        self.render_timer = QTimer(self)
        self.render_timer.timeout.connect(self.render_frame)
        self.render_timer.start(self.render_interval_ms)
        self.stats_timer = QTimer(self)
        self.stats_timer.timeout.connect(self._log_stats)
        self.stats_timer.start(10000)
        self._update_status_overlay("Connecting...", True)

    def on_leave(self):
        logger.info("Deactivated, releasing resources.")
        self.active = False
        if self.render_timer:
            self.render_timer.stop()
            self.render_timer.deleteLater()
            self.render_timer = None
        if self.stats_timer:
            self.stats_timer.stop()
            self.stats_timer.deleteLater()
            self.stats_timer = None
        if self.client:
            self.client.disconnect()
            try:
                self.client.frame_decoded.disconnect(self.on_decoded_pixmap)
            except TypeError:
                pass
            try:
                self.client.connection_status_changed.disconnect(self.on_connection_status_change) # Disconnect the signal
            except TypeError:
                pass
            self.client = None
        self.render_buffer.clear()
        self.video_label.clear()
        self.status_overlay.hide()
        self._last_conn_status_shown = None
        self._last_black_screen_state = False
        backlight_mgr = self._get_backlight_manager()
        if backlight_mgr:
            backlight_mgr.release_keep_screen_on()

    def _update_status_overlay(self, message, visible):
        if self.status_overlay.text() != message or self.status_overlay.isVisible() != visible:
            self.status_overlay.setText(message)
            if visible:
                self.status_overlay.show()
            else:
                self.status_overlay.hide()

    def on_connection_status_change(self, connected, message):
        if connected:
            self._update_status_overlay("", False)
            self._last_conn_status_shown = ("connected", "")

    def on_decoded_pixmap(self, pixmap):
        if self.active:
            self.render_buffer.append(pixmap)
            self._black_detection_frame_counter += 1
            if self._black_detection_frame_counter >= self._black_detection_interval:
                self._black_detection_frame_counter = 0 # Reset counter
                qimage = pixmap.toImage()
                if qimage.format() in [QImage.Format.Format_RGB32, QImage.Format.Format_ARGB32, QImage.Format.Format_RGBA8888]:
                     current_black_state = self._check_if_frame_is_black(qimage)
                else:
                    qimage = qimage.convertToFormat(QImage.Format.Format_RGB32)
                    current_black_state = self._check_if_frame_is_black(qimage)

                if current_black_state != self._last_black_screen_state:
                     logger.debug(f"Black screen state changed to: {current_black_state}")
                     backlight_mgr = self._get_backlight_manager()
                     if backlight_mgr:
                         if current_black_state:
                             logger.info("Detected black screen, lowering backlight.")
                             if hasattr(backlight_mgr, 'set_brightness'):
                                 backlight_mgr.set_brightness(BRIGHTNESS_MIN)
                             else:
                                 logger.warning("BacklightManager object does not have set_brightness method.")
                         else:
                             logger.info("Black screen ended, restoring backlight.")
                             backlight_mgr.request_keep_screen_on()
                     self._last_black_screen_state = current_black_state

    def render_frame(self):
        if self.active and self.render_timer:
            buf_len = len(self.render_buffer)
            self.buffer_len_count += buf_len
            if buf_len >= 1:
                pixmap = self.render_buffer.popleft()
                self.video_label.setPixmap(pixmap)
                self.rendered_frame_count += 1

        # Check client connection status for UI overlay, only if panel is active
        if self.client and not self.client._tcp_connected and self.active:
            if self.client._zmq_initialized:
                current_status_str = f"Desktop Stream\nStatus: Connection Error\nTarget: {DESKTOP_STREAM_IP}\nTCP:{DESKTOP_STREAM_TCP_PORT} ZMQ:{DESKTOP_STREAM_ZMQ_PORT}"
            else:
                current_status_str = f"Desktop Stream\nStatus: Connecting...\nTarget: {DESKTOP_STREAM_IP}\nTCP:{DESKTOP_STREAM_TCP_PORT} ZMQ:{DESKTOP_STREAM_ZMQ_PORT}"

            if self._last_conn_status_shown != ("connecting_tcp_ok_zmq_not", current_status_str):
                self._update_status_overlay(current_status_str, True)
                self._last_conn_status_shown = ("connecting_tcp_ok_zmq_not", current_status_str)
        elif self.client and self.client._tcp_connected and self.active:
            if self._last_conn_status_shown and self._last_conn_status_shown[0] in ("disconnected", "connecting_tcp_ok_zmq_not"):
                 self._update_status_overlay("", False)
                 self._last_conn_status_shown = ("connected", "")

    def _log_stats(self):
        if not self.active:
            logger.debug("Stats timer fired, but panel is inactive. Skipping log.")
            return
        current_time = time.time()
        duration = current_time - self.render_fps_start_time
        render_fps = (self.rendered_frame_count / duration) if duration > 0 else 0
        buffer_avg_size = (self.buffer_len_count / self.rendered_frame_count) if self.rendered_frame_count != 0 else 0

        logger.info(f"Desktop Panel Render FPS: {render_fps:.1f}, Buf Avg: {buffer_avg_size:.1f}({buffer_avg_size/DESKTOP_STREAM_BUFFER_SIZE:.0%})")
        self.rendered_frame_count = 0
        self.buffer_len_count = 0
        self.buffer_under_run_count = 0
        self.render_fps_start_time = current_time

    def closeEvent(self, event):
        if self.active:
            self.on_leave()
        super().closeEvent(event)
