# ui/main_window.py
"""主窗口 - 管理面板布局"""
from PyQt6.QtWidgets import QWidget, QStackedLayout
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QCursor
import logging
from ui.panels.ai_panel import AIChatPanel
from ui.panels.monitor_panel import MonitorPanel
from ui.panels.desktop_stream_panel import DesktopStreamPanel # Import the new panel
logger = logging.getLogger(__name__)

class MainWindow(QWidget):
    PANEL_MONITOR = 0
    PANEL_AI = 1
    PANEL_DESKTOP = 2  # 新增桌面串流面板

    def __init__(self, state_manager=None): # Receive state manager reference
        super().__init__()
        self.state_manager = state_manager
        self.init_ui()

    def init_ui(self):
        layout = QStackedLayout()
        # Pass state manager reference to MonitorPanel - THIS IS THE CRITICAL LINE
        self.monitor_panel = MonitorPanel(state_manager=self.state_manager)
        self.ai_panel = AIChatPanel()
        # Pass state manager reference to DesktopStreamPanel
        self.desktop_panel = DesktopStreamPanel(state_manager=self.state_manager)
        layout.addWidget(self.monitor_panel)
        layout.addWidget(self.ai_panel)
        layout.addWidget(self.desktop_panel) # 添加到布局
        self.setLayout(layout)
        self.showFullScreen()
        self.setCursor(QCursor(Qt.CursorShape.BlankCursor))

    def switch_to_panel(self, panel_id):
        """切换到指定面板"""
        logger.info(f"Switching UI to panel: {panel_id}")
        old_panel_id = self.layout().currentIndex() # Get the currently active panel ID
        self.layout().setCurrentIndex(panel_id)
        # Notify old panel it's being left
        if old_panel_id == self.PANEL_MONITOR:
            self.monitor_panel.on_leave()
        elif old_panel_id == self.PANEL_AI:
            self.ai_panel.on_leave()
        elif old_panel_id == self.PANEL_DESKTOP:
            self.desktop_panel.on_leave()
        # Notify new panel it's being entered
        if panel_id == self.PANEL_MONITOR:
            self.monitor_panel.on_enter()
        elif panel_id == self.PANEL_AI:
            self.ai_panel.on_enter()
        elif panel_id == self.PANEL_DESKTOP:
            self.desktop_panel.on_enter()