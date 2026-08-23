#include "TestRunner.hpp"

#include "core/solWorkspaceSession.hpp"

#include <QTest>

using namespace mrst;

namespace {

WorkspaceSession sessionWith( const QStringList& paths, const int activeIndex )
{
    WorkspaceSession session;
    session.workspaceRoot = QStringLiteral( "/w" );
    for( const QString& path : paths )
    {
        OpenDocumentState document;
        document.path = path;
        session.documents.push_back( document );
    }
    session.activeIndex = activeIndex;
    return session;
}

}  // namespace

/// activeIndex 를 탭 위젯의 번호로 착각한 적이 있다. 두 목록은 어긋난다 —
/// 핫 엑시트 스냅샷이 복원보다 먼저 탭을 열고, 사라진 파일은 건너뛴다. 그때
/// 엉뚱한 탭이 활성이 되고, 그 자리가 이름 없는 버퍼면 경로가 없어 프리뷰가
/// 아예 만들어지지 않았다. 규칙을 이름 있는 함수에 두고 여기서 못 박는다.
class TestWorkspaceSessionActive : public QObject
{
    Q_OBJECT

private slots:
    void returnsPathAtIndex();
    void emptyWhenNeverSet();
    void emptyWhenOutOfRange();
    void emptyWithoutDocuments();
};

void TestWorkspaceSessionActive::returnsPathAtIndex()
{
    const WorkspaceSession session = sessionWith(
        { QStringLiteral( "/w/a.rst" ), QStringLiteral( "/w/b.rst" ), QStringLiteral( "/w/c.md" ) }, 1 );
    QCOMPARE( activeDocumentPath( session ), QStringLiteral( "/w/b.rst" ) );
}

void TestWorkspaceSessionActive::emptyWhenNeverSet()
{
    // 기본값은 -1 이다. 그것을 0 으로 읽으면 열자마자 첫 문서로 튄다.
    const WorkspaceSession session = sessionWith( { QStringLiteral( "/w/a.rst" ) }, -1 );
    QVERIFY( activeDocumentPath( session ).isEmpty() );
}

void TestWorkspaceSessionActive::emptyWhenOutOfRange()
{
    // 세션 파일은 사람이 고칠 수 있고 문서가 지워지기도 한다.
    const WorkspaceSession session = sessionWith( { QStringLiteral( "/w/a.rst" ) }, 5 );
    QVERIFY( activeDocumentPath( session ).isEmpty() );
}

void TestWorkspaceSessionActive::emptyWithoutDocuments()
{
    QVERIFY( activeDocumentPath( sessionWith( {}, 0 ) ).isEmpty() );
}

MRST_REGISTER_TEST( TestWorkspaceSessionActive );

#include "tst_WorkspaceSessionActive.moc"
