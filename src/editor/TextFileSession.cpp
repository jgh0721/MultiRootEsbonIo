#include "text/TextFileSession.h"

#include "utils/EncodingDetector.h"
#include "utils/FileLoadHelper.h"

#include <QFile>

bool TextFileSession::open(const QString& filePath,
                           OpenMode mode,
                           const ProgressCallback& progress,
                           const CancelCallback& cancel,
                           bool* wasCanceled)
{
    clear();

    if (wasCanceled)
        *wasCanceled = false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    m_filePath = filePath;
    m_fileSize = file.size();
    m_openMode = mode;

    const qint64 sampleBytes = qMin(m_fileSize, DetectionSampleLimit);
    m_detectionSample = file.peek(sampleBytes);
    m_detectedEncoding = EncodingDetector::detectFromSample(m_detectionSample);
    m_hasBom = EncodingDetector::hasBom(m_detectionSample, m_detectedEncoding);

    const qint64 bytesToRead = (mode == OpenMode::Full)
        ? m_fileSize
        : qMin(m_fileSize, PreviewByteLimit);

    const FileLoadHelper::ReadResult readResult = FileLoadHelper::readFileInChunks(file,
                                                                                   bytesToRead,
                                                                                   &m_rawData,
                                                                                   progress,
                                                                                   cancel);
    if (readResult != FileLoadHelper::ReadResult::Success) {
        if (readResult == FileLoadHelper::ReadResult::Cancelled && wasCanceled)
            *wasCanceled = true;
        clear();
        return false;
    }

    m_truncated = bytesToRead < m_fileSize;
    return true;
}

QString TextFileSession::decodeWithEncoding(const QString& encoding) const
{
    return EncodingDetector::decode(m_rawData, encoding);
}

bool TextFileSession::saveText(const QString& filePath,
                               const QString& text,
                               const QString& encoding,
                               ScintillaDocument::LineEnding lineEnding,
                               bool includeBom,
                               const ProgressCallback& progress)
{
    if (filePath.isEmpty() || m_truncated)
        return false;

    const QByteArray data = EncodingDetector::encode(normalizeLineEndings(text, lineEnding),
                                                     encoding,
                                                     includeBom ? EncodingDetector::BomPolicy::Always
                                                                : EncodingDetector::BomPolicy::Never);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    constexpr qint64 chunkSize = 256LL * 1024LL;
    const qint64 totalBytes = data.size();
    qint64 writtenBytes = 0;

    if (progress)
        progress(0, totalBytes);

    while (writtenBytes < totalBytes) {
        const qint64 bytesToWrite = qMin(chunkSize, totalBytes - writtenBytes);
        const qint64 chunkWritten = file.write(data.constData() + writtenBytes, bytesToWrite);
        if (chunkWritten <= 0)
            return false;

        writtenBytes += chunkWritten;
        if (progress)
            progress(writtenBytes, totalBytes);
    }

    if (!file.flush())
        return false;

    m_filePath = filePath;
    m_rawData = data;
    m_fileSize = data.size();
    m_detectionSample = data.left(static_cast<int>(qMin<qint64>(data.size(), DetectionSampleLimit)));
    m_detectedEncoding = encoding;
    m_hasBom = EncodingDetector::hasBom(data, encoding);
    m_truncated = false;
    m_openMode = OpenMode::Full;
    return true;
}

void TextFileSession::clear()
{
    m_filePath.clear();
    m_rawData.clear();
    m_detectionSample.clear();
    m_detectedEncoding.clear();
    m_fileSize = 0;
    m_hasBom = false;
    m_truncated = false;
    m_openMode = OpenMode::Full;
}

bool TextFileSession::shouldPromptForLargeFile(qint64 fileSize)
{
    return fileSize >= LargeFilePromptThresholdBytes;
}

QString TextFileSession::normalizeLineEndings(QString text, ScintillaDocument::LineEnding lineEnding)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    switch (lineEnding) {
        case ScintillaDocument::CRLF:
            text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
            break;
        case ScintillaDocument::CR:
            text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
            break;
        case ScintillaDocument::LF:
        default:
            break;
    }

    return text;
}

