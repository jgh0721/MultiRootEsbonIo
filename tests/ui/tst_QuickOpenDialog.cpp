#include "TestRunner.hpp"

#include "core/solQuickOpenSearch.hpp"
#include "uis/QuickOpenDialog.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QInputMethodEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QSet>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTranslator>

namespace {

class RecordingTranslator final : public QTranslator
{
public:
    [[nodiscard]] QString translate( const char* context, const char* sourceText,
                                     const char* disambiguation = nullptr,
                                     int n = -1 ) const override
    {
        Q_UNUSED( disambiguation );
        Q_UNUSED( n );
        if( qstrcmp( sourceText, "%1, 경로 %2" ) == 0 )
        {
            contexts.insert( QString::fromLatin1( context ) );
            return QStringLiteral( "%1 / %2" );
        }
        if( qstrcmp( sourceText, "워크스페이스 루트" ) == 0 )
        {
            contexts.insert( QString::fromLatin1( context ) );
            return QStringLiteral( "ROOT" );
        }
        return {};
    }

    mutable QSet<QString> contexts;
};

class InstalledTranslator final
{
public:
    explicit InstalledTranslator( QTranslator* translator )
        : translator_( translator )
    {
        QCoreApplication::installTranslator( translator_ );
    }

    ~InstalledTranslator()
    {
        QCoreApplication::removeTranslator( translator_ );
    }

private:
    QTranslator* translator_ = nullptr;
};

}  // namespace

class TestQuickOpenDialog : public QObject
{
    Q_OBJECT

private slots:
    void exposesResultsInPagesOf150();
    void filtersAPathWithEitherSeparatorAndOpensIt();
    void enterDoesNotOpenWhileImeIsComposing();
    void searchesWithImePreeditBeforeCommit();
    void reportsProgressWhileIndexing();
    void streamsIndexBatchesWithoutStarvation();
    void prioritizesTypingOverIndexRefresh();
    void defersIndexRefreshWhileImeIsComposing();
    void displaysTheConfiguredShortcut();
    void reopensAfterCanceledRankingAndGlobalPoolClear();
    void internalTranslationContextsMatchCatalog();
    void missingFileReportsAnAccessibleError();
    void preservesSignificantPathWhitespace();
    void limitsQueryLengthToTheSearchContract();
};

void TestQuickOpenDialog::exposesResultsInPagesOf150()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    QStringList paths;
    for( int index = 0; index < 380; ++index )
    {
        paths << QStringLiteral( "bulk/file-%1.rst" )
                     .arg( index, 3, 10, QLatin1Char( '0' ) );
    }

    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace( workspace.path(), paths, false );

    auto* results = dialog.findChild< QListView* >( QStringLiteral( "quickOpenResults" ) );
    auto* status = dialog.findChild< QLabel* >( QStringLiteral( "quickOpenStatus" ) );
    QVERIFY( results != nullptr );
    QVERIFY( status != nullptr );

    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 150, 5000 );
    QVERIFY( results->model()->canFetchMore( {} ) );
    QVERIFY( status->text().contains( QStringLiteral( "1–150" ) ) );

    results->model()->fetchMore( {} );
    QCOMPARE( results->model()->rowCount(), 300 );
    results->model()->fetchMore( {} );
    QCOMPARE( results->model()->rowCount(), 380 );
    QVERIFY( !results->model()->canFetchMore( {} ) );
}

