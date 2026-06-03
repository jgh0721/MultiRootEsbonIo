#include "stdafx.h"
#include "DwmTitleBar.hpp"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dwmapi.h>
#endif

namespace DwmTitleBar {

namespace {

#ifdef Q_OS_WIN

constexpr DWORD kWindows10Build1809 = 17763;
constexpr DWORD kWindows11Build22000 = 22000;
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaCaptionColor = 35;

DWORD windowsBuildNumber()
{
    using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return 0;

    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        ::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion)
        return 0;

    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
        return 0;

    return info.dwBuildNumber;
}

COLORREF toColorRef(const QColor& color)
{
    const QColor valid = color.isValid() ? color : QColor(Qt::black);
    return RGB(valid.red(), valid.green(), valid.blue());
}

#endif

} // namespace

void applyTheme(QWidget* window, bool darkMode, const QColor& captionColor)
{
#ifdef Q_OS_WIN
    if (!window)
        return;

    HWND hwnd = reinterpret_cast<HWND>(window->window()->winId());
    if (!hwnd)
        return;

    const DWORD build = windowsBuildNumber();
    if (build >= kWindows10Build1809) {
        const BOOL useDarkMode = darkMode ? TRUE : FALSE;
        ::DwmSetWindowAttribute(hwnd,
                                kDwmwaUseImmersiveDarkMode,
                                &useDarkMode,
                                sizeof(useDarkMode));
    }

    if (build >= kWindows11Build22000) {
        const COLORREF caption = toColorRef(captionColor);
        ::DwmSetWindowAttribute(hwnd,
                                kDwmwaCaptionColor,
                                &caption,
                                sizeof(caption));
    }
#else
    Q_UNUSED(window)
    Q_UNUSED(darkMode)
    Q_UNUSED(captionColor)
#endif
}

} // namespace DwmTitleBar

