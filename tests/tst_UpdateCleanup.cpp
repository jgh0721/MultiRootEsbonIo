#include "TestRunner.hpp"
#include "core/solUpdateInstaller.hpp"
#include "utils/solBackgroundWork.hpp"
#include "utils/ProcessReaper.hpp"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QUuid>

class TestUpdateCleanup : public QObject
{
    Q_OBJECT
private slots:
    void retryCannotBeDeleted_data();
    void retryCannotBeDeleted();
    void refusesFilesystemRoot();
    void retriesInterruptedCleanup();
};

void TestUpdateCleanup::retryCannotBeDeleted_data()
{
    QTest::addColumn<bool>("directory");
    QTest::newRow("staging") << true;
    QTest::newRow("download-zip") << false;
}

void TestUpdateCleanup::retryCannotBeDeleted()
{
    QFETCH(bool, directory);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString reusable = fixture.filePath(directory ? "staging" : "update.zip");
    auto put = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (directory)
        QVERIFY(QDir().mkpath(reusable));
    QVERIFY(put(directory ? QDir(reusable).filePath("payload") : reusable, "old"));

    auto* pool = QThreadPool::globalInstance();
    QVERIFY(pool->waitForDone(5000));
    const int previous = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    pool->start([&] { entered.release(); release.acquire(); });
    entered.acquire();
    const auto unblock = qScopeGuard([&] {
        release.release();
        pool->waitForDone(5000);
        pool->setMaxThreadCount(previous);
    });
    mrst::UpdateInstaller installer;
    installer.removePathsAsync({reusable});
    QVERIFY(!QFileInfo::exists(reusable)); // detached before worker runs
    const auto retired = QDir(fixture.path()).entryList(
        {QStringLiteral(".mrst-update-cleanup-*")}, QDir::AllEntries | QDir::Hidden);
    QCOMPARE(retired.size(), 1);
    if (directory)
        QVERIFY(QDir().mkpath(reusable));
    const QString next = directory ? QDir(reusable).filePath("payload") : reusable;
    QVERIFY(put(next, "new"));
    release.release();
    QVERIFY(pool->waitForDone(5000));
    QFile file(next);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("new"));
    QVERIFY(!QFileInfo::exists(fixture.filePath(retired.first())));
}

void TestUpdateCleanup::refusesFilesystemRoot()
{
    mrst::UpdateInstaller installer;
    QSignalSpy failures(&installer, &mrst::UpdateInstaller::logMessage);
    installer.removePathsAsync({QDir::rootPath()});
    QCOMPARE(failures.size(), 1);
    QVERIFY(QDir(QDir::rootPath()).exists());
}

void TestUpdateCleanup::retriesInterruptedCleanup()
{
    constexpr qint64 deadPid = 2147483647;
    QVERIFY(!mrst::isProcessRunning(deadPid));
    mrst::UpdateInstaller first;
    QVERIFY(QDir().mkpath(first.workRoot()));
    const QString path = QDir(first.workRoot()).filePath(
        QStringLiteral(".mrst-update-cleanup-%1-%2").arg(deadPid)
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const auto removeFixture = qScopeGuard([&] { QFile::remove(path); });
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    mrst::UpdateInstaller restarted;
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QVERIFY(!QFileInfo::exists(path));
}

MRST_REGISTER_TEST(TestUpdateCleanup);
#include "tst_UpdateCleanup.moc"
