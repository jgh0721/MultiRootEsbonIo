#pragma once

#include <QColor>
#include <QWidget>

/// Windows DWM 제목 표시줄 테마 적용 유틸리티
namespace DwmTitleBar {

/// Windows 10 17763+/Windows 11: DWMWA_USE_IMMERSIVE_DARK_MODE(20)
/// Windows 11 22000+: DWMWA_CAPTION_COLOR(35)
void applyTheme(QWidget* window, bool darkMode, const QColor& captionColor);

} // namespace DwmTitleBar

