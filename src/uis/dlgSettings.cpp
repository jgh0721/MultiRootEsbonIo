#include "stdafx.h"
#include "dlgSettings.hpp"

#include "core/solAppSettings.hpp"
#include "core/solExternalChangeWatcher.hpp"
#include "core/solPythonEnvMgr.hpp"
#include "core/solUpdateManifest.hpp"
#include "core/solLanguageManager.hpp"
#include "core/solThemeManager.hpp"
#include "core/solShadowBackupStore.hpp"
#include "uniqueLibs/solEncodingDetector.hpp"
#include "utils/DwmTitleBar.hpp"

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
        // 전용 스타일 함수를 쓰는 렉서는 이 목록에서 뺀다. 아래 lexerDetailEntries()
        // 가 만드는 text.lexer.<lexer>.<10개 일반 토큰> 키를 그 렉서에도 만들어 주면
        // 사용자가 색을 바꿀 수는 있지만 applySyntaxStyles() 가 그 키를 읽지 않으므로
        // 아무 일도 일어나지 않는다. 전형적인 "설정했는데 반응이 없다" 버그다.
        // 그 렉서의 색은 자기 범위에 따로 있다 (markdown -> TEXT Lexer Markdown).
        for (const QString& specialised : {QStringLiteral("markdown")})
            lexers.removeAll(specialised);
        lexers.removeDuplicates();
        lexers.sort(Qt::CaseInsensitive);
        return lexers;
    }

    QList<ThemeManager::ColorEntry> lexerDetailEntries(const QString& lexerKey)
    {
        const QString lexer = lexerKey.trimmed().isEmpty() ? QStringLiteral("cpp") : lexerKey.trimmed();
        // 범위는 식별자로 넣고 표시 이름은 ThemeManager 한 곳에서만 만든다.
        // 여기서 tr("TEXT Lexer 상세") 를 따로 부르면 ThemeManager 컨텍스트의
        // 같은 원문과 별개 번역 항목이 되어, 둘이 어긋나는 순간 이 범위를 고른
        // 사용자에게 빈 표가 보인다.
        const QString groupId = QLatin1String(ThemeScopeIds::kTextLexerDetail);
        const QString group   = ThemeManager::scopeLabel(groupId);
        return {
            {QStringLiteral("text.lexer.%1.comment").arg(lexer), QSettingsDialog::tr("%1 주석").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.number").arg(lexer), QSettingsDialog::tr("%1 숫자").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.keyword").arg(lexer), QSettingsDialog::tr("%1 키워드").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.type").arg(lexer), QSettingsDialog::tr("%1 타입/보조 키워드").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.string").arg(lexer), QSettingsDialog::tr("%1 문자열").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.preprocessor").arg(lexer), QSettingsDialog::tr("%1 전처리/속성").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.operator").arg(lexer), QSettingsDialog::tr("%1 연산자").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.identifier").arg(lexer), QSettingsDialog::tr("%1 식별자").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.function").arg(lexer), QSettingsDialog::tr("%1 함수/클래스").arg(lexer), groupId, group},
            {QStringLiteral("text.lexer.%1.variable").arg(lexer), QSettingsDialog::tr("%1 변수").arg(lexer), groupId, group},
        };
    }

}  // namespace

QSettingsDialog::QSettingsDialog( QWidget* Parent )
    : QDialog( Parent )
{
    Ui.setupUi( this );

    // 페이지를 다시 만들어도 **한 번만** 걸려 있어야 하는 연결들. buildPages()
    // 안에 두면 언어를 바꿀 때마다 하나씩 더 붙는다.
    connect( Ui.lstCate, &QListWidget::currentRowChanged, Ui.stkWidget, &QStackedWidget::setCurrentIndex );
    connectPythonEnvSignals();

    buildPages();

    // 여기서 테마를 바꾸면(미리보기 포함) 제목 표시줄도 같이 따라가야 한다.
    connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]( ThemeManager::Theme ) { applyTitleBarTheme(); } );

    // 네이티브 창을 미리 만들어 두면 첫 표시 전에 DWM 속성을 걸 수 있다.
    // (창이 뜬 뒤에 걸면 밝은 제목 표시줄이 한 번 번쩍인다.)
    createWinId();
    applyTitleBarTheme();
}

void QSettingsDialog::showEvent( QShowEvent* Event )
{
    QDialog::showEvent( Event );

    // 창을 닫았다 다시 여는 사이에 테마가 바뀌었을 수 있다.
    applyTitleBarTheme();
}

void QSettingsDialog::applyTitleBarTheme()
{
    auto& themeManager = ThemeManager::instance();
    DwmTitleBar::applyTheme( this,
                            themeManager.currentTheme() == ThemeManager::Dark,
                            themeManager.toolBarColor() );
}

