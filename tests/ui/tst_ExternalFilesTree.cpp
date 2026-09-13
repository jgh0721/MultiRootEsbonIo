#include "TestRunner.hpp"
#include "uis/ExternalFilesProxy.hpp"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

class TestExternalFilesTree : public QObject
{
    Q_OBJECT
private slots:
    void classifiesAndDeduplicates();
    void survivesSourceChanges();
    void filesystemTree();
};

void TestExternalFilesTree::classifiesAndDeduplicates()
{
    QTemporaryDir directory;
    QVERIFY( directory.isValid() );
    const QString workspace = directory.filePath( "workspace" );
    QStandardItemModel source;
    source.appendRow( new QStandardItem( "workspace" ) );
    // QFileSystemModel exposes columns even when a directory has no files.
    source.item( 0 )->setColumnCount( 1 );
    // QFileSystemModel exposes columns even when a directory has no files.
    source.item( 0 )->setColumnCount( 1 );
    mrst::ExternalFilesProxy model;
    model.setSourceModel( &source );
    model.setWorkspace( workspace, source.index( 0, 0 ) );
    QAbstractItemModelTester tester( &model, QAbstractItemModelTester::FailureReportingMode::QtTest );
    QVERIFY( !model.addFile( workspace + "/inside.rst" ) );
    QVERIFY( !model.addFile( workspace + "/nested/../inside.md" ) );
    const QString outside = directory.filePath( "workspace-other/outside.md" );
    QVERIFY( model.addFile( outside ) );
    QVERIFY( !model.addFile( outside ) );
#ifdef Q_OS_WIN
    QVERIFY( !model.addFile( outside.toUpper() ) );
#endif
    auto branch = model.externalRoot();
    QCOMPARE( branch.parent(), model.mapFromSource( source.index( 0, 0 ) ) );
    QCOMPARE( branch.data().toString(), QString::fromUtf8( "외부" ) );
    auto file = model.indexForFile( outside );
    QCOMPARE( file.parent(), branch );
    QCOMPARE( file.data().toString(), QString( "outside.md" ) );
    QCOMPARE( model.filePath( file ), QDir::cleanPath( outside ) );
    QVERIFY( !model.mapToSource( file ).isValid() );
    QVERIFY( !model.canFetchMore( file ) );
    QVERIFY( !model.hasChildren( file ) );
    QCOMPARE( model.buddy( file ), file );
    QVERIFY( model.mapSelectionToSource( QItemSelection( file, file ) ).isEmpty() );
    QCOMPARE( model.itemData( file ).value( Qt::DisplayRole ).toString(), QString( "outside.md" ) );
    model.removeFile( outside );
    QVERIFY( !model.externalRoot().isValid() );
    model.setWorkspace( {}, {} );
    QVERIFY( model.addFile( workspace + "/inside.rst" ) );
    QVERIFY( !model.externalRoot().parent().isValid() );
}

void TestExternalFilesTree::survivesSourceChanges()
{
    QStandardItemModel source;
    auto* workspace = new QStandardItem( "workspace" );
    source.appendRow( workspace );
    workspace->appendRow( new QStandardItem( "b.rst" ) );
    QSortFilterProxyModel filter;
    filter.setSourceModel( &source );
    mrst::ExternalFilesProxy model;
    model.setSourceModel( &filter );
    model.setWorkspace( QDir::tempPath() + "/workspace", filter.index( 0, 0 ) );
    QAbstractItemModelTester tester( &model, QAbstractItemModelTester::FailureReportingMode::QtTest );
    const QString first = QDir::tempPath() + "/outside-first.rst";
    const QString second = QDir::tempPath() + "/outside-second.txt";
    QVERIFY( model.addFile( first ) );
    QVERIFY( model.addFile( second ) );
    QPersistentModelIndex branch( model.externalRoot() );
    QPersistentModelIndex file( model.indexForFile( second ) );
    workspace->insertRow( 0, new QStandardItem( "a.rst" ) );
    QCOMPARE( branch, model.externalRoot() );
    QCOMPARE( branch.row(), 2 );
    QCOMPARE( file.parent(), branch );
    filter.sort( 0, Qt::DescendingOrder );
    QCOMPARE( branch, model.externalRoot() );
    QCOMPARE( model.filePath( file ), QDir::cleanPath( second ) );
    QCOMPARE( model.index( 0, 0, branch.parent() ).data().toString(), QString( "b.rst" ) );
    workspace->removeRow( 0 );
    QCOMPARE( branch, model.externalRoot() );
    QCOMPARE( branch.row(), 1 );
    model.removeFile( first );
    QCOMPARE( file.row(), 0 );
    QCOMPARE( model.filePath( file ), QDir::cleanPath( second ) );
    model.removeFile( second );
    QVERIFY( !branch.isValid() );
    QVERIFY( !file.isValid() );
}

void TestExternalFilesTree::filesystemTree()
{
    QTemporaryDir directory;
    QVERIFY( directory.isValid() );
    const QString workspace = directory.filePath( "workspace" );
    QVERIFY( QDir().mkpath( workspace ) );
    const QString outside = directory.filePath( "outside.txt" );
    QFile file( outside );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( "ordinary text" );
    file.close();
    QFileSystemModel source;
    QSortFilterProxyModel filter;
    filter.setSourceModel( &source );
    const auto root = source.setRootPath( workspace );
    mrst::ExternalFilesProxy model;
    model.setSourceModel( &filter );
    model.setWorkspace( workspace, filter.mapFromSource( root ) );
    QTreeView tree;
    tree.setModel( &model );
    tree.setRootIndex( model.mapFromSource( filter.mapFromSource( root ) ) );
    QVERIFY( model.addFile( outside ) );
    tree.expand( model.externalRoot() );
    tree.resize( 500, 300 );
    tree.show();
    QTRY_VERIFY( tree.visualRect( model.indexForFile( outside ) ).isValid() );
    QTest::mouseClick( tree.viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree.visualRect( model.indexForFile( outside ) ).center() );
    QCOMPARE( model.filePath( tree.currentIndex() ), QDir::cleanPath( outside ) );
    QFile inside( workspace + "/new.rst" );
    QVERIFY( inside.open( QIODevice::WriteOnly ) );
    inside.close();
    QTRY_COMPARE( source.rowCount( root ), 1 );
    QTRY_COMPARE( model.externalRoot().row(), 1 );
    QCOMPARE( model.filePath( tree.currentIndex() ), QDir::cleanPath( outside ) );
}

MRST_REGISTER_TEST( TestExternalFilesTree );
#include "tst_ExternalFilesTree.moc"
