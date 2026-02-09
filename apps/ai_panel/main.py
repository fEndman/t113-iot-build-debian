#!/usr/bin/env python3
import sys
import os
import signal
import logging
from PyQt6.QtWidgets import QApplication
from logger_config import setup_logger

setup_logger()
logger = logging.getLogger(__name__)

os.environ["QT_QPA_PLATFORM"] = "eglfs"
os.environ["QT_QPA_EGLFS_DISABLE_INPUT"] = "1"

def check_environment():
    from config import check_api_key
    try:
        check_api_key()
    except ValueError as e:
        logger.error(f"Environment check failed: {e}")
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

def signal_handler(signum, frame):
    logger.info(f"Received signal {signum}, shutting down gracefully...")
    sys.exit(0)

def main():
    check_environment()
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    app = QApplication(sys.argv)

    from core import InputHandler, BacklightManager, StateManager
    from ui.main_window import MainWindow

    input_handler = InputHandler()
    backlight = BacklightManager()
    state_manager = StateManager(main_window=None, input_handler=input_handler)
    window = MainWindow(state_manager=state_manager)
    state_manager.window = window
    state_manager.set_backlight_manager(backlight)
    state_manager.window.switch_to_panel(state_manager.target_panel_id)
    input_handler.key_pressed.connect(backlight.reset_idle_timer)

    logger.info("Application started.")
    try:
        exit_code = app.exec()
    except KeyboardInterrupt:
        logger.info("Keyboard interrupt received.")
        exit_code = 0
    finally:
        state_manager.cleanup()
        input_handler.release()
        logger.info("Application exited gracefully.")
    sys.exit(exit_code)

if __name__ == "__main__":
    main()