#include "stdafx.h"
#include "solEncodingDetector.hpp"

#include <QStringConverter>
#include <QTextCodec>
#include <limits>
#include <algorithm>

// ── BOM 감지 ──
QString EncodingDetector::detectBOM(const QByteArray& data)
{
    if (data.size() >= 3 &&
        static_cast<uint8_t>(data[0]) == 0xEF &&
        static_cast<uint8_t>(data[1]) == 0xBB &&
        static_cast<uint8_t>(data[2]) == 0xBF)
        return QStringLiteral("UTF-8");

    if (data.size() >= 4) {
        uint8_t b0 = data[0], b1 = data[1], b2 = data[2], b3 = data[3];
        if (b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00)
            return QStringLiteral("UTF-32LE");
        if (b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF)
            return QStringLiteral("UTF-32BE");
    }

    if (data.size() >= 2) {
        uint8_t b0 = data[0], b1 = data[1];
        if (b0 == 0xFF && b1 == 0xFE) return QStringLiteral("UTF-16LE");
        if (b0 == 0xFE && b1 == 0xFF) return QStringLiteral("UTF-16BE");
    }


    return {};
}

// ── UTF-8 유효성 검사 ──
bool EncodingDetector::isValidUtf8(const QByteArray& data, bool mayBeIncomplete)
{
    for (qsizetype i = 0; i < data.size(); ) {
        const auto first = static_cast<uint8_t>(data[i]);
        if (first < 0x80) {
            ++i;
            continue;
        }
        const int width = first >= 0xC2 && first <= 0xDF ? 2
            : first >= 0xE0 && first <= 0xEF ? 3
            : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
        if (width == 0)
            return false;
        for (int j = 1; j < width; ++j) {
            if (i + j >= data.size())
                return mayBeIncomplete;
            const auto next = static_cast<uint8_t>(data[i + j]);
            if ((next & 0xC0) != 0x80)
                return false;
            if (j == 1 && ((first == 0xE0 && next < 0xA0) || (first == 0xED && next >= 0xA0)
                || (first == 0xF0 && next < 0x90) || (first == 0xF4 && next >= 0x90)))
                return false;
        }
        i += width;
    }
    return true;
}

// ── 휴리스틱 감지 ──
QString EncodingDetector::detectHeuristic(const QByteArray& data, bool mayBeIncomplete)
{
    if (isValidUtf8(data, mayBeIncomplete)) {
        // 비-ASCII 바이트가 있으면 UTF-8, 없으면 ASCII (UTF-8 호환)
        for (uint8_t b : data) {
            if (b & 0x80) return QStringLiteral("UTF-8");
        }
        return QStringLiteral("UTF-8");
    }

    // CP949 (EUC-KR) 감지 시도 — 한국어 범위
    bool couldBeCP949 = true;
    for (int i = 0; i < data.size(); ++i) {
        uint8_t b = data[i];
        if (b >= 0x80) {
            if (i + 1 >= data.size()) { couldBeCP949 = false; break; }
            uint8_t b2 = data[i + 1];
            // EUC-KR 범위: 첫 바이트 0x81~0xFE, 둘째 바이트 0x41~0xFE
            if (b < 0x81 || b2 < 0x41) { couldBeCP949 = false; break; }
            ++i;
        }
    }
    if (couldBeCP949)
        return QStringLiteral("EUC-KR");

    // 폴백
    return QStringLiteral("System");
}

// ── 공개 API ──
QString EncodingDetector::detect(const QByteArray& rawData)
{
    auto bom = detectBOM(rawData);
    return bom.isEmpty() ? detectHeuristic(rawData) : bom;
}

QString EncodingDetector::detectFromSample(const QByteArray& sampleData, bool mayBeIncomplete)
{
    const auto bom = detectBOM(sampleData);
    return bom.isEmpty() ? detectHeuristic(sampleData, mayBeIncomplete) : bom;
}

QString EncodingDetector::decode(const QByteArray& rawData, const QString& encoding,
                                 bool* ok, bool allowIncomplete)
{
    if (ok)
        *ok = false;
    QString decoded;
    bool valid = false;
    const auto enc = QStringConverter::encodingForName(encoding.toUtf8().constData());
    if (enc) {
        const auto flags = allowIncomplete ? QStringConverter::Flags{}
            : QStringConverter::Flags{QStringConverter::Flag::Stateless};
        QStringDecoder decoder(*enc, flags);
        decoded = decoder(rawData);
        valid = !decoder.hasError();
    } else if (const auto* codec = QTextCodec::codecForName(encoding.toUtf8())) {
        if (rawData.size() > (std::numeric_limits<int>::max)())
            return {};
        QTextCodec::ConverterState state;
        decoded = codec->toUnicode(rawData.constData(), static_cast<int>(rawData.size()), &state);
        valid = state.invalidChars == 0 && (allowIncomplete || state.remainingChars == 0);
    }
    if (ok)
        *ok = valid;
    return valid ? decoded : QString{};
}

