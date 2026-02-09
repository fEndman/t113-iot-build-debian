# ui/base.py
"""基础UI组件和样式"""
from PyQt6.QtWidgets import QWidget, QLabel, QProgressBar, QFrame
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont, QPalette, QColor
from config import FONT_FAMILY, FONT_SIZE_SMALL, FONT_SIZE_MEDIUM, FONT_SIZE_LARGE
import logging

logger = logging.getLogger(__name__)

class BasePanel(QWidget):
    """所有面板的基类"""
    def __init__(self):
        super().__init__()
        self.init_style()

    def init_style(self):
        """统一深灰背景风格"""
        palette = QPalette()
        palette.setColor(QPalette.ColorRole.Window, QColor(28, 28, 28))   # #1c1c1c
        palette.setColor(QPalette.ColorRole.WindowText, QColor(240, 240, 240)) # #f0f0f0
        self.setPalette(palette)
        self.setAutoFillBackground(True)

    def create_title(self, text: str, color: str = "#54a0ff") -> QLabel:
        """创建顶部标题"""
        label = QLabel(text)
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        label.setFont(QFont(FONT_FAMILY, FONT_SIZE_LARGE, QFont.Weight.DemiBold))
        label.setStyleSheet(f"color: {color};")
        return label

    def create_separator(self) -> QFrame:
        """创建分隔线"""
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet("color: #444;")
        return line

    def create_row(self, label_text: str, label_width: int = 28,
                   color: str = "#aaa") -> tuple:
        """创建左右布局行 (标签+内容)"""
        label = QLabel(label_text)
        label.setFont(QFont(FONT_FAMILY, FONT_SIZE_MEDIUM, QFont.Weight.Medium))
        label.setStyleSheet(f"color: {color}; min-width: {label_width}px;")
        label.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        value = QLabel("")
        value.setFont(QFont(FONT_FAMILY, FONT_SIZE_SMALL))
        value.setStyleSheet(f"color: {color};")
        value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        return label, value

    def create_progress_row(self, label_text: str, color: str,
                            bar_width: int = 70) -> tuple:
        """创建带进度条的行"""
        label = QLabel(label_text)
        label.setFont(QFont(FONT_FAMILY, FONT_SIZE_MEDIUM, QFont.Weight.Medium))
        label.setStyleSheet(f"color: {color}; min-width: 28px;")
        label.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)

        bar = QProgressBar()
        bar.setRange(0, 100)
        bar.setValue(0)
        bar.setTextVisible(False)
        bar.setFixedHeight(8)
        bar.setStyleSheet(f"""
        QProgressBar {{
            border: 1px solid #555;
            border-radius: 3px;
            background-color: #333;
            max-width: {bar_width}px;
        }}
        QProgressBar::chunk {{
            background-color: {color};
            border-radius: 2px;
        }}
        """)

        percent = QLabel("0%")
        percent.setFont(QFont(FONT_FAMILY, FONT_SIZE_SMALL))
        percent.setStyleSheet(f"color: {color}; min-width: 24px;")
        percent.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        capacity = QLabel("0.0M/0.0M")
        capacity.setFont(QFont(FONT_FAMILY, FONT_SIZE_SMALL))
        capacity.setStyleSheet("color: #aaa; min-width: 48px;")
        capacity.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        return label, bar, percent, capacity
