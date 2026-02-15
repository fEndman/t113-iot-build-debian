# services/ai_service.py
"""AI 服务 - 管理 Qwen 连接"""
import time
import threading
import base64
import logging
import os
from dashscope.audio.qwen_omni import OmniRealtimeConversation, OmniRealtimeCallback, MultiModality, AudioFormat
import dashscope

logger = logging.getLogger(__name__)
from config import QWEN_MODEL, QWEN_VOICE, QWEN_URL, QWEN_API_KEY

class AICallback(OmniRealtimeCallback):
    def __init__(self, ai_service_instance):
        super().__init__()
        self.ai_service = ai_service_instance
    def on_open(self):
        logger.info("[AI Service] Connection opened.")
        self.ai_service._on_connection_opened()
    def on_close(self, code, msg):
        logger.info(f"[AI Service] Connection closed. Code: {code}, Msg: {msg}")
        self.ai_service._on_connection_closed(code, msg)
    def on_error(self, error):
        logger.error(f"[AI Service] Connection error: {error}")
        self.ai_service._on_connection_error(error)
    def on_event(self, response):
        self.ai_service._handle_response_event(response)

class AIService:
    CONNECTION_TIMEOUT_SECONDS = 55
    def __init__(self, audio_play_service_instance):
        self.conversation = None
        self.callback_instance = AICallback(self)
        self.connected = False
        self.connection_lock = threading.Lock()
        self.response_done_event = threading.Event()
        self.session_id = None
        self.last_activity_timestamp = 0
        self.context_messages = []
        self.audio_play_service = audio_play_service_instance
        self.on_user_transcript_callback = None
        self.on_ai_text_callback = None
        self.on_audio_play_started_callback = None
        self.on_response_done_callback = None

    def ensure_connection(self):
        with self.connection_lock:
            now = time.time()
            if self.connected and (now - self.last_activity_timestamp) < self.CONNECTION_TIMEOUT_SECONDS:
                logger.debug("[AI Service] Connection is still valid.")
                return True
            logger.info("[AI Service] Connection needs refresh/reconnect.")
            self._disconnect_internal()
            success = self._connect_internal()
            if success:
                self.last_activity_timestamp = now
            return success

    def _connect_internal(self):
        try:
            dashscope.api_key = QWEN_API_KEY
            self.conversation = OmniRealtimeConversation(model=QWEN_MODEL, callback=self.callback_instance, url=QWEN_URL)
            self.conversation.connect()
            start_time = time.time()
            timeout = 10
            while not self.connected and (time.time() - start_time) < timeout: time.sleep(0.1)
            if not self.connected:
                logger.error("[AI Service] Connection timed out or failed to establish after SDK call.")
                return False
            self._configure_session()
            logger.info("[AI Service] Connected successfully.")
            return True
        except Exception as e:
            logger.error(f"[AI Service] Failed to connect: {e}")
            self.connected = False
            return False

    def _disconnect_internal(self):
        if self.conversation:
            try:
                self.conversation.close()
                logger.info("[AI Service] Disconnected via SDK.")
            except Exception as e:
                logger.error(f"[AI Service] Error disconnecting via SDK: {e}")
                try:
                    if hasattr(self.conversation, 'ws_conn') and self.conversation.ws_conn:
                        self.conversation.ws_conn.close()
                        logger.info("[AI Service] Disconnected via ws_conn fallback.")
                except Exception as e2: logger.error(f"[AI Service] Fallback disconnection also failed: {e2}")
        else:
            logger.debug("[AI Service] Attempted to disconnect, but no conversation object exists.")
        self.connected = False
        self.session_id = None
        self.last_activity_timestamp = 0
        self.response_done_event.clear()

    def _configure_session(self):
        if self.conversation:
            self.conversation.update_session(
                output_modalities=[MultiModality.AUDIO, MultiModality.TEXT],
                voice=QWEN_VOICE,
                input_audio_format=AudioFormat.PCM_16000HZ_MONO_16BIT,
                output_audio_format=AudioFormat.PCM_24000HZ_MONO_16BIT,
                enable_input_audio_transcription=True,
                input_audio_transcription_model='gummy-realtime-v1',
                enable_turn_detection=False,
                instructions="你是机器人塔塔，请简短友好地解答用户问题。"
            )

    def send_audio(self, audio_bytes):
        if not self.ensure_connection():
            logger.error("[AI Service] Cannot send audio, failed to ensure connection.")
            self.connected = False
            return False
        with self.connection_lock:
            if not self.connected or not self.conversation:
                logger.error("[AI Service] Cannot send audio, connection lost during ensure step.")
                self.connected = False
                return False
            self.response_done_event.clear()
            self.audio_play_service.playback_finished_event.clear()
            chunk_size = 3200
            try:
                for i in range(0, len(audio_bytes), chunk_size):
                    chunk = audio_bytes[i:i+chunk_size]
                    encoded_chunk = base64.b64encode(chunk).decode('ascii')
                    self.conversation.append_audio(encoded_chunk)
                self.conversation.commit()
                self.conversation.create_response()
                self.last_activity_timestamp = time.time()
                logger.info("[AI Service] Audio sent.")
                return True
            except Exception as e:
                logger.error(f"[AI Service] Error sending audio: {e}")
                self.connected = False
                return False

    def wait_for_response(self, timeout=30):
        api_done = self.response_done_event.wait(timeout=timeout)
        if not api_done:
            logger.warning("Timeout waiting for API response.")
            return False
        logger.debug("[AI Service] API response done, waiting for audio playback to finish...")
        audio_done = self.audio_play_service.wait_for_playback_to_finish(timeout=timeout)
        if not audio_done:
            logger.warning("[AI Service] Timeout waiting for audio playback to finish after API response.")
            return True
        logger.debug("[AI Service] Audio playback finished.")
        return True

    def _on_connection_opened(self):
        self.connected = True
        self.last_activity_timestamp = time.time()

    def _on_connection_closed(self, code, msg):
        logger.info(f"[AI Service] OnClose called, Code: {code}, Msg: {msg}")
        self.connected = False
        self.session_id = None
        self.last_activity_timestamp = 0
        self.response_done_event.clear()

    def _on_connection_error(self, error):
        logger.error(f"[AI Service] OnError called: {error}")
        self.connected = False
        self.session_id = None
        self.last_activity_timestamp = 0
        self.response_done_event.clear()

    def _handle_response_event(self, response):
        self.last_activity_timestamp = time.time()
        event_type = response.get('type', '')
        logger.debug(f"[AI Service] Received event: {event_type}")
        if event_type == 'session.created':
            self.session_id = response.get('session', {}).get('id')
            logger.info(f"[AI Service] Session created with ID: {self.session_id}")
        elif event_type == 'conversation.item.input_audio_transcription.completed':
            transcript = response.get('transcript', '')
            logger.debug(f"[AI Service] Transcribed user input: {transcript}")
            if self.on_user_transcript_callback: self.on_user_transcript_callback(transcript)
        elif event_type == 'response.audio_transcript.delta':
            delta_text = response.get('delta', '')
            logger.debug(f"[AI Service] AI text received: {delta_text}")
            if self.on_ai_text_callback: self.on_ai_text_callback(delta_text)
        elif event_type == 'response.audio.delta':
            audio_delta = response.get('delta', '')
            logger.debug(f"[AI Service] AI audio chunk received, length: {len(audio_delta)} chars.")
            try:
                if self.on_audio_play_started_callback and not self.audio_play_service._is_playing:
                    self.on_audio_play_started_callback()
                audio_bytes = base64.b64decode(audio_delta)
                self.audio_play_service.submit_audio_chunk(audio_bytes)
            except Exception as e:
                logger.error(f"[AI Service] Error decoding AI audio: {e}")
        elif event_type == 'response.done':
            logger.info("[AI Service] AI response completed (API side).")
            self.audio_play_service.submit_audio_chunk(None)
            self.response_done_event.set()
            if self.on_response_done_callback: self.on_response_done_callback()

    def set_callbacks(self, user_transcript_cb=None, ai_text_cb=None, audio_play_started_cb=None, response_done_cb=None):
        self.on_user_transcript_callback = user_transcript_cb
        self.on_ai_text_callback = ai_text_cb
        self.on_audio_play_started_callback = audio_play_started_cb
        self.on_response_done_callback = response_done_cb

    def release_resources(self):
        logger.info("[AI Service] Releasing AI connection resources...")
        if self.conversation:
            try: self.conversation.close()
            except Exception as e: logger.error(f"[AI Service] Error closing conversation: {e}")
        self.connected = False
        logger.info("[AI Service] AI connection resources released.")