QList< ShortcutItem > QSettingsDialog::DefaultShortcuts()
{
    return
    {
        { tr( "공통" ), QStringLiteral( "file.new" ), tr( "새 파일" ), QKeySequence::New, QKeySequence::New },
        {
            tr( "공통" ), QStringLiteral( "file.openWorkspace" ), tr( "열기" ), QKeySequence( Qt::CTRL | Qt::Key_O ),
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
            tr( "공통" ), QStringLiteral( "tab.next" ), tr( "다음 탭 (탭 목록)" ),
            QKeySequence( Qt::CTRL | Qt::Key_Tab ), QKeySequence( Qt::CTRL | Qt::Key_Tab )
        },
        {
            tr( "공통" ), QStringLiteral( "tab.previous" ), tr( "이전 탭 (탭 목록)" ),
            QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Tab ),
            QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Tab )
        },
        {
            tr( "프리뷰" ), QStringLiteral( "preview.rebuild" ), tr( "프리뷰 다시 빌드" ),
            QKeySequence( Qt::Key_F5 ), QKeySequence( Qt::Key_F5 )
        },
        {
            tr( "프리뷰" ), QStringLiteral( "preview.fullScreen" ), tr( "프리뷰 전체 화면" ),
            QKeySequence( Qt::Key_F11 ), QKeySequence( Qt::Key_F11 )
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
    savePreviewSettings();
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

void QSettingsDialog::changeEvent( QEvent* Event )
{
    if( Event != nullptr && Event->type() == QEvent::LanguageChange )
    {
        // 다시 만들기 전에 [적용] 대기 중인 편집을 커밋한다. 단축키 / 텍스트
        // 뷰어 / 프리뷰 / Esbonio 페이지는 즉시 저장이 아니라서, 그냥 다시
        // 만들면 방금 입력한 값이 조용히 사라진다. 사용자는 이미 즉시 저장인
        // 공통 페이지의 언어 콤보를 건드려 변경을 확정한 참이므로, 나머지도
        // 함께 확정하는 편이 잃는 것이 없다.
        on_btnApply_clicked();

        // .ui 쪽 문자열(창 제목, 확인/취소). btnApply 의 플레이스홀더
        // "PushButton" 도 여기서 되살아나는데, buildPages() 가 다시 덮는다.
        Ui.retranslateUi( this );
        buildPages();
        applyTitleBarTheme();
    }
    QDialog::changeEvent( Event );
}

void QSettingsDialog::buildPages()
{
    // 보고 있던 페이지로 돌아온다. 언어 콤보는 "공통" 에 있으니 지금은 늘 0
    // 이지만, 다른 경로로 언어가 바뀌어도 페이지가 튀지 않는다.
    const int previousRow = qMax( 0, Ui.lstCate->currentRow() );

    Ui.stkWidget->setContentsMargins( 0, 0, 0, 0 );
    while( Ui.stkWidget->count() > 0 )
    {
        QWidget* page = Ui.stkWidget->widget( 0 );
        Ui.stkWidget->removeWidget( page );
        // 지금 지우면 진행 중인 시그널이 발밑을 잃는다. 아래에서 새 페이지가
        // 멤버 포인터를 다시 채우므로 옛 위젯을 가리키는 시간은 없다.
        page->deleteLater();
    }

    Ui.lstCate->setFixedWidth( 140 );
    {
        // clear() → addItem() 도중에 currentRowChanged 가 튀어 나가면
        // stkWidget 이 아직 비어 있는 상태로 setCurrentIndex 를 받는다.
        const QSignalBlocker blocker( Ui.lstCate );
        Ui.lstCate->clear();
        Ui.lstCate->addItem( tr( "공통" ) );
        Ui.lstCate->addItem( tr( "단축키" ) );
        Ui.lstCate->addItem( tr( "텍스트 뷰어" ) );
        Ui.lstCate->addItem( tr( "프리뷰" ) );
        Ui.lstCate->addItem( tr( "Python/Esbonio" ) );
    }

    Ui.stkWidget->addWidget( createGeneralPage() );
    Ui.stkWidget->addWidget( createShortcutsPage() );
    Ui.stkWidget->addWidget( createEditorPage() );
    Ui.stkWidget->addWidget( createPreviewPage() );
    Ui.stkWidget->addWidget( createEsbonioPage() );

    // .ui 에는 "PushButton" 이라고 적혀 있다. Ui.retranslateUi() 를 부르면
    // 그 값이 되살아나므로 여기서 매번 덮어써야 한다.
    Ui.btnApply->setText( tr( "적용(&A)" ) );

    Ui.lstCate->setCurrentRow( previousRow );

    loadShortcuts();
    loadTextViewerSettings();
    loadEsbonioSettings();
}

void QSettingsDialog::connectPythonEnvSignals()
{
    if( m_pythonEnvManager == nullptr )
        m_pythonEnvManager = new mrst::PythonEnvManager( this );

    // 아래 람다들이 건드리는 위젯은 페이지를 다시 만들 때마다 새것으로 바뀐다.
    // 전부 this 를 캡처하고 멤버 포인터를 null 검사한 뒤 쓰므로, 연결은 그대로
    // 두고 위젯만 갈아 끼우면 된다.
    connect( m_pythonEnvManager, &mrst::PythonEnvManager::bootstrapLog, this, [this]( const QString& text ) {
        if( m_pythonEnvLog != nullptr && !text.isEmpty() )
            m_pythonEnvLog->append( text );
    } );

    // 진행률/취소는 비동기 부트스트랩과 함께 동작한다. 대화상자를 닫아도
    // 구성은 백그라운드에서 계속된다.
    connect( m_pythonEnvManager, &mrst::PythonEnvManager::progressChanged, this,
            [this]( const int percent, const QString& phase ) {
                if( m_pythonEnvProgress == nullptr )
                    return;
                m_pythonEnvProgress->setVisible( true );
                if( percent < 0 )
                {
                    m_pythonEnvProgress->setRange( 0, 0 );   // 불확정
                }
                else
                {
                    m_pythonEnvProgress->setRange( 0, 100 );
                    m_pythonEnvProgress->setValue( percent );
                }
                m_pythonEnvProgress->setFormat( phase );
            } );

    connect( m_pythonEnvManager, &mrst::PythonEnvManager::stateChanged, this,
            [this]( const mrst::EnvState ) {
                const bool busy = m_pythonEnvManager->isBusy();
                if( m_configurePythonButton != nullptr )
                    m_configurePythonButton->setEnabled( !busy );
                if( m_cancelPythonButton != nullptr )
                    m_cancelPythonButton->setEnabled( busy );
                if( m_pythonEnvProgress != nullptr && !busy )
                    m_pythonEnvProgress->setVisible( false );
                refreshEsbonioStatus();
            } );

    connect( m_pythonEnvManager, &mrst::PythonEnvManager::failed, this, [this]( const QString& message ) {
        QMessageBox::critical( this, tr( "Python/Esbonio 환경 구성 실패" ), message );
    } );

    connect( m_pythonEnvManager, &mrst::PythonEnvManager::readyChanged, this, [this]( const bool ready ) {
        if( ready )
            QMessageBox::information( this, tr( "Python/Esbonio 환경 구성" ), tr( "환경 구성이 완료되었습니다." ) );
    } );
}

QWidget* QSettingsDialog::createGeneralPage()
{
    auto* page   = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    // ── 언어 ──
    // 테마 그룹이 stretch 1 을 먹으므로 자동 업데이트 그룹과 마찬가지로 반드시
    // 그 앞에 넣는다. 뒤에 붙이면 색상 테이블에 눌려 거의 보이지 않는다.
    auto* languageGroup = new QGroupBox( tr( "언어" ), page );
    auto* languageForm  = new QFormLayout( languageGroup );

    m_languageCombo = new QComboBox( languageGroup );
    for( const auto& entry : LanguageManager::availableLanguages() )
        m_languageCombo->addItem( entry.displayName, entry.code );
    languageForm->addRow( tr( "표시 언어:" ), m_languageCombo );

    {
        // 로드가 곧 저장이 되지 않게 막는다. 지금은 connect 보다 앞이라 없어도
        // 되지만, 순서가 바뀌는 순간 되먹임이 생긴다.
        const QSignalBlocker blocker( m_languageCombo );
        const int index = m_languageCombo->findData( LanguageManager::instance().selectedLanguage() );
        m_languageCombo->setCurrentIndex( index < 0 ? 0 : index );
    }

    connect( m_languageCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this]( int ) {
        // 공통 페이지는 즉시 저장이다. setLanguage() 가 저장까지 하고, 뒤이은
        // QEvent::LanguageChange 가 changeEvent() 로 들어와 이 페이지를 통째로
        // 다시 만든다. 여기서 settingsApplied() 를 내보내지 않는 이유는,
        // changeEvent() 가 다시 만들기 직전에 on_btnApply_clicked() 로 이미
        // 내보내기 때문이다.
        LanguageManager::instance().setLanguage( m_languageCombo->currentData().toString() );
    } );

    layout->addWidget( languageGroup );

    auto* themeGroup  = new QGroupBox( tr( "테마" ), page );
    auto* themeLayout = new QVBoxLayout( themeGroup );

    auto* formLayout = new QFormLayout;

    m_themeCombo = new QComboBox( page );
    m_themeCombo->addItem( tr( "라이트" ), static_cast< int >( ThemeManager::Light ) );
    m_themeCombo->addItem( tr( "다크" ), static_cast< int >( ThemeManager::Dark ) );
    {
        // 현재 테마로 맞춰 둔다. 이게 없으면 다크로 실행해도 콤보는 "라이트" 를
        // 보이고, 그 뒤 색상 셀을 더블클릭하거나 [기본값 복원] 을 누르기만 해도
        // applyThemePreview() 가 콤보 값을 진실로 믿고 setTheme(Light) 를 불러
        // 다크 테마를 라이트로 되돌린 채 설정에 저장해 버린다.
        const QSignalBlocker blocker( m_themeCombo );
        m_themeCombo->setCurrentIndex(
            m_themeCombo->findData( static_cast< int >( ThemeManager::instance().currentTheme() ) ) );
    }
    formLayout->addRow( tr( "테마 모드:" ), m_themeCombo );

    m_themeNameLabel = new QLabel( themeGroup );
    formLayout->addRow( tr( "기본 팔레트:" ), m_themeNameLabel );

    m_themeScopeCombo = new QComboBox( themeGroup );
    m_themeScopeCombo->addItem( tr( "전체" ), QString() );
    for( const auto& entry : ThemeManager::editableColorEntries() )
    {
        // 보이는 것은 번역된 이름, 들고 있는 것은 번역하지 않는 식별자다.
        if( m_themeScopeCombo->findData( entry.groupId ) < 0 )
            m_themeScopeCombo->addItem( entry.group, entry.groupId );
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

    // ── 자동 업데이트 ──
    // 테마 그룹이 stretch 1 을 먹으므로 이 그룹은 반드시 그 앞에 넣는다.
    // 뒤에 붙이면 색상 테이블에 눌려 거의 보이지 않는다.
    auto* updateGroup  = new QGroupBox( tr( "자동 업데이트" ), page );
    auto* updateLayout = new QVBoxLayout( updateGroup );

    m_updateEnabledCheck = new QCheckBox( tr( "자동으로 업데이트 확인" ), updateGroup );
    m_updateEnabledCheck->setToolTip( tr( "앱을 시작한 뒤 백그라운드로 확인합니다.\n"
                                          "새 버전을 찾으면 알려 주기만 하고, 내려받기는 "
                                          "직접 누를 때만 시작합니다." ) );
    updateLayout->addWidget( m_updateEnabledCheck );

    auto* intervalRow    = new QWidget( updateGroup );
    auto* intervalLayout = new QHBoxLayout( intervalRow );
    intervalLayout->setContentsMargins( 0, 0, 0, 0 );

    intervalLayout->addWidget( new QLabel( tr( "확인 주기:" ), intervalRow ) );
    m_updateIntervalSpin = new QSpinBox( intervalRow );
    m_updateIntervalSpin->setRange( 1, mrst::kMaxCheckIntervalDays );
    //: 자동 업데이트 확인 주기 스핀박스의 접미사. 숫자 바로 뒤에 붙는다.
    //: 앞 공백을 포함해서 번역할 것 — 영어 " days", 일본어 " 日ごと".
    m_updateIntervalSpin->setSuffix( tr( " 일마다" ) );
    intervalLayout->addWidget( m_updateIntervalSpin );

    intervalLayout->addSpacing( 16 );
    intervalLayout->addWidget( new QLabel( tr( "마지막 확인:" ), intervalRow ) );
    m_updateLastCheckedLabel = createValueLabel( intervalRow );
    intervalLayout->addWidget( m_updateLastCheckedLabel );
    intervalLayout->addStretch( 1 );

    m_updateCheckNowButton = new QPushButton( tr( "지금 확인" ), intervalRow );
    intervalLayout->addWidget( m_updateCheckNowButton );
    updateLayout->addWidget( intervalRow );

    layout->addWidget( updateGroup );
    layout->addWidget( themeGroup, 1 );

    loadUpdateSettings();

    // 프리뷰 페이지와 같은 즉시 저장이다. 공통 페이지에는 saveGeneralSettings()
    // 가 없고 테마 위젯도 전부 즉시 반영이라, 여기만 [적용] 을 요구하면 같은
    // 페이지 안에서 동작이 갈린다.
    const auto storeInterval = [this] {
        // 체크를 끄면 0 을 저장한다. 0 은 "확인하지 않음" 센티널이다
        // (preview/unsavedEditMaxReadMs 의 "0 = 제한 없음" 과 같은 관례).
        const int days = m_updateEnabledCheck->isChecked() ? m_updateIntervalSpin->value() : 0;
        AppSettings().setValue( QStringLiteral( "update/checkIntervalDays" ), days );
        emit settingsApplied();
    };
    connect( m_updateEnabledCheck, &QCheckBox::toggled, this,
            [ this, intervalRow, storeInterval ]( const bool checked ) {
                intervalRow->setEnabled( checked );
                storeInterval();
            } );
    connect( m_updateIntervalSpin, &QSpinBox::valueChanged, this,
            [ storeInterval ]( int ) { storeInterval(); } );
    connect( m_updateCheckNowButton, &QPushButton::clicked, this,
            [ this ] { emit updateCheckRequested(); } );

    intervalRow->setEnabled( m_updateEnabledCheck->isChecked() );

    connect( m_themeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
        populateThemeColorTable();
        if( m_themeInstantPreviewCheck&& m_themeInstantPreviewCheck->isChecked())
        applyThemePreview();
    } );
    connect( m_themeScopeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [this] {
        populateThemeColorTable();
    } );
    connect( m_themeLexerList, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem*, QListWidgetItem* ) {
        if( m_themeScopeCombo&& m_themeScopeCombo->currentData().toString()
            == QLatin1String( ThemeScopeIds::kTextLexerDetail ) )
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
                                                               QStringLiteral( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
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
                                                         QStringLiteral( "MultiViewer Theme (*.json);;JSON (*.json)" ) );
        if( filePath.isEmpty() )
            return;
        if( !filePath.endsWith( QStringLiteral( ".json" ), Qt::CaseInsensitive ) )
            filePath += QStringLiteral( ".json" );
        //saveSettings();
        QString errorMessage;
        if( !ThemeManager::instance().exportThemeFile( filePath, &errorMessage ) )
            QMessageBox::warning( this, tr( "테마 내보내기 실패" ), errorMessage );
    } );

    // 색상 표를 처음부터 채운다. 지금까지는 사용자가 테마 모드나 편집 범위를
    // 한 번 건드리기 전까지 빈 표가 보였다 — 위젯은 다 만들어 두고 아무도
    // populateThemeColorTable() 을 부르지 않았기 때문이다.
    populateThemeColorTable();

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
    m_textFontSizeSpin->setSuffix( QStringLiteral( " pt" ) );
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
    m_textLineSpacingSpin->setSuffix( QStringLiteral( " x" ) );
    m_textLineSpacingSpin->setToolTip( tr( "글꼴의 기본 행 높이를 기준으로 한 배율입니다. 1.0은 글꼴 기본 행간입니다." ) );
    layout->addRow( tr( "행간 배율:" ), m_textLineSpacingSpin );

    // 눈금자 글꼴
    auto* rulerFontRow = new QHBoxLayout;
    m_textRulerFontCombo = new QFontComboBox( page );
    m_textRulerFontSizeSpin = new QSpinBox( page );
    m_textRulerFontSizeSpin->setRange( 6, 36 );
    m_textRulerFontSizeSpin->setSuffix( QStringLiteral( " pt" ) );
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

    // 자동 줄넘김
    m_textWrapModeCombo = new QComboBox( page );
    m_textWrapModeCombo->addItem( tr( "없음" ), 0 );
    m_textWrapModeCombo->addItem( tr( "단어 단위" ), 1 );
    m_textWrapModeCombo->addItem( tr( "문자 단위" ), 2 );
    m_textWrapModeCombo->addItem( tr( "공백 단위" ), 3 );
    m_textWrapModeCombo->setToolTip( tr( "창 폭에 맞춰 긴 줄을 접어서 보여 줍니다. 편집기에서 Alt+Z 로도 켜고 끌 수 있습니다." ) );
    layout->addRow( tr( "자동 줄넘김:" ), m_textWrapModeCombo );

    // 줄넘김 표시 (SC_WRAPVISUALFLAG_* 비트 조합)
    auto* wrapFlagRow = new QHBoxLayout;
    m_textWrapFlagEndCheck = new QCheckBox( tr( "줄 끝" ), page );
    m_textWrapFlagStartCheck = new QCheckBox( tr( "줄 시작" ), page );
    m_textWrapFlagMarginCheck = new QCheckBox( tr( "여백" ), page );
    wrapFlagRow->addWidget( m_textWrapFlagEndCheck );
    wrapFlagRow->addWidget( m_textWrapFlagStartCheck );
    wrapFlagRow->addWidget( m_textWrapFlagMarginCheck );
    wrapFlagRow->addStretch( 1 );
    layout->addRow( tr( "줄넘김 표시:" ), wrapFlagRow );

    // 줄넘김 들여쓰기
    m_textWrapIndentCombo = new QComboBox( page );
    m_textWrapIndentCombo->addItem( tr( "고정 (0열)" ), 0 );
    m_textWrapIndentCombo->addItem( tr( "원래 들여쓰기 유지" ), 1 );
    m_textWrapIndentCombo->addItem( tr( "한 단계 더" ), 2 );
    m_textWrapIndentCombo->addItem( tr( "두 단계 더" ), 3 );
    m_textWrapIndentCombo->setToolTip( tr( "접힌 뒷행의 시작 위치입니다. reST 는 들여쓰기가 문법이라 '원래 들여쓰기 유지'를 권합니다. '고정'은 열 눈금자와 화면 열이 계속 일치한다는 장점이 있습니다." ) );
    layout->addRow( tr( "줄넘김 들여쓰기:" ), m_textWrapIndentCombo );

    // 줄넘김이 꺼져 있으면 하위 두 행은 의미가 없다.
    const auto updateWrapRowsEnabled = [this] {
        const bool wrapOn = m_textWrapModeCombo->currentData().toInt() != 0;
        m_textWrapFlagEndCheck->setEnabled( wrapOn );
        m_textWrapFlagStartCheck->setEnabled( wrapOn );
        m_textWrapFlagMarginCheck->setEnabled( wrapOn );
        m_textWrapIndentCombo->setEnabled( wrapOn );
    };
    connect( m_textWrapModeCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
            [updateWrapRowsEnabled]( int ) { updateWrapRowsEnabled(); } );
    updateWrapRowsEnabled();

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
    m_textLargeFileMBSpin->setSuffix( QStringLiteral( " MB" ) );
    layout->addRow( tr( "대용량 파일 기준:" ), m_textLargeFileMBSpin );

    // 외부 편집 인식
    m_textExternalChangeActionCombo = new QComboBox( page );
    m_textExternalChangeActionCombo->addItem( tr( "무시" ),
        static_cast< int >( mrst::ExternalChangeWatcher::Action::Ignore ) );
    m_textExternalChangeActionCombo->addItem( tr( "자동 불러오기" ),
        static_cast< int >( mrst::ExternalChangeWatcher::Action::Reload ) );
    m_textExternalChangeActionCombo->addItem( tr( "사용자에게 묻기" ),
        static_cast< int >( mrst::ExternalChangeWatcher::Action::Ask ) );
    m_textExternalChangeActionCombo->setToolTip(
        tr( "열어 둔 파일을 다른 프로그램이 바꿨을 때 무엇을 할지 정합니다.\n"
            "저장하지 않은 편집이 있는 탭은 '자동 불러오기' 라도 먼저 묻습니다." ) );
    layout->addRow( tr( "외부 편집 인식:" ), m_textExternalChangeActionCombo );

    m_textExternalChangeDetectionCombo = new QComboBox( page );
    m_textExternalChangeDetectionCombo->addItem( tr( "파일 시스템 알림 (권장)" ),
        static_cast< int >( mrst::ExternalChangeWatcher::Detection::Notify ) );
    m_textExternalChangeDetectionCombo->addItem( tr( "폴링" ),
        static_cast< int >( mrst::ExternalChangeWatcher::Detection::Poll ) );
    m_textExternalChangeDetectionCombo->setToolTip(
        tr( "'파일 시스템 알림' 은 운영체제가 변경을 통보해 주므로 기다리는 동안 비용이 없습니다.\n"
            "'폴링' 은 정해진 간격마다 파일을 확인합니다. 알림이 오지 않는 네트워크 드라이브나\n"
            "가상 파일 시스템에서 쓰십시오. 알림을 걸지 못한 파일은 자동으로 폴링으로 넘어갑니다." ) );
    layout->addRow( tr( "감지 방식:" ), m_textExternalChangeDetectionCombo );

    m_textExternalChangePollSpin = new QSpinBox( page );
    m_textExternalChangePollSpin->setRange( mrst::ExternalChangeWatcher::minPollSeconds(),
                                           mrst::ExternalChangeWatcher::maxPollSeconds() );
    m_textExternalChangePollSpin->setSuffix( tr( " 초" ) );
    layout->addRow( tr( "폴링 간격:" ), m_textExternalChangePollSpin );

    // "무시" 면 아래 두 행은 의미가 없고, 알림 방식이면 간격도 의미가 없다.
    // (알림을 걸지 못해 폴링으로 넘어간 파일에는 이 값이 그대로 쓰이므로 값
    //  자체는 지운다기보다 비활성으로 남긴다.)
    const auto updateExternalChangeRows = [this] {
        const bool watching = m_textExternalChangeActionCombo->currentData().toInt()
            != static_cast< int >( mrst::ExternalChangeWatcher::Action::Ignore );
        const bool polling = m_textExternalChangeDetectionCombo->currentData().toInt()
            == static_cast< int >( mrst::ExternalChangeWatcher::Detection::Poll );
        m_textExternalChangeDetectionCombo->setEnabled( watching );
        m_textExternalChangePollSpin->setEnabled( watching && polling );
    };
    connect( m_textExternalChangeActionCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
            [updateExternalChangeRows]( int ) { updateExternalChangeRows(); } );
    connect( m_textExternalChangeDetectionCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
            [updateExternalChangeRows]( int ) { updateExternalChangeRows(); } );
    updateExternalChangeRows();

    return page;
}

