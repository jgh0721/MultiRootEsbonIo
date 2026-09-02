#include "TestRunner.hpp"

#include "uis/FileTreeFilterProxy.hpp"

#include <QSet>
#include <QStandardItemModel>
#include <QStringList>
#include <QTest>

using namespace mrst;

namespace {

/// 트리 하나를 만든다. QFileSystemModel 없이 검증하려는 것이 요점이다 —
/// 필터 규칙은 파일 시스템을 몰라야 한다.
///
///   docs/
///     index.rst
///     guide.rst
///   src/
///     main.py
///   readme.md
QStandardItemModel* makeTree( QObject* parent )
{
    auto* model = new QStandardItemModel( parent );

    auto* docs = new QStandardItem( QStringLiteral( "docs" ) );
    docs->appendRow( new QStandardItem( QStringLiteral( "index.rst" ) ) );
    docs->appendRow( new QStandardItem( QStringLiteral( "guide.rst" ) ) );

    auto* src = new QStandardItem( QStringLiteral( "src" ) );
    src->appendRow( new QStandardItem( QStringLiteral( "main.py" ) ) );

    model->appendRow( docs );
    model->appendRow( src );
    model->appendRow( new QStandardItem( QStringLiteral( "readme.md" ) ) );
    return model;
}

/// QFileSystemModel 의 게으름을 흉내 낸 모델.
///
/// ⚠ 진짜 QFileSystemModel 의 canFetchMore() 는 **디렉터리인지 보지 않는다** —
///   아직 훑지 않은 노드면 파일이라도 true 다(Qt 구현이 populatedChildren 만
///   본다). 그 성질이 이 클래스가 있는 이유다. 그것을 "자식이 더 있을지 모른다"
///   로 읽으면 필터가 모든 파일을 통과시킨다.
class LazyModel final : public QStandardItemModel
{
public:
    /// 자식이 아직 모델에 없어도 디렉터리로 볼 이름들.
    QSet< QString > lazyDirectories;

    bool canFetchMore( const QModelIndex& ) const override { return true; }
    void fetchMore( const QModelIndex& ) override {}

    bool hasChildren( const QModelIndex& parent ) const override
    {
        if( parent.isValid()
            && lazyDirectories.contains( parent.data( Qt::DisplayRole ).toString() ) )
        {
            return true;
        }
        return QStandardItemModel::hasChildren( parent );
    }
};

/// 프록시가 남긴 이름을 전위 순회로 늘어놓는다.
QStringList visibleNames( const FileTreeFilterProxy& proxy, const QModelIndex& parent = {} )
{
    QStringList names;
    for( int row = 0; row < proxy.rowCount( parent ); ++row )
    {
        const QModelIndex index = proxy.index( row, 0, parent );
        names << index.data( Qt::DisplayRole ).toString();
        names += visibleNames( proxy, index );
    }
    return names;
}

}  // namespace

/// 탐색기 필터가 조용히 틀리면 "결과가 없다" 와 "기능이 고장났다" 를 사용자가
/// 구분할 수 없다. 규칙 하나하나를 파일 시스템 없이 못 박아 둔다.
class TestFileTreeFilter : public QObject
{
    Q_OBJECT

private slots:
    void emptyFilterKeepsEverything();
    void substringMatchesAnywhereInName();
    void keepsAncestorsOfMatch();
    void keepsDescendantsOfMatchedDirectory();
    void rootIndexStopsAncestorWalk();
    void rootRowSurvivesWhenNothingMatches();
    void wildcardMatchesWholeName();
    void wildcardIsNotSubstring();
    void filterIsCaseInsensitive();
    void sortsNumbersAsNumbers();

    // ── 게으른 모델 (QFileSystemModel) ──
    void lazyModelDoesNotLeakFiles();
    void keepsUnreadDirectory();
};

void TestFileTreeFilter::emptyFilterKeepsEverything()
{
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );

    QVERIFY( !proxy.isFiltering() );
    QCOMPARE( visibleNames( proxy ).size(), 6 );
}

