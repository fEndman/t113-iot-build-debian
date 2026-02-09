# core/state_manager.py
import threading
import logging
import time
import subprocess
from PyQt6.QtWidgets import QApplication
from core.input_handler import InputHandler
from ui.main_window import MainWindow
from services.ai_service import AIService
from services.audio_record_service import AudioRecordService
from services.audio_play_service import AudioPlayService

logger = logging.getLogger(__name__)

class StateManager:
    STATE_MONITOR = "monitor"
    STATE_AI_IDLE = "ai_idle"
    STATE_AI_LISTENING = "ai_listening"
    STATE_AI_PROCESSING = "ai_processing"
    STATE_AI_PLAYING = "ai_playing"
    STATE_DESKTOP = "desktop"

    def __init__(self, main_window: MainWindow, input_handler: InputHandler):
        self.window = main_window
        self.input_handler = input_handler
        self.audio_play_service = AudioPlayService()
        self.audio_play_service.start()
        self.ai_service = AIService(self.audio_play_service)
        self.audio_record_service = AudioRecordService()
        self._current_ai_response_text = ""
        self._last_logged_user_transcript = ""
        self.input_handler.key_pressed.connect(self._on_key_pressed)
        self.backlight_manager = None
        self.current_state = self.STATE_MONITOR
        self.target_panel_id = MainWindow.PANEL_MONITOR
        self.shutdown_event = threading.Event()
        self.recording_thread = None
        self.ai_connect_thread = None
        if self.window:
            self.window.switch_to_panel(self.target_panel_id)

    def set_backlight_manager(self, backlight_manager):
        self.backlight_manager = backlight_manager

    def _on_key_pressed(self, key_name):
        logger.debug(f"State {self.current_state}, received key: {key_name}")
        # Check if Monitor Panel has focus and menu is open
        if (self.current_state == self.STATE_MONITOR and
            self.window and
            self.window.monitor_panel.menu_state == self.window.monitor_panel.MENU_OPEN):
            # If monitor menu is open, let it handle the key event.
            # If it returns True, the event was consumed, so we don't process it further.
            if self.window.monitor_panel.handle_key_event(key_name):
                return # Event was handled by the menu, do not process further

        # Process keys only if menu is not open or if the current state is not monitor
        if key_name == InputHandler.KEY_DOWN:
            self._switch_panel(direction=1)
        elif key_name == InputHandler.KEY_UP:
            self._switch_panel(direction=-1)
        elif self.current_state == self.STATE_MONITOR and key_name == InputHandler.KEY_ENTER:
            self.window.monitor_panel.show_menu()
        elif self.current_state == self.STATE_AI_IDLE and key_name == InputHandler.KEY_ENTER:
            self._start_recording()
        elif self.current_state == self.STATE_AI_LISTENING and key_name == InputHandler.KEY_ENTER:
            self._stop_recording()

    def request_app_exit(self):
        """Request application exit for systemd restart."""
        logger.info("State Manager received request to exit application.")
        self.cleanup()
        if self.window:
            self.window.close()
        if self.input_handler:
            self.input_handler.release()
        if self.backlight_manager:
            from config import BRIGHTNESS_EXIT
            if hasattr(self.backlight_manager, 'set_brightness'):
                 self.backlight_manager.set_brightness(BRIGHTNESS_EXIT)
        app = QApplication.instance()
        if app:
            app.exit(0) # Exit with code 0, systemd will restart based on Restart=on-failure

    def _switch_panel(self, direction=1):
        new_panel_id = (self.target_panel_id + direction) % 3

        if self.current_state == self.STATE_AI_LISTENING:
            logger.info("Stopping recording due to panel switch from STATE_AI_LISTENING.")
            self._stop_recording_immediate()
        elif self.current_state == self.STATE_AI_PROCESSING:
            logger.info("Cancelling AI processing due to panel switch from STATE_AI_PROCESSING.")
        elif self.current_state == self.STATE_AI_PLAYING:
            logger.info("Interrupting AI playback due to panel switch from STATE_AI_PLAYING.")
            self.window.ai_panel.stop_speaking_animation()

        if new_panel_id == MainWindow.PANEL_AI:
            new_state = self.STATE_AI_IDLE
        elif new_panel_id == MainWindow.PANEL_DESKTOP:
            new_state = self.STATE_DESKTOP
        else:
            new_state = self.STATE_MONITOR

        old_state = self.current_state
        self.current_state = new_state
        self.target_panel_id = new_panel_id
        logger.info(f"Panel switch executed. Old State: {old_state}, New State: {new_state}, Target Panel ID: {new_panel_id}")
        self.window.switch_to_panel(new_panel_id)

        if new_panel_id == MainWindow.PANEL_AI:
            logger.info("Switched to AI panel, initiating AI service connection...")
            self.ai_connect_thread = threading.Thread(target=self._connect_ai_service, daemon=True)
            self.ai_connect_thread.start()
        else:
            logger.info(f"Switched to panel {new_panel_id}, resetting AI panel status...")
            self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_SWITCHING)
            self.window.ai_panel.stop_speaking_animation()

    def _connect_ai_service(self):
        logger.info(f"_connect_ai_service thread {threading.current_thread().ident} started.")
        logger.info("AI service ready for on-demand connection. Setting callbacks.")
        self.ai_service.set_callbacks(
            user_transcript_cb=self._on_user_transcript,
            ai_text_cb=self._on_ai_text,
            audio_play_started_cb=self._on_audio_play_started,
            response_done_cb=self._on_response_done_api_side
        )
        logger.info(f"_connect_ai_service thread {threading.current_thread().ident} finished setting callbacks.")

    def _start_recording(self):
        if self.current_state != self.STATE_AI_IDLE:
            logger.warning(f"Cannot start recording in state: {self.current_state}")
            return
        logger.info("Starting recording sequence.")
        self.current_state = self.STATE_AI_LISTENING
        self.window.ai_panel.set_status(self.window.ai_panel.STATUS_LISTENING)
        self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_LISTENING)
        self.recording_thread = threading.Thread(target=self._recording_worker, daemon=True)
        self.recording_thread.start()

    def _stop_recording(self):
        if self.current_state != self.STATE_AI_LISTENING:
            logger.warning(f"Cannot stop recording in state: {self.current_state}")
            return
        logger.info("Stopping recording and preparing to send.")
        self.audio_record_service.stop_recording()

    def _stop_recording_immediate(self):
        logger.info("Immediately stopping recording.")
        self.audio_record_service.stop_recording()
        if self.current_state == self.STATE_AI_LISTENING:
            self.current_state = self.STATE_AI_IDLE
            self.window.ai_panel.set_status(self.window.ai_panel.STATUS_IDLE)
            self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_IDLE)

    def _recording_worker(self):
        logger.info(f"_recording_worker thread {threading.current_thread().ident} started.")
        if self.shutdown_event.is_set():
            logger.info("Shutdown event set before recording started, exiting worker.")
            return

        try:
            audio_data = self.audio_record_service.start_recording()
            logger.info(f"_recording_worker thread {threading.current_thread().ident} finished recording, got {len(audio_data)} bytes.")

            if audio_data:
                logger.info(f"_recording_worker thread {threading.current_thread().ident} sending audio to AI...")
                if self.current_state == self.STATE_AI_LISTENING:
                    self.current_state = self.STATE_AI_PROCESSING
                    self.window.ai_panel.set_status(self.window.ai_panel.STATUS_PROCESSING)
                    self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_THINKING)
                    logger.info(f"_recording_worker thread {threading.current_thread().ident} calling ai_service.send_audio.")
                    if self.ai_service.send_audio(audio_data):
                        logger.info(f"_recording_worker thread {threading.current_thread().ident} ai_service.send_audio returned True.")
                        success = self.ai_service.wait_for_response(timeout=30)
                        if not success:
                            logger.warning("Timeout waiting for AI response or audio playback to finish.")
                        if self.current_state in [self.STATE_AI_PROCESSING, self.STATE_AI_PLAYING]:
                            self.window.ai_panel.set_status("Response/Playback Timeout")
                        if self.current_state == self.STATE_AI_PLAYING:
                            self.window.ai_panel.stop_speaking_animation()
                        self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_IDLE)
                        self.current_state = self.STATE_AI_IDLE
                    else:
                        logger.error("Failed to send audio to AI service (likely connection issue).")
                        if self.current_state in [self.STATE_AI_PROCESSING, self.STATE_AI_PLAYING]:
                            self.window.ai_panel.stop_speaking_animation()
                            self.window.ai_panel.set_status("Send Error - Ready")
                        self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_IDLE)
                        self.current_state = self.STATE_AI_IDLE
            else:
                logger.info("Recording finished but no data captured.")

            if self.current_state == self.STATE_AI_LISTENING:
                self.window.ai_panel.set_status(self.window.ai_panel.STATUS_IDLE)
                self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_IDLE)

        except Exception as e:
            logger.error(f"Error in recording worker: {e}")
            if self.current_state in [self.STATE_AI_LISTENING, self.STATE_AI_PROCESSING, self.STATE_AI_PLAYING]:
                self.window.ai_panel.stop_speaking_animation()
                self.window.ai_panel.set_status("Error")
                self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_ERROR)
            self.current_state = self.STATE_AI_IDLE

        finally:
            if self.current_state in [self.STATE_AI_LISTENING, self.STATE_AI_PROCESSING, self.STATE_AI_PLAYING]:
                self.current_state = self.STATE_AI_IDLE
                self.window.ai_panel.set_status(self.window.ai_panel.STATUS_IDLE)
                self.window.ai_panel.set_emoticon(self.window.ai_panel.EMO_IDLE)
                self.window.ai_panel.stop_speaking_animation()

    def _on_user_transcript(self, text):
        if text != self._last_logged_user_transcript:
            logger.info(f"User said: {text}")
            self._last_logged_user_transcript = text

    def _on_ai_text(self, delta_text):
        self._current_ai_response_text += delta_text

    def _on_audio_play_started(self):
        logger.info("Audio playback started signal received from AI Service.")
        if self.current_state in [self.STATE_AI_PROCESSING]:
            logger.info("Transitioning to playing state and starting animation.")
            self.current_state = self.STATE_AI_PLAYING
            self.window.ai_panel.set_status(self.window.ai_panel.STATUS_PLAYING)
            self.window.ai_panel.trigger_animation_start.emit()

    def _on_response_done_api_side(self):
        logger.info("API response done signal received.")

    def cleanup(self):
        logger.info("Cleaning up State Manager...")
        self.shutdown_event.set()
        if self.current_state == self.STATE_AI_LISTENING:
            logger.info("Cleaning up: Stopping recording if active.")
            self._stop_recording_immediate()
        if self.recording_thread and self.recording_thread.is_alive():
            logger.info("Waiting for recording thread to finish...")
            self.recording_thread.join(timeout=2.0)
        if self.recording_thread and self.recording_thread.is_alive():
            logger.warning("Recording thread did not finish in time.")
        logger.info("Cleaning up: Releasing audio recording resources.")
        self.audio_record_service.release_resources()
        logger.info("Cleaning up: Disconnecting AI service.")
        self.ai_service.release_resources()
        self.window.ai_panel.trigger_animation_stop.emit()
        logger.info("Cleaning up: Stopping audio playback service.")
        self.audio_play_service.release_resources()
        logger.info("State Manager cleanup complete.")