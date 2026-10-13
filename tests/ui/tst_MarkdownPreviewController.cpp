#include "TestRunner.hpp"
#include "core/solMarkdownPreviewController.hpp"

#include <QSignalSpy>
#include <QTest>

class TestMarkdownPreviewController : public QObject
{
    Q_OBJECT
private slots:
    void readsOnceAfterTypingStops();
    void readinessAndCancelDelayReads();
    void bridgeReadinessPreservesTypingDebounce();
    void latestDuplicateCancelsOlderPendingRender();
    void forceAndSizeLimitArePreserved();
};

void TestMarkdownPreviewController::readsOnceAfterTypingStops()
{
    mrst::MarkdownPreviewController controller;
    controller.notifyBridgeReady();
    QSignalSpy pushes(&controller, &mrst::MarkdownPreviewController::pushRequested);
    int reads = 0;
    QString buffer;
    for (int i = 0; i < 1000; ++i) {
        buffer = QString::number(i);
        controller.requestRender("test.md", [&] {
            ++reads;
            return std::optional<QString>(buffer);
        }, false, false);
    }
    QCOMPARE(reads, 0);
    QTRY_COMPARE_WITH_TIMEOUT(pushes.size(), 1, 1000);
    QCOMPARE(reads, 1);
    QCOMPARE(pushes.first().at(0).toString(), QStringLiteral("999"));
}

void TestMarkdownPreviewController::readinessAndCancelDelayReads()
{
    mrst::MarkdownPreviewController controller;
    QSignalSpy pushes(&controller, &mrst::MarkdownPreviewController::pushRequested);
    int reads = 0;
    auto provider = [&] { ++reads; return std::optional<QString>("latest"); };
    controller.requestRender("test.md", provider, true, false);
    QTest::qWait(150);
    QCOMPARE(reads, 0);
    controller.notifyBridgeReady();
    QCOMPARE(reads, 1);
    QCOMPARE(pushes.size(), 1);
    controller.requestRender("test.md", provider, false, false);
    controller.cancel();
    QTest::qWait(150);
    QCOMPARE(reads, 1);
    controller.requestRender("test.md", [] { return std::optional<QString>{}; }, true, false);
    QCOMPARE(pushes.size(), 1);
}

void TestMarkdownPreviewController::bridgeReadinessPreservesTypingDebounce()
{
    mrst::MarkdownPreviewController controller;
    int reads = 0;
    controller.requestRender("test.md", [&] {
        ++reads;
        return std::optional<QString>("latest");
    }, false, false);
    controller.notifyBridgeReady();
    QCOMPARE(reads, 0);
    QTRY_COMPARE_WITH_TIMEOUT(reads, 1, 1000);
}

void TestMarkdownPreviewController::latestDuplicateCancelsOlderPendingRender()
{
    mrst::MarkdownPreviewController controller;
    controller.notifyBridgeReady();
    QSignalSpy pushes(&controller, &mrst::MarkdownPreviewController::pushRequested);
    controller.requestRender("test.md", QStringLiteral("A"), true, false);
    controller.requestRender("test.md", QStringLiteral("B"), false, false);
    controller.requestRender("test.md", QStringLiteral("A"), false, false);
    QTest::qWait(200);
    QCOMPARE(pushes.size(), 1);
    QCOMPARE(pushes.first().at(0).toString(), QStringLiteral("A"));
}

void TestMarkdownPreviewController::forceAndSizeLimitArePreserved()
{
    mrst::MarkdownPreviewController controller;
    controller.notifyBridgeReady();
    QSignalSpy pushes(&controller, &mrst::MarkdownPreviewController::pushRequested);
    controller.requestRender("test.md", QStringLiteral("A"), true, false);
    controller.requestRender("test.md", QStringLiteral("A"), true, true);
    QCOMPARE(pushes.size(), 2);
    const QString large(4 * 1024 * 1024 + 1, QLatin1Char('x'));
    controller.requestRender("test.md", large, false, false);
    QTest::qWait(200);
    QCOMPARE(pushes.size(), 2);
    controller.requestRender("test.md", large, true, false);
    QCOMPARE(pushes.size(), 3);
    QCOMPARE(pushes.last().at(0).toString().size(), large.size());
}

MRST_REGISTER_TEST(TestMarkdownPreviewController);
#include "tst_MarkdownPreviewController.moc"