QWidget* QSettingsDialog::createPreviewPage()
{
    auto* page   = new QWidget( this );
    auto* layout = new QVBoxLayout( page );

    auto* group       = new QGroupBox( tr( "외부 리소스" ), page );
    auto* groupLayout = new QVBoxLayout( group );

    m_previewAllowRemoteCheck =
        new QCheckBox( tr( "인터넷에서 스크립트·스타일·이미지 불러오기" ), group );
    m_previewAllowRemoteCheck->setToolTip(
        tr( "끄면 프리뷰가 외부로 요청을 보내지 않습니다.\n"
            "대신 CDN 스크립트로 그리는 수식과 다이어그램은 텍스트로 남습니다." ) );
    groupLayout->addWidget( m_previewAllowRemoteCheck );

    auto* hint = new QLabel(
        tr( "프리뷰는 로컬 문서로 열리는데, 브라우저 엔진은 로컬 문서가 외부 주소를 "
            "여는 것을 기본적으로 막습니다. 그래서 다음 요소는 이 항목이 켜져 있어야 "
            "렌더링됩니다.\n"
            "· reStructuredText — mermaid 다이어그램, MathJax 수식\n"
            "· Markdown — KaTeX 수식($...$), mermaid 다이어그램\n\n"
            "끄면 프리뷰는 로컬 자원만 읽습니다. 두 마크업 모두 본문·표·코드 블록은 "
            "그대로 표시되고 수식과 다이어그램만 원본 텍스트로 남습니다 — 폐쇄망에서는 "
            "어차피 받아올 수 없으므로 꺼 두는 편이 낫습니다.\n\n"
            "Markdown 본문을 그리는 변환기는 프로그램에 내장되어 있어, 이 항목을 끄거나 "
            "네트워크가 없어도 항상 동작합니다." ),
        group );
    hint->setWordWrap( true );
    groupLayout->addWidget( hint );

    layout->addWidget( group );

    auto* mathGroup  = new QGroupBox( tr( "수식" ), page );
    auto* mathLayout = new QFormLayout( mathGroup );

    m_previewMathRendererCombo = new QComboBox( mathGroup );
    // 저장값은 인덱스가 아니라 식별자다. 항목이 늘거나 순서가 바뀌어도 사용자의
    // 설정 파일이 다른 렌더러를 가리키게 되지 않는다.
    m_previewMathRendererCombo->addItem( QStringLiteral( "KaTeX" ), QStringLiteral( "katex" ) );
    mathLayout->addRow( tr( "수식 렌더러:" ), m_previewMathRendererCombo );

    // 항목이 하나뿐이어도 활성 상태로 보여 준다. 이것은 조작기이면서 **표시값**이기도
    // 하다 — 화면의 수식을 무엇이 그렸는지는 수식이 안 나올 때 사용자가 가장 먼저
    // 알고 싶어 하는 정보다. 비활성으로 두면 "무슨 조건이 안 맞아 잠긴 것" 으로 읽혀
    // 없는 조건을 찾아 헤매게 된다.
    auto* mathHint = new QLabel(
        tr( "지금은 KaTeX 만 지원합니다. Markdown 프리뷰에만 적용되며, 수식은 "
            "인터넷에서 받아 그리므로 위의 외부 리소스 항목이 켜져 있어야 합니다." ),
        mathGroup );
    mathHint->setWordWrap( true );
    mathLayout->addRow( mathHint );

    layout->addWidget( mathGroup );

    auto* unsavedGroup  = new QGroupBox( tr( "미저장 편집 (reStructuredText)" ), page );
    auto* unsavedLayout = new QVBoxLayout( unsavedGroup );

    m_previewUnsavedCheck =
        new QCheckBox( tr( "저장하지 않은 편집을 프리뷰에 반영" ), unsavedGroup );
    unsavedLayout->addWidget( m_previewUnsavedCheck );

    auto* limitRow    = new QWidget( unsavedGroup );
    auto* limitLayout = new QHBoxLayout( limitRow );
    limitLayout->setContentsMargins( 0, 0, 0, 0 );
    limitLayout->addWidget( new QLabel( tr( "재파싱 허용 시간:" ), limitRow ) );
    m_previewUnsavedMaxReadSpin = new QSpinBox( limitRow );
    m_previewUnsavedMaxReadSpin->setRange( 0, 60000 );
    m_previewUnsavedMaxReadSpin->setSingleStep( 100 );
    m_previewUnsavedMaxReadSpin->setSuffix( QStringLiteral( " ms" ) );
    m_previewUnsavedMaxReadSpin->setToolTip(
        tr( "0 이면 제한하지 않습니다." ) );
    limitLayout->addWidget( m_previewUnsavedMaxReadSpin );
    limitLayout->addStretch( 1 );
    unsavedLayout->addWidget( limitRow );

    m_previewStubDoxygenCheck = new QCheckBox(
        tr( "타이핑 중에는 doxygen 지시어를 생략하기" ), unsavedGroup );
    m_previewStubDoxygenCheck->setToolTip(
        tr( "켜면 위의 재파싱 허용 시간을 적용하지 않습니다." ) );
    unsavedLayout->addWidget( m_previewStubDoxygenCheck );

    auto* unsavedHint = new QLabel(
        tr( "켜면 저장하기 전의 편집 내용이 프리뷰에 바로 나타납니다. 다만 그러려면 "
            "그 문서를 편집할 때마다 다시 파싱해야 합니다.\n\n"
            "Breathe 로 C++ API 를 싣는 문서처럼 디렉티브 하나가 doxygen XML 수백 개를 "
            "훑는 경우 재파싱만 수십 초가 걸립니다. 직전 빌드에서 잰 파싱 시간이 위 "
            "값을 넘는 문서는 자동으로 제외되어 **저장할 때만** 갱신됩니다.\n\n"
            "마지막 항목을 켜면 그런 문서에서도 편집이 반영됩니다. 타이핑하는 동안 "
            "API 부분만 \"저장하면 표시됩니다\" 자리표시자로 두고, 저장하면 온전히 "
            "다시 만듭니다. 줄 번호는 그대로 유지되므로 스크롤 동기화는 어긋나지 "
            "않습니다." ),
        unsavedGroup );
    unsavedHint->setWordWrap( true );
    unsavedLayout->addWidget( unsavedHint );

    layout->addWidget( unsavedGroup );

    auto* virtualGroup  = new QGroupBox( tr( "가상 프로젝트 (reStructuredText)" ), page );
    auto* virtualLayout = new QVBoxLayout( virtualGroup );
    auto* virtualForm   = new QFormLayout;

    m_previewVirtualThemeCombo = new QComboBox( virtualGroup );
    // 저장값은 인덱스가 아니라 테마 이름이다 (수식 렌더러와 같은 규칙). 빈
    // 문자열이 "다른 프로젝트와 동일" 이고, 그것이 기본값이다.
    m_previewVirtualThemeCombo->addItem( tr( "다른 프로젝트와 동일" ), QString{} );
    m_previewVirtualThemeCombo->insertSeparator( m_previewVirtualThemeCombo->count() );
    //: 콤보박스 항목. "alabaster" 는 Sphinx 테마 이름이므로 옮기지 않는다.
    m_previewVirtualThemeCombo->addItem( tr( "alabaster (Sphinx 기본값)" ),
                                        QStringLiteral( "alabaster" ) );
    // Sphinx 에 내장된 테마. 추가 설치가 필요 없다.
    for( const char* builtin : { "classic", "sphinxdoc", "scrolls", "agogo", "nature",
                                 "haiku", "pyramid", "bizstyle", "traditional" } )
    {
        m_previewVirtualThemeCombo->addItem( QString::fromLatin1( builtin ),
                                            QString::fromLatin1( builtin ) );
    }
    m_previewVirtualThemeCombo->insertSeparator( m_previewVirtualThemeCombo->count() );
    // 내장 파이썬 환경이 함께 받아 두는 테마 (tools/pyproject.toml 의 themes 그룹).
    // 그 그룹을 끄고 환경을 만들었다면 없을 수 있고, 그때는 프리뷰 위에 설치
    // 안내 바가 뜬다.
    for( const char* bundled : { "furo", "sphinx_rtd_theme", "pydata_sphinx_theme",
                                 "sphinx_book_theme", "shibuya", "piccolo_theme",
                                 "sphinxawesome_theme" } )
    {
        m_previewVirtualThemeCombo->addItem( QString::fromLatin1( bundled ),
                                            QString::fromLatin1( bundled ) );
    }
    virtualForm->addRow( tr( "테마:" ), m_previewVirtualThemeCombo );
    virtualLayout->addLayout( virtualForm );

    auto* virtualHint = new QLabel(
        tr( "conf.py 가 없는 단독 문서는 임시 Sphinx 프로젝트를 만들어 미리보기합니다. "
            "그 임시 프로젝트가 쓸 테마를 여기서 정합니다. 실제 프로젝트에 속한 문서는 "
            "언제나 그 프로젝트의 conf.py 를 따르므로 이 설정과 무관합니다.\n\n"
            "\"다른 프로젝트와 동일\" 은 워크스페이스의 실제 프로젝트 conf.py 에 적힌 "
            "html_theme 를 그대로 씁니다. 여러 프로젝트가 서로 다른 테마를 쓰면 먼저 "
            "발견한 것을 쓰고, 실제 프로젝트가 없거나 어느 conf.py 도 테마를 선언하지 "
            "않으면 alabaster 를 씁니다.\n\n"
            "목록의 첫 묶음은 Sphinx 에 내장된 테마이고, 그 아래는 이 프로그램이 함께 "
            "설치하는 테마입니다. 없는 테마를 고르면 프리뷰 위에 설치 안내가 뜹니다." ),
        virtualGroup );
    virtualHint->setWordWrap( true );
    virtualLayout->addWidget( virtualHint );

    layout->addWidget( virtualGroup );

    auto* outlineGroup  = new QGroupBox( tr( "개요 트리 (활성 문서 / 프로젝트)" ), page );
    auto* outlineLayout = new QVBoxLayout( outlineGroup );
    auto* outlineForm   = new QFormLayout;

    m_previewOutlineDepthSpin = new QSpinBox( outlineGroup );
    // 0 이 "제한하지 않음" 인 것은 위의 재파싱 허용 시간과 같은 관례다.
    m_previewOutlineDepthSpin->setRange( 0, 9 );
    m_previewOutlineDepthSpin->setSpecialValueText( tr( "제한 없음" ) );
    //: 스핀박스 접미사. 앞의 공백을 지우지 말 것 — 숫자와 붙는다.
    m_previewOutlineDepthSpin->setSuffix( tr( " 단계" ) );
    outlineForm->addRow( tr( "나타낼 깊이:" ), m_previewOutlineDepthSpin );
    outlineLayout->addLayout( outlineForm );

    auto* outlineHint = new QLabel(
        tr( "왼쪽 개요 트리에 몇 단계까지 나타낼지 정합니다. 깊이는 섹션 단계로 세고, "
            "프로젝트 탭의 문서 줄은 세지 않습니다 — 두 탭이 같은 단계의 섹션까지 "
            "보여 줍니다.\n\n"
            "깊이에 걸려 빠진 하위 항목이 있으면 그 줄 끝에 \" …\" 이 붙고, 마우스를 "
            "올리면 몇 개가 빠졌는지 나옵니다. 표시 없이 자르면 그 자리가 문서의 마지막 "
            "단계인 것처럼 읽히기 때문입니다.\n\n"
            "이 설정은 트리에 무엇을 올릴지만 정합니다. 문서를 다시 읽지 않으므로 바꾸는 "
            "즉시 반영됩니다." ),
        outlineGroup );
    outlineHint->setWordWrap( true );
    outlineLayout->addWidget( outlineHint );

    layout->addWidget( outlineGroup );
    layout->addStretch( 1 );

    loadPreviewSettings();

    // 켜고 끈 결과를 바로 확인할 수 있어야 한다. 저장한 뒤 곧장 알리면
    // 컨트롤러가 프리뷰를 새 설정으로 다시 읽는다.
    connect( m_previewAllowRemoteCheck, &QCheckBox::toggled, this, [this]( const bool checked ) {
        AppSettings().setValue( QStringLiteral( "preview/allowRemoteContent" ), checked );
        emit settingsApplied();
    } );
    connect( m_previewMathRendererCombo, &QComboBox::currentIndexChanged, this, [this]( int ) {
        AppSettings().setValue( QStringLiteral( "preview/mathRenderer" ),
                                m_previewMathRendererCombo->currentData().toString() );
        emit settingsApplied();
    } );
    connect( m_previewVirtualThemeCombo, &QComboBox::currentIndexChanged, this, [this]( int ) {
        // 구분선 항목은 데이터가 없어 빈 문자열이 되는데, 그것이 마침
        // "다른 프로젝트와 동일" 과 같은 값이라 따로 걸러 낼 필요가 없다.
        AppSettings().setValue( QStringLiteral( "preview/virtualProjectTheme" ),
                                m_previewVirtualThemeCombo->currentData().toString() );
        emit settingsApplied();
    } );
    connect( m_previewUnsavedCheck, &QCheckBox::toggled, this, [this, limitRow]( const bool checked ) {
        m_previewStubDoxygenCheck->setEnabled( checked );
        limitRow->setEnabled( checked && !m_previewStubDoxygenCheck->isChecked() );
        AppSettings().setValue( QStringLiteral( "preview/applyUnsavedEdits" ), checked );
        emit settingsApplied();
    } );
    connect( m_previewOutlineDepthSpin, &QSpinBox::valueChanged, this, [this]( const int value ) {
        AppSettings().setValue( QStringLiteral( "preview/outlineMaxDepth" ), value );
        emit settingsApplied();
    } );
    connect( m_previewUnsavedMaxReadSpin, &QSpinBox::valueChanged, this, [this]( const int value ) {
        AppSettings().setValue( QStringLiteral( "preview/unsavedEditMaxReadMs" ), value );
        emit settingsApplied();
    } );
    connect( m_previewStubDoxygenCheck, &QCheckBox::toggled, this,
            [this, limitRow]( const bool checked ) {
                // 생략하면 재파싱이 어차피 싸므로 허용 시간이 의미가 없다.
                limitRow->setEnabled( m_previewUnsavedCheck->isChecked() && !checked );
                AppSettings().setValue( QStringLiteral( "preview/stubDoxygenWhileTyping" ), checked );
                emit settingsApplied();
            } );
    limitRow->setEnabled( m_previewUnsavedCheck->isChecked()
                          && !m_previewStubDoxygenCheck->isChecked() );
    m_previewStubDoxygenCheck->setEnabled( m_previewUnsavedCheck->isChecked() );

    return page;
}

