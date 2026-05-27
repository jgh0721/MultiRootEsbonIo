#include "stdafx.h"
#include "dlgSettings.hpp"
#include "core/solThemeManager.hpp"

QSettingsDialog::QSettingsDialog( QWidget* Parent )
    : QDialog( Parent )
{
    Ui.setupUi( this );

    setupUi();
}

QList<ShortcutItem> QSettingsDialog::DefaultShortcuts()
{
    return
    {
        { tr("공통"),      QStringLiteral("file.new"),        tr("새 파일"),            QKeySequence::New,                              QKeySequence::New },
        { tr("공통"),      QStringLiteral("file.open"),       tr("열기"),               QKeySequence(Qt::CTRL | Qt::Key_O),             QKeySequence(Qt::CTRL | Qt::Key_O) },
        { tr("공통"),      QStringLiteral("file.save"),       tr("저장"),               QKeySequence(Qt::CTRL | Qt::Key_S),             QKeySequence(Qt::CTRL | Qt::Key_S) },
        { tr("공통"),      QStringLiteral("file.saveAs"),     tr("다른 이름으로 저장"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S) },
        { tr("공통"),      QStringLiteral("file.print"),      tr("인쇄"),               QKeySequence(Qt::CTRL | Qt::Key_P),             QKeySequence(Qt::CTRL | Qt::Key_P) },
        { tr("공통"),      QStringLiteral("capture.screen"),  tr("화면 캡쳐"),          QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_S),  QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_S) },
        { tr("공통"),      QStringLiteral("tab.close"),       tr("현재 탭 닫기"),       QKeySequence(Qt::CTRL | Qt::Key_W),             QKeySequence(Qt::CTRL | Qt::Key_W) },
        { tr("공통"),      QStringLiteral("app.settings"),    tr("설정"),               QKeySequence(Qt::CTRL | Qt::Key_I),             QKeySequence(Qt::CTRL | Qt::Key_I) },
        { tr("텍스트"),    QStringLiteral("text.find"),       tr("찾기"),               QKeySequence(Qt::CTRL | Qt::Key_F),             QKeySequence(Qt::CTRL | Qt::Key_F) },
        { tr("텍스트"),    QStringLiteral("text.replace"),    tr("바꾸기"),             QKeySequence(Qt::CTRL | Qt::Key_H),             QKeySequence(Qt::CTRL | Qt::Key_H) },
        { tr("텍스트"),    QStringLiteral("text.findNext"),   tr("다음 찾기"),          QKeySequence(Qt::Key_F3),                       QKeySequence(Qt::Key_F3) },
        { tr("텍스트"),    QStringLiteral("text.findPrev"),   tr("이전 찾기"),          QKeySequence(Qt::SHIFT | Qt::Key_F3),           QKeySequence(Qt::SHIFT | Qt::Key_F3) },
        { tr("텍스트"),    QStringLiteral("text.goToLine"),   tr("줄 이동"),            QKeySequence(Qt::CTRL | Qt::Key_G),             QKeySequence(Qt::CTRL | Qt::Key_G) },
        { tr("텍스트"),    QStringLiteral("text.wordWrap"),   tr("자동 줄넘김 전환"),   QKeySequence(Qt::ALT | Qt::Key_Z),              QKeySequence(Qt::ALT | Qt::Key_Z) },
    };
}

void QSettingsDialog::on_btnOK_clicked( bool Checked )
{
}

void QSettingsDialog::on_btnCancel_clicked( bool Checked )
{
}

void QSettingsDialog::on_btnApply_clicked( bool Checked )
{
}

void QSettingsDialog::onResetShortcuts()
{
    m_shortcuts = DefaultShortcuts();
    for( int i = 0; i < m_shortcuts.size(); ++i )
    {
        auto* keyEdit = qobject_cast< QKeySequenceEdit * >( m_shortcutTable->cellWidget( i, 2 ) );
        if( keyEdit )
            keyEdit->setKeySequence( m_shortcuts[ i ].DefaultShortcut );
    }
}

void QSettingsDialog::setupUi()
{
    Ui.lstCate->setFixedWidth( 140 );
    Ui.lstCate->addItem( tr("공통") );
    Ui.lstCate->addItem( tr("단축키") );
    Ui.lstCate->addItem( tr("코드 편집기") );
    Ui.lstCate->addItem( tr("Python/Esbonio") );
}

