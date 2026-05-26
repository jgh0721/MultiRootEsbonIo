//
// Created by jgh07 on 26. 5. 26..
//

#include "MainWindow.hpp"

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


    // editorTabs_->setTabsClosable(true);
    // editorTabs_->setMovable(true);

    Ui.webEngineView->setHtml(QStringLiteral("<h1>MultiRoot reST C++ Port</h1><p>C++/Qt 전환 셸이 시작되었습니다.</p>"));
}

MainWindow::~MainWindow() = default;