void QSettingsDialog::loadPreviewSettings()
{
    if( m_previewAllowRemoteCheck == nullptr )
        return;

    const AppSettings settings;
    {
        const QSignalBlocker blocker( m_previewAllowRemoteCheck );
        m_previewAllowRemoteCheck->setChecked(
            settings.value( QStringLiteral( "preview/allowRemoteContent" ), true ).toBool() );
    }
    if( m_previewUnsavedCheck != nullptr )
    {
        const QSignalBlocker blocker( m_previewUnsavedCheck );
        m_previewUnsavedCheck->setChecked(
            settings.value( QStringLiteral( "preview/applyUnsavedEdits" ), true ).toBool() );
    }
    if( m_previewUnsavedMaxReadSpin != nullptr )
    {
        const QSignalBlocker blocker( m_previewUnsavedMaxReadSpin );
        m_previewUnsavedMaxReadSpin->setValue(
            settings.value( QStringLiteral( "preview/unsavedEditMaxReadMs" ), 2000 ).toInt() );
    }
    if( m_previewStubDoxygenCheck != nullptr )
    {
        const QSignalBlocker blocker( m_previewStubDoxygenCheck );
        m_previewStubDoxygenCheck->setChecked(
            settings.value( QStringLiteral( "preview/stubDoxygenWhileTyping" ), true ).toBool() );
    }
    if( m_previewOutlineDepthSpin != nullptr )
    {
        const QSignalBlocker blocker( m_previewOutlineDepthSpin );
        // 기본값 3 은 MainWindow.cpp 의 outlineDepthSetting() 과 같아야 한다.
        // 대화상자를 열어 본 적이 없으면 ini 에 키가 없고, 그때 양쪽이 서로 다른
        // 값을 기본으로 삼으면 대화상자를 여는 것만으로 트리가 바뀐다.
        m_previewOutlineDepthSpin->setValue(
            settings.value( QStringLiteral( "preview/outlineMaxDepth" ), 3 ).toInt() );
    }
    if( m_previewMathRendererCombo != nullptr )
    {
        // QSignalBlocker 가 없으면 언어를 바꿀 때 buildPages() 가 다시 돌면서
        // currentIndexChanged 가 발화해 설정을 덮어쓴다.
        const QSignalBlocker blocker( m_previewMathRendererCombo );
        const QString renderer =
            settings.value( QStringLiteral( "preview/mathRenderer" ), QStringLiteral( "katex" ) ).toString();
        const int index = m_previewMathRendererCombo->findData( renderer );
        m_previewMathRendererCombo->setCurrentIndex( index >= 0 ? index : 0 );
    }
    if( m_previewVirtualThemeCombo != nullptr )
    {
        const QSignalBlocker blocker( m_previewVirtualThemeCombo );
        const QString theme =
            settings.value( QStringLiteral( "preview/virtualProjectTheme" ) ).toString();
        // 목록에 없는 테마가 설정에 적혀 있을 수 있다 (ini 를 손으로 고쳤거나,
        // 목록에서 뺀 테마를 쓰던 설정이 남았거나). 지우지 않고 항목으로 살려
        // 둔다 — 여기서 0번으로 물러서면 대화상자를 열기만 해도 사용자의 테마가
        // 조용히 사라진다.
        int index = m_previewVirtualThemeCombo->findData( theme );
        if( index < 0 && !theme.isEmpty() )
        {
            m_previewVirtualThemeCombo->addItem( theme, theme );
            index = m_previewVirtualThemeCombo->count() - 1;
        }
        m_previewVirtualThemeCombo->setCurrentIndex( index >= 0 ? index : 0 );
    }
}

