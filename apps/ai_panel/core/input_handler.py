# core/input_handler.py
"""输入处理器 - 处理GPIO按键输入"""
import evdev
import asyncio
from evdev import categorize, ecodes
from PyQt6.QtCore import QObject, pyqtSignal
import logging
import threading
import time

logger = logging.getLogger(__name__)

class InputHandler(QObject):
    # 定义按键名称常量
    KEY_UP = "up"
    KEY_DOWN = "down"
    KEY_ENTER = "enter"

    # PyQt信号，用于在主线程中处理按键事件
    key_pressed = pyqtSignal(str)

    def __init__(self, device_path="/dev/input/by-path/platform-gpio-keys-event"):
        super().__init__()
        self.device_path = device_path
        self.device = None
        self.running = False  # Add a flag to control the loop
        self.read_thread = None # Keep track of the reading thread
        self._initialize_device()

    def _initialize_device(self):
        """初始化输入设备"""
        try:
            self.device = evdev.InputDevice(self.device_path)
            logger.info(f"Grabbed input device: {self.device.name}")
            self.device.grab()  # Grab the device to prevent other apps from receiving events
            self.running = True # Set running flag to True
            # Start the reading loop in a separate thread
            self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.read_thread.start()
        except FileNotFoundError:
            logger.error(f"Input device not found: {self.device_path}")
            raise
        except PermissionError:
            logger.error(f"Permission denied to access input device: {self.device_path}")
            raise
        except Exception as e:
            logger.error(f"Failed to initialize input device: {e}")
            raise

    def _read_loop(self):
        """在单独线程中持续读取按键事件"""
        logger.debug(f"_read_loop thread {threading.current_thread().ident} started.")
        while self.running: # Use the running flag to control the loop
            try:
                # Use async iterator to handle potential interruption
                # However, evdev.read_loop() is blocking. We need to handle interruption differently.
                # Since we can't interrupt a blocking read easily, we'll use the running flag
                # and catch the OSError when device is closed externally.
                for event in self.device.read_loop():
                    if not self.running: # Check flag frequently inside the loop
                        break
                    if event.type == ecodes.EV_KEY and event.value == 1:  # Key pressed
                        key_name = self._map_keycode(event.code)
                        if key_name:
                            logger.debug(f"Key pressed: {key_name}")
                            # Emit the signal to the main thread
                            self.key_pressed.emit(key_name)
            except OSError as e:
                # This catches the "Bad file descriptor" error when the device is closed
                if e.errno == 9: # errno 9 is EBADF (Bad file descriptor)
                    logger.debug("Input device closed, exiting read loop.")
                    break
                else:
                    logger.error(f"Error reading from input device: {e}")
                    break # Break on other errors too
            except Exception as e:
                logger.error(f"Unexpected error in input read loop: {e}")
                break # Break on unexpected errors

        logger.debug(f"_read_loop thread {threading.current_thread().ident} finished.")

    def _map_keycode(self, keycode):
        """映射按键码到自定义名称"""
        key_map = {
            ecodes.KEY_UP: self.KEY_UP,
            ecodes.KEY_DOWN: self.KEY_DOWN,
            ecodes.KEY_ENTER: self.KEY_ENTER,
            # Add more mappings as needed
        }
        return key_map.get(keycode)

    def release(self):
        """释放输入设备"""
        logger.info("Releasing input device.")
        self.running = False # Set the flag to stop the loop
        if self.read_thread:
            # Wait for the thread to finish gracefully
            self.read_thread.join(timeout=2.0) # Give it some time to stop
            if self.read_thread.is_alive():
                logger.warning("Input read thread did not stop in time.")
        if self.device:
            try:
                self.device.ungrab()  # Ungrab the device
                self.device.close()   # Close the device
                logger.info("Released input device.")
            except OSError as e:
                # Log warning if closing fails (e.g., already closed)
                logger.warning(f"Error releasing input device: {e}")
            except Exception as e:
                logger.error(f"Unexpected error releasing input device: {e}")
            finally:
                self.device = None # Clear the reference
        else:
            logger.info("Input device was already released or not initialized.")
