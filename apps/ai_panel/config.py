# config.py
"""全局配置常量"""
import os

# === 屏幕尺寸 ===
SCREEN_WIDTH = 160
SCREEN_HEIGHT = 128

# === 背光配置 ===
BACKLIGHT_PATH = "/sys/class/backlight/backlight/brightness"
BRIGHTNESS_START = 9   # 启动舒适亮度
BRIGHTNESS_MIN = 1     # 最低可见亮度
BRIGHTNESS_EXIT = 9   # 退出亮度
BRIGHTNESS_DIM_TIME = 60  # 亮屏时间
BRIGHTNESS_DIM_STEP = 0.1  # 每步间隔

# === 字体配置（小屏优化）===
FONT_FAMILY = "DejaVu Sans Mono"  # 全等宽字体确保对齐
FONT_SIZE_SMALL = 8   # 数值/状态
FONT_SIZE_MEDIUM = 9  # 标签/对话
FONT_SIZE_LARGE = 11  # 时间

# === 音频配置 ===
AUDIO_RATE_PLAYBACK = 24000 # AI 播放采样率
AUDIO_RATE_RECORDING = 16000 # AI 录音采样率
AUDIO_CHANNELS = 1
AUDIO_FORMAT = 8  # paInt16 (PyAudio常量)

# === Qwen API ===
QWEN_MODEL = "qwen3-omni-flash-realtime"
QWEN_VOICE = "Momo"
QWEN_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
# 不再在这里检查 API KEY
QWEN_API_KEY = os.getenv("QWEN_API_KEY", "none")

# 可以定义一个函数用于检查
def check_api_key():
    if not QWEN_API_KEY or QWEN_API_KEY == "none":
        raise ValueError("Environment variable QWEN_API_KEY is not set correctly.")

# === 桌面串流配置 ===
DESKTOP_STREAM_IP = os.getenv("DESKTOP_STREAM_IP", "192.168.113.2")
DESKTOP_STREAM_TCP_PORT = 5655
DESKTOP_STREAM_ZMQ_PORT = 5555
DESKTOP_STREAM_PUB_KEY_PATH = "server_public.key"
DESKTOP_STREAM_TARGET_FPS = 60
DESKTOP_STREAM_BUFFER_SIZE = 2