void TestQuickOpenDialog::filtersAPathWithEitherSeparatorAndOpensIt()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );
    QVERIFY( QDir( workspace.path() ).mkpath( QStringLiteral( "src/uis" ) ) );
    QFile chosenFile( QDir( workspace.path() ).absoluteFilePath(
        QStringLiteral( "src/uis/MainWindow.cpp" ) ) );
    const bool opened = chosenFile.open( QIODevice::WriteOnly );
    QVERIFY( opened );
    chosenFile.close();

    mrst::QuickOpenDialog dialog;
    QSignalSpy chosen( &dialog, &mrst::QuickOpenDialog::fileChosen );
    dialog.showForWorkspace(
        workspace.path(),
        { QStringLiteral( "src/uis/MainWindow.cpp" ), QStringLiteral( "docs/MainWindow.md" ) },
        false );

    auto* query = dialog.findChild< QLineEdit* >( QStringLiteral( "quickOpenQuery" ) );
    auto* results = dialog.findChild< QListView* >( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( results != nullptr );

    query->setText( QStringLiteral( R"(src\ui\main)" ) );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );
    QCOMPARE( results->model()->index( 0, 0 ).data( Qt::ToolTipRole ).toString(),
              QStringLiteral( "src/uis/MainWindow.cpp" ) );

    QTest::keyClick( query, Qt::Key_Return );
    QCOMPARE( chosen.size(), 1 );
    QCOMPARE( QDir::cleanPath( chosen.first().first().toString() ),
              QDir::cleanPath( QDir( workspace.path() ).absoluteFilePath(
                  QStringLiteral( "src/uis/MainWindow.cpp" ) ) ) );
}

void TestQuickOpenDialog::enterDoesNotOpenWhileImeIsComposing()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );
    QFile chosenFile( QDir( workspace.path() ).absoluteFilePath(
        QStringLiteral( "한글문서.rst" ) ) );
    const bool opened = chosenFile.open( QIODevice::WriteOnly );
    QVERIFY( opened );
    chosenFile.close();

    mrst::QuickOpenDialog dialog;
    QSignalSpy chosen( &dialog, &mrst::QuickOpenDialog::fileChosen );
    dialog.showForWorkspace( workspace.path(), { QStringLiteral( "한글문서.rst" ) }, false );

    auto* query = dialog.findChild< QLineEdit* >( QStringLiteral( "quickOpenQuery" ) );
    auto* results = dialog.findChild< QListView* >( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );

    QInputMethodEvent composing( QStringLiteral( "ㅎ" ), {} );
    QApplication::sendEvent( query, &composing );
    QTest::keyClick( query, Qt::Key_Return );
    QCOMPARE( chosen.size(), 0 );
    QVERIFY( dialog.isVisible() );

    QInputMethodEvent committed;
    QApplication::sendEvent( query, &committed );
    QTest::keyClick( query, Qt::Key_Return );
    QCOMPARE( chosen.size(), 1 );
}

void TestQuickOpenDialog::searchesWithImePreeditBeforeCommit()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace(
        workspace.path(),
        { QStringLiteral( "3ㅂ-초안.rst" ), QStringLiteral( "3부-안내.rst" ),
          QStringLiteral( "기타.rst" ) },
        false );

    auto* query = dialog.findChild<QLineEdit*>( QStringLiteral( "quickOpenQuery" ) );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( results != nullptr );

    query->setText( QStringLiteral( "3" ) );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 2, 5000 );

    QSignalSpy resets( results->model(), &QAbstractItemModel::modelReset );
    QVERIFY( resets.isValid() );
    QInputMethodEvent initialPreedit( QStringLiteral( "ㅂ" ), {} );
    QApplication::sendEvent( query, &initialPreedit );
    QCOMPARE( query->text(), QStringLiteral( "3" ) );
    QTRY_VERIFY_WITH_TIMEOUT( resets.size() >= 1, 5000 );
    QTRY_COMPARE_WITH_TIMEOUT(
        results->model()->index( 0, 0 ).data( Qt::ToolTipRole ).toString(),
        QStringLiteral( "3ㅂ-초안.rst" ), 5000 );
    QCOMPARE( results->model()->rowCount(), 1 );

    QInputMethodEvent syllablePreedit( QStringLiteral( "부" ), {} );
    QApplication::sendEvent( query, &syllablePreedit );
    QCOMPARE( query->text(), QStringLiteral( "3" ) );
    QTRY_COMPARE_WITH_TIMEOUT(
        results->model()->index( 0, 0 ).data( Qt::ToolTipRole ).toString(),
        QStringLiteral( "3부-안내.rst" ), 5000 );
    QCOMPARE( results->model()->rowCount(), 1 );

    QInputMethodEvent committed;
    committed.setCommitString( QStringLiteral( "부" ) );
    QApplication::sendEvent( query, &committed );
    QCOMPARE( query->text(), QStringLiteral( "3부" ) );
    QTRY_COMPARE_WITH_TIMEOUT(
        results->model()->index( 0, 0 ).data( Qt::ToolTipRole ).toString(),
        QStringLiteral( "3부-안내.rst" ), 5000 );
}