QByteArray EncodingDetector::encode(const QString& text, const QString& encoding)
{
    return encode(text, encoding, BomPolicy::Never);
}

QByteArray EncodingDetector::encode(const QString& text, const QString& encoding, BomPolicy bomPolicy,
                                    bool* ok)
{
    if (ok)
        *ok = false;
    const auto enc = QStringConverter::encodingForName(encoding.toUtf8().constData());
    QByteArray encoded;
    bool valid = false;
    if (enc) {
        QStringEncoder encoder(*enc, QStringConverter::Flag::Stateless);
        encoded = encoder(text);
        valid = !encoder.hasError();
        // Latin-1의 대체 문자 출력은 Qt 버전에 따라 hasError에 잡히지 않는다.
        if (*enc == QStringConverter::Latin1)
            valid = valid && std::all_of(text.cbegin(), text.cend(), [](QChar ch) {
                return ch.unicode() <= 0xFF;
            });
    } else if (const auto* codec = QTextCodec::codecForName(encoding.toUtf8())) {
        if (text.size() > (std::numeric_limits<int>::max)())
            return {};
        QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
        encoded = codec->fromUnicode(text.constData(), static_cast<int>(text.size()), &state);
        valid = state.invalidChars == 0 && codec->canEncode(text);
    }
    if (!valid)
        return {};

    const QByteArray bom = bomBytesForEncoding(encoding);
    if (!bom.isEmpty()) {
        if (bomPolicy == BomPolicy::Always) {
            if (!encoded.startsWith(bom))
                encoded.prepend(bom);
        } else if (encoded.startsWith(bom)) {
            encoded.remove(0, bom.size());
        }
    }

    if (ok)
        *ok = true;
    return encoded;
}

QStringList EncodingDetector::availableEncodings()
{
    QStringList encodings{
        "UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE",
        "EUC-KR", "System",
        "ISO-8859-1", "Shift_JIS", "GB18030"
    };
    encodings.removeIf([](const QString& name) {
        return !QStringConverter::encodingForName(name.toUtf8().constData())
            && !QTextCodec::codecForName(name.toUtf8());
    });
    return encodings;
}

bool EncodingDetector::hasBom(const QByteArray& rawData, const QString& encoding)
{
    if (rawData.isEmpty())
        return false;

    const QString normalized = encoding.trimmed().toUpper();
    const QString bomEncoding = detectBOM(rawData).toUpper();

    if (normalized.isEmpty())
        return !bomEncoding.isEmpty();

    if (normalized == QStringLiteral("UTF-8"))
        return bomEncoding == QStringLiteral("UTF-8");
    if (normalized == QStringLiteral("UTF-16") || normalized == QStringLiteral("UTF-16LE"))
        return bomEncoding == QStringLiteral("UTF-16LE");
    if (normalized == QStringLiteral("UTF-16BE"))
        return bomEncoding == QStringLiteral("UTF-16BE");
    if (normalized == QStringLiteral("UTF-32") || normalized == QStringLiteral("UTF-32LE"))
        return bomEncoding == QStringLiteral("UTF-32LE");
    if (normalized == QStringLiteral("UTF-32BE"))
        return bomEncoding == QStringLiteral("UTF-32BE");

    return false;
}

bool EncodingDetector::supportsBom(const QString& encoding)
{
    return !bomBytesForEncoding(encoding).isEmpty();
}

QByteArray EncodingDetector::bomBytesForEncoding(const QString& encoding)
{
    const QString normalized = encoding.trimmed().toUpper();
    if (normalized == QStringLiteral("UTF-8"))
        return QByteArray::fromHex("EFBBBF");
    if (normalized == QStringLiteral("UTF-16") || normalized == QStringLiteral("UTF-16LE"))
        return QByteArray::fromHex("FFFE");
    if (normalized == QStringLiteral("UTF-16BE"))
        return QByteArray::fromHex("FEFF");
    if (normalized == QStringLiteral("UTF-32") || normalized == QStringLiteral("UTF-32LE"))
        return QByteArray::fromHex("FFFE0000");
    if (normalized == QStringLiteral("UTF-32BE"))
        return QByteArray::fromHex("0000FEFF");
    return {};
}

