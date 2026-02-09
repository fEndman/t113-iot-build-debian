# services/audio_play_service.py
"""音频播放服务 - 独立线程播放音频"""
import threading
import time
import logging
import pyaudio
from queue import Queue
from config import AUDIO_RATE_PLAYBACK, AUDIO_CHANNELS

logger = logging.getLogger(__name__)

class AudioPlayService:
    def __init__(self):
        self.pya = pyaudio.PyAudio() # Create PyAudio instance once
        self.output_stream = None
        self.stream_lock = threading.Lock()
        self.audio_queue = Queue() # Thread-safe queue for audio data
        self.playback_finished_event = threading.Event()
        self.shutdown_event = threading.Event() # Signal to stop the audio thread
        self.audio_thread = None
        # NEW: Flag to track if we are currently playing
        self._is_playing = False
        self._playback_lock = threading.Lock() # Lock to protect _is_playing flag

    def start(self):
        """Start the audio playback thread."""
        if self.audio_thread is None or not self.audio_thread.is_alive():
            self.shutdown_event.clear()
            self.playback_finished_event.clear() # Clear before start
            self.audio_thread = threading.Thread(target=self._playback_worker, daemon=True)
            self.audio_thread.start()
            logger.info("[Audio Play Service] Audio playback thread started.")

    def stop(self):
        """Stop the audio playback thread."""
        logger.info("[Audio Play Service] Stopping audio playback thread...")
        self.shutdown_event.set()
        self.audio_queue.put(None) # Wake up the thread if it's waiting on the queue
        if self.audio_thread and self.audio_thread.is_alive():
            self.audio_thread.join(timeout=5.0) # Wait up to 5 seconds
            if self.audio_thread.is_alive():
                logger.warning("[Audio Play Service] Audio thread did not stop gracefully.")
        logger.info("[Audio Play Service] Audio playback thread stopped.")

    def submit_audio_chunk(self, audio_bytes):
        """Submit an audio chunk for playback."""
        if audio_bytes is not None:
             logger.debug(f"[Audio Play Service] Submitting audio chunk of {len(audio_bytes)} bytes.")
        # NEW: Mark that we are starting a new playback session if the first chunk arrives
        with self._playback_lock:
            if not self._is_playing:
                self._is_playing = True
                # Clear the finished event at the start of a new session
                self.playback_finished_event.clear()
        self.audio_queue.put(audio_bytes)

    def wait_for_playback_to_finish(self, timeout=10):
        """Wait for the current playback session to finish."""
        logger.debug("[Audio Play Service] Waiting for playback to finish...")
        # NEW: Only wait if we think we are playing
        with self._playback_lock:
            if not self._is_playing:
                logger.debug("[Audio Play Service] Not currently playing, returning immediately.")
                return True
        finished = self.playback_finished_event.wait(timeout=timeout)
        if finished:
            logger.debug("[Audio Play Service] Playback finished signal received.")
        else:
            logger.warning("[Audio Play Service] Timeout waiting for playback to finish.")
        return finished

    def _get_output_stream(self):
        """获取或创建输出流"""
        with self.stream_lock:
            if not self.output_stream or self.output_stream.is_stopped():
                if self.output_stream:
                    try:
                        self.output_stream.close()
                    except Exception as e:
                        logger.warning(f"[Audio Play Service] Error closing old output stream: {e}")
                    self.output_stream = None

                try:
                    logger.debug("[Audio Play Service] Opening new audio output stream.")
                    self.output_stream = self.pya.open(
                        format=pyaudio.paInt16, # Assuming 16-bit output
                        channels=AUDIO_CHANNELS,
                        rate=AUDIO_RATE_PLAYBACK,
                        output=True,
                        frames_per_buffer=4800 # 200ms @ 24k
                    )
                    logger.debug(f"[Audio Play Service] Output stream initialized at {AUDIO_RATE_PLAYBACK}Hz.")
                except Exception as e:
                    logger.error(f"[Audio Play Service] Failed to initialize output stream: {e}")
                    return None
            return self.output_stream

    def _playback_worker(self):
        """The main loop of the audio playback thread."""
        logger.debug("[Audio Play Service] Playback worker thread started.")
        while not self.shutdown_event.is_set():
            try:
                # Get audio data from the queue, blocking with timeout to allow shutdown checks
                audio_data = self.audio_queue.get(timeout=0.1)
            except: # queue.Empty
                continue # Check shutdown event again

            if audio_data is None: # End-of-stream marker for the *current* session
                logger.debug("[Audio Play Service] Received end-of-stream marker in queue.")
                # Play remaining buffered data
                with self.stream_lock:
                    if self.output_stream:
                        try:
                            logger.debug("[Audio Play Service] Calling stop_stream() to flush buffer.")
                            # NEW: Sleep for output latency BEFORE calling stop_stream
                            try:
                                latency = self.output_stream.get_output_latency() + 0.2
                                logger.debug(f"[Audio Play Service] Sleeping for output latency: {latency} seconds before stop_stream.")
                                time.sleep(latency)
                            except Exception as e:
                                logger.warning(f"[Audio Play Service] Could not get or sleep for output latency: {e}")
                            # This blocks until the buffer is played
                            self.output_stream.stop_stream()
                            logger.debug("[Audio Play Service] stop_stream() returned, buffer flushed.")
                            # Close the stream
                            self.output_stream.close()
                            logger.debug("[Audio Play Service] Output stream closed after playback.")
                            self.output_stream = None
                        except Exception as e:
                            logger.error(f"[Audio Play Service] Error during stream closure: {e}")
                # Set the finished event
                self.playback_finished_event.set()
                logger.debug("[Audio Play Service] Playback finished event set.")
                # NEW: Mark that we are no longer playing
                with self._playback_lock:
                    self._is_playing = False
                # NEW: DO NOT BREAK LOOP. Continue waiting for new data or shutdown.
                continue
            else:
                # Play the audio data
                stream = self._get_output_stream()
                if stream:
                    try:
                        stream.write(audio_data)
                        logger.debug(f"[Audio Play Service] Wrote {len(audio_data)} bytes to stream.")
                    except Exception as e:
                        logger.error(f"[Audio Play Service] Error writing to stream: {e}")
                else:
                    logger.error("[Audio Play Service] Could not get output stream for playback.")

        # Upon shutdown, ensure stream is closed if it's still open
        with self.stream_lock:
            if self.output_stream:
                try:
                    # NEW: Sleep for latency during shutdown too, before stop_stream
                    try:
                        latency = self.output_stream.get_output_latency() + 0.2
                        logger.debug(f"[Audio Play Service] Shutdown: Sleeping for output latency: {latency} seconds before stop_stream.")
                        time.sleep(latency)
                    except Exception as e:
                        logger.warning(f"[Audio Play Service] Shutdown: Could not get or sleep for output latency: {e}")
                    if not self.output_stream.is_stopped():
                        self.output_stream.stop_stream()
                    self.output_stream.close()
                    logger.debug("[Audio Play Service] Output stream closed on shutdown.")
                except Exception as e:
                    logger.error(f"[Audio Play Service] Error closing stream on shutdown: {e}")
                self.output_stream = None

        # NEW: Do NOT terminate PyAudio here if it's meant to be reused across sessions.
        # The PyAudio instance should ideally be terminated in the destructor or a dedicated cleanup method.
        # However, for a daemon thread, this might be okay if the program exits.
        # If the service lifetime matches the application, consider moving pya termination to StateManager.cleanup.

        logger.debug("[Audio Play Service] Playback worker thread finished.")


    def release_resources(self):
        """Stop the service thread."""
        self.stop()
        # NEW: Terminate PyAudio here, when the whole service is being shut down
        try:
            self.pya.terminate()
            logger.debug("[Audio Play Service] PyAudio terminated in release_resources.")
        except Exception as e:
            logger.error(f"[Audio Play Service] Error terminating PyAudio: {e}")