void TestQuickOpenDialog::reportsProgressWhileIndexing()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace( workspace.path(), {}, true );

    auto* status = dialog.findChild< QLabel* >( QStringLiteral( "quickOpenStatus" ) );
    auto* results = dialog.findChild< QListView* >( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( status != nullptr );
    QVERIFY( results != nullptr );
    QVERIFY( status->text().contains( QStringLiteral( "인덱싱 중" ) ) );

    QStringList batch;
    for( int index = 0; index < 170; ++index )
        batch << QStringLiteral( "docs/result-%1.md" ).arg( index );
    dialog.appendIndexedPaths( batch, 170 );

    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 150, 5000 );
    QVERIFY( status->text().contains( QStringLiteral( "170" ) ) );
    dialog.finishIndexing();
    QVERIFY( !status->text().contains( QStringLiteral( "인덱싱 중" ) ) );
}

void TestQuickOpenDialog::streamsIndexBatchesWithoutStarvation()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    QElapsedTimer resetClock;
    QVector<qint64> resetTimes;
    resetClock.start();
    mrst::QuickOpenDialog dialog;
    dialog.showForPathIndex( workspace.path(), {}, true );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( results != nullptr );
    connect( results->model(), &QAbstractItemModel::modelReset, &dialog,
             [ &resetClock, &resetTimes ] { resetTimes.push_back( resetClock.elapsed() ); } );

    bool publishedWhileBatchesWereStillArriving = false;
    qsizetype total = 0;
    for( int batchIndex = 0; batchIndex < 24; ++batchIndex )
    {
        QStringList batch;
        for( int item = 0; item < 5; ++item )
        {
            batch.push_back( QStringLiteral( "stream/batch-%1-file-%2.rst" )
                                 .arg( batchIndex ).arg( item ) );
        }
        total += batch.size();
        dialog.appendPathIndexBatch( batch, total );
        QTest::qWait( 20 );
        publishedWhileBatchesWereStillArriving =
            publishedWhileBatchesWereStillArriving || results->model()->rowCount() > 0;
    }

    QVERIFY( publishedWhileBatchesWereStillArriving );
    QTRY_VERIFY_WITH_TIMEOUT( resetTimes.size() >= 2, 2500 );
    for( qsizetype index = 1; index < resetTimes.size(); ++index )
    {
        const qint64 interval = resetTimes.at( index ) - resetTimes.at( index - 1 );
        QVERIFY2( interval >= 200,
                  qPrintable( QStringLiteral( "증분 재검색 간격이 너무 짧음: %1ms" )
                                  .arg( interval ) ) );
    }
    dialog.finishPathIndexing( QStringList( {
        QStringLiteral( "stream/final.rst" ),
    } ) );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );
}