void QSettingsDialog::savePreviewSettings()
{
    if( m_previewAllowRemoteCheck == nullptr )
        return;

    AppSettings settings;
    settings.setValue( QStringLiteral( "preview/allowRemoteContent" ),
                       m_previewAllowRemoteCheck->isChecked() );
    if( m_previewUnsavedCheck != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/applyUnsavedEdits" ),
                           m_previewUnsavedCheck->isChecked() );
    }
    if( m_previewUnsavedMaxReadSpin != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/unsavedEditMaxReadMs" ),
                           m_previewUnsavedMaxReadSpin->value() );
    }
    if( m_previewStubDoxygenCheck != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/stubDoxygenWhileTyping" ),
                           m_previewStubDoxygenCheck->isChecked() );
    }
    if( m_previewOutlineDepthSpin != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/outlineMaxDepth" ),
                           m_previewOutlineDepthSpin->value() );
    }
    if( m_previewMathRendererCombo != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/mathRenderer" ),
                           m_previewMathRendererCombo->currentData().toString() );
    }
    if( m_previewVirtualThemeCombo != nullptr )
    {
        settings.setValue( QStringLiteral( "preview/virtualProjectTheme" ),
                           m_previewVirtualThemeCombo->currentData().toString() );
    }
}