QWidget* QSettingsDialog::createGeneralPage()
{
    auto* page   = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    auto* themeGroup  = new QGroupBox( tr( "테마" ), page );
    auto* themeLayout = new QVBoxLayout( themeGroup );

    auto* formLayout = new QFormLayout;

    m_themeCombo = new QComboBox( page );
    m_themeCombo->addItem( tr( "라이트" ), static_cast< int >( ThemeManager::Light ) );
    m_themeCombo->addItem( tr( "다크" ), static_cast< int >( ThemeManager::Dark ) );
    formLayout->addRow( tr( "테마 모드:" ), m_themeCombo );

    m_themeNameLabel = new QLabel( themeGroup );
    formLayout->addRow( tr( "기본 팔레트:" ), m_themeNameLabel );

    m_themeScopeCombo = new QComboBox( themeGroup );
    m_themeScopeCombo->addItem( tr( "전체" ), QString() );
    for( const auto& entry : ThemeManager::editableColorEntries() )
    {
        if( m_themeScopeCombo->findData( entry.group ) < 0 )
            m_themeScopeCombo->addItem( entry.group, entry.group );
    }
    formLayout->addRow( tr( "편집 범위:" ), m_themeScopeCombo );

    m_themeLexerListLabel = new QLabel( tr( "Lexer 목록:" ), themeGroup );
    m_themeLexerList      = new QListWidget( themeGroup );
    m_themeLexerList->setMinimumHeight( 120 );
    m_themeLexerList->setMaximumHeight( 180 );
    m_themeLexerList->setSelectionMode( QAbstractItemView::SingleSelection );
    formLayout->addRow( m_themeLexerListLabel, m_themeLexerList );

    m_themeInstantPreviewCheck = new QCheckBox( tr( "색상 변경 즉시 앱에 적용" ), themeGroup );
    m_themeInstantPreviewCheck->setChecked( true );
    formLayout->addRow( tr( "미리보기:" ), m_themeInstantPreviewCheck );
    themeLayout->addLayout( formLayout );

    auto* buttonLayout  = new QHBoxLayout;
    m_themeResetButton  = new QPushButton( tr( "기본값 복원" ), themeGroup );
    m_themeImportButton = new QPushButton( tr( "가져오기..." ), themeGroup );
    m_themeExportButton = new QPushButton( tr( "내보내기..." ), themeGroup );
    buttonLayout->addWidget( m_themeResetButton );
    buttonLayout->addStretch( 1 );
    buttonLayout->addWidget( m_themeImportButton );
    buttonLayout->addWidget( m_themeExportButton );
    themeLayout->addLayout( buttonLayout );

    m_themeColorTable = new QTableWidget( themeGroup );
    m_themeColorTable->setColumnCount( 4 );
    m_themeColorTable->setHorizontalHeaderLabels( { tr( "범위" ), tr( "항목" ), tr( "색상" ), tr( "값" ) } );
    m_themeColorTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
    m_themeColorTable->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
    m_themeColorTable->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::ResizeToContents );
    m_themeColorTable->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );
    m_themeColorTable->verticalHeader()->setVisible( false );
    m_themeColorTable->setSelectionBehavior( QAbstractItemView::SelectRows );
    m_themeColorTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
    themeLayout->addWidget( m_themeColorTable, 1 );

    layout->addWidget( themeGroup, 1 );
    //
    // connect( m_themeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
    //     populateThemeColorTable();
    //     if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
    //     applyThemePreview();
    // } );
    // connect( m_themeScopeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
    //     populateThemeColorTable();
    // } );
    // connect( m_themeLexerList, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem*, QListWidgetItem* ) {
    //     if( m_themeScopeCombo&& m_themeScopeCombo->currentData().toString() == tr( "TEXT Lexer 상세" ))
    //     populateThemeColorTable();
    // } );
    // connect( m_themeColorTable, &QTableWidget::cellDoubleClicked, this, [this]( int row, int column ) {
    //     if( column != 2 && column != 3 )
    //         return;
    //     auto* colorItem = m_themeColorTable ? m_themeColorTable->item( row, 2 ) : nullptr;
    //     auto* valueItem = m_themeColorTable ? m_themeColorTable->item( row, 3 ) : nullptr;
    //     if( !colorItem || !valueItem )
    //         return;
    //
    //     const QColor current( colorItem->data( Qt::UserRole + 1 ).toString() );
    //     const QColor chosen = QColorDialog::getColor( current, this, tr( "테마 색상 선택" ), QColorDialog::ShowAlphaChannel );
    //     if( !chosen.isValid() )
    //         return;
    //
    //     updateThemeColorItem( colorItem, chosen );
    //     valueItem->setText( chosen.name( QColor::HexArgb ) );
    //     valueItem->setData( Qt::UserRole + 1, chosen.name( QColor::HexArgb ) );
    //     if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
    //     applyThemePreview();
    // } );
    // connect( m_themeResetButton, &QPushButton::clicked, this, [this] {
    //     const auto theme = static_cast< ThemeManager::Theme >( m_themeCombo->currentData().toInt() );
    //     ThemeManager::instance().resetColorOverrides( theme );
    //     populateThemeColorTable();
    //     if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
    //     applyThemePreview();
    // } );
    // connect( m_themeImportButton, &QPushButton::clicked, this, [this] {
    //     const QString filePath = QFileDialog::getOpenFileName( this, tr( "테마 가져오기" ), QString(),
    //                                                            tr( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
    //     if( filePath.isEmpty() )
    //         return;
    //     QString errorMessage;
    //     if( !ThemeManager::instance().importThemeFile( filePath, &errorMessage ) )
    //     {
    //         QMessageBox::warning( this, tr( "테마 가져오기 실패" ), errorMessage );
    //         return;
    //     }
    //     m_themeCombo->setCurrentIndex( m_themeCombo->
    //                                   findData( static_cast< int >( ThemeManager::instance().currentTheme() ) ) );
    //     populateThemeColorTable();
    //     emit settingsApplied();
    // } );
    // connect( m_themeExportButton, &QPushButton::clicked, this, [this] {
    //     QString filePath = QFileDialog::getSaveFileName( this, tr( "테마 내보내기" ),
    //                                                      QStringLiteral( "MultiViewer-%1.json" ).
    //                                                      arg( ThemeManager::themeName( static_cast<
    //                                                              ThemeManager::Theme >( m_themeCombo->currentData()
    //                                                             .toInt() ) ).replace( QLatin1Char( ' ' ),
    //                                                              QLatin1Char( '-' ) ) ),
    //                                                      tr( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
    //     if( filePath.isEmpty() )
    //         return;
    //     if( !filePath.endsWith( QStringLiteral( ".json" ), Qt::CaseInsensitive ) )
    //         filePath += QStringLiteral( ".json" );
    //     saveSettings();
    //     QString errorMessage;
    //     if( !ThemeManager::instance().exportThemeFile( filePath, &errorMessage ) )
    //         QMessageBox::warning( this, tr( "테마 내보내기 실패" ), errorMessage );
    // } );

    return page;
}

QWidget* QSettingsDialog::createShortcutsPage()
{
    auto* page   = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    m_shortcutTable = new QTableWidget( page );
    m_shortcutTable->setColumnCount( 3 );
    m_shortcutTable->setHorizontalHeaderLabels( { tr( "카테고리" ), tr( "설명" ), tr( "단축키" ) } );
    m_shortcutTable->horizontalHeader()->setStretchLastSection( true );
    m_shortcutTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
    m_shortcutTable->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
    m_shortcutTable->verticalHeader()->setVisible( false );
    m_shortcutTable->setSelectionBehavior( QAbstractItemView::SelectRows );
    m_shortcutTable->setSelectionMode( QAbstractItemView::SingleSelection );
    layout->addWidget( m_shortcutTable );

    auto* resetBtn = new QPushButton( tr( "기본값으로 초기화" ), page );
    connect( resetBtn, &QPushButton::clicked, this, &QSettingsDialog::onResetShortcuts );
    layout->addWidget( resetBtn );

    return page;
}

QWidget* QSettingsDialog::createEditorPage()
{
    return {};
}

QWidget* QSettingsDialog::createEsbonioPage()
{
    return {};
}
