#pragma once

#include <QtGui>
#include <QtWidgets>
#include <QWebEngineView>

#include "ui_mainWindow.h"

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow( QWidget* parent = nullptr );
    ~MainWindow() override;

public slots:
    void                                onSettings();

private:
    void                                createMenus();

    QFileSystemModel*                   treLeftFolderTreeModel_ = nullptr;

    Ui::MainWindow                      Ui;
};
