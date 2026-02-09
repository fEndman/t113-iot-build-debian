# ui/panels/ai_panel.py
"""AI 聊天界面"""
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QLabel, QFrame
from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QFont, QPalette, QColor, QPixmap, QPainter
from ui.base import BasePanel
import logging
import os

logger = logging.getLogger(__name__)

class AIChatPanel(BasePanel):
    trigger_animation_start = pyqtSignal()
    trigger_animation_stop = pyqtSignal()

    STATUS_IDLE = "AI Ready"
    STATUS_LISTENING = "Listening..."
    STATUS_PROCESSING = "Processing..."
    STATUS_PLAYING = "Playing..."

    EMO_LISTENING = "O v O"
    EMO_THINKING = "? _ ?"
    EMO_SPEAKING_STATIC = "> _ <"
    EMO_IDLE = "> w <"
    EMO_ERROR = "X o X"
    EMO_SWITCHING = "> w <"

    MOUTH_SHAPES = ['o', 'O', 'o', '0', 'o', 'O', 'o', '0']

    def __init__(self):
        super().__init__()
        
        self.emoticon_text = self.EMO_IDLE
        self.animation_timer = None
        self.current_mouth_index = 0
        self.base_speaking_emoticon = self.EMO_SPEAKING_STATIC
        self.speaking_emoticon_with_mouth = self.base_speaking_emoticon
        
        self.init_ui()
        self.set_emoticon(self.EMO_IDLE)
        self.trigger_animation_start.connect(self.start_speaking_animation)
        self.trigger_animation_stop.connect(self.stop_speaking_animation)

    def init_ui(self):
        layout = QVBoxLayout()
        layout.setContentsMargins(3, 2, 3, 2)
        layout.setSpacing(1)

        # Title label
        title = QLabel("TA-TA")
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        title.setFont(QFont("DejaVu Sans Mono", 11, QFont.Weight.DemiBold))
        title.setStyleSheet("color: #54a0ff;")
        layout.addWidget(title)

        # Separator line below title using QFrame
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet("color: #444;")
        layout.addWidget(line)

        # Create a container that will hold both background and emoticon
        self.container_widget = QWidget()
        self.container_widget.setFixedSize(160, 100)
        
        # Set up the container's custom paint event to draw both elements
        self.container_widget.paintEvent = self._container_paint_event
        self.container_widget.update()
        
        layout.addWidget(self.container_widget, alignment=Qt.AlignmentFlag.AlignCenter)
        
        # Add stretch to fill remaining space
        layout.addStretch(1)

        self.setLayout(layout)
        self.setFixedSize(160, 128)
        
        # Load background image after UI is initialized
        self.load_background_image()

    def load_background_image(self):
        """Load and scale the background image."""
        image_path = os.path.join('ui', 'assets', 'TA-TA.png')
        if os.path.exists(image_path):
            pixmap = QPixmap(image_path)
            if not pixmap.isNull():
                # Scale pixmap to fit the container while keeping aspect ratio
                scaled_pixmap = pixmap.scaled(
                    self.container_widget.width(),
                    self.container_widget.height(),
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation
                )
                self.background_pixmap = scaled_pixmap
                logger.info(f"Loaded and scaled background image: {image_path}")
            else:
                logger.error(f"Failed to load image as pixmap: {image_path}")
                self.background_pixmap = None
        else:
            logger.error(f"Background image file not found: {image_path}")
            self.background_pixmap = None

    def _container_paint_event(self, event):
        """Paint event for the container widget."""
        painter = QPainter(self.container_widget)
        
        # Draw background pixmap if available
        if hasattr(self, 'background_pixmap') and self.background_pixmap:
            # Center the pixmap in the container
            x_offset = (self.container_widget.width() - self.background_pixmap.width()) // 2
            y_offset = (self.container_widget.height() - self.background_pixmap.height()) // 2
            y_offset = max(y_offset, 0)
            painter.drawPixmap(x_offset, y_offset, self.background_pixmap)

        # Draw centered emoticon on top
        if hasattr(self, 'emoticon_text'):
            painter.setPen(QColor("#00bcd4"))
            font = QFont("DejaVu Sans Mono", 12, QFont.Weight.Bold)
            painter.setFont(font)
            # Calculate bounding rectangle for the text
            fm = painter.fontMetrics()
            text_width = fm.horizontalAdvance(self.emoticon_text)
            text_height = fm.height()
            # Center the text
            text_x = (self.container_widget.width() - text_width) // 2
            # Use ascent to calculate baseline correctly for vertical centering
            text_baseline = (self.container_widget.height() + text_height) // 2 - (text_height - fm.ascent()) // 2 - 2
            painter.drawText(text_x, text_baseline, self.emoticon_text)

    def resizeEvent(self, event):
        """Handle widget resize to adjust background image."""
        super().resizeEvent(event)
        # Reload and rescale image when size changes
        self.load_background_image()
        if hasattr(self, 'container_widget'):
            self.container_widget.update()  # Trigger repaint

    def set_status(self, status_text):
        logger.debug(f"AI Panel status update request ignored (label removed): {status_text}")

    def set_emoticon(self, emoticon_text):
        """Update the stored emoticon text and trigger a repaint."""
        self.emoticon_text = emoticon_text
        logger.debug(f"AI Panel emoticon updated to: {emoticon_text}")
        if hasattr(self, 'container_widget'):
            self.container_widget.update()  # Trigger repaint

    def start_speaking_animation(self):
        logger.debug("Starting speaking animation.")
        if self.animation_timer and self.animation_timer.isActive():
            logger.debug("Animation already running, skipping start.")
            return

        self.current_mouth_index = 0
        mouth_char = self.MOUTH_SHAPES[self.current_mouth_index]
        # Build emoticon without parentheses
        self.speaking_emoticon_with_mouth = self.base_speaking_emoticon.replace('_', mouth_char, 1)
        self.set_emoticon(self.speaking_emoticon_with_mouth)

        # Create new timer if needed
        if not self.animation_timer:
            self.animation_timer = QTimer(self)
            self.animation_timer.timeout.connect(self._animate_mouth)
        
        self.animation_timer.start(200)

    def _animate_mouth(self):
        if not self.animation_timer or not self.animation_timer.isActive():
            return

        self.current_mouth_index = (self.current_mouth_index + 1) % len(self.MOUTH_SHAPES)
        mouth_char = self.MOUTH_SHAPES[self.current_mouth_index]
        # Build emoticon without parentheses
        self.speaking_emoticon_with_mouth = self.base_speaking_emoticon.replace('_', mouth_char, 1)
        self.set_emoticon(self.speaking_emoticon_with_mouth)

    def stop_speaking_animation(self):
        logger.debug("Stopping speaking animation.")
        if hasattr(self, 'animation_timer') and self.animation_timer:
            self.animation_timer.stop()
            # Don't delete the timer here since we reuse it
            # self.animation_timer.deleteLater()
            # self.animation_timer = None

    def on_enter(self):
        logger.info("AI panel activated.")
        self.set_emoticon(self.EMO_IDLE)

    def on_leave(self):
        logger.info("AI panel deactivated.")
        self.set_emoticon("zzz") # Changed to match new style
        self.trigger_animation_stop.emit()