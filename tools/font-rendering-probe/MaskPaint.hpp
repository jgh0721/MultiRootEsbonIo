#pragma once

#include <QCache>
#include <QDataStream>
#include <QFontMetricsF>
#include <QImage>
#include <QIODevice>
#include <QPainter>
#include <QtMath>
#include <array>

namespace mrst::probe {
inline quint64 maskCalls = 0;
inline quint64 cacheHits = 0;
inline quint64 pixelsProcessed = 0;
inline QCache<QByteArray, QImage>& maskCache()
{
    static QCache<QByteArray, QImage> cache(4096);
    return cache;
}

inline void drawText(QPainter& painter, const QPointF& baseline, const QString& text)
{
    static const bool enabled = qEnvironmentVariableIntValue("MRST_PROBE_MASK") == 1;
    if (!enabled || text.isEmpty()) {
        painter.drawText(baseline, text);
        return;
    }
    ++maskCalls;
    static const double gamma = qMax(0.1, qEnvironmentVariable("MRST_PROBE_GAMMA", "1.0").toDouble());
    static const double strength = qMax(0.1, qEnvironmentVariable("MRST_PROBE_STRENGTH", "1.0").toDouble());
    static const bool useCache = qEnvironmentVariableIntValue("MRST_PROBE_CACHE") == 1;
    static const std::array<int, 256> coverage = [] {
        std::array<int, 256> lut{};
        for (int i = 0; i < 256; ++i)
            lut[i] = qBound(0, qRound(qPow(i / 255.0, 1.0 / gamma) * strength * 255.0), 255);
        return lut;
    }();
    auto& cache = maskCache(); // 4 MiB per process
    const qreal dpr = painter.device()->devicePixelRatioF();
    const QFontMetricsF metrics(painter.font(), painter.device());
    const qreal left = qFloor((baseline.x() - 8) * dpr) / dpr;
    const qreal top = qFloor((baseline.y() - metrics.ascent() - 8) * dpr) / dpr;
    const QPointF local = baseline - QPointF(left, top);
    const QSize pixels(qCeil((metrics.horizontalAdvance(text) + 16) * dpr),
                       qCeil((metrics.height() + 16) * dpr));
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << painter.font().key() << text << painter.pen().color().rgba()
           << dpr << local << pixels;
    const QImage* mask = useCache ? cache.object(key) : nullptr;
    QImage generated;
    if (mask) {
        ++cacheHits;
    } else {
        generated = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        generated.setDevicePixelRatio(dpr);
        generated.fill(Qt::transparent);
        QPainter glyphs(&generated);
        glyphs.setFont(painter.font());
        glyphs.setPen(painter.pen());
        glyphs.setRenderHints(painter.renderHints());
        glyphs.drawText(local, text);
        glyphs.end();
        pixelsProcessed += quint64(pixels.width()) * pixels.height();
        for (int y = 0; y < generated.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(generated.scanLine(y));
            for (int x = 0; x < generated.width(); ++x) {
                const QRgb rgba = qUnpremultiply(row[x]);
                row[x] = qPremultiply(qRgba(qRed(rgba), qGreen(rgba), qBlue(rgba), coverage[qAlpha(rgba)]));
            }
        }
        if (useCache) {
            const int cost = int((generated.sizeInBytes() + 1023) / 1024);
            auto* owned = new QImage(generated);
            if (cache.insert(key, owned, cost)) mask = owned;
        }
        if (!mask) mask = &generated;
    }
    if (painter.backgroundMode() == Qt::OpaqueMode)
        painter.fillRect(metrics.boundingRect(text).translated(baseline), painter.background());
    painter.drawImage(QPointF(left, top), *mask);
}
} // namespace mrst::probe
