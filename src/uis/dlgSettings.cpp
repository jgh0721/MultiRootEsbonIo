#include "stdafx.h"
#include "dlgSettings.hpp"

#include "core/solAppSettings.hpp"
#include "core/solPythonEnvMgr.hpp"
#include "core/solThemeManager.hpp"
#include "core/solShadowBackupStore.hpp"
#include "uniqueLibs/solEncodingDetector.hpp"

#include <ILexer.h>
#include <Lexilla.h>


namespace
{
    constexpr auto kTextViewerShowRulerWidgetKey = "TextViewer/ShowRulerWidget";

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

    class RevertableKeySequenceEdit : public QKeySequenceEdit
    {
    public:
        explicit RevertableKeySequenceEdit( const QKeySequence& ks, QWidget* parent = nullptr )
            : QKeySequenceEdit( ks, parent )
        {
            m_cancelBtn = new QToolButton( this );
            m_cancelBtn->setText( QStringLiteral( "✕" ) );
            m_cancelBtn->setCursor( Qt::ArrowCursor );
            m_cancelBtn->setStyleSheet( QStringLiteral(
                "QToolButton { border: none; padding: 2px; text-align: center; font-weight: bold; } "
                "QToolButton:hover { color: red; }"
            ) );
            m_cancelBtn->hide();

            connect( m_cancelBtn, &QToolButton::clicked, this, [this]() {
                this->setKeySequence( m_storedSequence );
                this->clearFocus();
            } );
        }

    protected:
        void focusInEvent( QFocusEvent* e ) override
        {
            m_storedSequence = this->keySequence();
            QKeySequenceEdit::focusInEvent( e );
            updateButtonGeometry();
            m_cancelBtn->show();
        }

        void focusOutEvent( QFocusEvent* e ) override
        {
            QKeySequenceEdit::focusOutEvent( e );
            // Delay hiding because clicking the button might trigger focusOut first
            QTimer::singleShot( 100, this, [this]() {
                if( !this->hasFocus() && !m_cancelBtn->underMouse() )
                {
                    m_cancelBtn->hide();
                }
            } );
        }

        void resizeEvent( QResizeEvent* e ) override
        {
            QKeySequenceEdit::resizeEvent( e );
            if( m_cancelBtn->isVisible() )
            {
                updateButtonGeometry();
            }
        }

    private:
        void updateButtonGeometry()
        {
            const int btnSize = height() - 4;
            m_cancelBtn->setFixedSize( btnSize, btnSize );
            m_cancelBtn->move( width() - btnSize - 2, 2 );
        }

        QToolButton* m_cancelBtn = nullptr;
        QKeySequence m_storedSequence;
    };

    QStringList availableLexillaLexerKeys()
    {
        QStringList lexers;
    #if defined(MV_DIRECT_SCINTILLA_HAS_LEXILLA_LEXERS)
        const int lexerCount = GetLexerCount();
        for (int i = 0; i < lexerCount; ++i) {
            char name[128] = {};
            GetLexerName(static_cast<unsigned int>(i), name, static_cast<int>(sizeof(name)));
            const QString lexer = QString::fromLatin1(name).trimmed();
            if (!lexer.isEmpty())
                lexers.append(lexer);
        }
    #endif
        if (lexers.isEmpty()) {
            lexers = {QStringLiteral("cpp"), QStringLiteral("python"), QStringLiteral("json"),
                      QStringLiteral("xml"), QStringLiteral("hypertext"), QStringLiteral("css"),
                      QStringLiteral("bash"), QStringLiteral("sql")};
        }
        lexers.removeDuplicates();
        lexers.sort(Qt::CaseInsensitive);
        return lexers;
    }

    QList<ThemeManager::ColorEntry> lexerDetailEntries(const QString& lexerKey)
    {
        const QString lexer = lexerKey.trimmed().isEmpty() ? QStringLiteral("cpp") : lexerKey.trimmed();
        const QString group = QSettingsDialog::tr("TEXT Lexer 상세");
        return {
            {QStringLiteral("text.lexer.%1.comment").arg(lexer), QSettingsDialog::tr("%1 주석").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.number").arg(lexer), QSettingsDialog::tr("%1 숫자").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.keyword").arg(lexer), QSettingsDialog::tr("%1 키워드").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.type").arg(lexer), QSettingsDialog::tr("%1 타입/보조 키워드").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.string").arg(lexer), QSettingsDialog::tr("%1 문자열").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.preprocessor").arg(lexer), QSettingsDialog::tr("%1 전처리/속성").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.operator").arg(lexer), QSettingsDialog::tr("%1 연산자").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.identifier").arg(lexer), QSettingsDialog::tr("%1 식별자").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.function").arg(lexer), QSettingsDialog::tr("%1 함수/클래스").arg(lexer), group},
            {QStringLiteral("text.lexer.%1.variable").arg(lexer), QSettingsDialog::tr("%1 변수").arg(lexer), group},
        };
    }

}  // namespace

