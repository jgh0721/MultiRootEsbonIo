#include "TestRunner.hpp"

#include <oclero/qlementine/style/QlementineStyle.hpp>

#include <QAbstractItemView>
#include <QComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QSignalSpy>
#include <QStyledItemDelegate>
#include <QStyleFactory>
#include <QTest>
#include <QTreeView>
#include <QWindow>

class PopupTestDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
};

class PopupInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void menuClickAfterRepolish_data();
    void menuClickAfterRepolish();
    void comboClickAfterRepolish_data();
    void comboClickAfterRepolish();
    void contextMenuExecAndMenuBarClick();
    void comboViewReplacementKeepsCustomDelegate();

private:
    QString previousStyle_;
    QString previousStyleSheet_;
    QFont previousFont_;
    QPalette previousPalette_;
};

void PopupInteractionTest::initTestCase()
{
    previousStyle_ = qApp->style()->objectName();
    previousStyleSheet_ = qApp->styleSheet();
    previousFont_ = qApp->font();
    previousPalette_ = qApp->palette();
    qApp->setStyleSheet( {} );
    qApp->setStyle( new oclero::qlementine::QlementineStyle );
}

void PopupInteractionTest::cleanupTestCase()
{
    auto* previous = QStyleFactory::create( previousStyle_ );
    qApp->setStyle( previous ? previous : QStyleFactory::create( "Fusion" ) );
    qApp->setFont( previousFont_ );
    qApp->setPalette( previousPalette_ );
    qApp->setStyleSheet( previousStyleSheet_ );
}

void PopupInteractionTest::menuClickAfterRepolish_data()
{
    QTest::addColumn<int>( "repolishes" );
    QTest::newRow( "initial" ) << 0;
    QTest::newRow( "once" ) << 1;
    QTest::newRow( "repeated" ) << 4;
}

void PopupInteractionTest::menuClickAfterRepolish()
{
    QFETCH( int, repolishes );
    QTreeView tree;
    tree.resize( 400, 300 );
    tree.show();
    QTest::mouseMove( &tree, QPoint( 250, 250 ) );
    QMenu menu( &tree );
    auto* action = menu.addAction( "New file" );
    QSignalSpy triggered( action, &QAction::triggered );
    menu.ensurePolished();
    for( int i = 0; i < repolishes; ++i )
    {
        menu.style()->unpolish( &menu );
        menu.style()->polish( &menu );
    }
    menu.popup( tree.mapToGlobal( QPoint( 30, 30 ) ) );
    QTest::qWait( 300 );
    QTest::mouseMove( &menu, menu.actionGeometry( action ).center() );
    QTest::mouseClick( &menu, Qt::LeftButton, Qt::NoModifier,
                      menu.actionGeometry( action ).center() );
    QTRY_COMPARE_WITH_TIMEOUT( triggered.count(), 1, 1000 );
    QVERIFY( !menu.isVisible() );
}

void PopupInteractionTest::comboClickAfterRepolish_data()
{
    menuClickAfterRepolish_data();
}

