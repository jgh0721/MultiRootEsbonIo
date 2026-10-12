#include "TestRunner.hpp"
#include "core/solAppSettings.hpp"
#include "core/solShadowBackupStore.hpp"

#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QUuid>
#include <atomic>

class TestShadowBackupOrdering : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void newestSnapshotWins_data();
    void newestSnapshotWins();
    void discardedSnapshotStaysDeleted_data();
    void discardedSnapshotStaysDeleted();
    void deleteAllInvalidatesPendingWrites_data();
    void deleteAllInvalidatesPendingWrites();
private:
    QVariant previousEnabled_;
};

namespace {
void kinds()
{
    QTest::addColumn<bool>("untitled");
    QTest::newRow("file") << false;
    QTest::newRow("untitled") << true;
}
TextShadowBackupStore::Snapshot makeSnapshot(const QTemporaryDir& directory, bool untitled)
{
    TextShadowBackupStore::Snapshot snapshot;
    snapshot.isUntitled = untitled;
    snapshot.untitledId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    snapshot.originalFilePath = directory.filePath("document.rst");
    snapshot.text = "old contents";
    snapshot.encoding = "UTF-8";
    return snapshot;
}
QString backupPath(const TextShadowBackupStore::Snapshot& snapshot)
{
    return snapshot.isUntitled ? TextShadowBackupStore::backupPathForUntitled(snapshot.untitledId)
        : TextShadowBackupStore::backupPathForFile(snapshot.originalFilePath);
}
void discard(const TextShadowBackupStore::Snapshot& snapshot)
{
    if (snapshot.isUntitled)
        TextShadowBackupStore::deleteUntitledSnapshot(snapshot.untitledId);
    else
        TextShadowBackupStore::deleteSnapshot(snapshot.originalFilePath);
}
}

void TestShadowBackupOrdering::initTestCase()
{
    AppSettings settings;
    previousEnabled_ = settings.value("textView/hotExitEnabled");
    settings.setValue("textView/hotExitEnabled", true);
    settings.sync();
}

void TestShadowBackupOrdering::cleanupTestCase()
{
    AppSettings settings;
    if (previousEnabled_.isValid())
        settings.setValue("textView/hotExitEnabled", previousEnabled_);
    else
        settings.remove("textView/hotExitEnabled");
    settings.sync();
}

void TestShadowBackupOrdering::newestSnapshotWins_data() { kinds(); }
void TestShadowBackupOrdering::discardedSnapshotStaysDeleted_data() { kinds(); }
void TestShadowBackupOrdering::deleteAllInvalidatesPendingWrites_data() { kinds(); }

void TestShadowBackupOrdering::newestSnapshotWins()
{
    QFETCH(bool, untitled);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto old = makeSnapshot(directory, untitled);
    const auto cleanup = qScopeGuard([&] { discard(old); });
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(old));

    QThreadPool pool;
    QSemaphore release;
    std::atomic_bool saved = false;
    const auto drain = qScopeGuard([&] { release.release(); pool.waitForDone(); });
    pool.start([&] { release.acquire(); saved = TextShadowBackupStore::saveSnapshot(old); });

    auto latest = old;
    latest.text = "latest exit contents";
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(latest));
    QVERIFY(TextShadowBackupStore::saveSnapshot(latest));
    release.release();
    QVERIFY(pool.waitForDone(5000));
    QVERIFY(saved.load());
    TextShadowBackupStore::Snapshot restored;
    const bool loaded = untitled ? TextShadowBackupStore::loadUntitledSnapshot(old.untitledId, &restored)
        : TextShadowBackupStore::loadSnapshot(old.originalFilePath, &restored);
    QVERIFY(loaded);
    QCOMPARE(restored.text, latest.text);
}

void TestShadowBackupOrdering::discardedSnapshotStaysDeleted()
{
    QFETCH(bool, untitled);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto pending = makeSnapshot(directory, untitled);
    const auto cleanup = qScopeGuard([&] { discard(pending); });
    // 파일이 아직 만들어지지 않은 예약도 폐기해야 한다.
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(pending));
    discard(pending);
    QVERIFY(TextShadowBackupStore::saveSnapshot(pending));
    QVERIFY(!QFile::exists(backupPath(pending)));

    auto reopened = pending;
    reopened.text = "new editing session";
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(reopened));
    QVERIFY(TextShadowBackupStore::saveSnapshot(reopened));
    QVERIFY(QFile::exists(backupPath(reopened)));
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(pending));
    discard(reopened);
    QVERIFY(TextShadowBackupStore::saveSnapshot(pending));
    QVERIFY(!QFile::exists(backupPath(pending)));
}

void TestShadowBackupOrdering::deleteAllInvalidatesPendingWrites()
{
    QFETCH(bool, untitled);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto pending = makeSnapshot(directory, untitled);
    const auto cleanup = qScopeGuard([&] { discard(pending); });
    QVERIFY(TextShadowBackupStore::reserveSnapshotWrite(pending));
    QVERIFY(TextShadowBackupStore::deleteAllBackups());
    QVERIFY(TextShadowBackupStore::saveSnapshot(pending));
    QVERIFY(!QFile::exists(backupPath(pending)));
}

MRST_REGISTER_TEST(TestShadowBackupOrdering);
#include "tst_ShadowBackupOrdering.moc"
