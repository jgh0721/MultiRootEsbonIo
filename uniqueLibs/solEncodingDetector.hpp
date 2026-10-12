#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

/// 파일 인코딩 자동 감지 및 변환 유틸리티
class EncodingDetector
{
public:
    enum class BomPolicy {
        Never,
        Always
    };

    /// BOM + 휴리스틱으로 인코딩 감지
    static QString detect(const QByteArray& rawData);

    /// 샘플 바이트만으로 인코딩 감지
    static QString detectFromSample(const QByteArray& sampleData, bool mayBeIncomplete = false);

    /// 지정 인코딩으로 디코딩. 미지원·손실 변환은 빈 결과와 ok=false로 알린다.
    static QString decode(const QByteArray& rawData, const QString& encoding,
                          bool* ok = nullptr, bool allowIncomplete = false);

    /// 지정 인코딩으로 인코딩. 성공한 빈 문서는 ok=true이며 결과는 비어 있다.
    static QByteArray encode(const QString& text, const QString& encoding);
    static QByteArray encode(const QString& text, const QString& encoding, BomPolicy bomPolicy,
                             bool* ok = nullptr);

    /// 지원하는 인코딩 목록
    static QStringList availableEncodings();

    /// 인코딩별 BOM 존재 여부 확인
    static bool hasBom(const QByteArray& rawData, const QString& encoding = {});
    static bool supportsBom(const QString& encoding);

private:
    static QByteArray bomBytesForEncoding(const QString& encoding);
    static QString detectBOM(const QByteArray& data);
    static QString detectHeuristic(const QByteArray& data, bool mayBeIncomplete = false);
    static bool    isValidUtf8(const QByteArray& data, bool mayBeIncomplete = false);
};

