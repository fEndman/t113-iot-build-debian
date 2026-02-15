# core/backlight.py
"""背光管理 - 精准计时器控制"""
import os
import sys
import atexit
import signal
import logging
from PyQt6.QtCore import QTimer, QObject

logger = logging.getLogger(__name__)

# 从 config 导入常量
from config import BACKLIGHT_PATH, BRIGHTNESS_START, BRIGHTNESS_MIN, BRIGHTNESS_EXIT, BRIGHTNESS_DIM_TIME, BRIGHTNESS_DIM_STEP

BRIGHTNESS_STEPS = BRIGHTNESS_START - BRIGHTNESS_MIN

class BacklightManager(QObject):
    def __init__(self):
        super().__init__()
        self.brightness = BRIGHTNESS_START
        self.dim_step = 0
        # 60秒无操作计时器
        self.idle_timer = QTimer(self)
        self.idle_timer.setSingleShot(True)
        self.idle_timer.timeout.connect(self.start_dimming)
        # 渐暗计时器 (每x秒降1级)
        self.dim_timer = QTimer(self)
        self.dim_timer.setInterval(int(BRIGHTNESS_DIM_STEP * 1000))
        self.dim_timer.timeout.connect(self.dim_step_down)

        # Flag to indicate external requests to keep screen on
        self._keep_screen_on_requested = False

        # 初始化背光
        self.set_brightness(BRIGHTNESS_START)
        self.reset_idle_timer()

    def reset_idle_timer(self):
        """按键时立即重置计时器并恢复亮度"""
        if not self._keep_screen_on_requested:
            self.idle_timer.stop()
            self.idle_timer.start(int(BRIGHTNESS_DIM_TIME * 1000))
            # 停止渐暗
            self.dim_timer.stop()
            self.dim_step = 0
            # 恢复亮度
            if self.brightness != BRIGHTNESS_START:
                self.set_brightness(BRIGHTNESS_START)
                self.brightness = BRIGHTNESS_START

    def request_keep_screen_on(self):
        """请求保持屏幕开启"""
        logger.info("Screen-on request received.")
        self._keep_screen_on_requested = True
        self.idle_timer.stop()
        self.dim_timer.stop()
        self.dim_step = 0
        if self.brightness != BRIGHTNESS_START:
            self.set_brightness(BRIGHTNESS_START)
            self.brightness = BRIGHTNESS_START

    def release_keep_screen_on(self):
        """释放屏幕开启请求"""
        logger.info("Screen-on request released.")
        self._keep_screen_on_requested = False
        self.reset_idle_timer()

    def set_brightness(self, value: int) -> bool:
        try:
            val = max(1, min(19, int(value)))
            with open(BACKLIGHT_PATH, 'w') as f:
                f.write(str(val))
            self.brightness = val
            return True
        except Exception as e:
            logger.error(f"Error setting backlight: {e}")
            return False

    def start_dimming(self):
        """开始渐暗过程"""
        if self._keep_screen_on_requested:
            return
        if self.brightness <= BRIGHTNESS_MIN:
            return
        self.dim_step = 0
        if self._keep_screen_on_requested: # Double-check before starting timer
            return
        self.dim_timer.start()

    def dim_step_down(self):
        """每x秒降低一级亮度"""
        if self._keep_screen_on_requested:
            return
        if self.dim_step >= BRIGHTNESS_STEPS:
            self.dim_timer.stop()
            return
        self.brightness -= 1
        self.set_brightness(self.brightness)
        self.dim_step += 1
        if self.brightness <= BRIGHTNESS_MIN:
            self.dim_timer.stop()

# --- 退出保障 ---
def restore_exit_brightness():
    """恢复退出时的亮度"""
    try:
        with open(BACKLIGHT_PATH, 'w') as f:
            f.write(str(BRIGHTNESS_EXIT))
        logger.info(f"[EXIT] Backlight restored to {BRIGHTNESS_EXIT}")
    except Exception as e:
        logger.error(f"[EXIT] Failed to restore backlight: {e}")

# 注册程序正常退出时的回调
atexit.register(restore_exit_brightness)

# 注册信号处理器，捕获 Ctrl+C (SIGINT) 和 kill (SIGTERM) 信号
def signal_handler(signum, frame):
    logger.info(f"Received signal {signum}, restoring backlight and exiting...")
    restore_exit_brightness()
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)
