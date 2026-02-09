# services/audio_record_service.py
"""音频录制服务 - 录音"""
import pyaudio
import threading
import logging
from config import AUDIO_RATE_RECORDING, AUDIO_CHANNELS

logger = logging.getLogger(__name__)
AUDIO_FORMAT_PA = pyaudio.paInt16

class AudioRecordService:
    def __init__(self):
        self.pya = pyaudio.PyAudio()
        self.input_stream = None
        self.recording_lock = threading.Lock()
        self.recording_stop_event = threading.Event() # 用于停止录音的事件

    def start_recording(self):
        """开始录音，阻塞直到用户停止或收到停止信号"""
        with self.recording_lock:
            if self.input_stream:
                logger.warning("Recording already in progress, ignoring start request.")
                return b""

            logger.info("Starting audio recording...")
            # 查找输入设备
            try:
                device_idx = self.pya.get_default_input_device_info()['index']
            except Exception as e:
                logger.error(f"No default input device: {e}")
                # 尝试查找其他输入设备
                device_found = False
                for i in range(self.pya.get_device_count()):
                    try:
                        info = self.pya.get_device_info_by_index(i)
                        if info['maxInputChannels'] > 0:
                            device_idx = i
                            device_found = True
                            break
                    except: continue
                if not device_found:
                    logger.error("No input device found for recording.")
                    return b''

            try:
                self.input_stream = self.pya.open(
                    format=AUDIO_FORMAT_PA,
                    channels=AUDIO_CHANNELS,
                    rate=AUDIO_RATE_RECORDING,
                    input=True,
                    input_device_index=device_idx,
                    frames_per_buffer=3200 # 100ms @ 16k
                )
            except Exception as e:
                logger.error(f"Failed to open input stream: {e}")
                return b''

            frames = []
            self.recording_stop_event.clear() # 确保停止事件未被设置

            # 循环录音
            while not self.recording_stop_event.is_set(): # 检查停止事件
                try:
                    data = self.input_stream.read(3200, exception_on_overflow=False)
                    frames.append(data)
                except Exception as e:
                    logger.error(f"Error reading from input stream: {e}")
                    break

            # 录音结束，清理资源
            recorded_data = b''.join(frames)
            logger.info(f"Recording stopped. Captured {len(recorded_data)} bytes.")
            self._safe_close_input_stream()
            return recorded_data

    def stop_recording(self):
        """外部调用以停止录音"""
        self.recording_stop_event.set()

    def _safe_close_input_stream(self):
        """安全关闭输入流"""
        if self.input_stream:
            try:
                if not self.input_stream.is_stopped():
                    self.input_stream.stop_stream()
            except Exception as e: logger.warning(f"Error stopping input stream: {e}")
            try:
                self.input_stream.close()
            except Exception as e: logger.warning(f"Error closing input stream: {e}")
            self.input_stream = None
        self.recording_stop_event.clear()

    def release_resources(self):
        """释放录音相关的 PyAudio 资源"""
        logger.info("Releasing audio recording resources...")
        self.stop_recording()
        self._safe_close_input_stream()
        logger.info("Audio recording resources released.")