void QSettingsDialog::loadUpdateSettings()
{
    if( m_updateEnabledCheck == nullptr )
        return;

    const AppSettings settings;
    const int days = mrst::clampCheckIntervalDays(
        settings.value( QStringLiteral( "update/checkIntervalDays" ),
                        mrst::kDefaultCheckIntervalDays ).toInt() );

    // 로드하면서 즉시 저장 시그널이 되먹임하지 않게 막는다.
    {
        const QSignalBlocker blocker( m_updateEnabledCheck );
        m_updateEnabledCheck->setChecked( days > 0 );
    }
    {
        const QSignalBlocker blocker( m_updateIntervalSpin );
        // 0 은 센티널이라 스핀박스에 넣을 수 없다. 체크를 다시 켤 때 쓸 값으로
        // 기본 주기를 보여 준다.
        m_updateIntervalSpin->setValue( days > 0 ? days : mrst::kDefaultCheckIntervalDays );
    }

    refreshUpdateStatus();
}

void QSettingsDialog::refreshUpdateStatus()
{
    if( m_updateLastCheckedLabel == nullptr )
        return;

    // 저장은 UTC ISO 문자열이다. 사람에게는 지역 시각으로 보여 준다.
    const QDateTime last = QDateTime::fromString(
        AppSettings().value( QStringLiteral( "update/lastCheckedAt" ) ).toString(), Qt::ISODate );
    m_updateLastCheckedLabel->setText(
        last.isValid() ? last.toLocalTime().toString( QStringLiteral( "yyyy-MM-dd HH:mm" ) )
                       : tr( "(없음)" ) );
}

