#include "TestRunner.hpp"
#include "core/solPreviewRequestState.hpp"
#include <QTest>

class TestPreviewRequestState : public QObject
{
    Q_OBJECT
private slots:
    void markdownSwitchRejectsOldSphinxResult();
    void newRequestInvalidatesSameDocumentGate();
    void clearedDocumentRejectsPendingResults();
    void projectAndDocumentSwitchRejectsOldBuild();
};

void TestPreviewRequestState::markdownSwitchRejectsOldSphinxResult()
{
    mrst::PreviewRequestState state;
    state.begin("/docs/a.rst", "project-a", true);
    const auto pending = state.ticket();
    QVERIFY(state.accepts(pending));
    state.begin("/notes/b.md", {}, false);
    QVERIFY(!state.accepts(pending));
    QVERIFY(!state.acceptsSphinxResult("/docs/a.rst", "project-a"));
    QVERIFY(!state.acceptsSphinxResult("/notes/b.md", {}));
}

void TestPreviewRequestState::newRequestInvalidatesSameDocumentGate()
{
    mrst::PreviewRequestState state;
    state.begin("/docs/a.rst", "project", true);
    const auto cached = state.ticket();
    // F5나 미저장 사본 빌드가 캐시 판정 없이 새 요청을 시작한 경우다.
    state.begin("/docs/a.rst", "project", true);
    QVERIFY(!state.accepts(cached));
    QVERIFY(state.accepts(state.ticket()));
    QVERIFY(state.acceptsSphinxResult("/docs/a.rst", "project"));
}

void TestPreviewRequestState::clearedDocumentRejectsPendingResults()
{
    mrst::PreviewRequestState state;
    state.begin("/docs/a.rst", "project", true);
    const auto pending = state.ticket();
    state.invalidate();
    QVERIFY(!state.accepts(pending));
    QVERIFY(!state.acceptsSphinxResult("/docs/a.rst", "project"));
    QVERIFY(!state.accepts(state.ticket()));
}

void TestPreviewRequestState::projectAndDocumentSwitchRejectsOldBuild()
{
    mrst::PreviewRequestState state;
    state.begin("/docs/a.rst", "project-a", true);
    const auto pending = state.ticket();
    state.begin("/docs/b.rst", "project-a", true);
    QVERIFY(!state.accepts(pending));
    QVERIFY(!state.acceptsSphinxResult("/docs/a.rst", "project-a"));
    QVERIFY(state.acceptsSphinxResult("/docs/b.rst", "project-a"));
    state.begin("/docs/b.rst", "project-b", true);
    QVERIFY(!state.acceptsSphinxResult("/docs/b.rst", "project-a"));
    QVERIFY(state.acceptsSphinxResult("/docs/b.rst", "project-b"));
}

MRST_REGISTER_TEST(TestPreviewRequestState);
#include "tst_PreviewRequestState.moc"
