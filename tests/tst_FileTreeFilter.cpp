#include "TestRunner.hpp"

#include "uis/FileTreeFilterProxy.hpp"

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
    void wildcardMatchesWholeName();
    void wildcardIsNotSubstring();
    void filterIsCaseInsensitive();
    void sortsNumbersAsNumbers();
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

MRST_REGISTER_TEST( TestFileTreeFilter );

#include "tst_FileTreeFilter.moc"