void TestFileTreeFilter::substringMatchesAnywhereInName()
{
    // 사용자가 요구한 성질이다 — 이름 앞머리가 아니라 어디든 걸려야 한다.
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );
    proxy.setFilterText( QStringLiteral( "uid" ) );   // guide.rst 의 한가운데

    QVERIFY( proxy.isFiltering() );
    QVERIFY( visibleNames( proxy ).contains( QStringLiteral( "guide.rst" ) ) );
}

void TestFileTreeFilter::keepsAncestorsOfMatch()
{
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );
    proxy.setFilterText( QStringLiteral( "guide" ) );

    const QStringList names = visibleNames( proxy );
    // 부모가 남지 않으면 걸린 항목이 화면에 나올 길이 없다.
    QVERIFY( names.contains( QStringLiteral( "docs" ) ) );
    QVERIFY( names.contains( QStringLiteral( "guide.rst" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "index.rst" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "src" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "readme.md" ) ) );
}

void TestFileTreeFilter::keepsDescendantsOfMatchedDirectory()
{
    // 폴더 이름을 쳤는데 그 안이 비어 보이면 필터가 고장난 것처럼 읽힌다.
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );
    proxy.setFilterText( QStringLiteral( "docs" ) );

    const QStringList names = visibleNames( proxy );
    QVERIFY( names.contains( QStringLiteral( "docs" ) ) );
    QVERIFY( names.contains( QStringLiteral( "index.rst" ) ) );
    QVERIFY( names.contains( QStringLiteral( "guide.rst" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "src" ) ) );
}

void TestFileTreeFilter::rootIndexStopsAncestorWalk()
{
    // 워크스페이스 폴더 이름이 우연히 필터와 맞으면(D:\docs 에서 "docs")
    // 조상 규칙이 필터를 통째로 무력하게 만든다. 뿌리에서 멈춰야 한다.
    auto* model = new QStandardItemModel;
    auto* root = new QStandardItem( QStringLiteral( "docs" ) );
    auto* inner = new QStandardItem( QStringLiteral( "guide.rst" ) );
    auto* other = new QStandardItem( QStringLiteral( "notes.txt" ) );
    root->appendRow( inner );
    root->appendRow( other );
    model->appendRow( root );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.setRootSourceIndex( model->indexFromItem( root ) );
    proxy.setFilterText( QStringLiteral( "docs" ) );

    // 뿌리 아래에서 본 결과: "docs" 라는 이름은 뿌리 자신의 것이므로
    // 그 안의 항목에까지 통과권을 주지 않는다.
    const QModelIndex proxyRoot = proxy.mapFromSource( model->indexFromItem( root ) );
    QCOMPARE( proxy.rowCount( proxyRoot ), 0 );
}

void TestFileTreeFilter::rootRowSurvivesWhenNothingMatches()
{
    // 실측 회귀: 아무것도 걸리지 않는 문구를 치면 워크스페이스 행 자체가
    // 프록시에서 사라졌다. 그러면 QTreeView 의 rootIndex 가 무효해지고, 뷰는
    // 무효한 뿌리를 **모델의 최상위**로 읽는다 — 트리에 드라이브 전체가
    // 나타나고 필터가 그것을 훑기 시작했다.
    auto* model = new QStandardItemModel;
    auto* drive = new QStandardItem( QStringLiteral( "D:" ) );
    auto* root = new QStandardItem( QStringLiteral( "workspace" ) );
    root->appendRow( new QStandardItem( QStringLiteral( "guide.rst" ) ) );
    drive->appendRow( root );
    model->appendRow( drive );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.setRootSourceIndex( model->indexFromItem( root ) );
    proxy.setFilterText( QStringLiteral( "zzzz" ) );

    // 뿌리와 그 조상은 남아야 뷰가 뿌리를 잃지 않는다.
    const QModelIndex proxyRoot = proxy.mapFromSource( model->indexFromItem( root ) );
    QVERIFY( proxyRoot.isValid() );
    QVERIFY( proxy.mapFromSource( model->indexFromItem( drive ) ).isValid() );
    // 그렇다고 통과권을 아래로 물려주지는 않는다. 그 안은 비어야 한다.
    QCOMPARE( proxy.rowCount( proxyRoot ), 0 );
}

