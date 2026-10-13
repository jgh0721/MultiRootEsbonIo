#pragma once

#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <cmath>

namespace mrst {
inline double renderingValue(const QSettings& settings, const char* key,
                             double fallback, double minimum, double maximum)
{
    bool ok = false;
    const double value = settings.value(QLatin1String(key), fallback).toDouble(&ok);
    return ok && std::isfinite(value) && value >= minimum && value <= maximum ? value : fallback;
}

struct TextRenderingSettings
{
    bool editorEnabled = false;
    double editorGamma = 1.2;
    double editorStrength = 1.1;
    bool previewEnabled = false;
    double previewGamma = 2.2;
    double previewContrast = 0.5;
    bool strokeEnabled = false;
    double strokeWidth = 0.15;

    static TextRenderingSettings load(const QSettings& settings)
    {
        TextRenderingSettings result;
        result.editorEnabled = settings.value("textView/mask/enabled", false).toBool();
        result.editorGamma = renderingValue(settings, "textView/mask/gamma", 1.2, 0.5, 3.0);
        result.editorStrength = renderingValue(settings, "textView/mask/strength", 1.1, 0.5, 2.0);
        result.previewEnabled = settings.value("preview/rendering/enabled", false).toBool();
        result.previewGamma = renderingValue(settings, "preview/rendering/gamma", 2.2, 0.5, 3.0);
        result.previewContrast = renderingValue(settings, "preview/rendering/contrast", 0.5, 0.0, 1.0);
        result.strokeEnabled = settings.value("preview/rendering/strokeEnabled", false).toBool();
        result.strokeWidth = renderingValue(settings, "preview/rendering/strokeWidth", 0.15, 0.0, 0.5);
        return result;
    }

    // Preserve unrelated external flags verbatim, including quoted paths. Only
    // replace our switches when the user explicitly enables the override.
    QByteArray chromiumFlags(const QByteArray& inherited) const
    {
        QString flags = QString::fromUtf8(inherited);
        if (previewEnabled) {
            static const QRegularExpression owned(
                R"((?<!\S)--text-(?:gamma|contrast)(?:=(?:"[^"]*"|'[^']*'|\S*)|\s+(?:"[^"]*"|'[^']*'|[^\s-]\S*))?(?=\s|$))");
            flags.remove(owned);
            flags += QStringLiteral(" --text-gamma=%1 --text-contrast=%2")
                .arg(previewGamma, 0, 'f', 2).arg(previewContrast, 0, 'f', 2);
        }
        if (!QProcess::splitCommand(flags).contains(QStringLiteral("--no-sandbox")))
            flags += QStringLiteral(" --no-sandbox");
        return flags.trimmed().toUtf8();
    }
};
} // namespace mrst
