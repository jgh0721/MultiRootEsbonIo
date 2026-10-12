#include "TestRunner.hpp"
#include "editor/TextFileSession.hpp"

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

MRST_REGISTER_TEST(TestTextFileSession);
#include "tst_TextFileSession.moc"