void TestFileTreeFilter::wildcardMatchesWholeName()
{
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );
    proxy.setFilterText( QStringLiteral( "*.rst" ) );

    const QStringList names = visibleNames( proxy );
    QVERIFY( names.contains( QStringLiteral( "index.rst" ) ) );
    QVERIFY( names.contains( QStringLiteral( "guide.rst" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "readme.md" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "main.py" ) ) );
}

void TestFileTreeFilter::wildcardIsNotSubstring()
{
    auto* model = new QStandardItemModel;
    model->appendRow( new QStandardItem( QStringLiteral( "a.rst" ) ) );
    model->appendRow( new QStandardItem( QStringLiteral( "a.rst.bak" ) ) );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.setFilterText( QStringLiteral( "*.rst" ) );

    // 전체 일치다. 부분 일치였다면 .bak 까지 걸려 와일드카드의 뜻이 사라진다.
    QCOMPARE( visibleNames( proxy ), QStringList{ QStringLiteral( "a.rst" ) } );
}

void TestFileTreeFilter::filterIsCaseInsensitive()
{
    FileTreeFilterProxy proxy;
    proxy.setSourceModel( makeTree( &proxy ) );
    proxy.setFilterText( QStringLiteral( "README" ) );

    QVERIFY( visibleNames( proxy ).contains( QStringLiteral( "readme.md" ) ) );
}

void TestFileTreeFilter::sortsNumbersAsNumbers()
{
    auto* model = new QStandardItemModel;
    for( const char* name : { "part10.rst", "part2.rst", "part1.rst" } )
        model->appendRow( new QStandardItem( QLatin1String( name ) ) );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.sort( 0, Qt::AscendingOrder );

    // 글자 순이면 part10 이 part2 앞에 온다. 문서 번호를 그렇게 늘어놓으면
    // 트리에서 순서를 읽을 수 없다.
    QCOMPARE( visibleNames( proxy ),
             ( QStringList{ QStringLiteral( "part1.rst" ), QStringLiteral( "part2.rst" ),
                           QStringLiteral( "part10.rst" ) } ) );
}

// ── 게으른 모델 (QFileSystemModel) ─────────────────────────

void TestFileTreeFilter::lazyModelDoesNotLeakFiles()
{
    // 실측 회귀: `.md` 로 걸렀는데 말단의 .py · .txt 가 그대로 남았다.
    // "아직 안 읽었으면 남긴다" 예외가 canFetchMore() 만 보았기 때문이다.
    auto* model = new LazyModel;
    auto* docs = new QStandardItem( QStringLiteral( "docs" ) );
    docs->appendRow( new QStandardItem( QStringLiteral( "guide.md" ) ) );
    docs->appendRow( new QStandardItem( QStringLiteral( "build.py" ) ) );
    docs->appendRow( new QStandardItem( QStringLiteral( "notes.txt" ) ) );
    model->appendRow( docs );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.setFilterText( QStringLiteral( ".md" ) );

    const QStringList names = visibleNames( proxy );
    QVERIFY( names.contains( QStringLiteral( "guide.md" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "build.py" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "notes.txt" ) ) );
}

void TestFileTreeFilter::keepsUnreadDirectory()
{
    // 반대쪽 벽. 아직 읽지 않은 디렉터리까지 지우면 뷰가 펼칠 수 없고, 펼치지
    // 않으면 모델이 읽지 않는다 — 그 안의 일치 항목에 닿을 길이 영영 막힌다.
    auto* model = new LazyModel;
    model->lazyDirectories << QStringLiteral( "unread" );
    model->appendRow( new QStandardItem( QStringLiteral( "unread" ) ) );
    model->appendRow( new QStandardItem( QStringLiteral( "plain.py" ) ) );

    FileTreeFilterProxy proxy;
    model->setParent( &proxy );
    proxy.setSourceModel( model );
    proxy.setFilterText( QStringLiteral( ".md" ) );

    const QStringList names = visibleNames( proxy );
    QVERIFY( names.contains( QStringLiteral( "unread" ) ) );
    QVERIFY( !names.contains( QStringLiteral( "plain.py" ) ) );
}

MRST_REGISTER_TEST( TestFileTreeFilter );

#include "tst_FileTreeFilter.moc"