QWidget* QSettingsDialog::createEsbonioPage()
{
    // m_pythonEnvManager 는 생성자의 connectPythonEnvSignals() 가 만든다.
    // 이 함수는 언어를 바꿀 때마다 다시 도는데, 매니저는 대화상자만큼 오래
    // 살아야 하고 거기 건 연결도 한 벌이어야 한다.
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

    // 멀티루트에서는 프로젝트마다 Esbonio 서버가 하나씩 뜬다. 동시에 몇 개까지
    // 유지할지 정한다. 초과분은 가장 오래 쓰지 않은 것부터 종료된다.
    m_maxLspProcessesSpin = new QSpinBox( uvGroup );
    m_maxLspProcessesSpin->setRange( 1, 8 );
    m_maxLspProcessesSpin->setValue( AppSettings().value(
                                         QStringLiteral( "esbonio/maxLspProcesses" ), 3 ).toInt() );
    m_maxLspProcessesSpin->setToolTip( tr( "동시에 유지할 Esbonio 서버 수입니다.\n"
                                          "초과하면 가장 오래 사용하지 않은 프로젝트의 서버를 종료합니다." ) );
    formLayout->addRow( tr( "최대 Esbonio 프로세스:" ), m_maxLspProcessesSpin );

    connect( m_maxLspProcessesSpin, &QSpinBox::valueChanged, this, [this]( const int value ) {
        AppSettings().setValue( QStringLiteral( "esbonio/maxLspProcesses" ), value );
        emit settingsApplied();
    } );

    uvLayout->addLayout( formLayout );

    auto* actionLayout      = new QHBoxLayout;
    m_pythonEnvProgress     = new QProgressBar( uvGroup );
    m_pythonEnvProgress->setVisible( false );
    m_pythonEnvProgress->setTextVisible( true );
    m_configurePythonButton = new QPushButton( tr( "구성" ), uvGroup );
    m_cancelPythonButton    = new QPushButton( tr( "취소" ), uvGroup );
    m_cancelPythonButton->setEnabled( false );
    actionLayout->addWidget( m_pythonEnvProgress, 1 );
    actionLayout->addWidget( m_configurePythonButton );
    actionLayout->addWidget( m_cancelPythonButton );
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
                                                               QStringLiteral( "%1 (uv.exe);;%2 (*.exe);;%3 (*.*)" ).arg( tr( "UV 실행 파일" ), tr( "실행 파일" ), tr( "모든 파일" ) ) );
        if( selected.isEmpty() )
            return;
        m_uvPathEdit->setText( nativePath( selected ) );
        saveEsbonioSettings();
        refreshEsbonioStatus();
    } );

    connect( m_configurePythonButton, &QPushButton::clicked, this, [this] {
        saveEsbonioSettings();
        if( m_pythonEnvLog != nullptr )
            m_pythonEnvLog->clear();
        m_pythonEnvManager->configureEnvironmentAsync( true );
    } );

    connect( m_cancelPythonButton, &QPushButton::clicked, this, [this] {
        m_pythonEnvManager->cancel();
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
    m_textWrapModeCombo->setCurrentIndex(
        m_textWrapModeCombo->findData( s.value( "textView/wordWrapMode", 2 ).toInt() ) );
    const int wrapFlags = s.value( "textView/wrapVisualFlags", 1 ).toInt();
    m_textWrapFlagEndCheck->setChecked( ( wrapFlags & 0x1 ) != 0 );
    m_textWrapFlagStartCheck->setChecked( ( wrapFlags & 0x2 ) != 0 );
    m_textWrapFlagMarginCheck->setChecked( ( wrapFlags & 0x4 ) != 0 );
    m_textWrapIndentCombo->setCurrentIndex(
        m_textWrapIndentCombo->findData( s.value( "textView/wrapIndentMode", 1 ).toInt() ) );
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

    // 외부 편집 인식. 기본값과 범위 판정은 감시자가 들고 있다 — 여기서 한 벌
    // 더 쓰면 둘이 어긋나는 날이 온다.
    m_textExternalChangeActionCombo->setCurrentIndex(
        m_textExternalChangeActionCombo->findData(
            static_cast< int >( mrst::ExternalChangeWatcher::configuredAction() ) ) );
    m_textExternalChangeDetectionCombo->setCurrentIndex(
        m_textExternalChangeDetectionCombo->findData(
            static_cast< int >( mrst::ExternalChangeWatcher::configuredDetection() ) ) );
    m_textExternalChangePollSpin->setValue( mrst::ExternalChangeWatcher::configuredPollSeconds() );
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
    const int wrapMode = m_textWrapModeCombo->currentData().toInt();
    s.setValue( "textView/wordWrapMode", wrapMode );
    // Alt+Z 의 복귀 대상. "없음" 은 담지 않는다.
    if( wrapMode != 0 )
        s.setValue( "textView/wordWrapLastMode", wrapMode );
    s.setValue( "textView/wrapVisualFlags",
               ( m_textWrapFlagEndCheck->isChecked() ? 0x1 : 0 )
               | ( m_textWrapFlagStartCheck->isChecked() ? 0x2 : 0 )
               | ( m_textWrapFlagMarginCheck->isChecked() ? 0x4 : 0 ) );
    s.setValue( "textView/wrapIndentMode", m_textWrapIndentCombo->currentData().toInt() );
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
    s.setValue( "textView/externalChangeAction", m_textExternalChangeActionCombo->currentData().toInt() );
    s.setValue( "textView/externalChangeDetection", m_textExternalChangeDetectionCombo->currentData().toInt() );
    s.setValue( "textView/externalChangePollSeconds", m_textExternalChangePollSpin->value() );

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

    const QString detailScope   = QLatin1String( ThemeScopeIds::kTextLexerDetail );
    const QString currentScope  = m_themeScopeCombo ? m_themeScopeCombo->currentData().toString() : QString();
    const QString selectedLexer = ( m_themeLexerList && m_themeLexerList->currentItem() )
                                      ? m_themeLexerList->currentItem()->data( Qt::UserRole ).toString()
                                      : QStringLiteral( "cpp" );

    auto entries = ThemeManager::editableColorEntries();
    for( int i = entries.size() - 1; i >= 0; --i )
    {
        if( entries.at( i ).groupId == detailScope )
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
        // 범위 필터가 쓰는 값. 보이는 텍스트(entry.group)는 번역되므로 그것으로
        // 비교하면 언어를 바꾼 순간 모든 행이 숨는다.
        groupItem->setData( Qt::UserRole + 2, entry.groupId );
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
    const bool    lexerDetailScope = scope == QLatin1String( ThemeScopeIds::kTextLexerDetail );
    if( m_themeLexerListLabel )
        m_themeLexerListLabel->setVisible( lexerDetailScope );
    if( m_themeLexerList )
        m_themeLexerList->setVisible( lexerDetailScope );

    for( int row = 0; row < m_themeColorTable->rowCount(); ++row )
    {
        const auto* groupItem = m_themeColorTable->item( row, 0 );
        const bool  visible   = scope.isEmpty()
                                || ( groupItem
                                     && groupItem->data( Qt::UserRole + 2 ).toString() == scope );
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
    // 설정 저장은 setTheme() 이 한다. 여기서 또 쓰면 저장 지점이 둘이 되고,
    // 나중에 키 이름이나 형식을 바꿀 때 한쪽만 고치기 쉽다.
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

