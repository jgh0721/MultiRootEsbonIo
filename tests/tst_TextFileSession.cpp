#include "TestRunner.hpp"
#include "editor/TextFileSession.hpp"
#include "uniqueLibs/solEncodingDetector.hpp"

#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
QByteArray contents(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class TestTextFileSession : public QObject
{
    Q_OBJECT
private slots:
    void originalStaysIntactUntilCommit();
    void failedCommitPreservesOriginal();
    void savesEmptyDocument();
    void legacyCodecs_data();
    void legacyCodecs();
    void rejectsLossySave();
    void rejectsUnknownCodec();
    void validatesDecodeAndPreviewTail();
    void offeredCodecsAreUsable();
    void utf8SampleBoundary();
    void utf32BomRoundTrip_data();
    void utf32BomRoundTrip();
};

void TestTextFileSession::originalStaysIntactUntilCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("document.rst");
    const QByteArray original("original document\n");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();

    TextFileSession session;
    QVERIFY(session.open(path, TextFileSession::OpenMode::Full));
    const QString replacement(600000, QLatin1Char('x'));
    int progressCalls = 0;
    bool originalIntact = true;
    QVERIFY(session.saveText(path, replacement, "UTF-8", ScintillaDocument::LF, false,
        [&](qint64, qint64) {
            ++progressCalls;
            originalIntact = originalIntact && contents(path) == original;
        }));
    QVERIFY(progressCalls > 2);
    QVERIFY(originalIntact);
    QCOMPARE(contents(path), replacement.toUtf8());
    QCOMPARE(session.rawData(), replacement.toUtf8());
}

void TestTextFileSession::failedCommitPreservesOriginal()
{
#ifndef Q_OS_WIN
    QSKIP("Windows 파일 교체 잠금으로 commit 실패를 검증한다");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("document.md");
    const QByteArray original("original document\n");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    TextFileSession session;
    QVERIFY(session.open(path, TextFileSession::OpenMode::Full));

    // 쓰기는 허용하고 파일 교체만 막는다. 직접 쓰기는 원본을 손상시킨다.
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] { CloseHandle(handle); });
    QVERIFY(!session.saveText(path, "replacement", "UTF-8", ScintillaDocument::LF, false));
    QCOMPARE(contents(path), original);
    QCOMPARE(session.rawData(), original);
#endif
}

void TestTextFileSession::savesEmptyDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("empty.md");
    TextFileSession session;
    QVERIFY(session.saveText(path, {}, "UTF-8", ScintillaDocument::CRLF, false));
    QVERIFY(QFile::exists(path));
    QCOMPARE(contents(path), QByteArray{});
}

void TestTextFileSession::legacyCodecs_data()
{
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("korean") << QString("EUC-KR") << QString::fromUtf8("가나다")
        << QByteArray::fromHex("b0a1b3aab4d9");
    QTest::newRow("japanese") << QString("Shift_JIS") << QString::fromUtf8("あいう")
        << QByteArray::fromHex("82a082a282a4");
    QTest::newRow("chinese") << QString("GB18030") << QString::fromUtf8("中文")
        << QByteArray::fromHex("d6d0cec4");
}

void TestTextFileSession::legacyCodecs()
{
    QFETCH(QString, encoding);
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    QVERIFY(EncodingDetector::availableEncodings().contains(encoding));
    bool ok = false;
    QCOMPARE(EncodingDetector::decode(bytes, encoding, &ok), text);
    QVERIFY(ok);
    QCOMPARE(EncodingDetector::encode(text, encoding, EncodingDetector::BomPolicy::Never, &ok), bytes);
    QVERIFY(ok);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("legacy.txt");
    TextFileSession session;
    QVERIFY(session.saveText(path, text, encoding, ScintillaDocument::LF, false));
    QCOMPARE(contents(path), bytes);
    TextFileSession opened;
    QVERIFY(opened.open(path, TextFileSession::OpenMode::Full));
    QCOMPARE(opened.decodeWithEncoding(encoding, &ok), text);
    QVERIFY(ok);
}

