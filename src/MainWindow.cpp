#include "stdafx.h"
#include "MainWindow.hpp"

#include "uis/dlgSettings.hpp"

MainWindow::MainWindow( QWidget* parent )
    : QMainWindow( parent )
{
    Ui.setupUi( this );

    treLeftFolderTreeModel_ = new QFileSystemModel( this );
    treLeftFolderTreeModel_->setRootPath("");
    treLeftFolderTreeModel_->setNameFilters( QStringList() << "*.rst" << "*.md" << "*.py" << "*.json" << "*.txt" );
    treLeftFolderTreeModel_->setNameFilterDisables( false );
    Ui.treLeftSideFolterTree->setModel( treLeftFolderTreeModel_ );
    Ui.treLeftSideFolterTree->header()->hideSection(1);
    Ui.treLeftSideFolterTree->header()->hideSection(2);
    Ui.treLeftSideFolterTree->header()->hideSection(3);
    Ui.treLeftSideFolterTree->setIndentation( 15 );

    Ui.tabEditor->InitializeEmpty();
    connect( Ui.treLeftSideFolterTree, &QTreeView::doubleClicked, this, [this]( const QModelIndex& index ) {
        const QFileInfo fileInfo = treLeftFolderTreeModel_->fileInfo( index );
        if( !fileInfo.isFile() )
            return;

        Ui.tabEditor->OpenFile( fileInfo.absoluteFilePath() );
    } );
    connect( Ui.tabEditor, &EditorTabWidget::activeEditorChanged, this, [this]( BaseEdit* editor ) {
        if( editor == nullptr )
        {
            statusBar()->clearMessage();
            return;
        }

        statusBar()->showMessage( tr( "Active editor: %1" ).arg( QDir::toNativeSeparators( editor->FilePath() ) ) );
    } );
    connect( Ui.tabEditor, &EditorTabWidget::fileOpenFailed, this, [this]( const QString& filePath, const QString& reason ) {
        QMessageBox::warning( this, tr( "Open File" ), tr( "Failed to open '%1'.\n%2" ).arg( QDir::toNativeSeparators( filePath ), reason ) );
    } );

    // editorTabs_->setTabsClosable(true);
    // editorTabs_->setMovable(true);

    Ui.webEngineView->setHtml(QStringLiteral("<h1>MultiRoot reST C++ Port</h1><p>C++/Qt 전환 셸이 시작되었습니다.</p>"));

    createMenus();
}

MainWindow::~MainWindow() = default;

void MainWindow::onSettings()
{
    QSettingsDialog dlg(this);
    // connect(&dlg, &QSettingsDialog::settingsApplied, this, [this] {
    //     // 단축키 즉시 적용
    //     const auto shortcuts = QSettingsDialog::loadShortcutsFromSettings();
    //     QSettingsDialog::applyShortcutsToActions(shortcuts, this);
    //     // 열려있는 뷰어에 변경된 설정 적용
    //     applySettingsToAllViews();
    // });
    dlg.exec();
}

void MainWindow::createMenus()
{
    auto* settingsMenu = menuBar()->addMenu(tr("설정(&S)"));
    auto* settingsAction = settingsMenu->addAction(tr("설정(&I)..."), this, &MainWindow::onSettings);
    settingsAction->setObjectName(QStringLiteral("app.settings"));
    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    settingsAction->setShortcutContext(Qt::ApplicationShortcut);

}