QSettingsDialog::QSettingsDialog( QWidget* Parent )
    : QDialog( Parent )
{
    Ui.setupUi( this );

    setupUi();
}

QList< ShortcutItem > QSettingsDialog::DefaultShortcuts()
{
    return
    {
        { tr( "공통" ), QStringLiteral( "file.new" ), tr( "새 파일" ), QKeySequence::New, QKeySequence::New },
        {
            tr( "공통" ), QStringLiteral( "file.open" ), tr( "열기" ), QKeySequence( Qt::CTRL | Qt::Key_O ),
            QKeySequence( Qt::CTRL | Qt::Key_O )
        },
        {
            tr( "공통" ), QStringLiteral( "file.save" ), tr( "저장" ), QKeySequence( Qt::CTRL | Qt::Key_S ),
            QKeySequence( Qt::CTRL | Qt::Key_S )
        },
        {
            tr( "공통" ), QStringLiteral( "file.saveAs" ), tr( "다른 이름으로 저장" ),
            QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_S ), QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_S )
        },
        {
            tr( "공통" ), QStringLiteral( "file.print" ), tr( "인쇄" ), QKeySequence( Qt::CTRL | Qt::Key_P ),
            QKeySequence( Qt::CTRL | Qt::Key_P )
        },
        {
            tr( "공통" ), QStringLiteral( "capture.screen" ), tr( "화면 캡쳐" ),
            QKeySequence( Qt::ALT | Qt::SHIFT | Qt::Key_S ), QKeySequence( Qt::ALT | Qt::SHIFT | Qt::Key_S )
        },
        {
            tr( "공통" ), QStringLiteral( "tab.close" ), tr( "현재 탭 닫기" ), QKeySequence( Qt::CTRL | Qt::Key_W ),
            QKeySequence( Qt::CTRL | Qt::Key_W )
        },
        {
            tr( "공통" ), QStringLiteral( "app.settings" ), tr( "설정" ), QKeySequence( Qt::CTRL | Qt::Key_I ),
            QKeySequence( Qt::CTRL | Qt::Key_I )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.find" ), tr( "찾기" ), QKeySequence( Qt::CTRL | Qt::Key_F ),
            QKeySequence( Qt::CTRL | Qt::Key_F )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.replace" ), tr( "바꾸기" ), QKeySequence( Qt::CTRL | Qt::Key_H ),
            QKeySequence( Qt::CTRL | Qt::Key_H )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.findNext" ), tr( "다음 찾기" ), QKeySequence( Qt::Key_F3 ),
            QKeySequence( Qt::Key_F3 )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.findPrev" ), tr( "이전 찾기" ), QKeySequence( Qt::SHIFT | Qt::Key_F3 ),
            QKeySequence( Qt::SHIFT | Qt::Key_F3 )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.goToLine" ), tr( "줄 이동" ), QKeySequence( Qt::CTRL | Qt::Key_G ),
            QKeySequence( Qt::CTRL | Qt::Key_G )
        },
        {
            tr( "텍스트" ), QStringLiteral( "text.wordWrap" ), tr( "자동 줄넘김 전환" ), QKeySequence( Qt::ALT | Qt::Key_Z ),
            QKeySequence( Qt::ALT | Qt::Key_Z )
        },
    };
}

void QSettingsDialog::on_btnOK_clicked( bool Checked )
{
    Q_UNUSED( Checked )
    on_btnApply_clicked();
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
    saveShortcuts();
    saveTextViewerSettings();
    saveEsbonioSettings();
    refreshEsbonioStatus();
    emit settingsApplied();
}

QList< ShortcutItem > QSettingsDialog::LoadShortcutsFromSettings()
{
    auto shortcuts = DefaultShortcuts();
    AppSettings settings;
    settings.beginGroup( "shortcuts" );
    for( auto& entry : shortcuts )
    {
        QVariant value = settings.value( entry.Id );
        if( !value.isValid() && entry.Id == QStringLiteral( "file.new" ) )
            value = settings.value( QStringLiteral( "text.new" ) );
        if( value.isValid() )
            entry.Shortcut = QKeySequence( value.toString() );
    }
    settings.endGroup();
    return shortcuts;
}

bool QSettingsDialog::IsTextViewerRulerWidgetVisible()
{
    return QSettings().value( QString::fromLatin1( kTextViewerShowRulerWidgetKey ), true ).toBool();
}