void TestQuickOpenDialog::prioritizesTypingOverIndexRefresh()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForPathIndex( workspace.path(),
                             { QStringLiteral( "target-one.rst" ) }, true );
    auto* query = dialog.findChild<QLineEdit*>( QStringLiteral( "quickOpenQuery" ) );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );

    QVector<qint64> resetTimes;
    QElapsedTimer clock;
    clock.start();
    connect( results->model(), &QAbstractItemModel::modelReset, &dialog,
             [ &clock, &resetTimes ] { resetTimes.push_back( clock.elapsed() ); } );

    query->setText( QStringLiteral( "target" ) );
    QTRY_VERIFY_WITH_TIMEOUT( !resetTimes.isEmpty(), 5000 );
    const qsizetype userResultCount = resetTimes.size();
    clock.restart();
    dialog.appendPathIndexBatch( { QStringLiteral( "target-two.rst" ) }, 2 );

    // 인덱스 batch가 연속되더라도 방금 입력한 질의 결과와 키 입력 처리를
    // 최소 300ms 동안 우선한다.
    QTest::qWait( 300 );
    QCOMPARE( resetTimes.size(), userResultCount );
    QTRY_VERIFY_WITH_TIMEOUT( resetTimes.size() > userResultCount, 2000 );
    QVERIFY2( resetTimes.constLast() >= 300,
              qPrintable( QStringLiteral( "인덱스 갱신이 입력 직후 너무 빨리 실행됨: %1ms" )
                              .arg( resetTimes.constLast() ) ) );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 2, 5000 );
}

void TestQuickOpenDialog::defersIndexRefreshWhileImeIsComposing()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForPathIndex( workspace.path(),
                             { QStringLiteral( "ㅎ-first-result.rst" ) }, true );
    auto* query = dialog.findChild<QLineEdit*>( QStringLiteral( "quickOpenQuery" ) );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );

    QSignalSpy resets( results->model(), &QAbstractItemModel::modelReset );
    QVERIFY( resets.isValid() );

    QInputMethodEvent composing( QStringLiteral( "ㅎ" ), {} );
    QApplication::sendEvent( query, &composing );
    QTRY_VERIFY_WITH_TIMEOUT( resets.size() >= 1, 5000 );
    QCOMPARE( results->model()->rowCount(), 1 );
    resets.clear();
    dialog.appendPathIndexBatch( { QStringLiteral( "ㅎ-second-result.rst" ) }, 2 );

    // preedit 질의 자체는 검색하되, 인덱스 진행만 조합 중에 끼어들지 않는다.
    QTest::qWait( 700 );
    QCOMPARE( query->text(), QString{} );
    QCOMPARE( resets.size(), 0 );
    QCOMPARE( results->model()->rowCount(), 1 );

    // 빈 preedit 이벤트로 조합을 끝내면 보류된 최신 snapshot을 다시 랭킹한다.
    QInputMethodEvent compositionEnded;
    QApplication::sendEvent( query, &compositionEnded );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 2, 2000 );
    QCOMPARE( resets.size(), 1 );
}

void TestQuickOpenDialog::displaysTheConfiguredShortcut()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace( workspace.path(), { QStringLiteral( "file.rst" ) }, false,
                             {}, QStringLiteral( "Ctrl+K" ) );
    auto* shortcut = dialog.findChild<QLabel*>( QStringLiteral( "quickOpenShortcut" ) );
    QVERIFY( shortcut != nullptr );
    QCOMPARE( shortcut->text(), QStringLiteral( "Ctrl+K" ) );
    QVERIFY( !shortcut->isHidden() );

    dialog.reject();
    dialog.showForWorkspace( workspace.path(), { QStringLiteral( "file.rst" ) }, false );
    QVERIFY( shortcut->text().isEmpty() );
    QVERIFY( shortcut->isHidden() );
}

void TestQuickOpenDialog::reopensAfterCanceledRankingAndGlobalPoolClear()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    QStringList firstPaths;
    for( int index = 0; index < 50'000; ++index )
        firstPaths.push_back( QStringLiteral( "old/file-%1.rst" ).arg( index ) );

    mrst::QuickOpenDialog dialog;
    dialog.showForPathIndex( workspace.path(), firstPaths, false );
    QTest::qWait( 100 );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( results != nullptr );
    dialog.reject();
    QCOMPARE( results->model()->rowCount(), 0 );

    // MainWindow 종료가 global pool을 비운 뒤 취소되는 실제 순서를 흉내 낸다.
    QThreadPool::globalInstance()->clear();
    dialog.showForPathIndex( workspace.path(),
                             { QStringLiteral( "new/only-result.rst" ) }, false );

    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );
    QCOMPARE( results->model()->index( 0, 0 ).data( Qt::ToolTipRole ).toString(),
              QStringLiteral( "new/only-result.rst" ) );
}

