#include "TestRunner.hpp"

#include "core/solWorkspaceSession.hpp"
#include "core/solShadowBackupStore.hpp"
#include <QKeySequence>
#include "core/solAppSettings.hpp"

#include <QTest>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QScopeGuard>

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
    void externalFilesRoundTrip();
    void legacySession();
    void standaloneSessionPath();
    void externalHotExit();
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

void TestWorkspaceSessionActive::externalFilesRoundTrip()
{
    QTemporaryDir workspace;
    QTemporaryDir outside;
    QVERIFY( workspace.isValid() && outside.isValid() );
    auto session = sessionWith( { workspace.filePath( "inside.rst" ), outside.filePath( "outside.md" ),
                                 outside.filePath( "notes.txt" ) }, 1 );
    session.workspaceRoot = workspace.path();
    session.externalFiles = { outside.filePath( "outside.md" ), outside.filePath( "notes.txt" ),
                              outside.filePath( "closed-tab.rst" ) };
    session.documents[1].caretLine = 15;
    session.documents[1].caretColumn = 7;
    session.documents[1].firstVisibleLine = 10;
    QVERIFY( saveWorkspaceSession( session ) );
    const auto restored = loadWorkspaceSession( workspace.path() );
    QCOMPARE( restored.externalFiles, session.externalFiles );
    QCOMPARE( restored.documents.size(), 3 );
    QCOMPARE( activeDocumentPath( restored ), outside.filePath( "outside.md" ) );
    QCOMPARE( restored.documents[1].caretLine, 15 );
    QCOMPARE( restored.documents[1].caretColumn, 7 );
    QCOMPARE( restored.documents[1].firstVisibleLine, 10 );
    QVERIFY( isPathInWorkspace( restored.documents[0].path, workspace.path() ) );
    QVERIFY( !isPathInWorkspace( restored.documents[1].path, workspace.path() ) );
}

void TestWorkspaceSessionActive::legacySession()
{
    auto json = sessionToJson( sessionWith( { "/outside/doc.md" }, 0 ) );
    json.remove( "externalFiles" );
    const auto restored = sessionFromJson( json );
    QVERIFY( restored.externalFiles.isEmpty() );
    QCOMPARE( activeDocumentPath( restored ), QString( "/outside/doc.md" ) );
}

void TestWorkspaceSessionActive::standaloneSessionPath()
{
    const QString path = sessionFilePath( {} );
    QVERIFY( !path.isEmpty() );
    QVERIFY( path.startsWith( QStandardPaths::writableLocation( QStandardPaths::AppLocalDataLocation ) ) );
    auto session = sessionWith( { "/outside/doc.md" }, 0 );
    session.workspaceRoot.clear();
    session.externalFiles = { "/outside/doc.md" };
    const auto restored = sessionFromJson( sessionToJson( session ) );
    QVERIFY( restored.workspaceRoot.isEmpty() );
    QCOMPARE( restored.externalFiles, session.externalFiles );
    QCOMPARE( activeDocumentPath( restored ), QString( "/outside/doc.md" ) );
}

void TestWorkspaceSessionActive::externalHotExit()
{
    QTemporaryDir workspace;
    QTemporaryDir outside;
    QVERIFY( workspace.isValid() && outside.isValid() );
    AppSettings settings;
    const auto enabledKey = QStringLiteral( "textView/hotExitEnabled" );
    const QVariant oldEnabled = settings.value( enabledKey );
    settings.setValue( enabledKey, true );
    settings.sync();
    const auto restoreSetting = qScopeGuard( [&] {
        if( oldEnabled.isValid() )
            settings.setValue( enabledKey, oldEnabled );
        else
            settings.remove( enabledKey );
        settings.sync();
    } );
    for( const auto& name : { "external.rst", "external.md", "external.txt" } )
    {
        const QString path = outside.filePath( name );
        QFile original( path );
        QVERIFY( original.open( QIODevice::WriteOnly ) );
        original.write( "saved content" );
        original.close();
        QVERIFY( !isPathInWorkspace( path, workspace.path() ) );
        TextShadowBackupStore::Snapshot snapshot;
        snapshot.originalFilePath = path;
        snapshot.text = QString::fromUtf8( "저장하지 않은 변경\nsecond line" );
        snapshot.encoding = "UTF-8";
        snapshot.caretPosition = 5;
        snapshot.firstVisibleLine = 1;
        snapshot.originalSize = QFileInfo( path ).size();
        snapshot.originalLastModifiedUtcMs = QFileInfo( path ).lastModified().toUTC().toMSecsSinceEpoch();
        const auto removeBackup = qScopeGuard( [&] { TextShadowBackupStore::deleteSnapshot( path ); } );
        QVERIFY( TextShadowBackupStore::saveSnapshot( snapshot ) );
        TextShadowBackupStore::Snapshot restored;
        QVERIFY( TextShadowBackupStore::loadSnapshot( path, &restored ) );
        QCOMPARE( restored.text, snapshot.text );
        QCOMPARE( restored.caretPosition, snapshot.caretPosition );
        QCOMPARE( restored.firstVisibleLine, snapshot.firstVisibleLine );
        QVERIFY( TextShadowBackupStore::originalFileMatchesSnapshot( restored ) );
        QVERIFY( TextShadowBackupStore::restorableFilePaths().contains( QFileInfo( path ).canonicalFilePath() ) );
        QVERIFY( original.open( QIODevice::ReadOnly ) );
        QCOMPARE( original.readAll(), QByteArray( "saved content" ) );
    }
}

MRST_REGISTER_TEST( TestWorkspaceSessionActive );

#include "tst_WorkspaceSessionActive.moc"
