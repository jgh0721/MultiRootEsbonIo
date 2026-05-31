#include "stdafx.h"
#include "dlgSettings.hpp"
#include "core/solPythonEnvMgr.hpp"
#include "core/solThemeManager.hpp"

namespace {

QString nativePath( const QString& path )
{
    return QDir::toNativeSeparators( path );
}

QLabel* createValueLabel( QWidget* parent )
{
    auto* label = new QLabel( parent );
    label->setTextInteractionFlags( Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard );
    label->setWordWrap( true );
    return label;
}

}  // namespace

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
    Q_UNUSED( Checked )
    saveEsbonioSettings();
    accept();
}

void QSettingsDialog::on_btnCancel_clicked( bool Checked )
{
    Q_UNUSED( Checked )
    reject();
}

void QSettingsDialog::on_btnApply_clicked( bool Checked )
{
    Q_UNUSED( Checked )
    saveEsbonioSettings();
    refreshEsbonioStatus();
}

QList<ShortcutItem> QSettingsDialog::LoadShortcutsFromSettings()
{
    return DefaultShortcuts();
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
    Ui.stkWidget->setContentsMargins( 0, 0, 0, 0 );
    while( Ui.stkWidget->count() > 0 )
    {
        QWidget* page = Ui.stkWidget->widget( 0 );
        Ui.stkWidget->removeWidget( page );
        page->deleteLater();
    }

    Ui.lstCate->setFixedWidth( 140 );
    Ui.lstCate->clear();
    Ui.lstCate->addItem( tr("공통") );
    Ui.lstCate->addItem( tr("단축키") );
    Ui.lstCate->addItem( tr("코드 편집기") );
    Ui.lstCate->addItem( tr("Python/Esbonio") );

    Ui.stkWidget->addWidget( createGeneralPage() );
    Ui.stkWidget->addWidget( createShortcutsPage() );
    Ui.stkWidget->addWidget( createEditorPage() );
    Ui.stkWidget->addWidget( createEsbonioPage() );

    Ui.btnApply->setText( tr( "적용(&A)" ) );

    connect( Ui.lstCate, &QListWidget::currentRowChanged, Ui.stkWidget, &QStackedWidget::setCurrentIndex );
    Ui.lstCate->setCurrentRow( 0 );
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
    auto* page = new QWidget( this );
    auto* layout = new QVBoxLayout( page );
    auto* label = new QLabel( tr( "코드 편집기 설정은 추후 추가될 예정입니다." ), page );
    label->setAlignment( Qt::AlignCenter );
    layout->addWidget( label, 1 );
    return page;
}