void TestQuickOpenDialog::internalTranslationContextsMatchCatalog()
{
    RecordingTranslator translator;
    const InstalledTranslator installed( &translator );
    Q_UNUSED( installed );

    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );
    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace(
        workspace.path(),
        { QStringLiteral( "docs/item.rst" ), QStringLiteral( "root.rst" ) }, false );

    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 2, 5000 );
    (void)results->model()->index( 0, 0 ).data( Qt::AccessibleTextRole );

    QImage image( 640, 40, QImage::Format_ARGB32_Premultiplied );
    image.fill( Qt::transparent );
    QPainter painter( &image );
    QStyleOptionViewItem option;
    option.initFrom( results );
    option.rect = image.rect();
    option.font = results->font();
    results->itemDelegate()->paint( &painter, option, results->model()->index( 1, 0 ) );

    QVERIFY( translator.contexts.contains( QStringLiteral( "mrst::QuickOpenListModel" ) ) );
    QVERIFY( translator.contexts.contains( QStringLiteral( "mrst::QuickOpenItemDelegate" ) ) );
}

void TestQuickOpenDialog::missingFileReportsAnAccessibleError()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    QSignalSpy chosen( &dialog, &mrst::QuickOpenDialog::fileChosen );
    dialog.showForWorkspace( workspace.path(), { QStringLiteral( "missing.rst" ) }, false );

    auto* query = dialog.findChild<QLineEdit*>( QStringLiteral( "quickOpenQuery" ) );
    auto* status = dialog.findChild<QLabel*>( QStringLiteral( "quickOpenStatus" ) );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( query != nullptr );
    QVERIFY( status != nullptr );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 1, 5000 );

    QTest::keyClick( query, Qt::Key_Return );
    QCOMPARE( chosen.size(), 0 );
    QVERIFY( dialog.isVisible() );
    QVERIFY( status->text().contains( QStringLiteral( "열 수 없습니다" ) ) );
    QCOMPARE( status->accessibleDescription(), status->text() );
}

void TestQuickOpenDialog::preservesSignificantPathWhitespace()
{
    QTemporaryDir workspace;
    QVERIFY( workspace.isValid() );

    mrst::QuickOpenDialog dialog;
    dialog.showForWorkspace(
        workspace.path(),
        { QStringLiteral( " leading.rst" ), QStringLiteral( "trailing.rst " ) }, false );
    auto* results = dialog.findChild<QListView*>( QStringLiteral( "quickOpenResults" ) );
    QVERIFY( results != nullptr );
    QTRY_COMPARE_WITH_TIMEOUT( results->model()->rowCount(), 2, 5000 );

    QStringList shownPaths;
    for( int row = 0; row < results->model()->rowCount(); ++row )
        shownPaths.push_back( results->model()->index( row, 0 ).data( Qt::ToolTipRole ).toString() );
    QVERIFY( shownPaths.contains( QStringLiteral( " leading.rst" ) ) );
    QVERIFY( shownPaths.contains( QStringLiteral( "trailing.rst " ) ) );
}

void TestQuickOpenDialog::limitsQueryLengthToTheSearchContract()
{
    mrst::QuickOpenDialog dialog;
    auto* query = dialog.findChild<QLineEdit*>( QStringLiteral( "quickOpenQuery" ) );
    QVERIFY( query != nullptr );
    QCOMPARE( query->maxLength(), mrst::kQuickOpenMaximumQueryLength );

    query->setText( QString( mrst::kQuickOpenMaximumQueryLength + 1,
                             QLatin1Char( 'a' ) ) );
    QCOMPARE( query->text().size(), mrst::kQuickOpenMaximumQueryLength );
}

MRST_REGISTER_TEST( TestQuickOpenDialog );

#include "tst_QuickOpenDialog.moc"