void PopupInteractionTest::comboClickAfterRepolish()
{
    QFETCH( int, repolishes );
    QWidget host;
    host.resize( 400, 300 );
    host.move( 100, 100 );
    QComboBox combo( &host );
    combo.addItems( { "100%", "125%", "150%" } );
    combo.resize( 160, 40 );
    host.show();
    combo.ensurePolished();
    for( int i = 0; i < repolishes; ++i )
    {
        combo.style()->unpolish( &combo );
        combo.style()->polish( &combo );
    }
    QSignalSpy activated( &combo, &QComboBox::activated );
    QTest::mouseMove( &combo, combo.rect().center() );
    QTest::mouseClick( &combo, Qt::LeftButton );
    QTest::qWait( 250 );
    auto* view = combo.view();
    QVERIFY( view->isVisible() );
    const auto row = combo.model()->index( 1, 0 );
    QCOMPARE( view->indexAt( view->visualRect( row ).center() ), row );
    QVERIFY( view->viewport()->rect().contains( view->visualRect( row ).center() ) );
    auto* popupWindow = view->window()->windowHandle();
    QVERIFY( popupWindow != nullptr );
    const QPoint point = popupWindow->mapFromGlobal(
        view->viewport()->mapToGlobal( view->visualRect( row ).center() ) );
    // QWindow overload delivers a mouse move even on the offscreen platform;
    // QWidget's overload only moves the OS cursor with no buttons pressed.
    // Move from another point: Qt suppresses a same-position move across
    // successive popups and would keep its opening-click release guard active.
    QTest::mouseMove( popupWindow, QPoint( 1, 1 ) );
    QTest::mouseMove( popupWindow, point );
    QTest::mouseClick( popupWindow, Qt::LeftButton, Qt::NoModifier, point );
    QTRY_COMPARE_WITH_TIMEOUT( combo.currentIndex(), 1, 1000 );
    QCOMPARE( activated.count(), 1 );
    QTRY_VERIFY( !view->isVisible() );
}

void PopupInteractionTest::contextMenuExecAndMenuBarClick()
{
    QTreeView tree;
    tree.setStyleSheet( "QTreeView { color: palette(text); }" );
    tree.resize( 400, 300 );
    tree.show();
    QTest::mouseMove( &tree, QPoint( 250, 250 ) );
    QMenu context( &tree );
    auto* toggle = context.addAction( "Show all files" );
    toggle->setCheckable( true );
    QSignalSpy triggered( toggle, &QAction::triggered );
    QTimer timeout;
    timeout.setSingleShot( true );
    connect( &timeout, &QTimer::timeout, &context, &QMenu::close );
    timeout.start( 2000 );
    QTimer::singleShot( 300, &context, [&] {
        QTest::mouseClick( &context, Qt::LeftButton, Qt::NoModifier,
                          context.actionGeometry( toggle ).center() );
    } );
    QCOMPARE( context.exec( tree.mapToGlobal( QPoint( 30, 30 ) ) ), toggle );
    timeout.stop();
    QCOMPARE( triggered.count(), 1 );
    QVERIFY( toggle->isChecked() );
    QVERIFY( QApplication::activePopupWidget() == nullptr );

    QMenuBar bar;
    auto* menu = bar.addMenu( "File" );
    auto* action = menu->addAction( "New file" );
    QSignalSpy menuTriggered( action, &QAction::triggered );
    bar.resize( 400, 40 );
    bar.show();
    QTest::mouseMove( &bar, bar.actionGeometry( menu->menuAction() ).center() );
    QTest::mouseClick( &bar, Qt::LeftButton, Qt::NoModifier,
                      bar.actionGeometry( menu->menuAction() ).center() );
    QTRY_VERIFY( menu->isVisible() );
    QTest::qWait( 300 );
    QTest::mouseClick( menu, Qt::LeftButton, Qt::NoModifier,
                      menu->actionGeometry( action ).center() );
    QTRY_COMPARE( menuTriggered.count(), 1 );
    QVERIFY( QApplication::activePopupWidget() == nullptr );
}

void PopupInteractionTest::comboViewReplacementKeepsCustomDelegate()
{
    QWidget host;
    host.setStyleSheet( "QComboBox { color: palette(text); }" );
    QComboBox combo( &host );
    combo.addItems( { "One", "Two" } );
    host.show();
    combo.ensurePolished();
    auto* view = new QTreeView;
    combo.setView( view );
    auto* delegate = new PopupTestDelegate( &combo );
    combo.setItemDelegate( delegate );
    QTest::qWait( 50 );
    QCOMPARE( combo.view(), view );
    QCOMPARE( combo.itemDelegate(), delegate );
}

MRST_REGISTER_TEST( PopupInteractionTest );

#include "tst_PopupInteraction.moc"
