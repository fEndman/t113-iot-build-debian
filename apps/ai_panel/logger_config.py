# logger_config.py
import logging
import sys

def setup_logger():
    """配置全局日志记录器"""
    logger = logging.getLogger() # 获取根记录器
    logger.setLevel(logging.INFO) # 设置最低捕获级别

    handler = logging.StreamHandler(sys.stdout)
    formatter = logging.Formatter(
        fmt='[%(levelname)s] %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    handler.setFormatter(formatter)
    logger.addHandler(handler)

    # 为第三方库设置较低的日志级别，避免过多噪音
    logging.getLogger("urllib3").setLevel(logging.INFO)
    logging.getLogger("requests").setLevel(logging.INFO)
    logging.getLogger("dashscope").setLevel(logging.INFO)
    logging.getLogger("pyaudio").setLevel(logging.INFO)
    logging.getLogger('PIL').setLevel(logging.INFO)