void QSettingsDialog::ApplyShortcutsToActions( const QList<ShortcutItem>& shortcuts, QWidget* topLevel )
{
    if( !topLevel )
        return;

    QMap<QString, QKeySequence> map;
    for( const auto& e : shortcuts )
        map.insert( e.Id, e.Shortcut );

    const auto actions = topLevel->findChildren<QAction*>();
    for( auto* action : actions )
    {
        const QString shortcutId = action->property( "mv.shortcutId" ).toString();
        const QString name = shortcutId.isEmpty() ? action->objectName() : shortcutId;
        if( !name.isEmpty() && map.contains( name ) )
            action->setShortcut( map.value( name ) );
    }
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
    Ui.lstCate->addItem( tr( "공통" ) );
    Ui.lstCate->addItem( tr( "단축키" ) );
    Ui.lstCate->addItem( tr( "텍스트 뷰어" ) );
    Ui.lstCate->addItem( tr( "Python/Esbonio" ) );

    Ui.stkWidget->addWidget( createGeneralPage() );
    Ui.stkWidget->addWidget( createShortcutsPage() );
    Ui.stkWidget->addWidget( createEditorPage() );
    Ui.stkWidget->addWidget( createEsbonioPage() );

    Ui.btnApply->setText( tr( "적용(&A)" ) );

    connect( Ui.lstCate, &QListWidget::currentRowChanged, Ui.stkWidget, &QStackedWidget::setCurrentIndex );
    Ui.lstCate->setCurrentRow( 0 );

    loadShortcuts();
    loadTextViewerSettings();
    loadEsbonioSettings();
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

    connect( m_themeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
        populateThemeColorTable();
        if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
        applyThemePreview();
    } );
    connect( m_themeScopeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
        populateThemeColorTable();
    } );
    connect( m_themeLexerList, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem*, QListWidgetItem* ) {
        if( m_themeScopeCombo&& m_themeScopeCombo->currentData().toString() == tr( "TEXT Lexer 상세" ))
        populateThemeColorTable();
    } );
    connect( m_themeColorTable, &QTableWidget::cellDoubleClicked, this, [this]( int row, int column ) {
        if( column != 2 && column != 3 )
            return;
        auto* colorItem = m_themeColorTable ? m_themeColorTable->item( row, 2 ) : nullptr;
        auto* valueItem = m_themeColorTable ? m_themeColorTable->item( row, 3 ) : nullptr;
        if( !colorItem || !valueItem )
            return;

        const QColor current( colorItem->data( Qt::UserRole + 1 ).toString() );
        const QColor chosen = QColorDialog::getColor( current, this, tr( "테마 색상 선택" ), QColorDialog::ShowAlphaChannel );
        if( !chosen.isValid() )
            return;

        updateThemeColorItem( colorItem, chosen );
        valueItem->setText( chosen.name( QColor::HexArgb ) );
        valueItem->setData( Qt::UserRole + 1, chosen.name( QColor::HexArgb ) );
        if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
        applyThemePreview();
    } );
    connect( m_themeResetButton, &QPushButton::clicked, this, [this] {
        const auto theme = static_cast< ThemeManager::Theme >( m_themeCombo->currentData().toInt() );
        ThemeManager::instance().resetColorOverrides( theme );
        populateThemeColorTable();
        if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
        applyThemePreview();
    } );
    connect( m_themeImportButton, &QPushButton::clicked, this, [this] {
        const QString filePath = QFileDialog::getOpenFileName( this, tr( "테마 가져오기" ), QString(),
                                                               tr( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
        if( filePath.isEmpty() )
            return;
        QString errorMessage;
        if( !ThemeManager::instance().importThemeFile( filePath, &errorMessage ) )
        {
            QMessageBox::warning( this, tr( "테마 가져오기 실패" ), errorMessage );
            return;
        }
        m_themeCombo->setCurrentIndex( m_themeCombo->
                                      findData( static_cast< int >( ThemeManager::instance().currentTheme() ) ) );
        populateThemeColorTable();
        emit settingsApplied();
    } );
    connect( m_themeExportButton, &QPushButton::clicked, this, [this] {
        QString filePath = QFileDialog::getSaveFileName( this, tr( "테마 내보내기" ),
                                                         QStringLiteral( "MultiViewer-%1.json" ).
                                                         arg( ThemeManager::themeName( static_cast<
                                                                 ThemeManager::Theme >( m_themeCombo->currentData()
                                                                .toInt() ) ).replace( QLatin1Char( ' ' ),
                                                                 QLatin1Char( '-' ) ) ),
                                                         tr( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
        if( filePath.isEmpty() )
            return;
        if( !filePath.endsWith( QStringLiteral( ".json" ), Qt::CaseInsensitive ) )
            filePath += QStringLiteral( ".json" );
        //saveSettings();
        QString errorMessage;
        if( !ThemeManager::instance().exportThemeFile( filePath, &errorMessage ) )
            QMessageBox::warning( this, tr( "테마 내보내기 실패" ), errorMessage );
    } );

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
    auto* layout = new QFormLayout( page );

    // 기본 글꼴
    auto* fontRow = new QHBoxLayout;
    m_textFontCombo = new QFontComboBox( page );
    m_textFontSizeSpin = new QSpinBox( page );
    m_textFontSizeSpin->setRange( 6, 72 );
    m_textFontSizeSpin->setSuffix( tr( " pt" ) );
    fontRow->addWidget( m_textFontCombo );
    fontRow->addWidget( m_textFontSizeSpin );
    layout->addRow( tr( "기본 글꼴:" ), fontRow );

    // 글꼴 렌더링
    m_textFontRenderCombo = new QComboBox( page );
    m_textFontRenderCombo->addItem( tr( "기본" ), 0 );
    m_textFontRenderCombo->addItem( tr( "비안티앨리어싱" ), 1 );
    m_textFontRenderCombo->addItem( tr( "그레이스케일" ), 2 );
    m_textFontRenderCombo->addItem( tr( "LCD 최적화 (ClearType)" ), 3 );
    layout->addRow( tr( "글꼴 렌더링:" ), m_textFontRenderCombo );

    // 행간
    m_textLineSpacingSpin = new QDoubleSpinBox( page );
    m_textLineSpacingSpin->setRange( 1.0, 3.0 );
    m_textLineSpacingSpin->setSingleStep( 0.1 );
    m_textLineSpacingSpin->setDecimals( 1 );
    m_textLineSpacingSpin->setSuffix( tr( " x" ) );
    m_textLineSpacingSpin->setToolTip( tr( "글꼴의 기본 행 높이를 기준으로 한 배율입니다. 1.0은 글꼴 기본 행간입니다." ) );
    layout->addRow( tr( "행간 배율:" ), m_textLineSpacingSpin );

    // 눈금자 글꼴
    auto* rulerFontRow = new QHBoxLayout;
    m_textRulerFontCombo = new QFontComboBox( page );
    m_textRulerFontSizeSpin = new QSpinBox( page );
    m_textRulerFontSizeSpin->setRange( 6, 36 );
    m_textRulerFontSizeSpin->setSuffix( tr( " pt" ) );
    rulerFontRow->addWidget( m_textRulerFontCombo );
    rulerFontRow->addWidget( m_textRulerFontSizeSpin );
    layout->addRow( tr( "눈금자 글꼴:" ), rulerFontRow );

    // 탭 간격
    m_textTabWidthSpin = new QSpinBox( page );
    m_textTabWidthSpin->setRange( 1, 16 );
    layout->addRow( tr( "탭 간격:" ), m_textTabWidthSpin );

    // 탭 사용
    m_textUseTabsCheck = new QCheckBox( tr( "탭 문자 사용" ), page );
    layout->addRow( tr( "들여쓰기:" ), m_textUseTabsCheck );

    // Indent Guide
    auto* indentGuideRow = new QHBoxLayout;
    m_textIndentGuidesCheck = new QCheckBox( tr( "표시" ), page );
    m_textIndentGuideStyleCombo = new QComboBox( page );
    m_textIndentGuideStyleCombo->addItem( tr( "실제 들여쓰기" ), 1 );
    m_textIndentGuideStyleCombo->addItem( tr( "다음 들여쓰기까지" ), 2 );
    m_textIndentGuideStyleCombo->addItem( tr( "양방향 들여쓰기" ), 3 );
    indentGuideRow->addWidget( m_textIndentGuidesCheck );
    indentGuideRow->addWidget( m_textIndentGuideStyleCombo, 1 );
    layout->addRow( tr( "Indent Guide:" ), indentGuideRow );

    // 제어문자 표시
    m_textWhitespaceCheck = new QCheckBox( tr( "표시" ), page );
    layout->addRow( tr( "제어문자 표시:" ), m_textWhitespaceCheck );

    // 수정 내역 표시
    m_textChangeHistoryCombo = new QComboBox( page );
    m_textChangeHistoryCombo->addItem( tr( "끄기" ), 0 );
    m_textChangeHistoryCombo->addItem( tr( "마커" ), 1 );
    m_textChangeHistoryCombo->addItem( tr( "인디케이터" ), 2 );
    m_textChangeHistoryCombo->addItem( tr( "마커 + 인디케이터" ), 3 );
    layout->addRow( tr( "수정 내역 표시:" ), m_textChangeHistoryCombo );

    // 코드 폴딩
    m_textCodeFoldingCheck = new QCheckBox( tr( "사용" ), page );
    layout->addRow( tr( "코드 폴딩:" ), m_textCodeFoldingCheck );

    // 괄호 강조
    m_textBraceHighlightCheck = new QCheckBox( tr( "사용" ), page );
    layout->addRow( tr( "괄호 강조:" ), m_textBraceHighlightCheck );

    // 저장 대화상자 기본 인코딩
    m_textSaveEncodingCombo = new QComboBox( page );
    m_textSaveEncodingCombo->addItems( EncodingDetector::availableEncodings() );
    layout->addRow( tr( "저장 기본 인코딩:" ), m_textSaveEncodingCombo );

    m_textSaveBomCombo = new QComboBox( page );
    m_textSaveBomCombo->addItem( tr( "자동" ), 0 );
    m_textSaveBomCombo->addItem( tr( "BOM 포함" ), 1 );
    m_textSaveBomCombo->addItem( tr( "BOM 없음" ), 2 );
    layout->addRow( tr( "저장 기본 BOM:" ), m_textSaveBomCombo );

    // 핫 엑시트
    m_textHotExitCheck = new QCheckBox( tr( "사용" ), page );
    m_textHotExitCheck->setToolTip( tr( "저장하지 않은 텍스트 변경사항을 백그라운드 백업으로 보존합니다.\n끄면 기존 핫 엑시트 백업이 즉시 삭제됩니다." ) );
    layout->addRow( tr( "핫 엑시트:" ), m_textHotExitCheck );

    // 대용량 파일 기준
    m_textLargeFileMBSpin = new QSpinBox( page );
    m_textLargeFileMBSpin->setRange( 1, 100 );
    m_textLargeFileMBSpin->setSuffix( tr( " MB" ) );
    layout->addRow( tr( "대용량 파일 기준:" ), m_textLargeFileMBSpin );

    return page;
}

QWidget* QSettingsDialog::createEsbonioPage()
{
    if( m_pythonEnvManager == nullptr )
        m_pythonEnvManager = new mrst::PythonEnvManager( this );

    auto* page   = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    auto* uvGroup    = new QGroupBox( tr( "UV 환경 구성" ), page );
    auto* uvLayout   = new QVBoxLayout( uvGroup );
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

    auto* actionLayout      = new QHBoxLayout;
    m_configurePythonButton = new QPushButton( tr( "구성" ), uvGroup );
    actionLayout->addStretch( 1 );
    actionLayout->addWidget( m_configurePythonButton );
    uvLayout->addLayout( actionLayout );

    layout->addWidget( uvGroup );

    auto* logGroup  = new QGroupBox( tr( "구성 로그" ), page );
    auto* logLayout = new QVBoxLayout( logGroup );
    m_pythonEnvLog  = new QTextEdit( logGroup );
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

void QSettingsDialog::loadShortcuts()
{
    m_shortcuts = LoadShortcutsFromSettings();

    m_shortcutTable->setRowCount( static_cast< int >( m_shortcuts.size() ) );
    for( int i = 0; i < m_shortcuts.size(); ++i )
    {
        const auto& entry = m_shortcuts[ i ];
        auto* catItem = new QTableWidgetItem( entry.Category );
        catItem->setFlags( catItem->flags() & ~Qt::ItemIsEditable );
        m_shortcutTable->setItem( i, 0, catItem );

        auto* descItem = new QTableWidgetItem( entry.Description );
        descItem->setFlags( descItem->flags() & ~Qt::ItemIsEditable );
        m_shortcutTable->setItem( i, 1, descItem );

        auto* keyEdit = new RevertableKeySequenceEdit( entry.Shortcut, m_shortcutTable );
        m_shortcutTable->setCellWidget( i, 2, keyEdit );
    }
}

void QSettingsDialog::saveShortcuts()
{
    AppSettings s;
    s.beginGroup( "shortcuts" );
    s.remove( QStringLiteral( "text.new" ) );
    for( int i = 0; i < m_shortcuts.size(); ++i )
    {
        auto* keyEdit = qobject_cast< QKeySequenceEdit* >( m_shortcutTable->cellWidget( i, 2 ) );
        if( keyEdit )
        {
            m_shortcuts[ i ].Shortcut = keyEdit->keySequence();
            s.setValue( m_shortcuts[ i ].Id, m_shortcuts[ i ].Shortcut.toString() );
        }
    }
    s.endGroup();
}

void QSettingsDialog::loadTextViewerSettings()
{
    AppSettings s;

    // 텍스트
    m_textFontCombo->setCurrentFont( QFont( s.value( "textView/fontFamily", "Consolas" ).toString() ) );
    m_textFontSizeSpin->setValue( s.value( "textView/fontSize", 10 ).toInt() );
    m_textFontRenderCombo->setCurrentIndex(
        m_textFontRenderCombo->findData( s.value( "textView/fontRendering", 2 ).toInt() ) );
    m_textLineSpacingSpin->setValue( qBound( 1.0, s.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 ) );
    m_textRulerFontCombo->setCurrentFont( QFont( s.value( "textView/rulerFontFamily", "Consolas" ).toString() ) );
    m_textRulerFontSizeSpin->setValue( s.value( "textView/rulerFontSize", 8 ).toInt() );
    m_textTabWidthSpin->setValue( s.value( "textView/tabWidth", 4 ).toInt() );
    m_textUseTabsCheck->setChecked( s.value( "textView/useTabs", true ).toBool() );
    m_textIndentGuidesCheck->setChecked( s.value( "textView/showIndentationGuides", true ).toBool() );
    m_textIndentGuideStyleCombo->setCurrentIndex(
        m_textIndentGuideStyleCombo->findData( s.value( "textView/indentGuideStyle", 1 ).toInt() ) );
    m_textWhitespaceCheck->setChecked( s.value( "textView/showWhitespace", false ).toBool() );
    m_textChangeHistoryCombo->setCurrentIndex(
        m_textChangeHistoryCombo->findData( s.value( "textView/changeHistoryMode", 3 ).toInt() ) );
    m_textCodeFoldingCheck->setChecked( s.value( "textView/showCodeFolding", true ).toBool() );
    m_textBraceHighlightCheck->setChecked( s.value( "textView/braceHighlight", true ).toBool() );
    m_textSaveEncodingCombo->setCurrentText( s.value( "textView/saveEncoding", QStringLiteral( "UTF-8" ) ).toString() );
    m_textSaveBomCombo->setCurrentIndex(
        m_textSaveBomCombo->findData( s.value( "textView/saveBomMode", 1 ).toInt() ) );
    m_textHotExitCheck->setChecked( s.value( "textView/hotExitEnabled", true ).toBool() );
    m_textLargeFileMBSpin->setValue( s.value( "textView/largeFileMB", 1 ).toInt() );
}

void QSettingsDialog::saveTextViewerSettings()
{
    AppSettings s;


    // 텍스트
    const bool wasHotExitEnabled = s.value( "textView/hotExitEnabled", true ).toBool();
    const bool hotExitEnabled = m_textHotExitCheck->isChecked();
    s.setValue( "textView/fontFamily", m_textFontCombo->currentFont().family() );
    s.setValue( "textView/fontSize", m_textFontSizeSpin->value() );
    s.setValue( "textView/fontRendering", m_textFontRenderCombo->currentData().toInt() );
    s.setValue( "textView/lineSpacing", m_textLineSpacingSpin->value() );
    s.setValue( "textView/rulerFontFamily", m_textRulerFontCombo->currentFont().family() );
    s.setValue( "textView/rulerFontSize", m_textRulerFontSizeSpin->value() );
    s.setValue( "textView/tabWidth", m_textTabWidthSpin->value() );
    s.setValue( "textView/useTabs", m_textUseTabsCheck->isChecked() );
    s.setValue( "textView/showIndentationGuides", m_textIndentGuidesCheck->isChecked() );
    s.setValue( "textView/indentGuideStyle", m_textIndentGuideStyleCombo->currentData().toInt() );
    s.setValue( "textView/showWhitespace", m_textWhitespaceCheck->isChecked() );
    s.setValue( "textView/changeHistoryMode", m_textChangeHistoryCombo->currentData().toInt() );
    s.setValue( "textView/showCodeFolding", m_textCodeFoldingCheck->isChecked() );
    s.setValue( "textView/braceHighlight", m_textBraceHighlightCheck->isChecked() );
    s.setValue( "textView/saveEncoding", m_textSaveEncodingCombo->currentText().trimmed().isEmpty()
        ? QStringLiteral( "UTF-8" )
        : m_textSaveEncodingCombo->currentText().trimmed() );
    s.setValue( "textView/saveBomMode", m_textSaveBomCombo->currentData().toInt() );
    s.setValue( "textView/hotExitEnabled", hotExitEnabled );
    s.setValue( "textView/largeFileMB", m_textLargeFileMBSpin->value() );

    if( wasHotExitEnabled && !hotExitEnabled )
        TextShadowBackupStore::deleteAllBackups();
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

void QSettingsDialog::populateThemeColorTable()
{
    if( !m_themeColorTable || !m_themeCombo )
        return;

    populateThemeLexerCombo();

    const auto theme = static_cast< ThemeManager::Theme >( m_themeCombo->currentData().toInt() );
    if( m_themeNameLabel )
        m_themeNameLabel->setText( ThemeManager::themeName( theme ) );

    const QString detailScope   = tr( "TEXT Lexer 상세" );
    const QString currentScope  = m_themeScopeCombo ? m_themeScopeCombo->currentData().toString() : QString();
    const QString selectedLexer = ( m_themeLexerList && m_themeLexerList->currentItem() )
                                      ? m_themeLexerList->currentItem()->data( Qt::UserRole ).toString()
                                      : QStringLiteral( "cpp" );

    auto entries = ThemeManager::editableColorEntries();
    for( int i = entries.size() - 1; i >= 0; --i )
    {
        if( entries.at( i ).group == detailScope )
            entries.removeAt( i );
    }
    if( currentScope == detailScope )
        entries.append( lexerDetailEntries( selectedLexer ) );

    const auto           colors    = ThemeManager::instance().effectiveColors( theme );
    const auto           overrides = ThemeManager::instance().colorOverrides( theme );
    const QSignalBlocker blocker( m_themeColorTable );
    for( int row = 0; row < m_themeColorTable->rowCount(); ++row )
    {
        if( auto* widget = m_themeColorTable->cellWidget( row, 2 ) )
        {
            m_themeColorTable->removeCellWidget( row, 2 );
            widget->deleteLater();
        }
    }
    m_themeColorTable->clearContents();
    m_themeColorTable->setRowCount( 0 );
    m_themeColorTable->setRowCount( entries.size() );
    for( int row = 0; row < entries.size(); ++row )
    {
        const auto&       entry    = entries.at( row );
        QColor            color    = colors.value( entry.key, ThemeManager::defaultColors( theme ).value( entry.key ) );
        const QStringList keyParts = entry.key.split( QLatin1Char( '.' ) );
        if( keyParts.size() == 4 && keyParts.at( 0 ) == QStringLiteral( "text" )
            && keyParts.at( 1 ) == QStringLiteral( "lexer" ) && !overrides.contains( entry.key ) )
        {
            color = colors.value( QStringLiteral( "text.lexer.%1" ).arg( keyParts.at( 3 ) ), color );
        }

        auto* groupItem = new QTableWidgetItem( entry.group );
        groupItem->setFlags( groupItem->flags() & ~Qt::ItemIsEditable );
        groupItem->setData( Qt::UserRole, entry.key );
        m_themeColorTable->setItem( row, 0, groupItem );

        auto* labelItem = new QTableWidgetItem( entry.label );
        labelItem->setFlags( labelItem->flags() & ~Qt::ItemIsEditable );
        labelItem->setData( Qt::UserRole, entry.key );
        m_themeColorTable->setItem( row, 1, labelItem );

        auto* colorItem = new QTableWidgetItem( QStringLiteral( "■" ) );
        colorItem->setFlags( colorItem->flags() & ~Qt::ItemIsEditable );
        colorItem->setTextAlignment( Qt::AlignCenter );
        colorItem->setData( Qt::UserRole, entry.key );
        updateThemeColorItem( colorItem, color );
        m_themeColorTable->setItem( row, 2, colorItem );

        auto* valueItem = new QTableWidgetItem( color.name( QColor::HexArgb ) );
        valueItem->setFlags( valueItem->flags() & ~Qt::ItemIsEditable );
        valueItem->setData( Qt::UserRole, entry.key );
        valueItem->setData( Qt::UserRole + 1, color.name( QColor::HexArgb ) );
        m_themeColorTable->setItem( row, 3, valueItem );
    }
    applyThemeScopeFilter();
}

void QSettingsDialog::populateThemeLexerCombo()
{
    if( !m_themeLexerList )
        return;

    const QString current = m_themeLexerList->currentItem()
                                ? m_themeLexerList->currentItem()->data( Qt::UserRole ).toString()
                                : QStringLiteral( "cpp" );
    const QSignalBlocker blocker( m_themeLexerList );
    m_themeLexerList->clear();
    const QStringList lexers      = availableLexillaLexerKeys();
    int               selectedRow = -1;
    for( const QString& lexer : lexers )
    {
        auto* item = new QListWidgetItem( lexer, m_themeLexerList );
        item->setData( Qt::UserRole, lexer );
        if( lexer.compare( current, Qt::CaseInsensitive ) == 0 )
            selectedRow = m_themeLexerList->count() - 1;
    }
    if( selectedRow < 0 )
        selectedRow = qMax( 0, m_themeLexerList->findItems( QStringLiteral( "cpp" ), Qt::MatchExactly ).isEmpty()
                                   ? 0
                                   : m_themeLexerList->row( m_themeLexerList->
                                                           findItems( QStringLiteral( "cpp" ), Qt::MatchExactly ).
                                                           first() ) );
    if( m_themeLexerList->count() > 0 )
        m_themeLexerList->setCurrentRow( selectedRow );
}

void QSettingsDialog::applyThemeScopeFilter()
{
    if( !m_themeColorTable || !m_themeScopeCombo )
        return;

    const QString scope            = m_themeScopeCombo->currentData().toString();
    const bool    lexerDetailScope = scope == tr( "TEXT Lexer 상세" );
    if( m_themeLexerListLabel )
        m_themeLexerListLabel->setVisible( lexerDetailScope );
    if( m_themeLexerList )
        m_themeLexerList->setVisible( lexerDetailScope );

    for( int row = 0; row < m_themeColorTable->rowCount(); ++row )
    {
        const auto* groupItem = m_themeColorTable->item( row, 0 );
        const bool  visible   = scope.isEmpty() || ( groupItem && groupItem->text() == scope );
        m_themeColorTable->setRowHidden( row, !visible );
        for( int column = 0; column < m_themeColorTable->columnCount(); ++column )
        {
            if( auto* widget = m_themeColorTable->cellWidget( row, column ) )
                widget->setVisible( visible );
        }
    }
}

void QSettingsDialog::applyThemePreview()
{
    if (!m_themeCombo)
        return;

    const auto theme = static_cast<ThemeManager::Theme>(m_themeCombo->currentData().toInt());
    AppSettings settings;
    settings.setValue("theme", static_cast<int>(theme));
    ThemeManager::instance().setTheme(theme);
    ThemeManager::instance().setColorOverrides(collectThemeColors());
    emit settingsApplied();
}

QHash<QString, QColor> QSettingsDialog::collectThemeColors() const
{
    if (!m_themeColorTable || !m_themeCombo)
        return {};

    const auto theme = static_cast<ThemeManager::Theme>(m_themeCombo->currentData().toInt());
    QHash<QString, QColor> colors = ThemeManager::instance().colorOverrides(theme);
    const auto defaults = ThemeManager::defaultColors(theme);
    const auto effective = ThemeManager::instance().effectiveColors(theme);

    for (int row = 0; row < m_themeColorTable->rowCount(); ++row) {
        const auto* keyItem = m_themeColorTable->item(row, 0);
        const auto* colorItem = m_themeColorTable->item(row, 2);
        if (!keyItem || !colorItem)
            continue;
        const QString key = keyItem->data(Qt::UserRole).toString();
        const QColor color(colorItem->data(Qt::UserRole + 1).toString());
        QColor defaultColor = defaults.value(key);
        const QStringList keyParts = key.split(QLatin1Char('.'));
        if (keyParts.size() == 4 && keyParts.at(0) == QStringLiteral("text")
            && keyParts.at(1) == QStringLiteral("lexer")) {
            defaultColor = effective.value(QStringLiteral("text.lexer.%1").arg(keyParts.at(3)), defaultColor);
            }
        if (!key.isEmpty() && color.isValid()
            && color.name(QColor::HexArgb).compare(defaultColor.name(QColor::HexArgb), Qt::CaseInsensitive) != 0) {
            colors.insert(key, color);
            } else {
                colors.remove(key);
            }
    }
    return colors;
}

void QSettingsDialog::updateThemeColorButton(QPushButton* button, const QColor& color) const
{
    if (!button)
        return;

    button->setText(QStringLiteral("■"));
    button->setProperty("themeColor", color.name(QColor::HexArgb));
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid palette(mid); min-width: 42px; }"
    ).arg(color.name(QColor::HexArgb), color.lightnessF() < 0.45 ? QStringLiteral("#ffffff") : QStringLiteral("#111111")));
    button->setToolTip(color.name(QColor::HexArgb));
}

void QSettingsDialog::updateThemeColorItem(QTableWidgetItem* item, const QColor& color) const
{
    if (!item)
        return;

    const QString colorName = color.name(QColor::HexArgb);
    item->setText(QStringLiteral("■"));
    item->setToolTip(tr("더블클릭하여 색상 변경: %1").arg(colorName));
    item->setData(Qt::UserRole + 1, colorName);
    item->setBackground(QBrush(color));
    item->setForeground(QBrush(color.lightnessF() < 0.45 ? QColor(QStringLiteral("#ffffff")) : QColor(QStringLiteral("#111111"))));
}