void TestTextFileSession::rejectsLossySave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("original.md");
    TextFileSession session;
    QVERIFY(session.saveText(path, "original", "UTF-8", ScintillaDocument::LF, false));
    QString error;
    QVERIFY(!session.saveText(path, QString::fromUtf8("한글🙂"), "ISO-8859-1",
        ScintillaDocument::LF, false, {}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(contents(path), QByteArray("original"));
    QCOMPARE(session.rawData(), QByteArray("original"));
    QCOMPARE(session.detectedEncoding(), QString("UTF-8"));
}

void TestTextFileSession::rejectsUnknownCodec()
{
    bool ok = true;
    QVERIFY(EncodingDetector::encode("text", "not-a-codec", EncodingDetector::BomPolicy::Never, &ok).isEmpty());
    QVERIFY(!ok);
    ok = true;
    QVERIFY(EncodingDetector::decode("text", "not-a-codec", &ok).isEmpty());
    QVERIFY(!ok);
}

void TestTextFileSession::validatesDecodeAndPreviewTail()
{
    bool ok = true;
    QVERIFY(EncodingDetector::decode(QByteArray::fromHex("c080"), "UTF-8", &ok).isEmpty());
    QVERIFY(!ok);
    QVERIFY(EncodingDetector::decode(QByteArray::fromHex("41eab0"), "UTF-8", &ok).isEmpty());
    QVERIFY(!ok);
    QCOMPARE(EncodingDetector::decode(QByteArray::fromHex("41eab0"), "UTF-8", &ok, true), QString("A"));
    QVERIFY(ok);
}

void TestTextFileSession::offeredCodecsAreUsable()
{
    for (const QString& encoding : EncodingDetector::availableEncodings()) {
        bool ok = false;
        EncodingDetector::encode({}, encoding, EncodingDetector::BomPolicy::Never, &ok);
        QVERIFY2(ok, qPrintable(encoding));
    }
}

void TestTextFileSession::utf8SampleBoundary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString text(87382, QChar(0xAC00));
    const QByteArray bytes = text.toUtf8();
    QCOMPARE(bytes.size(), qsizetype(262146));
    QFile file(directory.filePath("sample.rst"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    TextFileSession session;
    QVERIFY(session.open(file.fileName(), TextFileSession::OpenMode::Full));
    QCOMPARE(session.detectedEncoding(), QString("UTF-8"));
    bool ok = false;
    QCOMPARE(session.decodeWithEncoding(session.detectedEncoding(), &ok), text);
    QVERIFY(ok);
    QCOMPARE(EncodingDetector::detectFromSample(bytes.left(262144), true), QString("UTF-8"));
    QVERIFY(EncodingDetector::detectFromSample(QByteArray::fromHex("eda080"), true) != "UTF-8");
    QVERIFY(EncodingDetector::detectFromSample(QByteArray::fromHex("c080"), true) != "UTF-8");
    QVERIFY(EncodingDetector::detectFromSample(QByteArray::fromHex("f4908080"), true) != "UTF-8");
    QVERIFY(EncodingDetector::detectFromSample(QByteArray::fromHex("41eab0")) != "UTF-8");
}

void TestTextFileSession::utf32BomRoundTrip_data()
{
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<QByteArray>("bom");
    QTest::newRow("little-endian") << QString("UTF-32LE") << QByteArray::fromHex("fffe0000");
    QTest::newRow("big-endian") << QString("UTF-32BE") << QByteArray::fromHex("0000feff");
}

void TestTextFileSession::utf32BomRoundTrip()
{
    QFETCH(QString, encoding);
    QFETCH(QByteArray, bom);
    const QString text = QString::fromUtf8("한글🙂");
    bool ok = false;
    const auto bytes = EncodingDetector::encode(text, encoding, EncodingDetector::BomPolicy::Always, &ok);
    QVERIFY(ok);
    QVERIFY(bytes.startsWith(bom));
    QCOMPARE(EncodingDetector::detect(bytes), encoding);
    QVERIFY(EncodingDetector::hasBom(bytes, encoding));
    QVERIFY(EncodingDetector::supportsBom(encoding));
    QCOMPARE(EncodingDetector::decode(bytes, encoding, &ok), text);
    QVERIFY(ok);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TextFileSession session;
    const auto file = directory.filePath("unicode.rst");
    QVERIFY(session.saveText(file, text, encoding, ScintillaDocument::LF, true));
    TextFileSession opened;
    QVERIFY(opened.open(file, TextFileSession::OpenMode::Full));
    QCOMPARE(opened.detectedEncoding(), encoding);
    QVERIFY(opened.hasBom());
    QCOMPARE(opened.decodeWithEncoding(encoding, &ok), text);
    QVERIFY(ok);
}

MRST_REGISTER_TEST(TestTextFileSession);
#include "tst_TextFileSession.moc"
