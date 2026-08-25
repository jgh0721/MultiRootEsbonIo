#include "TestRunner.hpp"

#include "core/solRecentItems.hpp"
#include "core/solWorkspaceSession.hpp"

#include <QDir>
#include <QTest>

using namespace mrst;

namespace {

/// 이 스위트가 쓰는 경로를 플랫폼 표기로 만든다. 기대값을 리터럴로 적으면
/// Windows 에서만 통과하거나 Windows 에서만 깨지는 검사가 된다.
QString native( const QString& path )
{
    return QDir::toNativeSeparators( path );
}

}  // namespace

/// 최근 목록은 "지난번에 뭘 하고 있었나" 에 답하는 자리다. 같은 항목이 두 번
/// 적히거나, 방금 연 것이 맨 앞이 아니거나, 길이가 무한히 자라면 그 답이 아니다.
/// 규칙을 순수 함수에 두고 여기서 못 박는다.
class TestRecentItems : public QObject
{
    Q_OBJECT

private slots:
    void newestGoesFirst();
    void existingEntryMovesToFront();
    void comparesPathsLikeTheFileSystem();
    void normalizesSeparators();
    void honoursMaximum();
    void ignoresEmptyEntry();
    void removesEntry();
    void sessionCarriesWindowGeometry();
};

void TestRecentItems::newestGoesFirst()
{
    QStringList entries;
    entries = prependRecentEntry( entries, QStringLiteral( "/w/a.rst" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/b.rst" ) );

    QCOMPARE( entries.size(), 2 );
    QCOMPARE( entries.at( 0 ), native( QStringLiteral( "/w/b.rst" ) ) );
    QCOMPARE( entries.at( 1 ), native( QStringLiteral( "/w/a.rst" ) ) );
}

void TestRecentItems::existingEntryMovesToFront()
{
    // 같은 파일을 다시 열면 항목이 하나 더 생기는 것이 아니라 위로 올라온다.
    QStringList entries;
    entries = prependRecentEntry( entries, QStringLiteral( "/w/a.rst" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/b.rst" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/a.rst" ) );

    QCOMPARE( entries.size(), 2 );
    QCOMPARE( entries.at( 0 ), native( QStringLiteral( "/w/a.rst" ) ) );
    QCOMPARE( entries.at( 1 ), native( QStringLiteral( "/w/b.rst" ) ) );
}

void TestRecentItems::comparesPathsLikeTheFileSystem()
{
    QStringList entries = prependRecentEntry( {}, QStringLiteral( "/w/Doc.rst" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/doc.rst" ) );

#ifdef Q_OS_WIN
    // Windows 에서 두 경로는 같은 파일이다. 둘로 세면 메뉴에 같은 항목이 두 번 뜬다.
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ), native( QStringLiteral( "/w/doc.rst" ) ) );
#else
    QCOMPARE( entries.size(), 2 );
#endif
}

void TestRecentItems::normalizesSeparators()
{
    // 워크스페이스 경로는 파일 대화상자에서 `/` 로, 드롭에서 `\` 로 들어온다.
    // 정규화하지 않으면 같은 폴더가 두 항목으로 남는다.
    QStringList entries = prependRecentEntry( {}, QStringLiteral( "/w/docs/" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/./docs" ) );

    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ), native( QStringLiteral( "/w/docs" ) ) );
}

void TestRecentItems::honoursMaximum()
{
    QStringList entries;
    for( int index = 0; index < 5; ++index )
        entries = prependRecentEntry( entries, QStringLiteral( "/w/%1.rst" ).arg( index ), 3 );

    QCOMPARE( entries.size(), 3 );
    QCOMPARE( entries.at( 0 ), native( QStringLiteral( "/w/4.rst" ) ) );
    QCOMPARE( entries.at( 2 ), native( QStringLiteral( "/w/2.rst" ) ) );
}

void TestRecentItems::ignoresEmptyEntry()
{
    // 이름 없는 버퍼의 빈 경로가 목록에 들어가면 메뉴에 글자 없는 항목이 생긴다.
    const QStringList entries{ native( QStringLiteral( "/w/a.rst" ) ) };
    QCOMPARE( prependRecentEntry( entries, QString{} ), entries );
}

void TestRecentItems::removesEntry()
{
    QStringList entries;
    entries = prependRecentEntry( entries, QStringLiteral( "/w/a.rst" ) );
    entries = prependRecentEntry( entries, QStringLiteral( "/w/b.rst" ) );

    const QStringList pruned = removeRecentEntry( entries, QStringLiteral( "/w/a.rst" ) );
    QCOMPARE( pruned.size(), 1 );
    QCOMPARE( pruned.at( 0 ), native( QStringLiteral( "/w/b.rst" ) ) );

    // 없는 항목을 빼도 목록은 그대로다. 호출 측이 "바뀌었는가" 를 비교로 판정한다.
    QCOMPARE( removeRecentEntry( entries, QStringLiteral( "/w/c.rst" ) ), entries );
}

void TestRecentItems::sessionCarriesWindowGeometry()
{
    // 창 크기는 워크스페이스에 딸린 상태다. 이 키가 왕복에서 빠지면 창은 언제나
    // 기본 크기로 뜨는데, 그 실패는 조용해서 눈에 띄지 않는다.
    WorkspaceSession session;
    session.workspaceRoot = QStringLiteral( "/w" );
    session.windowGeometry = QStringLiteral( "AdnQywADAAAAAAAAAAAAAA==" );

    const WorkspaceSession roundTripped = sessionFromJson( sessionToJson( session ) );
    QCOMPARE( roundTripped.windowGeometry, session.windowGeometry );

    // 이 키는 나중에 생겼다. 없는 세션(예전 파일)은 빈 문자열이어야 하고,
    // 그때는 복원 쪽이 기본 크기를 그대로 둔다.
    QJsonObject legacy = sessionToJson( session );
    legacy.remove( QStringLiteral( "windowGeometry" ) );
    QVERIFY( sessionFromJson( legacy ).windowGeometry.isEmpty() );
    QCOMPARE( sessionFromJson( legacy ).workspaceRoot, QStringLiteral( "/w" ) );
}

MRST_REGISTER_TEST( TestRecentItems );

#include "tst_RecentItems.moc"