QWidget* QSettingsDialog::createEsbonioPage()
{
    if( m_pythonEnvManager == nullptr )
        m_pythonEnvManager = new mrst::PythonEnvManager( this );

    auto* page = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    auto* uvGroup = new QGroupBox( tr( "UV 환경 구성" ), page );
    auto* uvLayout = new QVBoxLayout( uvGroup );
    auto* formLayout = new QFormLayout;

    m_useExternalUvCheck = new QCheckBox( tr( "외부 UV를 사용하여 환경 구성" ), uvGroup );
    formLayout->addRow( QString(), m_useExternalUvCheck );

    auto* uvPathWidget = new QWidget( uvGroup );
    auto* uvPathLayout = new QHBoxLayout( uvPathWidget );
    uvPathLayout->setContentsMargins( 0, 0, 0, 0 );
    m_uvPathEdit = new QLineEdit( uvPathWidget );
    m_uvPathEdit->setPlaceholderText( tr( "uv.exe 경로" ) );
    m_uvBrowseButton = new QPushButton( tr( "찾아보기..." ), uvPathWidget );
    uvPathLayout->addWidget( m_uvPathEdit, 1 );
    uvPathLayout->addWidget( m_uvBrowseButton );
    formLayout->addRow( tr( "UV 경로:" ), uvPathWidget );

    m_detectedUvLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "인식된 UV:" ), m_detectedUvLabel );

    m_environmentRootLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "환경 디렉터리:" ), m_environmentRootLabel );

    m_configuredDateLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "구성일:" ), m_configuredDateLabel );

    m_pythonExeLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "Python:" ), m_pythonExeLabel );

    m_sphinxBuildExeLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "Sphinx:" ), m_sphinxBuildExeLabel );

    m_esbonioExeLabel = createValueLabel( uvGroup );
    formLayout->addRow( tr( "Esbonio:" ), m_esbonioExeLabel );

    uvLayout->addLayout( formLayout );

    auto* actionLayout = new QHBoxLayout;
    m_configurePythonButton = new QPushButton( tr( "구성" ), uvGroup );
    actionLayout->addStretch( 1 );
    actionLayout->addWidget( m_configurePythonButton );
    uvLayout->addLayout( actionLayout );

    layout->addWidget( uvGroup );

    auto* logGroup = new QGroupBox( tr( "구성 로그" ), page );
    auto* logLayout = new QVBoxLayout( logGroup );
    m_pythonEnvLog = new QTextEdit( logGroup );
    m_pythonEnvLog->setReadOnly( true );
    m_pythonEnvLog->setMinimumHeight( 160 );
    logLayout->addWidget( m_pythonEnvLog );
    layout->addWidget( logGroup, 1 );

    loadEsbonioSettings();
    refreshEsbonioStatus();

    connect( m_useExternalUvCheck, &QCheckBox::toggled, this, [this]( const bool checked ) {
        m_uvPathEdit->setEnabled( checked );
        m_uvBrowseButton->setEnabled( checked );
        m_pythonEnvManager->setUseExternalUv( checked );
        saveEsbonioSettings();
        refreshEsbonioStatus();
    } );

    connect( m_uvPathEdit, &QLineEdit::editingFinished, this, [this] {
        saveEsbonioSettings();
        refreshEsbonioStatus();
    } );

    connect( m_uvBrowseButton, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getOpenFileName( this, tr( "UV 실행 파일 선택" ), m_uvPathEdit->text(),
                                                               tr( "UV 실행 파일 (uv.exe);;실행 파일 (*.exe);;모든 파일 (*.*)" ) );
        if( selected.isEmpty() )
            return;
        m_uvPathEdit->setText( nativePath( selected ) );
        saveEsbonioSettings();
        refreshEsbonioStatus();
    } );

    connect( m_pythonEnvManager, &mrst::PythonEnvManager::bootstrapLog, this, [this]( const QString& text ) {
        if( m_pythonEnvLog != nullptr && !text.isEmpty() )
            m_pythonEnvLog->append( text );
    } );

    connect( m_configurePythonButton, &QPushButton::clicked, this, [this] {
        saveEsbonioSettings();
        if( m_pythonEnvLog != nullptr )
            m_pythonEnvLog->clear();
        m_configurePythonButton->setEnabled( false );
        const bool ok = m_pythonEnvManager->configureEnvironment( this );
        m_configurePythonButton->setEnabled( true );
        refreshEsbonioStatus();
        if( ok )
            QMessageBox::information( this, tr( "Python/Esbonio 환경 구성" ), tr( "환경 구성이 완료되었습니다." ) );
    } );

    return page;
}

void QSettingsDialog::loadEsbonioSettings()
{
    if( m_pythonEnvManager == nullptr )
        return;

    const bool useExternal = m_pythonEnvManager->useExternalUv();
    if( m_useExternalUvCheck != nullptr )
        m_useExternalUvCheck->setChecked( useExternal );
    if( m_uvPathEdit != nullptr )
    {
        m_uvPathEdit->setText( nativePath( m_pythonEnvManager->externalUvPath() ) );
        m_uvPathEdit->setEnabled( useExternal );
    }
    if( m_uvBrowseButton != nullptr )
        m_uvBrowseButton->setEnabled( useExternal );
}

void QSettingsDialog::saveEsbonioSettings()
{
    if( m_pythonEnvManager == nullptr )
        return;

    if( m_useExternalUvCheck != nullptr )
        m_pythonEnvManager->setUseExternalUv( m_useExternalUvCheck->isChecked() );
    if( m_uvPathEdit != nullptr )
        m_pythonEnvManager->setExternalUvPath( m_uvPathEdit->text() );
    m_pythonEnvManager->saveUvSettings();
}

void QSettingsDialog::refreshEsbonioStatus()
{
    if( m_pythonEnvManager == nullptr )
        return;

    const bool ready = m_pythonEnvManager->isReady();
    if( m_detectedUvLabel != nullptr )
        m_detectedUvLabel->setText( m_pythonEnvManager->uvDescription() );
    if( m_environmentRootLabel != nullptr )
        m_environmentRootLabel->setText( nativePath( m_pythonEnvManager->runtimeRoot() ) );
    if( m_configuredDateLabel != nullptr )
        m_configuredDateLabel->setText( ready ? m_pythonEnvManager->configuredDateText() : tr( "구성되지 않음" ) );
    if( m_pythonExeLabel != nullptr )
        m_pythonExeLabel->setText( nativePath( m_pythonEnvManager->pythonExe() ) );
    if( m_sphinxBuildExeLabel != nullptr )
        m_sphinxBuildExeLabel->setText( nativePath( m_pythonEnvManager->sphinxBuildExe() ) );
    if( m_esbonioExeLabel != nullptr )
        m_esbonioExeLabel->setText( nativePath( m_pythonEnvManager->esbonioExe() ) );
    if( m_configurePythonButton != nullptr )
        m_configurePythonButton->setText( ready ? tr( "재구성" ) : tr( "구성" ) );
}
