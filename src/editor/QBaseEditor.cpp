#include "stdafx.h"
#include "QBaseEditor.hpp"

#include "core/solAppSettings.hpp"
#include "core/solShadowBackupStore.hpp"
#include "core/solThemeManager.hpp"

#include "ColumnRulerWidget.hpp"
#include "FindReplaceWidget.hpp"
#include "TextLexerRegistry.hpp"
#include "TextSaveDialog.hpp"
#include "ScintillaQtDirectBackend.hpp"

#include "utils/FileLoadHelper.hpp"
#include "uniqueLibs/solEncodingDetector.hpp"

//#include "core/ToolbarIcons.h"

#include <QAbstractScrollArea>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QComboBox>
#include <QFontDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QAction>
#include <QPushButton>
#include <QScrollBar>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QThreadPool>
#include <QUuid>

#include <limits>
#include <utility>

namespace
{
    struct TextOpenResult
    {
        bool success = false;
        QString filePath;
        QString errorMessage;
        QString text;
        TextFileSession session;
        TextFileSession::OpenMode openMode = TextFileSession::OpenMode::Full;
    };

    bool matchesCurrentSelection( ScintillaQtDirectBackend* editor,
                                 const QString& findText,
                                 const bool regex,
                                 const bool caseSensitive )
    {
        if( !editor || regex || !editor->hasSelectedText() )
            return false;

        return editor->selectedText().compare( findText,
                                              caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive ) == 0;
    }

    void moveCursorPastSelection( ScintillaQtDirectBackend* editor, bool forward )
    {
        if( !editor )
            return;

        int lineFrom = 0;
        int indexFrom = 0;
        int lineTo = 0;
        int indexTo = 0;
        editor->getSelectionRange( lineFrom, indexFrom, lineTo, indexTo );
        editor->setCursorPosition( forward ? lineTo : lineFrom,
                                  forward ? indexTo : indexFrom );
    }

    void moveCursorToSearchBoundary( ScintillaQtDirectBackend* editor, bool forward )
    {
        if( !editor )
            return;

        if( forward )
        {
            editor->setCursorPosition( 0, 0 );
            return;
        }

        editor->setCursorPosition( qMax( 0, editor->lineCount() - 1 ), std::numeric_limits<int>::max() );
    }

    QString preferredSaveEncoding()
    {
        AppSettings settings;
        const QString encoding = settings.value( "textView/saveEncoding", QStringLiteral( "UTF-8" ) ).toString().trimmed();
        return encoding.isEmpty() ? QStringLiteral( "UTF-8" ) : encoding;
    }

    TextSaveDialog::BomMode preferredSaveBomMode()
    {
        AppSettings settings;
        const int mode = qBound( 0, settings.value( "textView/saveBomMode", 1 ).toInt(), 2 );
        return static_cast< TextSaveDialog::BomMode >( mode );
    }

    bool includeBomForPreferredSaveMode()
    {
        return preferredSaveBomMode() != TextSaveDialog::BomMode::Exclude;
    }

    /// 도구모음 라벨.
    ///
    /// **QToolBar 는 자기 backgroundRole 을 QPalette::Button 으로 설정한다**
    /// (QToolBarPrivate::init). 자식 QLabel 은 foregroundRole 을 지정하지 않으면
    /// 그 배경 역할에서 유도된 **ButtonText** 로 글자를 그린다. Qlementine 테마에서
    /// ButtonText = secondaryColorForeground 인데, 그 값이 툴바 배경
    /// (backgroundColorMain2) 과 다크에서는 **완전히 같고**(#282b33) 라이트에서도
    /// 거의 같다(#ffffff vs #f3f3f3). 그래서 라벨이 통째로 보이지 않았다.
    ///
    /// 역할을 WindowText 로 되돌리고, 위젯이 Inactive/Disabled 그룹으로 해석돼
    /// secondaryColorDisabled(다크 #ffffff33)로 흐려지는 것까지 막는다.
    QLabel* makeToolBarLabel( QToolBar* toolBar, const QString& text )
    {
        auto* label = new QLabel( text, toolBar );
        label->setForegroundRole( QPalette::WindowText );

        QPalette     pal    = label->palette();
        const QColor active = pal.color( QPalette::Active, QPalette::WindowText );
        pal.setColor( QPalette::All, QPalette::WindowText, active );
        pal.setColor( QPalette::All, QPalette::ButtonText, active );
        pal.setColor( QPalette::All, QPalette::Text, active );
        label->setPalette( pal );
        return label;
    }

    /// 구분선(Qlementine 에서 16px)보다 좁은 항목 사이 간격.
    QWidget* makeToolBarSpacer( QToolBar* toolBar, int width = 8 )
    {
        auto* spacer = new QWidget( toolBar );
        spacer->setFixedWidth( width );
        return spacer;
    }

} // namespace

// ═══════════════════════════════════════════════════════════
// 생성 / 소멸
// ═══════════════════════════════════════════════════════════
QTextView::QTextView( QWidget* parent )
    : QBaseView( parent )
{
    m_editorSettings = ScintillaEditorSettings::standard();
    loadPersistedEditorPreferences();

    // 눈금자 위젯 (에디터 위에 배치)
    m_ruler = new ColumnRulerWidget( this );
    if( layout() )
        layout()->addWidget( m_ruler );

    // 설정 파일에서 눈금자 표시 여부 읽기 (기본: on)
    AppSettings settings;
    const bool rulerVisible = settings.value( "textView/showRuler", true ).toBool();
    m_ruler->setVisible( rulerVisible );
    m_hotExitUntitledId = newHotExitUntitledId();
    applyHotExitSettingsFromPreferences();

    m_hotExitTimer = new QTimer( this );
    m_hotExitTimer->setInterval( 2000 );
    m_hotExitTimer->setSingleShot( false );
    connect( m_hotExitTimer, &QTimer::timeout, this, &QTextView::onHotExitBackupTimer );

    ensureEditorBackend();

    // 찾기/바꾸기 위젯 (눈금자 아래, 에디터 위에 배치)
    setupFindWidget();

    // 단축키 액션 등록
    auto* findAction = new QAction( this );
    findAction->setObjectName( QStringLiteral( "text.find" ) );
    findAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_F ) );
    connect( findAction, &QAction::triggered, this, [this] {
        if( m_findWidget && m_findWidget->isVisible() && !m_findWidget->isReplaceMode() )
        {
            hideFindBar();
        }
        else
        {
            showFindBar( false );
        }
    } );
    addAction( findAction );

    auto* replaceAction = new QAction( this );
    replaceAction->setObjectName( QStringLiteral( "text.replace" ) );
    replaceAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_H ) );
    connect( replaceAction, &QAction::triggered, this, [this] {
        if( m_findWidget && m_findWidget->isVisible() && m_findWidget->isReplaceMode() )
        {
            hideFindBar();
        }
        else
        {
            showFindBar( true );
        }
    } );
    addAction( replaceAction );

    auto* findNextAction = new QAction( this );
    findNextAction->setObjectName( QStringLiteral( "text.findNext" ) );
    findNextAction->setShortcut( QKeySequence( Qt::Key_F3 ) );
    connect( findNextAction, &QAction::triggered, this, &QTextView::performFindNext );
    addAction( findNextAction );

    auto* findPrevAction = new QAction( this );
    findPrevAction->setObjectName( QStringLiteral( "text.findPrev" ) );
    findPrevAction->setShortcut( QKeySequence( Qt::SHIFT | Qt::Key_F3 ) );
    connect( findPrevAction, &QAction::triggered, this, &QTextView::performFindPrev );
    addAction( findPrevAction );

    auto* goToLineAction = new QAction( this );
    goToLineAction->setObjectName( QStringLiteral( "text.goToLine" ) );
    goToLineAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_G ) );
    connect( goToLineAction, &QAction::triggered, this, &QTextView::showGoToLineDialog );
    addAction( goToLineAction );
}

QTextView::~QTextView() = default;

// ═══════════════════════════════════════════════════════════
// 파일 관리
// ═══════════════════════════════════════════════════════════
bool QTextView::openFile( const QString& filePath )
{
    if( !ensureEditorBackend() )
    {
        QMessageBox::warning( this,
                             tr( "텍스트 백엔드 오류" ),
                             tr( "텍스트 편집기 백엔드를 초기화하지 못했습니다." ) );
        return false;
    }

    TextFileSession::OpenMode openMode = TextFileSession::OpenMode::Full;
    if( !promptOpenModeForFile( filePath, openMode ) )
        return false;

    closeFile();
    m_filePath = filePath;
    setDisplayTitle( {} );
    emit sigTitleChanged( title() );

    const int requestId = m_openRequestId.fetch_add( 1 ) + 1;
    const QString readMessage = openMode == TextFileSession::OpenMode::LimitedPreview
        ? tr( "텍스트 파일 제한 모드로 읽는 중..." )
        : tr( "텍스트 파일 읽는 중..." );

    beginLoading( readMessage, 1000 );

    QPointer<QTextView> self( this );
    QThreadPool::globalInstance()->start( [self, requestId, filePath, openMode, readMessage] {
        TextOpenResult result;
        result.filePath = filePath;
        result.openMode = openMode;

        TextFileSession session;
        bool wasCanceled = false;
        const bool opened = session.open( filePath,
                                         openMode,
                                         [self, requestId, readMessage]( qint64 bytesRead, qint64 totalBytes ) {
                                             if( !self )
                                                 return;

                                             const int scaledValue = FileLoadHelper::scaledProgressValue( bytesRead, totalBytes );
                                             QMetaObject::invokeMethod( self, [self, requestId, readMessage, scaledValue] {
                                                 if( !self || self->m_openRequestId.load() != requestId )
                                                     return;
                                                 self->updateLoadingProgress( readMessage, scaledValue, 1000 );
                                             }, Qt::QueuedConnection );
        },
                                         [self, requestId] {
                                             return !self || self->m_openRequestId.load() != requestId;
                                         },
                                         &wasCanceled );

        if( wasCanceled )
            return;

        if( !opened )
        {
            result.errorMessage = QTextView::tr( "파일을 열 수 없습니다:\n%1" ).arg( filePath );
        }
        else
        {
            result.session = std::move( session );
            const QString detectedEncoding = result.session.detectedEncoding();
            QMetaObject::invokeMethod( self, [self, requestId] {
                if( !self || self->m_openRequestId.load() != requestId )
                    return;
                self->updateLoadingProgress( QTextView::tr( "텍스트 인코딩 해석 중..." ), 1000, 1000 );
            }, Qt::QueuedConnection );

            if( !self || self->m_openRequestId.load() != requestId )
                return;

            result.text = result.session.decodeWithEncoding( detectedEncoding );
            result.success = true;
        }

        if( !self )
            return;

        QMetaObject::invokeMethod( self, [self, requestId, result = std::move( result )]() mutable {
            if( !self || self->m_openRequestId.load() != requestId )
                return;

            if( !result.success )
            {
                self->m_fileSession.clear();
                self->m_filePath.clear();
                self->m_encoding.clear();
                self->m_detectedEncoding.clear();
                self->endLoading();
                emit self->sigTitleChanged( self->title() );
                emit self->sigFileOpenFailed( result.filePath, result.errorMessage );
                return;
            }

            self->m_filePath = result.filePath;
            self->m_fileSession = std::move( result.session );
            self->m_document.setPreviewOnly( result.openMode == TextFileSession::OpenMode::LimitedPreview );
            self->m_document.setTruncated( self->m_fileSession.isTruncated() );
            self->m_detectedEncoding = self->m_fileSession.detectedEncoding();
            self->m_encoding = self->m_detectedEncoding;

            TextShadowBackupStore::Snapshot hotExitSnapshot;
            const bool restoredFromHotExit = self->m_hotExitEnabled
                && !self->m_document.isPreviewOnly()
                && TextShadowBackupStore::loadSnapshot( result.filePath, &hotExitSnapshot )
                && TextShadowBackupStore::originalFileMatchesSnapshot( hotExitSnapshot );
            if( restoredFromHotExit )
            {
                result.text = hotExitSnapshot.text;
                if( !hotExitSnapshot.encoding.trimmed().isEmpty() )
                    self->m_encoding = hotExitSnapshot.encoding;
                if( !hotExitSnapshot.detectedEncoding.trimmed().isEmpty() )
                    self->m_detectedEncoding = hotExitSnapshot.detectedEncoding;
            }

            self->m_editorSettings = self->m_document.isPreviewOnly()
                ? ScintillaEditorSettings::limitedPreview()
                : ScintillaEditorSettings::standard();
            self->applyPersistedEditorPreferences( self->m_editorSettings );
            self->applyEditorSettings();

            self->m_document.setLineEnding( restoredFromHotExit
                ? hotExitSnapshot.lineEnding
                : toDocumentLineEnding( detectLineEnding( result.text ) ) );

            self->updateLoadingProgress( tr( "에디터에 반영 중..." ), 0, 0 );
            if( !self->ensureEditorBackend() )
            {
                self->m_fileSession.clear();
                self->m_filePath.clear();
                self->m_encoding.clear();
                self->m_detectedEncoding.clear();
                self->endLoading();
                emit self->sigTitleChanged( self->title() );
                emit self->sigFileOpenFailed( result.filePath,
                                              tr( "텍스트 편집기 백엔드를 사용할 수 없습니다:\n%1" ).arg( result.filePath ) );
                return;
            }
            self->m_applyingFileContent = true;
            self->m_editor->setText( result.text );
            self->m_applyingFileContent = false;
            self->m_editor->setModified( restoredFromHotExit );
            self->m_cachedCurrentLine = 1;
            self->m_cachedCurrentColumn = 1;
            if( restoredFromHotExit )
                self->m_editor->restoreViewState( hotExitSnapshot.caretPosition, hotExitSnapshot.firstVisibleLine );
            else
                self->m_editor->setCursorPosition( 0, 0 );
            self->m_hotExitDirty = restoredFromHotExit;
            self->updateMetrics();

            if( self->m_document.isPreviewOnly() )
                self->setLanguage( QStringLiteral( "None" ) );
            else
                self->autoDetectLanguage();

            emit self->sigFileOpened( result.filePath );
            emit self->encodingChanged( self->m_encoding );
            emit self->statusChanged();
            self->updateCopyAvailability();
            self->endLoading();
        }, Qt::QueuedConnection );
    } );

    return true;
}

bool QTextView::saveFile( const QString& filePath )
{
    if( !m_editor )
        return false;

    if( isLoading() )
        return false;

    QString path = filePath.isEmpty() ? m_filePath : filePath;
    if( path.isEmpty() )
        return saveWithEncodingDialog();

    if( m_fileSession.isTruncated() )
    {
        QMessageBox::information( this,
                                 tr( "저장 불가" ),
                                 tr( "제한 모드로 연 파일은 현재 저장할 수 없습니다. 파일을 전체 로드로 다시 열어 주세요." ) );
        return false;
    }

    const bool hasCurrentEncoding = !m_encoding.trimmed().isEmpty();
    return saveWithEncoding( path,
                            hasCurrentEncoding ? m_encoding : preferredSaveEncoding(),
                            hasCurrentEncoding ? currentFileHasBom() : includeBomForPreferredSaveMode() );
}

bool QTextView::saveWithEncoding( const QString& filePath, const QString& encoding, bool includeBom )
{
    if( !m_editor )
        return false;

    if( isLoading() )
        return false;

    const QString effectivePath = filePath.trimmed();
    if( effectivePath.isEmpty() )
        return false;

    const int requestId = m_openRequestId.load();
    const QString text = m_editor->text();
    const QString previousPath = m_filePath;
    const bool wasUntitledForHotExit = previousPath.trimmed().isEmpty();
    const QString previousHotExitUntitledId = m_hotExitUntitledId;
    const ScintillaDocument::LineEnding lineEnding = m_document.lineEnding();

    beginLoading( tr( "텍스트 저장 중..." ), 1000 );
    updateLoadingProgress( tr( "텍스트 저장 준비 중..." ), 0, 1000 );

    QPointer<QTextView> self( this );
    QThreadPool::globalInstance()->start( [ self,
                                          requestId,
                                          effectivePath,
                                          encoding,
                                          includeBom,
                                          text,
                                          lineEnding,
                                           previousPath,
                                           wasUntitledForHotExit,
                                           previousHotExitUntitledId ] {
                                               Q_UNUSED( previousPath );
                                               QString errorMessage;

                                               QMetaObject::invokeMethod( self, [self, requestId] {
                                                   if( !self || self->m_openRequestId.load() != requestId )
                                                       return;
                                                   self->updateLoadingProgress( QTextView::tr( "텍스트 인코딩 변환 중..." ), 150, 1000 );
                                               }, Qt::QueuedConnection );

                                               TextFileSession savedSession;
                                               const bool saved = savedSession.saveText( effectivePath,
                                                                                        text,
                                                                                        encoding,
                                                                                        lineEnding,
                                                                                        includeBom,
                                                                                        [self, requestId]( qint64 bytesWritten, qint64 totalBytes ) {
                                                                                                                                   if( !self )
                                                                                                                                       return;

                                                                                                                                   const int scaled = totalBytes > 0
                                                                                                                                       ? 150 + qRound( ( static_cast< double >( bytesWritten ) * 850.0 ) / totalBytes )
                                                                                                                                       : 1000;
                                                                                                                                   QMetaObject::invokeMethod( self, [self, requestId, scaled] {
                                                                                                                                       if( !self || self->m_openRequestId.load() != requestId )
                                                                                                                                           return;
                                                                                                                                       self->updateLoadingProgress( QTextView::tr( "텍스트 파일 저장 중..." ),
                                                                                                                                                                   qBound( 150, scaled, 1000 ),
                                                                                                                                                                   1000 );
                                                                                                                                   }, Qt::QueuedConnection );
                                                                                        } );

                                               if( !saved )
                                                   errorMessage = QTextView::tr( "파일을 저장하지 못했습니다. 제한 모드 여부와 대상 경로를 확인해 주세요." );

                                               if( !self )
                                                   return;

                                               QMetaObject::invokeMethod( self,
                                                                         [ self,
                                                                          requestId,
                                                                          effectivePath,
                                                                          encoding,
                                                                          includeBom,
                                                                          saved,
                                                                          savedSession = std::move( savedSession ),
                                                                           errorMessage,
                                                                           wasUntitledForHotExit,
                                                                           previousHotExitUntitledId ]( ) mutable {
                                                                                                                      if( !self || self->m_openRequestId.load() != requestId )
                                                                                                                          return;

                                                                                                                      if( !saved )
                                                                                                                      {
                                                                                                                          self->endLoading();
                                                                                                                          QMessageBox::warning( self,
                                                                                                                                               QTextView::tr( "저장 실패" ),
                                                                                                                                               errorMessage );
                                                                                                                          return;
                                                                                                                      }

                                                                                                                      self->m_fileSession = std::move( savedSession );
                                                                                                                      self->m_filePath = effectivePath;
                                                                                                                      self->m_encoding = encoding;
                                                                                                                      self->m_detectedEncoding = encoding;
                                                                                                                      self->setDisplayTitle( {} );
                                                                                                                      self->m_editor->setModified( false );
                                                                                                                      self->m_hotExitDirty = false;
                                                                                                                      TextShadowBackupStore::deleteSnapshot( effectivePath );
                                                                                                                      if( wasUntitledForHotExit )
                                                                                                                          TextShadowBackupStore::deleteUntitledSnapshot( previousHotExitUntitledId );
                                                                                                                      emit self->encodingChanged( self->m_encoding );
                                                                                                                      emit self->sigTitleChanged( self->title() );
                                                                                                                      emit self->statusChanged();
                                                                                                                      self->endLoading();
                                                                         },
                                                                         Qt::QueuedConnection );
    } );

    return true;
}

bool QTextView::saveFileAs()
{
    return saveWithEncodingDialog( m_filePath );
}

void QTextView::closeFile()
{
    m_openRequestId.fetch_add( 1 );
    endLoading();
    const bool shuttingDown = isShuttingDown();
    clearCurrentMatchHighlight();
    clearExcludedRanges();
    const bool keepHotExitBackup = shouldUseHotExitForCurrentFile() && isModified();
    if( keepHotExitBackup )
        writeHotExitBackupNow( true );
    else
        discardHotExitBackup();
    if( !shuttingDown && m_editor )
        m_editor->clear();
    m_fileSession.clear();
    m_filePath.clear();
    m_encoding.clear();
    m_detectedEncoding.clear();
    m_document = ScintillaDocument();
    m_editorSettings = ScintillaEditorSettings::standard();
    applyPersistedEditorPreferences( m_editorSettings );
    if( !shuttingDown )
    {
        applyEditorSettings();
        if( m_editor )
            m_editor->applyLanguage( QStringLiteral( "None" ) );
    }
    m_cachedLineCount = 1;
    m_cachedCurrentLine = 1;
    m_cachedCurrentColumn = 1;
    m_hotExitDirty = false;
    m_hotExitUntitledId = newHotExitUntitledId();
    updateMetrics();
    emit sigFileClosed();
    emit statusChanged();
    updateCopyAvailability();
}

bool QTextView::isModified() const { return m_editor && m_editor->isModified(); }

QStringList QTextView::supportedExtensions() const
{
    return TextLexerRegistry::instance().supportedExtensions();
}

bool QTextView::opensFileAsynchronously() const
{
    return true;
}

// ═══════════════════════════════════════════════════════════
// 인코딩
// ═══════════════════════════════════════════════════════════
QString QTextView::detectedEncoding() const { return m_detectedEncoding; }
QString QTextView::currentEncoding() const { return m_encoding; }

QString QTextView::currentEncodingDisplayName() const
{
    const QString normalized = m_encoding.trimmed().toUpper();
    if( normalized == QStringLiteral( "UTF-8" )
        || normalized == QStringLiteral( "UTF-16" )
        || normalized == QStringLiteral( "UTF-16LE" )
        || normalized == QStringLiteral( "UTF-16BE" ) )
    {
        return tr( "%1 (%2)" )
            .arg( m_encoding,
                 m_fileSession.hasBom() ? tr( "BOM 있음" ) : tr( "BOM 없음" ) );
    }

    return m_encoding;
}

void QTextView::reloadWithEncoding( const QString& encoding )
{
    if( !m_editor )
        return;

    m_encoding = encoding;
    QString text = m_fileSession.decodeWithEncoding( encoding );
    m_document.setLineEnding( toDocumentLineEnding( detectLineEnding( text ) ) );
    m_applyingFileContent = true;
    m_editor->setText( text );
    m_applyingFileContent = false;
    m_editor->setModified( false );
    m_cachedCurrentLine = 1;
    m_cachedCurrentColumn = 1;
    m_editor->setCursorPosition( 0, 0 );
    m_editor->applyLanguage( m_document.language() );
    updateMetrics();
    emit encodingChanged( encoding );
    emit statusChanged();
    updateCopyAvailability();
}

QStringList QTextView::availableEncodings() { return EncodingDetector::availableEncodings(); }

bool QTextView::saveWithEncodingDialog( const QString& initialFilePath )
{
    if( !m_editor || isLoading() )
        return false;

    if( m_fileSession.isTruncated() )
    {
        QMessageBox::information( this,
                                 tr( "저장 불가" ),
                                 tr( "제한 모드로 연 파일은 현재 저장할 수 없습니다. 파일을 전체 로드로 다시 열어 주세요." ) );
        return false;
    }

    TextSaveDialog dialog( this );
    dialog.setWindowTitle( tr( "다른 이름으로 저장" ) );
    dialog.setDialogFilters( { tr( "텍스트 파일 (*.*)" ) }, tr( "텍스트 파일 (*.*)" ) );
    dialog.setInitialFilePath( initialFilePath.isEmpty() ? tr( "새 텍스트 파일.txt" ) : initialFilePath );
    dialog.setEncodingOptions( availableEncodings(),
                              preferredSaveEncoding(),
                              currentFileHasBom(),
                              preferredSaveBomMode() );

    if( dialog.exec() != QDialog::Accepted )
        return false;

    const auto options = dialog.selectedOptions();
    if( options.filePath.isEmpty() )
        return false;

    return saveWithEncoding( options.filePath,
                            options.encoding.isEmpty() ? QStringLiteral( "UTF-8" ) : options.encoding,
                            dialog.selectedBomIncluded() );
}

bool QTextView::currentFileHasBom() const
{
    return m_fileSession.hasBom();
}

// ═══════════════════════════════════════════════════════════
// 글꼴
// ═══════════════════════════════════════════════════════════
void QTextView::setEditorFont( const QFont& font )
{
    if( !m_editor )
        return;

    m_editor->setEditorFont( font );
    applyEditorSettings();
    m_editor->applyLanguage( m_document.language() );
    m_editor->applyThemeColors( currentTheme() == Theme::Dark );
    if( m_ruler )
        m_ruler->setEditorFont( font );

    AppSettings settings;
    const int pointSize = font.pointSize() > 0
        ? font.pointSize()
        : ( font.pointSizeF() > 0.0 ? qRound( font.pointSizeF() ) : settings.value( "textView/fontSize", 10 ).toInt() );
    settings.setValue( "textView/fontFamily", font.family() );
    settings.setValue( "textView/fontSize", qBound( 6, pointSize, 72 ) );
}

QFont QTextView::editorFont() const { return m_editor ? m_editor->editorFont() : QFont(); }

double QTextView::lineSpacingScale() const
{
    return m_editorSettings.lineSpacingScale;
}

void QTextView::setLineSpacingScale( double scale )
{
    scale = qBound( 1.0, scale, 3.0 );
    if( qFuzzyCompare( m_editorSettings.lineSpacingScale, scale ) )
        return;

    m_editorSettings.lineSpacingScale = scale;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/lineSpacing", scale );
}

// ═══════════════════════════════════════════════════════════
// 구문 강조
// ═══════════════════════════════════════════════════════════
void QTextView::setLanguage( const QString& language )
{
    const QString normalizedLanguage = language.trimmed().isEmpty() ? QStringLiteral( "None" ) : language.trimmed();
    m_document.setLanguage( normalizedLanguage );
    if( m_editor )
        m_editor->applyLanguage( normalizedLanguage );

    emit languageChanged( normalizedLanguage );
    emit statusChanged();
}

QString QTextView::currentLanguage() const { return m_document.language(); }

void QTextView::autoDetectLanguage()
{
    setLanguage( TextLexerRegistry::instance().languageForExtension( QFileInfo( m_filePath ).suffix() ) );
}

// ═══════════════════════════════════════════════════════════
// 편집
// ═══════════════════════════════════════════════════════════
void QTextView::setReadOnly( bool on )
{
    if( !m_editor )
        return;

    if( m_editor->isReadOnly() == on )
        return;

    m_editor->setReadOnly( on );
    emit statusChanged();
}
bool QTextView::isReadOnly() const { return m_editor && m_editor->isReadOnly(); }
bool QTextView::isLimitedPreviewMode() const { return m_document.isPreviewOnly(); }
bool QTextView::isContentTruncated() const { return m_document.isTruncated(); }

QString QTextView::contentLoadModeText() const
{
    if( m_document.isPreviewOnly() )
    {
        return m_document.isTruncated()
            ? tr( "제한 모드 (부분 로드)" )
            : tr( "제한 모드" );
    }

    return tr( "전체 로드" );
}

bool QTextView::isHotExitEnabled() const
{
    return m_hotExitEnabled;
}

void QTextView::setHotExitEnabled( bool enabled )
{
    if( m_hotExitEnabled == enabled )
    {
        if( !enabled )
            discardHotExitBackup();
        return;
    }

    m_hotExitEnabled = enabled;
    if( !m_hotExitEnabled )
    {
        if( m_hotExitTimer )
            m_hotExitTimer->stop();
        m_hotExitDirty = false;
        discardHotExitBackup();
        return;
    }

    if( isModified() )
        scheduleHotExitBackup();
}

bool QTextView::openHotExitBackup( const QString& untitledId )
{
    if( !m_hotExitEnabled || !ensureEditorBackend() )
        return false;

    TextShadowBackupStore::Snapshot snapshot;
    if( !TextShadowBackupStore::loadUntitledSnapshot( untitledId, &snapshot ) || !snapshot.isUntitled )
        return false;

    closeFile();
    m_hotExitUntitledId = snapshot.untitledId.trimmed().isEmpty()
        ? newHotExitUntitledId()
        : snapshot.untitledId.trimmed();
    m_filePath.clear();
    setDisplayTitle( snapshot.displayTitle.trimmed().isEmpty() ? tr( "제목없음" ) : snapshot.displayTitle.trimmed() );
    emit sigTitleChanged( title() );

    m_fileSession.clear();
    m_document = ScintillaDocument();
    m_document.setPreviewOnly( false );
    m_document.setTruncated( false );
    m_document.setLineEnding( snapshot.lineEnding );
    m_encoding = snapshot.encoding.trimmed().isEmpty() ? preferredSaveEncoding() : snapshot.encoding.trimmed();
    m_detectedEncoding = snapshot.detectedEncoding;

    m_editorSettings = ScintillaEditorSettings::standard();
    applyPersistedEditorPreferences( m_editorSettings );
    applyEditorSettings();
    m_applyingFileContent = true;
    m_editor->setText( snapshot.text );
    m_applyingFileContent = false;
    m_editor->setModified( true );
    m_editor->restoreViewState( snapshot.caretPosition, snapshot.firstVisibleLine );
    m_hotExitDirty = true;
    updateMetrics();
    autoDetectLanguage();
    emit encodingChanged( m_encoding );
    emit statusChanged();
    updateCopyAvailability();
    return true;
}

void QTextView::flushHotExitBackup()
{
    if( shouldUseHotExitForCurrentFile() && isModified() )
        writeHotExitBackupNow( true );
}

void QTextView::discardHotExitBackup()
{
    if( !m_filePath.isEmpty() )
        TextShadowBackupStore::deleteSnapshot( m_filePath );
    else if( !m_hotExitUntitledId.isEmpty() )
        TextShadowBackupStore::deleteUntitledSnapshot( m_hotExitUntitledId );
    m_hotExitDirty = false;
}

bool QTextView::shouldUseHotExitForCurrentFile() const
{
    return m_hotExitEnabled
        && m_editor
        && !m_document.isPreviewOnly()
        && !m_document.isTruncated()
        && !isLoading();
}

void QTextView::scheduleHotExitBackup()
{
    if( !shouldUseHotExitForCurrentFile() )
        return;

    m_hotExitDirty = true;
    if( m_hotExitTimer && !m_hotExitTimer->isActive() )
        m_hotExitTimer->start();
}

void QTextView::writeHotExitBackupNow( bool synchronous )
{
    if( !shouldUseHotExitForCurrentFile() )
        return;

    const bool isUntitled = m_filePath.trimmed().isEmpty();
    QFileInfo fileInfo( m_filePath );
    TextShadowBackupStore::Snapshot snapshot;
    snapshot.isUntitled = isUntitled;
    snapshot.untitledId = isUntitled ? m_hotExitUntitledId : QString();
    snapshot.displayTitle = isUntitled ? title() : QString();
    snapshot.originalFilePath = m_filePath;
    snapshot.text = m_editor->text();
    snapshot.encoding = m_encoding;
    snapshot.detectedEncoding = m_detectedEncoding;
    snapshot.lineEnding = m_document.lineEnding();
    snapshot.caretPosition = m_editor->currentPos();
    snapshot.firstVisibleLine = m_editor->firstVisibleLine();
    snapshot.originalSize = !isUntitled && fileInfo.exists() ? fileInfo.size() : -1;
    snapshot.originalLastModifiedUtcMs = !isUntitled && fileInfo.exists()
        ? fileInfo.lastModified().toUTC().toMSecsSinceEpoch()
        : 0;
    snapshot.savedAtUtc = QDateTime::currentDateTimeUtc();

    m_hotExitDirty = false;
    if( synchronous )
    {
        TextShadowBackupStore::saveSnapshot( snapshot );
        return;
    }

    QThreadPool::globalInstance()->start( [snapshot = std::move( snapshot )] {
        TextShadowBackupStore::saveSnapshot( snapshot );
    } );
}

void QTextView::onHotExitBackupTimer()
{
    if( !m_hotExitDirty )
    {
        if( m_hotExitTimer )
            m_hotExitTimer->stop();
        return;
    }

    if( !isModified() )
    {
        discardHotExitBackup();
        if( m_hotExitTimer )
            m_hotExitTimer->stop();
        return;
    }

    writeHotExitBackupNow();
}

void QTextView::applyHotExitSettingsFromPreferences()
{
    AppSettings settings;
    setHotExitEnabled( settings.value( "textView/hotExitEnabled", true ).toBool() );
}

bool QTextView::canCopyToClipboard() const
{
    return m_editor && m_editor->hasSelectedText();
}

bool QTextView::copySelectionToClipboard()
{
    if( !canCopyToClipboard() )
        return false;

    m_editor->copy();
    return true;
}

bool QTextView::canPasteFromClipboard() const
{
    return m_editor && !m_editor->isReadOnly() && m_editor->canPaste();
}

bool QTextView::pasteFromClipboard()
{
    if( !canPasteFromClipboard() )
        return false;

    m_editor->paste();
    return true;
}

void QTextView::goToLine( int line )
{
    goToPosition( line, 1 );
}

void QTextView::goToPosition( int line, int column )
{
    if( !m_editor )
        return;

    const int safeLine = qBound( 1, line, lineCount() );
    const int safeColumn = qMax( 1, column );
    // 개요·진단·검색으로 뛰어든 줄이 접혀 있을 수 있다. 그대로 캐럿만 옮기면
    // 캐럿이 보이지 않는 곳에 놓인다. 먼저 그 블록을 펼친다.
    m_editor->ensureLineVisible( safeLine - 1 );
    m_editor->setCursorPosition( safeLine - 1, safeColumn - 1 );
}

int  QTextView::characterCount() const
{
    return m_editor
        ? static_cast< int >( qMin( m_editor->text().size(), qsizetype( std::numeric_limits<int>::max() ) ) )
        : 0;
}

// ═══════════════════════════════════════════════════════════
// Sphinx/Esbonio 서비스 계층용 접근자

QString QTextView::text() const
{
    return m_editor ? m_editor->text() : QString{};
}

QString QTextView::lineText( int line ) const
{
    // 밖에서는 1-based 줄 번호를 쓴다.
    return m_editor ? m_editor->lineText( line - 1 ) : QString{};
}

QString QTextView::textRange( int startPos, int endPos ) const
{
    return m_editor ? QString::fromUtf8( m_editor->textRangeUtf8( startPos, endPos ) ) : QString{};
}

int QTextView::positionFromLineColumn( int line, int column ) const
{
    return m_editor ? m_editor->positionFromLineColumn( qMax( 0, line - 1 ), qMax( 0, column - 1 ) ) : 0;
}

int QTextView::currentPosition() const
{
    return m_editor ? m_editor->currentPos() : 0;
}

int QTextView::caretLine() const
{
    return m_editor ? m_editor->lineFromPosition( m_editor->currentPos() ) + 1 : 1;
}

int QTextView::caretColumn() const
{
    return m_editor ? m_editor->columnFromPosition( m_editor->currentPos() ) + 1 : 1;
}

int QTextView::firstVisibleLine() const
{
    return m_editor ? m_editor->firstVisibleLine() + 1 : 1;
}

void QTextView::scrollToLine( int line, double viewportRatio )
{
    scrollFractionalLineToViewportRatio( line, viewportRatio );
}

double QTextView::fractionalLineAtViewportRatio( const double ratio ) const
{
    return m_editor ? m_editor->fractionalLineAtViewportRatio( ratio ) : 1.0;
}

void QTextView::scrollFractionalLineToViewportRatio( const double fractionalLine, const double ratio )
{
    if( m_editor )
        m_editor->scrollFractionalLineToViewportRatio( fractionalLine, ratio );
}

QPoint QTextView::caretGlobalPos() const
{
    if( !m_editor || !m_editor->widget() )
        return {};

    const QPoint local = m_editor->pointFromPosition( m_editor->currentPos() );
    // 캐럿 아래쪽에 팝업이 오도록 한 줄 높이만큼 내린다.
    const int lineHeight = qMax( 1, m_editor->textHeight( m_editor->lineFromPosition( m_editor->currentPos() ) ) );
    return m_editor->widget()->mapToGlobal( local + QPoint( 0, lineHeight ) );
}

void QTextView::replaceRangeAtCursor( int backspaceCount, const QString& insertText )
{
    if( !m_editor )
        return;

    const int caret = m_editor->currentPos();
    int start = caret;
    if( backspaceCount > 0 )
    {
        // backspaceCount 는 글자 수이므로 바이트가 아니라 문서 위치로 환산해야 한다.
        const int line = m_editor->lineFromPosition( caret );
        const int column = m_editor->columnFromPosition( caret );
        start = m_editor->positionFromLineColumn( line, qMax( 0, column - backspaceCount ) );
    }

    m_editor->setSearchTargetRange( start, caret );
    m_editor->replaceInTarget( insertText );

    const int newCaret = m_editor->targetEnd();
    m_editor->setSelectionByPos( newCaret, newCaret );
}

// ── 진단 표시 ──────────────────────────────────────────────

int QTextView::diagnosticIndicatorFor( int severity )
{
    switch( severity )
    {
        case 1:  return kDiagnosticErrorIndicatorId;
        case 2:  return kDiagnosticWarningIndicatorId;
        case 3:  return kDiagnosticInfoIndicatorId;
        default: return kDiagnosticHintIndicatorId;
    }
}

void QTextView::configureDiagnosticIndicators()
{
    if( !m_editor || m_diagnosticIndicatorsReady )
        return;

    const ThemeManager& theme = ThemeManager::instance();
    const QColor errorColor = theme.color( QStringLiteral( "text.diagnostic.error" ) );
    const QColor warningColor = theme.color( QStringLiteral( "text.diagnostic.warning" ) );
    const QColor infoColor = theme.color( QStringLiteral( "text.diagnostic.info" ) );

    // 테마에 아직 진단 색이 없을 수 있으므로 유효하지 않으면 표준색으로 대체한다.
    m_editor->setIndicatorStyle( kDiagnosticErrorIndicatorId, 1,   // INDIC_SQUIGGLE
                                errorColor.isValid() ? errorColor : QColor( 0xE5, 0x14, 0x00 ) );
    m_editor->setIndicatorStyle( kDiagnosticWarningIndicatorId, 1, // INDIC_SQUIGGLE
                                warningColor.isValid() ? warningColor : QColor( 0xBF, 0x81, 0x03 ) );
    m_editor->setIndicatorStyle( kDiagnosticInfoIndicatorId, 1,    // INDIC_SQUIGGLE
                                infoColor.isValid() ? infoColor : QColor( 0x1A, 0x85, 0xFF ) );
    m_editor->setIndicatorStyle( kDiagnosticHintIndicatorId, 10,   // INDIC_DOTS
                                infoColor.isValid() ? infoColor : QColor( 0x80, 0x80, 0x80 ) );
    m_diagnosticIndicatorsReady = true;
}

void QTextView::setDiagnosticMarks( const QVector< mrst::DiagnosticEntry >& entries )
{
    if( !m_editor )
        return;

    configureDiagnosticIndicators();

    for( const int indicatorId : { kDiagnosticErrorIndicatorId,
                                  kDiagnosticWarningIndicatorId,
                                  kDiagnosticInfoIndicatorId,
                                  kDiagnosticHintIndicatorId } )
    {
        m_editor->clearAllIndicator( indicatorId );
    }

    m_diagnostics = entries;
    if( entries.isEmpty() )
        return;

    const int documentEnd = m_editor->documentLength();
    for( const mrst::DiagnosticEntry& entry : entries )
    {
        const int start = m_editor->positionFromLineColumn( qMax( 0, entry.line - 1 ),
                                                           qMax( 0, entry.character - 1 ) );

        // 끝 위치가 없거나 시작보다 앞이면 해당 줄 끝까지 표시한다.
        int end = documentEnd;
        if( entry.endLine >= entry.line )
            end = m_editor->positionFromLineColumn( qMax( 0, entry.endLine - 1 ),
                                                   qMax( 0, entry.endCharacter - 1 ) );
        if( end <= start )
            end = m_editor->lineEndPosition( qMax( 0, entry.line - 1 ) );

        // 빈 줄이라 길이가 0이면 최소 한 글자는 칠해야 눈에 보인다.
        const int length = qMax( 1, end - start );
        if( start >= documentEnd )
            continue;

        m_editor->applyIndicator( diagnosticIndicatorFor( entry.severity ), start,
                                 qMin( length, documentEnd - start ) );
    }
}

void QTextView::feedRstCompletionVocabulary( const QStringList& directives, const QStringList& roles )
{
    if( !m_editor )
        return;

    mrst::rst::RstMetadataCache* cache = m_editor->rstMetadataCache();
    if( cache == nullptr )
        return;   // 컨테이너 렉싱 중이 아니다 (.rst 가 아닌 문서)

    bool changed = false;
    for( const QString& name : directives )
        changed |= cache->directives.insert( name.toStdString() ).second;
    for( const QString& name : roles )
        changed |= cache->roles.insert( name.toStdString() ).second;

    // 3-state 의 핵심. 비어 있을 때는 UNKNOWN 이라 아무것도 빨갛지 않다가,
    // 여기서 처음 채워지는 순간부터 목록에 없는 이름이 INVALID 가 된다.
    if( !directives.isEmpty() && !cache->directivesPopulated )
    {
        cache->directivesPopulated = true;
        changed = true;
    }
    if( !roles.isEmpty() && !cache->rolesPopulated )
    {
        cache->rolesPopulated = true;
        changed = true;
    }

    if( changed )
        m_editor->restyleDocument();
}

QString QTextView::diagnosticTooltipAt( int position ) const
{
    if( !m_editor || m_diagnostics.isEmpty() )
        return {};

    const int line = m_editor->lineFromPosition( position ) + 1;
    return mrst::diagnosticTooltipText( m_diagnostics, currentFilePath(), line );
}

void QTextView::handleDwellStart( int position, const QPoint& viewportPos )
{
    if( !m_editor || position < 0 )
    {
        emit sigRoleHoverEnded();
        return;
    }

    // 롤 형태는 렉서(RstContainerLexer)가 쓰는 것과 같다.
    // 도메인 표기(:py:func:`x`)까지 한 번에 잡는다.
    static const QRegularExpression roleRe(
        QStringLiteral( R"(:([a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?):`([^`]*)`)" ) );

    const int line = m_editor->lineFromPosition( position );
    const int lineStart = m_editor->positionFromLine( line );
    const QString text = m_editor->lineText( line );

    // Scintilla 위치는 UTF-8 바이트 오프셋이다. 한글이 섞인 줄에서 문자 인덱스와
    // 어긋나므로, 줄 앞부분의 바이트 길이로 문자 인덱스를 되돌린다.
    const int byteOffset = qMax( 0, position - lineStart );
    const QByteArray lineUtf8 = text.toUtf8();
    const int charIndex = QString::fromUtf8( lineUtf8.left( qMin( byteOffset, lineUtf8.size() ) ) ).size();

    QRegularExpressionMatchIterator it = roleRe.globalMatch( text );
    while( it.hasNext() )
    {
        const QRegularExpressionMatch match = it.next();
        if( charIndex < match.capturedStart() || charIndex >= match.capturedEnd() )
            continue;

        const QPoint globalPos = m_editor->widget() != nullptr
                                     ? m_editor->widget()->mapToGlobal( viewportPos )
                                     : mapToGlobal( viewportPos );
        emit sigRoleHovered( match.captured( 1 ), match.captured( 2 ), globalPos );
        return;
    }

    emit sigRoleHoverEnded();
}

int QTextView::tabWidth() const
{
    return m_editorSettings.tabWidth;
}

void QTextView::setTabWidth( int width )
{
    width = qBound( 1, width, 16 );
    if( m_editorSettings.tabWidth == width )
        return;
    m_editorSettings.tabWidth = width;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/tabWidth", width );
}

bool QTextView::useTabs() const
{
    return m_editorSettings.useTabs;
}

void QTextView::setUseTabs( bool use )
{
    if( m_editorSettings.useTabs == use )
        return;
    m_editorSettings.useTabs = use;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/useTabs", use );
    emit statusChanged();
}

bool QTextView::indentationGuidesVisible() const
{
    return m_editorSettings.showIndentationGuides;
}

void QTextView::setIndentationGuidesVisible( bool visible )
{
    if( m_editorSettings.showIndentationGuides == visible )
        return;
    m_editorSettings.showIndentationGuides = visible;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/showIndentationGuides", visible );
}

ScintillaEditorSettings::IndentGuideStyle QTextView::indentGuideStyle() const
{
    return m_editorSettings.indentGuideStyle;
}

void QTextView::setIndentGuideStyle( ScintillaEditorSettings::IndentGuideStyle style )
{
    style = static_cast< ScintillaEditorSettings::IndentGuideStyle >(
        qBound( static_cast< int >( ScintillaEditorSettings::IndentGuideReal ),
                static_cast< int >( style ),
                static_cast< int >( ScintillaEditorSettings::IndentGuideLookBoth ) ) );
    if( m_editorSettings.indentGuideStyle == style )
        return;
    m_editorSettings.indentGuideStyle = style;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/indentGuideStyle", static_cast< int >( style ) );
}

int  QTextView::selectedCharacterCount() const
{
    return ( m_editor && m_editor->hasSelectedText() )
        ? static_cast< int >( qMin( m_editor->selectedText().size(), qsizetype( std::numeric_limits<int>::max() ) ) )
        : 0;
}

int  QTextView::lineCount() const { return qMax( 1, m_cachedLineCount ); }
int  QTextView::currentLine() const { return qMax( 1, m_cachedCurrentLine ); }
int  QTextView::currentColumn() const { return qMax( 1, m_cachedCurrentColumn ); }

// ═══════════════════════════════════════════════════════════
// 검색 / 치환
// ═══════════════════════════════════════════════════════════
void QTextView::findText( const QString& text,
                         bool regex,
                         bool caseSensitive,
                         bool wholeWords,
                         bool forward )
{
    if( !m_editor || text.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    m_editor->findFirst( text, regex, caseSensitive, wholeWords, true, forward );
}

void QTextView::replaceText( const QString& find,
                            const QString& replace,
                            bool regex,
                            bool caseSensitive,
                            bool wholeWords,
                            bool forward )
{
    if( !m_editor || find.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    if( matchesCurrentSelection( m_editor, find, regex, caseSensitive ) )
    {
        m_editor->replace( replace );
        moveCursorPastSelection( m_editor, forward );
        return;
    }

    if( m_editor->findFirst( find, regex, caseSensitive, wholeWords, true, forward ) )
    {
        m_editor->replace( replace );
        moveCursorPastSelection( m_editor, forward );
    }
}

void QTextView::replaceAll( const QString& find,
                           const QString& replace,
                           bool regex,
                           bool caseSensitive,
                           bool wholeWords,
                           bool forward )
{
    if( !m_editor || find.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    moveCursorToSearchBoundary( m_editor, forward );
    while( m_editor->findFirst( find, regex, caseSensitive, wholeWords, false, forward ) )
    {
        m_editor->replace( replace );
        moveCursorPastSelection( m_editor, forward );
    }
}

QTextView::LineEnding QTextView::detectedLineEnding() const { return fromDocumentLineEnding( m_document.lineEnding() ); }

void QTextView::setLineEnding( LineEnding e )
{
    if( fromDocumentLineEnding( m_document.lineEnding() ) == e )
        return;

    m_document.setLineEnding( toDocumentLineEnding( e ) );
    if( !m_editor )
        return;

    m_editor->setLineEnding( toDocumentLineEnding( e ), true );
    m_editor->setModified( true );
    emit statusChanged();
}

bool QTextView::isWordWrapEnabled() const
{
    return m_editorSettings.wordWrap;
}

void QTextView::setWordWrap( bool enabled )
{
    if( m_editorSettings.wordWrap == enabled )
        return;
    m_editorSettings.wordWrap = enabled;
    applyEditorSettings();
}

ScintillaEditorSettings::FontRenderingMode QTextView::fontRenderingMode() const
{
    return m_editorSettings.fontRendering;
}

void QTextView::setFontRenderingMode( ScintillaEditorSettings::FontRenderingMode mode )
{
    if( m_editorSettings.fontRendering == mode )
        return;
    m_editorSettings.fontRendering = mode;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/fontRendering", static_cast< int >( mode ) );
}

bool QTextView::isRulerVisible() const
{
    return m_ruler && m_ruler->isVisible();
}

void QTextView::setRulerVisible( bool visible )
{
    if( m_ruler )
        m_ruler->setVisible( visible );

    AppSettings settings;
    settings.setValue( "textView/showRuler", visible );
}

bool QTextView::isWhitespaceVisible() const
{
    return m_editorSettings.showWhitespace;
}

void QTextView::setWhitespaceVisible( bool visible )
{
    if( m_editorSettings.showWhitespace == visible )
        return;
    m_editorSettings.showWhitespace = visible;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/showWhitespace", visible );
}

bool QTextView::isCodeFoldingEnabled() const
{
    return m_editorSettings.showCodeFolding;
}

void QTextView::setCodeFoldingEnabled( bool enabled )
{
    if( m_editorSettings.showCodeFolding == enabled )
        return;

    m_editorSettings.showCodeFolding = enabled;
    applyEditorSettings();
    if( m_editor )
        m_editor->applyLanguage( m_document.language() );
    if( m_ruler && m_editor )
        m_ruler->setLeftMarginWidth( m_editor->leftMarginWidth() );

    AppSettings settings;
    settings.setValue( "textView/showCodeFolding", enabled );
}

void QTextView::foldAll( const bool contract )
{
    if( !m_editor || !m_editorSettings.showCodeFolding )
        return;

    m_editor->foldAll( contract );
}

bool QTextView::isBraceHighlightEnabled() const
{
    return m_editorSettings.braceMatching;
}

void QTextView::setBraceHighlightEnabled( bool enabled )
{
    if( m_editorSettings.braceMatching == enabled )
        return;

    m_editorSettings.braceMatching = enabled;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/braceHighlight", enabled );
}

ScintillaEditorSettings::ChangeHistoryMode QTextView::changeHistoryMode() const
{
    if( !m_editor )
        return m_editorSettings.changeHistoryMode;

    const int flags = m_editor->changeHistoryFlags();
    const bool hasMarkers = ( flags & 2 ) != 0;
    const bool hasIndicators = ( flags & 4 ) != 0;
    if( hasMarkers && hasIndicators )
        return ScintillaEditorSettings::ChangeHistoryBoth;
    if( hasMarkers )
        return ScintillaEditorSettings::ChangeHistoryMarkers;
    if( hasIndicators )
        return ScintillaEditorSettings::ChangeHistoryIndicators;
    return ScintillaEditorSettings::ChangeHistoryOff;
}

void QTextView::setChangeHistoryMode( ScintillaEditorSettings::ChangeHistoryMode mode )
{
    if( m_editorSettings.changeHistoryMode == mode && ( !m_editor || changeHistoryMode() == mode ) )
        return;

    m_editorSettings.changeHistoryMode = mode;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue( "textView/changeHistoryMode", static_cast< int >( mode ) );
}

void QTextView::setTheme( Theme theme )
{
    QBaseView::setTheme( theme );
    if( m_editor )
    {
        m_editor->applyThemeColors( theme == Theme::Dark );
        // ScintillaEditBase는 자체 렌더링을 사용하므로 Qt stylesheet 제외
        if( m_editor->widget() )
            m_editor->widget()->setStyleSheet( QStringLiteral( "/* no-inherit */" ) );
    }
}

// ═══════════════════════════════════════════════════════════
// 도구모음
// ═══════════════════════════════════════════════════════════
QToolBar* QTextView::createToolBar()
{
    auto* tb = QBaseView::createToolBar();
    if( !m_editor )
        return tb;

    // 언어 선택
    // 주의: 여기서 setSizeAdjustPolicy() 를 부르면 안 된다.
    // QComboBox::setSizeAdjustPolicy -> adjustComboBoxSize -> viewContainer() 로
    // 이어지는데, Qlementine 이 polish 에서 설치한 ComboboxItemViewFilter 가
    // 컨테이너 생성 도중의 ChildAdded 를 받아 view() 를 다시 부르면서 무한 재귀에
    // 빠진다 (0xC00000FD). Qlementine 은 필터를 달기 **전에** 이미
    // AdjustToContents 를 설정해 두므로 우리가 다시 지정할 이유도 없다.
    // (solThemeManager.cpp 의 전역 스타일시트 금지 주석과 같은 상류 버그다.)
    auto* langCombo = new QComboBox( tb );
    langCombo->addItems( TextLexerRegistry::instance().displayNames() );
    langCombo->setCurrentText( m_document.language() );
    langCombo->setMaximumWidth( 200 );
    connect( langCombo, &QComboBox::currentTextChanged, this, &QTextView::setLanguage );
    tb->addWidget( makeToolBarLabel( tb, tr( "언어:" ) ) );
    tb->addWidget( langCombo );

    tb->addWidget( makeToolBarSpacer( tb ) );

    // 인코딩 선택
    auto* encCombo = new QComboBox( tb );
    encCombo->addItems( availableEncodings() );
    encCombo->setCurrentText( m_encoding );
    encCombo->setMaximumWidth( 200 );
    connect( encCombo, &QComboBox::currentTextChanged, this, &QTextView::reloadWithEncoding );
    tb->addWidget( makeToolBarLabel( tb, tr( "인코딩:" ) ) );
    tb->addWidget( encCombo );

    tb->addWidget( makeToolBarSpacer( tb ) );

    // 줄바꿈
    auto* eolCombo = new QComboBox( tb );
    eolCombo->addItem( "CRLF", CRLF );
    eolCombo->addItem( "LF", LF );
    eolCombo->addItem( "CR", CR );
    eolCombo->setCurrentIndex( eolCombo->findData( detectedLineEnding() ) );
    eolCombo->setMaximumWidth( 90 );
    connect( eolCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
            [this, eolCombo]( int idx ) { setLineEnding( static_cast< LineEnding >( eolCombo->itemData( idx ).toInt() ) ); } );
    tb->addWidget( makeToolBarLabel( tb, tr( "줄바꿈:" ) ) );
    tb->addWidget( eolCombo );

    tb->addWidget( makeToolBarSpacer( tb ) );

    // 탭 간격
    auto* tabSpin = new QSpinBox( tb );
    tabSpin->setRange( 1, 16 );
    tabSpin->setValue( tabWidth() );
    tabSpin->setPrefix( tr( "Tab: " ) );
    tabSpin->setToolTip( tr( "탭 간격 (칸)" ) );
    connect( tabSpin, QOverload<int>::of( &QSpinBox::valueChanged ), this, &QTextView::setTabWidth );
    tb->addWidget( tabSpin );

    //tb->addSeparator();

    //// 글꼴
    //connect( MV_CREATE_TOOLBAR_ACTION( tb, "text.font" ),
    //        &QAction::triggered, this, [this] {
    //            bool ok;
    //            QFont font = QFontDialog::getFont( &ok, editorFont(), this );
    //            if( ok ) setEditorFont( font );
    //} );

    //// 읽기 전용
    //tb->addSeparator();
    //auto* wrapAction = MV_CREATE_TOOLBAR_ACTION( tb, "text.wordWrap" );
    //wrapAction->setChecked( isWordWrapEnabled() );
    //connect( wrapAction, &QAction::toggled, this, &QTextView::setWordWrap );

    //auto* foldingAction = MV_CREATE_TOOLBAR_ACTION( tb, "text.codeFolding" );
    //foldingAction->setChecked( isCodeFoldingEnabled() );
    //connect( foldingAction, &QAction::toggled, this, &QTextView::setCodeFoldingEnabled );

    //auto* wsCheck = new QCheckBox( tr( "¶" ), tb );
    //wsCheck->setChecked( isWhitespaceVisible() );
    //wsCheck->setToolTip( tr( "탭/공백/개행 문자 표시" ) );
    //connect( wsCheck, &QCheckBox::toggled, this, &QTextView::setWhitespaceVisible );
    //tb->addWidget( wsCheck );

    //auto* roAction = MV_CREATE_TOOLBAR_ACTION( tb, "text.readOnly" );
    //roAction->setChecked( isReadOnly() );
    //connect( roAction, &QAction::toggled, this, &QTextView::setReadOnly );

    return tb;
}

bool QTextView::promptOpenModeForFile( const QString& filePath, TextFileSession::OpenMode& openMode ) const
{
    const QFileInfo info( filePath );
    if( !TextFileSession::shouldPromptForLargeFile( info.size() ) )
    {
        openMode = TextFileSession::OpenMode::Full;
        return true;
    }

    QMessageBox box( const_cast< QTextView* >( this ) );
    box.setIcon( QMessageBox::Warning );
    box.setWindowTitle( tr( "대용량 파일 열기" ) );
    box.setText( tr( "이 파일은 %1 MB 입니다." ).arg( QString::number( static_cast< double >( info.size() ) / ( 1024.0 * 1024.0 ), 'f', 1 ) ) );
    box.setInformativeText( tr( "전체 로드는 메모리 사용량이 커질 수 있습니다. 제한 모드는 앞부분만 읽기 전용으로 엽니다." ) );

    QPushButton* fullButton = box.addButton( tr( "전체 로드" ), QMessageBox::AcceptRole );
    QPushButton* limitedButton = box.addButton( tr( "제한 모드" ), QMessageBox::ActionRole );
    box.addButton( QMessageBox::Cancel );
    box.exec();

    if( box.clickedButton() == fullButton )
    {
        openMode = TextFileSession::OpenMode::Full;
        return true;
    }
    if( box.clickedButton() == limitedButton )
    {
        openMode = TextFileSession::OpenMode::LimitedPreview;
        return true;
    }
    return false;
}

bool QTextView::ensureEditorBackend()
{
    if( m_editor )
        return true;
    
    m_editor = new ScintillaQtDirectBackend( this, this );
    if( !m_editor || !m_editor->widget() )
    {
        m_editor = nullptr;
        return false;
    }

    if( layout() )
        layout()->addWidget( m_editor->widget() );

    connect( m_editor, &QObject::destroyed, this, [this] {
        m_editor = nullptr;
    } );
    connect( m_editor, &ScintillaQtDirectBackend::modificationChanged,
            this, &QBaseView::sigModifiedChanged );
    connect( m_editor, &ScintillaQtDirectBackend::cursorPositionChanged, this,
            [this]( int line, int index ) {
                m_cachedCurrentLine = line + 1;
                m_cachedCurrentColumn = index + 1;
                emit statusChanged();
                emit sigCursorMoved( m_cachedCurrentLine, m_cachedCurrentColumn );
            } );
    connect( m_editor, &ScintillaQtDirectBackend::charAdded, this, &QTextView::sigCharAdded );
    connect( m_editor, &ScintillaQtDirectBackend::viewportScrolled, this, &QTextView::sigViewportScrolled );
    connect( m_editor, &ScintillaQtDirectBackend::dwellStarted, this,
            [this]( int position, const QPoint& viewportPos ) { handleDwellStart( position, viewportPos ); } );
    connect( m_editor, &ScintillaQtDirectBackend::dwellEnded, this,
            [this] { emit sigRoleHoverEnded(); } );
    connect( m_editor, &ScintillaQtDirectBackend::linesChanged, this, [this] {
        m_cachedLineCount = qMax( 1, m_editor ? m_editor->lineCount() : 1 );
        if( m_ruler && m_editor )
            m_ruler->setLeftMarginWidth( m_editor->leftMarginWidth() );
        emit statusChanged();
    } );
    connect( m_editor, &ScintillaQtDirectBackend::textChanged, this, [this] {
        updateMetrics();
        scheduleHotExitBackup();
        emit statusChanged();

        // 파일을 채우는 중이면 사용자의 편집이 아니다.
        if( m_applyingFileContent )
            return;

        // 진단은 편집 즉시 낡은 정보가 되므로 다음 publish 까지 표시를 지운다.
        if( !m_diagnostics.isEmpty() )
            setDiagnosticMarks( {} );

        emit sigTextEdited();
    } );
    connect( m_editor, &ScintillaQtDirectBackend::selectionChanged, this, [this] {
        updateMetrics();
        emit statusChanged();
        updateCopyAvailability();
    } );

    if( QAbstractScrollArea* area = m_editor->scrollArea() )
    {
        if( QScrollBar* horizontalScrollBar = area->horizontalScrollBar() )
        {
            connect( horizontalScrollBar, &QScrollBar::valueChanged, this, [this, horizontalScrollBar] {
                if( m_ruler )
                {
                    m_ruler->setScrollOffset( horizontalScrollBar->value() );
                    m_ruler->setLeftMarginWidth( m_editor->leftMarginWidth() );
                }
            } );
        }
    }

    // 눈금자에 에디터 폰트 설정
    if( m_ruler )
    {
        m_ruler->setEditorFont( m_editor->editorFont() );
        m_ruler->setLeftMarginWidth( m_editor->leftMarginWidth() );
    }

    applyEditorSettings();
    m_editor->applyThemeColors( currentTheme() == Theme::Dark );
    if( m_editor->widget() )
        m_editor->widget()->setStyleSheet( QStringLiteral( "/* no-inherit */" ) );
    updateMetrics();
    updateCopyAvailability();
    return true;
}

void QTextView::loadPersistedEditorPreferences()
{
    applyPersistedEditorPreferences( m_editorSettings );
}

void QTextView::applyPersistedEditorPreferences( ScintillaEditorSettings& settings ) const
{
    AppSettings persisted;
    settings.tabWidth = qBound( 1, persisted.value( "textView/tabWidth", 4 ).toInt(), 16 );
    settings.useTabs = persisted.value( "textView/useTabs", true ).toBool();
    settings.showIndentationGuides = persisted.value( "textView/showIndentationGuides", true ).toBool();
    const int indentGuideValue = persisted.value( "textView/indentGuideStyle",
                                                 static_cast< int >( ScintillaEditorSettings::IndentGuideReal ) ).toInt();
    settings.indentGuideStyle = static_cast< ScintillaEditorSettings::IndentGuideStyle >(
        qBound( static_cast< int >( ScintillaEditorSettings::IndentGuideReal ),
                indentGuideValue,
                static_cast< int >( ScintillaEditorSettings::IndentGuideLookBoth ) ) );
    settings.showWhitespace = persisted.value( "textView/showWhitespace", false ).toBool();
    settings.showCodeFolding = persisted.value( "textView/showCodeFolding", true ).toBool();
    settings.braceMatching = persisted.value( "textView/braceHighlight", true ).toBool();
    settings.lineSpacingScale = qBound( 1.0, persisted.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 );
    const int fontRenderingValue = persisted.value( "textView/fontRendering",
                                                   static_cast< int >( ScintillaEditorSettings::Antialiased ) ).toInt();
    settings.fontRendering = static_cast< ScintillaEditorSettings::FontRenderingMode >(
        qBound( static_cast< int >( ScintillaEditorSettings::Default ),
                fontRenderingValue,
                static_cast< int >( ScintillaEditorSettings::LcdOptimized ) ) );

    const int changeHistoryValue = persisted.value( "textView/changeHistoryMode",
                                                   static_cast< int >( ScintillaEditorSettings::ChangeHistoryBoth ) ).toInt();
    settings.changeHistoryMode = static_cast< ScintillaEditorSettings::ChangeHistoryMode >(
        qBound( static_cast< int >( ScintillaEditorSettings::ChangeHistoryOff ),
                changeHistoryValue,
                static_cast< int >( ScintillaEditorSettings::ChangeHistoryBoth ) ) );
}

void QTextView::applyEditorSettings()
{
    if( m_editor )
        m_editor->applySettings( m_editorSettings );
}

void QTextView::showGoToLineDialog()
{
    if( !m_editor )
        return;

    QDialog dialog( this );
    dialog.setWindowTitle( tr( "줄/열 이동" ) );
    auto* layout = new QVBoxLayout( &dialog );

    auto* label = new QLabel( tr( "이동할 위치를 줄[:열] 형식으로 입력하세요.\n예: 42 또는 42:5" ), &dialog );
    layout->addWidget( label );

    auto* edit = new QLineEdit( &dialog );
    edit->setText( QStringLiteral( "%1:%2" ).arg( currentLine() ).arg( currentColumn() ) );
    edit->selectAll();
    edit->setPlaceholderText( tr( "줄[:열]" ) );
    layout->addWidget( edit );

    auto* rangeLabel = new QLabel( tr( "현재 문서: 1–%1줄" ).arg( lineCount() ), &dialog );
    layout->addWidget( rangeLabel );

    auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
    layout->addWidget( buttons );

    const QRegularExpression pattern( QStringLiteral( "^\\s*(\\d+)(?:\\s*[:;,]\\s*(\\d+))?\\s*$" ) );
    auto updateOkButton = [edit, buttons, pattern] {
        buttons->button( QDialogButtonBox::Ok )->setEnabled( pattern.match( edit->text() ).hasMatch() );
        };
    connect( edit, &QLineEdit::textChanged, &dialog, updateOkButton );
    connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    updateOkButton();

    if( dialog.exec() != QDialog::Accepted )
        return;

    const auto match = pattern.match( edit->text() );
    if( !match.hasMatch() )
        return;

    const int line = match.captured( 1 ).toInt();
    const int column = match.captured( 2 ).isEmpty() ? 1 : match.captured( 2 ).toInt();
    goToPosition( line, column );
}

void QTextView::updateMetrics()
{
    if( !m_editor )
        return;

    m_cachedLineCount = qMax( 1, m_editor->lineCount() );

    ScintillaDocument::Metrics metrics;
    metrics.lineCount = m_cachedLineCount;
    metrics.currentLine = qMax( 1, m_cachedCurrentLine );
    metrics.currentColumn = qMax( 1, m_cachedCurrentColumn );
    metrics.characterCount = characterCount();
    metrics.selectedCharacterCount = selectedCharacterCount();
    m_document.setMetrics( metrics );
}

ScintillaDocument::LineEnding QTextView::toDocumentLineEnding( LineEnding ending )
{
    switch( ending )
    {
        case LF:
            return ScintillaDocument::LF;
        case CR:
            return ScintillaDocument::CR;
        case CRLF:
        default:
            return ScintillaDocument::CRLF;
    }
}

QTextView::LineEnding QTextView::fromDocumentLineEnding( ScintillaDocument::LineEnding ending )
{
    switch( ending )
    {
        case ScintillaDocument::LF:
            return LF;
        case ScintillaDocument::CR:
            return CR;
        case ScintillaDocument::CRLF:
        default:
            return CRLF;
    }
}

QTextView::LineEnding QTextView::detectLineEnding( const QString& text )
{
    if( text.contains( QStringLiteral( "\r\n" ) ) )
        return CRLF;
    if( text.contains( QLatin1Char( '\r' ) ) )
        return CR;
    return LF;
}

QString QTextView::newHotExitUntitledId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

// ═══════════════════════════════════════════════════════════
// 찾기/바꾸기 위젯
// ═══════════════════════════════════════════════════════════
void QTextView::setupFindWidget()
{
    m_findWidget = new FindReplaceWidget( this );
    m_findWidget->setVisible( false );

    if( auto* vbox = qobject_cast< QVBoxLayout* >( layout() ) )
    {
        // 눈금자(인덱스 0) 다음에 삽입
        vbox->insertWidget( 1, m_findWidget );
    }
    else if( layout() )
    {
        layout()->addWidget( m_findWidget );
    }

    connectFindWidgetSignals();
}

void QTextView::connectFindWidgetSignals()
{
    // 검색 디바운스 타이머 (입력 후 400ms 대기)
    m_searchDebounceTimer = new QTimer( this );
    m_searchDebounceTimer->setSingleShot( true );
    m_searchDebounceTimer->setInterval( 400 );
    connect( m_searchDebounceTimer, &QTimer::timeout, this, &QTextView::performSearch );

    connect( m_findWidget, &FindReplaceWidget::findTextChanged, this, [this]( const QString& ) {
        clearExcludedRanges();
        m_searchDebounceTimer->start(); // 디바운스: 입력 멈춘 후 400ms 뒤 검색
    } );
    connect( m_findWidget, &FindReplaceWidget::findNext, this, &QTextView::performFindNext );
    connect( m_findWidget, &FindReplaceWidget::findPrev, this, &QTextView::performFindPrev );
    connect( m_findWidget, &FindReplaceWidget::replaceRequested, this, &QTextView::performReplace );
    connect( m_findWidget, &FindReplaceWidget::replaceAllRequested, this, &QTextView::performReplaceAll );
    connect( m_findWidget, &FindReplaceWidget::excludeRequested, this, &QTextView::performExclude );
    connect( m_findWidget, &FindReplaceWidget::closed, this, &QTextView::hideFindBar );
    connect( m_findWidget, &FindReplaceWidget::optionsChanged, this, [this] {
        clearExcludedRanges();
        performSearch();
    } );
}

void QTextView::showFindBar( bool replaceMode )
{
    if( !m_findWidget )
        return;

    // 포커스 이동 전에 선택 범위 저장
    m_isSearching = true; // 설정 중 선택 변경 무시
    if( m_editor )
    {
        const int selStart = m_editor->selectionStartPos();
        const int selEnd = m_editor->selectionEndPos();
        if( selStart != selEnd )
        {
            m_findWidget->setSelectionRange( selStart, selEnd );
            m_findWidget->setSearchInSelectionEnabled( true );
            // 시그널 차단하여 optionsChanged → performSearch 연쇄 방지
            const bool blocked = m_findWidget->blockSignals( true );
            m_findWidget->setSearchInSelectionChecked( true );
            m_findWidget->blockSignals( blocked );
        }
        else
        {
            m_findWidget->setSearchInSelectionEnabled( false );
        }
    }

    m_findWidget->setReplaceMode( replaceMode );
    m_findWidget->setVisible( true );
    m_findWidget->focusSearchField();
    m_isSearching = false;
    updateMatchCount();
}

void QTextView::hideFindBar()
{
    if( !m_findWidget )
        return;

    m_findWidget->setVisible( false );
    if( m_searchDebounceTimer )
        m_searchDebounceTimer->stop();
    clearCurrentMatchHighlight();
    clearExcludedRanges();

    // 에디터로 포커스 반환
    if( m_editor && m_editor->widget() )
        m_editor->widget()->setFocus();
}

void QTextView::performSearch()
{
    if( !m_editor || !m_findWidget )
        return;

    const QString text = m_findWidget->searchText();
    if( text.isEmpty() )
    {
        clearCurrentMatchHighlight();
        m_searchResumePos = -1;
        m_findWidget->setMatchCount( 0 );
        return;
    }

    m_isSearching = true;
    clearCurrentMatchHighlight();
    m_searchResumePos = -1;

    updateMatchCount();

    // 자동 스크롤 옵션이 켜져 있으면 첫 번째 매치로 이동
    if( m_findWidget->isAutoScrollToFirst() )
    {
        if( m_findWidget->isSearchInSelection() )
        {
            findInSelectionRange( true, true, true );
        }
        else
        {
            findInRange( 0, m_editor->documentLength(), true, true, true );
        }
    }

    // updateUi 시그널이 이벤트 루프에서 처리될 수 있으므로 지연 해제
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void QTextView::performFindNext()
{
    if( !m_findWidget || !m_editor || m_findWidget->searchText().isEmpty() )
        return;

    m_isSearching = true;

    if( m_findWidget->isSearchInSelection() )
    {
        findInSelectionRange( true, true );
    }
    else
    {
        findInRange( 0, m_editor->documentLength(), true, true );
    }
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void QTextView::performFindPrev()
{
    if( !m_findWidget || !m_editor || m_findWidget->searchText().isEmpty() )
        return;

    m_isSearching = true;

    if( m_findWidget->isSearchInSelection() )
    {
        findInSelectionRange( false, true );
    }
    else
    {
        findInRange( 0, m_editor->documentLength(), false, true );
    }
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void QTextView::performReplace()
{
    if( !m_findWidget || !m_editor )
        return;

    if( m_findWidget->isSearchInSelection() )
    {
        if( !hasCurrentMatch() )
        {
            if( !findInSelectionRange( true, true, true ) )
                return;
        }

        const QString searchText = m_findWidget->searchText();
        if( searchText.isEmpty() )
            return;

        const int flags = ( m_findWidget->isCaseSensitive() ? 0x04 : 0 )
            | ( m_findWidget->isWholeWord() ? 0x02 : 0 );
        const int matchStart = m_currentMatchStart;
        const int matchLength = m_currentMatchLength;
        const int delta = m_findWidget->replaceText().size() - searchText.size();

        m_editor->setSearchTargetRange( matchStart, matchStart + matchLength );
        if( m_editor->searchInTarget( searchText, flags ) >= 0 )
            m_editor->replaceInTarget( m_findWidget->replaceText() );

        const int selectionStart = m_findWidget->selectionRangeStart();
        const int selectionEnd = m_findWidget->selectionRangeEnd();
        if( matchStart < selectionEnd )
            m_findWidget->setSelectionRange( selectionStart, selectionEnd + delta );

        clearCurrentMatchHighlight();
        updateMatchCount();
        performSearch();
        return;
    }

    replaceText( m_findWidget->searchText(),
                m_findWidget->replaceText(),
                false,
                m_findWidget->isCaseSensitive(),
                m_findWidget->isWholeWord(),
                true );
    updateMatchCount();
}

void QTextView::performReplaceAll()
{
    if( !m_findWidget || !m_editor )
        return;

    const QString searchStr = m_findWidget->searchText();
    const QString replaceStr = m_findWidget->replaceText();
    if( searchStr.isEmpty() )
        return;

    const bool caseSensitive = m_findWidget->isCaseSensitive();
    const bool wholeWord = m_findWidget->isWholeWord();

    // 선택 영역에서 검색 모드
    const bool inSelection = m_findWidget->isSearchInSelection();
    const int rangeStart = inSelection ? m_findWidget->selectionRangeStart() : 0;
    const int rangeEnd = inSelection ? m_findWidget->selectionRangeEnd() : m_editor->documentLength();

    int flags = 0;
    if( caseSensitive ) flags |= 0x04; // SCFIND_MATCHCASE
    if( wholeWord )     flags |= 0x02; // SCFIND_WHOLEWORD

    // 제외된 범위를 건너뛰며 역순으로 치환 (오프셋 유지 위해)
    // 먼저 모든 매치 위치를 수집
    QVector<QPair<int, int>> matches;
    int pos = rangeStart;
    while( pos < rangeEnd )
    {
        m_editor->setSearchTargetRange( pos, rangeEnd );
        int found = m_editor->searchInTarget( searchStr, flags );
        if( found < 0 ) break;
        int matchEnd = m_editor->targetEnd();
        int matchLen = matchEnd - found;
        if( matchLen <= 0 ) { pos = found + 1; continue; }

        // 제외 목록에 있는지 확인
        bool excluded = false;
        for( const auto& excl : m_excludedRanges )
        {
            if( found == excl.first && matchLen == excl.second )
            {
                excluded = true;
                break;
            }
        }
        if( !excluded )
        {
            matches.append( { found, matchLen } );
        }
        pos = matchEnd;
    }

    // 역순으로 치환
    for( int i = matches.size() - 1; i >= 0; --i )
    {
        m_editor->setSearchTargetRange( matches[ i ].first, matches[ i ].first + matches[ i ].second );
        m_editor->searchInTarget( searchStr, flags );
        m_editor->replaceInTarget( replaceStr );
    }

    clearCurrentMatchHighlight();
    clearExcludedRanges();
    updateMatchCount();
}

void QTextView::performExclude()
{
    if( !m_editor || !m_findWidget )
        return;

    const bool searchInSelection = m_findWidget->isSearchInSelection();

    int matchStart = -1;
    int len = 0;
    if( hasCurrentMatch() )
    {
        matchStart = m_currentMatchStart;
        len = m_currentMatchLength;
    }
    else
    {
        const int selStart = m_editor->selectionStartPos();
        const int selEnd = m_editor->selectionEndPos();
        if( selStart == selEnd )
            return;
        matchStart = selStart;
        len = selEnd - selStart;
    }

    if( len <= 0 )
        return;

    if( isExcludedRange( matchStart, len ) )
    {
        m_searchResumePos = qMin( m_editor->documentLength(), matchStart + len );
        if( !searchInSelection )
            m_editor->setSelectionByPos( m_searchResumePos, m_searchResumePos );
        clearCurrentMatchHighlight();
        return;
    }

    m_excludedRanges.append( { matchStart, len } );

    // Indicator로 시각 마킹 (더 진한 배경 + 박스 + 취소선)
    m_editor->setIndicatorStyle( kExcludeBackgroundIndicatorId, 8, QColor( 128, 72, 72, 150 ) ); // INDIC_STRAIGHTBOX
    m_editor->applyIndicator( kExcludeBackgroundIndicatorId, matchStart, len );
    m_editor->setIndicatorStyle( kExcludeBorderIndicatorId, 7, QColor( 186, 110, 110, 170 ) ); // INDIC_ROUNDBOX
    m_editor->applyIndicator( kExcludeBorderIndicatorId, matchStart, len );
    m_editor->setIndicatorStyle( kExcludeStrikeIndicatorId, 5, QColor( 210, 210, 210, 255 ) ); // INDIC_STRIKE
    m_editor->applyIndicator( kExcludeStrikeIndicatorId, matchStart, len );
    m_searchResumePos = qMin( m_editor->documentLength(), matchStart + len );
    if( !searchInSelection )
        m_editor->setSelectionByPos( m_searchResumePos, m_searchResumePos );
    clearCurrentMatchHighlight();
    updateExcludedCount();
    updateMatchCount();
}

void QTextView::clearExcludedRanges()
{
    if( !m_editor )
        return;

    if( !m_excludedRanges.isEmpty() )
    {
        m_editor->clearAllIndicator( kExcludeBackgroundIndicatorId );
        m_editor->clearAllIndicator( kExcludeBorderIndicatorId );
        m_editor->clearAllIndicator( kExcludeStrikeIndicatorId );
        m_excludedRanges.clear();
    }

    m_searchResumePos = -1;
    updateExcludedCount();
}

void QTextView::updateExcludedCount()
{
    if( m_findWidget )
        m_findWidget->setExcludedCount( m_excludedRanges.size() );
}

void QTextView::clearCurrentMatchHighlight()
{
    if( !m_editor )
        return;

    if( m_currentMatchStart >= 0 && m_currentMatchLength > 0 )
        m_editor->clearIndicator( kCurrentMatchIndicatorId, m_currentMatchStart, m_currentMatchLength );

    m_currentMatchStart = -1;
    m_currentMatchLength = 0;
}

void QTextView::setCurrentMatchHighlight( int startPos, int length, bool scrollToMatch )
{
    if( !m_editor || length <= 0 )
        return;

    clearCurrentMatchHighlight();
    m_editor->setIndicatorStyle( kCurrentMatchIndicatorId, 7, QColor( 255, 215, 0, 110 ) ); // INDIC_ROUNDBOX
    m_editor->applyIndicator( kCurrentMatchIndicatorId, startPos, length );
    m_currentMatchStart = startPos;
    m_currentMatchLength = length;

    if( scrollToMatch )
        m_editor->scrollRangeToView( startPos, startPos + length );
}

int QTextView::currentSearchFlags() const
{
    if( !m_findWidget )
        return 0;

    int flags = 0;
    if( m_findWidget->isCaseSensitive() )
        flags |= 0x04;
    if( m_findWidget->isWholeWord() )
        flags |= 0x02;
    return flags;
}

bool QTextView::isExcludedRange( int startPos, int length ) const
{
    for( const auto& excludedRange : m_excludedRanges )
    {
        if( excludedRange.first == startPos && excludedRange.second == length )
            return true;
    }
    return false;
}

bool QTextView::findInRange( int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary )
{
    if( !m_editor || !m_findWidget )
        return false;

    const QString text = m_findWidget->searchText();
    const bool searchInSelection = m_findWidget->isSearchInSelection();
    if( text.isEmpty() )
    {
        clearCurrentMatchHighlight();
        m_searchResumePos = -1;
        return false;
    }

    if( rangeStart >= rangeEnd )
    {
        clearCurrentMatchHighlight();
        return false;
    }

    const int flags = currentSearchFlags();
    const auto clampToRange = [rangeStart, rangeEnd]( int pos ) {
        return qBound( rangeStart, pos, rangeEnd );
        };

    auto searchOnce = [&]( int startPos, int endPos ) -> bool {
        int nextStart = startPos;

        while( forward ? ( nextStart < endPos ) : ( nextStart > endPos ) )
        {
            m_editor->setSearchTargetRange( nextStart, endPos );
            const int found = m_editor->searchInTarget( text, flags );
            if( found < 0 )
                return false;

            const int matchEnd = m_editor->targetEnd();
            const int matchLength = matchEnd - found;
            if( matchLength <= 0 )
            {
                nextStart = forward ? qMin( endPos, found + 1 )
                    : qMax( endPos, found - 1 );
                continue;
            }

            if( isExcludedRange( found, matchLength ) )
            {
                nextStart = forward ? qMin( rangeEnd, found + qMax( 1, matchLength ) )
                    : qMax( rangeStart, found - 1 );
                continue;
            }

            if( !searchInSelection )
                m_editor->setSelectionByPos( found, matchEnd );
            setCurrentMatchHighlight( found, matchLength, true );
            m_searchResumePos = -1;
            return true;
        }

        return false;
        };

    const int selectionAnchor = forward ? m_editor->selectionEndPos()
        : m_editor->selectionStartPos();
    int searchFrom = forward ? rangeStart : rangeEnd;
    if( !fromRangeBoundary )
    {
        if( hasCurrentMatch() )
        {
            searchFrom = forward ? qMin( rangeEnd, m_currentMatchStart + qMax( 1, m_currentMatchLength ) )
                : qMax( rangeStart, m_currentMatchStart - 1 );
        }
        else if( m_searchResumePos >= 0 )
        {
            searchFrom = forward ? clampToRange( m_searchResumePos )
                : clampToRange( m_searchResumePos - 1 );
        }
        else
        {
            searchFrom = searchInSelection
                ? ( forward ? rangeStart : rangeEnd )
                : clampToRange( selectionAnchor );
        }
    }

    if( forward )
    {
        if( searchOnce( searchFrom, rangeEnd ) )
            return true;
        if( wrap && searchFrom > rangeStart && searchOnce( rangeStart, searchFrom ) )
            return true;
    }
    else
    {
        if( searchOnce( searchFrom, rangeStart ) )
            return true;
        if( wrap && searchFrom < rangeEnd && searchOnce( rangeEnd, searchFrom ) )
            return true;
    }

    clearCurrentMatchHighlight();
    m_searchResumePos = -1;
    return false;
}

bool QTextView::findInSelectionRange( bool forward, bool wrap, bool fromRangeBoundary )
{
    if( !m_editor || !m_findWidget || !m_findWidget->isSearchInSelection() )
        return false;

    const int rangeStart = m_findWidget->selectionRangeStart();
    const int rangeEnd = m_findWidget->selectionRangeEnd();
    return findInRange( rangeStart, rangeEnd, forward, wrap, fromRangeBoundary );
}

bool QTextView::hasCurrentMatch() const
{
    return m_currentMatchStart >= 0 && m_currentMatchLength > 0;
}

void QTextView::updateMatchCount()
{
    if( !m_findWidget || !m_editor )
        return;

    const QString text = m_findWidget->searchText();
    if( text.isEmpty() )
    {
        m_findWidget->setMatchCount( 0 );
        m_findWidget->setExcludedCount( 0 );
        return;
    }

    int count = 0;
    if( m_findWidget->isSearchInSelection() )
    {
        count = m_editor->countMatchesInRange( text, false,
                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord(),
                    m_findWidget->selectionRangeStart(), m_findWidget->selectionRangeEnd() );
    }
    else
    {
        count = m_editor->countMatches( text, false,
                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord() );
    }
    m_findWidget->setMatchCount( count );
    updateExcludedCount();
}


//
//namespace
//{
//    constexpr int kRulerHeight = 24;
//    constexpr int kRulerMajorStep = 10;
//    constexpr int kRulerMinorStep = 5;
//
//    class TextRulerWidget final : public QWidget
//    {
//    public:
//        explicit TextRulerWidget( QBaseEditor* Owner )
//            : QWidget( Owner )
//            , m_owner( Owner )
//        {
//            setFixedHeight( kRulerHeight );
//            setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
//        }
//
//    protected:
//        void paintEvent( QPaintEvent* Event ) override
//        {
//            QWidget::paintEvent( Event );
//
//            QPainter painter( this );
//            painter.fillRect( rect(), palette().window() );
//            painter.setPen( palette().mid().color() );
//            painter.drawLine( rect().bottomLeft(), rect().bottomRight() );
//
//            const QScintillaEdit* scintilla = m_owner ? m_owner->Scintilla() : nullptr;
//            const int leftMarginWidth = scintilla ? scintilla->LeftMarginWidth() : 0;
//            const QWidget* editorWidget = scintilla ? scintilla->Editor() : nullptr;
//            const QFontMetrics metrics( editorWidget ? editorWidget->font() : font() );
//            const int charWidth = qMax( 1, metrics.horizontalAdvance( QLatin1Char( '0' ) ) );
//            const int xOffset = scintilla ? static_cast< int >( scintilla->Send( SCI_GETXOFFSET ) ) : 0;
//
//            painter.setPen( palette().text().color() );
//            for( int column = 0; ; ++column )
//            {
//                const int x = leftMarginWidth + column * charWidth - xOffset;
//                if( x >= width() )
//                    break;
//                if( x < leftMarginWidth - charWidth )
//                    continue;
//
//                const bool major = column % kRulerMajorStep == 0;
//                const bool minor = column % kRulerMinorStep == 0;
//                const int tickTop = major ? 4 : ( minor ? 10 : 14 );
//                painter.drawLine( x, tickTop, x, height() - 3 );
//
//                if( major )
//                    painter.drawText( x + 2, 2, 40, 12, Qt::AlignLeft | Qt::AlignVCenter, QString::number( column ) );
//            }
//        }
//
//    private:
//        QBaseEditor* m_owner = nullptr;
//    };
//}
//
//bool matchesCurrentSelection( QScintillaEdit* editor,                             const QString& findText,                             const bool regex,                             const bool caseSensitive )
//{
//    if( !editor || regex || !editor->HasSelectedText() )
//        return false;
//
//    return editor->SelectedText().compare( findText,
//                                          caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive ) == 0;
//}
//
//void moveCursorPastSelection( QScintillaEdit* editor, bool forward )
//{
//    if( !editor )
//        return;
//
//    int lineFrom = 0;
//    int indexFrom = 0;
//    int lineTo = 0;
//    int indexTo = 0;
//    editor->GetSelectionRange( lineFrom, indexFrom, lineTo, indexTo );
//    editor->SetCursorPosition( forward ? lineTo : lineFrom,
//                              forward ? indexTo : indexFrom );
//}
//
//void moveCursorToSearchBoundary( QScintillaEdit* editor, bool forward )
//{
//    if( !editor )
//        return;
//
//    if( forward )
//    {
//        editor->SetCursorPosition( 0, 0 );
//        return;
//    }
//
//    editor->SetCursorPosition( qMax( 0, editor->GetLineCount() - 1 ), std::numeric_limits<int>::max() );
//}
//
//QBaseEditor::QBaseEditor( QWidget* Parent )
//    : QBaseView( Parent )
//    , m_scintilla( new QScintillaEdit( this, this ) )
//{
//    auto* layout = new QVBoxLayout( this );
//    layout->setContentsMargins( 0, 0, 0, 0 );
//    layout->setSpacing( 0 );
//    m_rulerWidget = new TextRulerWidget( this );
//    layout->addWidget( m_rulerWidget );
//    layout->addWidget( m_scintilla->Editor() );
//
//    // 찾기/바꾸기 위젯 (눈금자 아래, 에디터 위에 배치)
//    setupFindWidget();
//
//    // 단축키 액션 등록
//    auto* findAction = new QAction( this );
//    findAction->setObjectName( QStringLiteral( "text.find" ) );
//    findAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_F ) );
//    connect( findAction, &QAction::triggered, this, [this] {
//        if( m_findWidget && m_findWidget->isVisible() && !m_findWidget->isReplaceMode() )
//        {
//            HideFindBar();
//        }
//        else
//        {
//            ShowFindBar( false );
//        }
//    } );
//    addAction( findAction );
//
//
//    auto* replaceAction = new QAction( this );
//    replaceAction->setObjectName( QStringLiteral( "text.replace" ) );
//    replaceAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_H ) );
//    connect( replaceAction, &QAction::triggered, this, [this] {
//        if( m_findWidget && m_findWidget->isVisible() && m_findWidget->isReplaceMode() )
//        {
//            HideFindBar();
//        }
//        else
//        {
//            ShowFindBar( true );
//        }
//    } );
//    addAction( replaceAction );
//
//    auto* findNextAction = new QAction( this );
//    findNextAction->setObjectName( QStringLiteral( "text.findNext" ) );
//    findNextAction->setShortcut( QKeySequence( Qt::Key_F3 ) );
//    connect( findNextAction, &QAction::triggered, this, &QBaseEditor::performFindNext );
//    addAction( findNextAction );
//
//    auto* findPrevAction = new QAction( this );
//    findPrevAction->setObjectName( QStringLiteral( "text.findPrev" ) );
//    findPrevAction->setShortcut( QKeySequence( Qt::SHIFT | Qt::Key_F3 ) );
//    connect( findPrevAction, &QAction::triggered, this, &QBaseEditor::performFindPrev );
//    addAction( findPrevAction );
//
//    connect( m_scintilla, &QScintillaEdit::modificationChanged, this, &QBaseEditor::modificationChanged );
//    connect( m_scintilla, &QScintillaEdit::cursorPositionChanged, this, &QBaseEditor::cursorPositionChanged );
//    connect( m_scintilla, &QScintillaEdit::linesChanged, this, &QBaseEditor::linesChanged );
//    connect( m_scintilla, &QScintillaEdit::textChanged, this, &QBaseEditor::textChanged );
//    connect( m_scintilla, &QScintillaEdit::selectionChanged, this, &QBaseEditor::selectionChanged );
//    connect( m_scintilla, &QScintillaEdit::cursorPositionChanged, this, [this]( int, int ) { RefreshRuler(); } );
//    connect( m_scintilla, &QScintillaEdit::linesChanged, this, [this] { RefreshRuler(); } );
//
//    SetRulerVisible( QSettings().value( QStringLiteral( "TextViewer/ShowRulerWidget" ), true ).toBool() );
//}
//
//QBaseEditor::~QBaseEditor() = default;
//
//QScintillaEdit* QBaseEditor::Scintilla() const
//{
//    return m_scintilla;
//}
//
//QWidget* QBaseEditor::EditorWidget() const
//{
//    return m_scintilla ? m_scintilla->Editor() : nullptr;
//}
//
//QString QBaseEditor::FilePath() const
//{
//    return m_filePath;
//}
//
//QString QBaseEditor::NormalizedFilePath() const
//{
//    return m_normalizedFilePath;
//}
//
//QString QBaseEditor::DisplayName() const
//{
//    if( m_filePath.isEmpty() )
//        return tr( "Untitled" );
//
//    const QString fileName = QFileInfo( m_filePath ).fileName();
//    return fileName.isEmpty() ? m_filePath : fileName;
//}
//
//bool QBaseEditor::LoadFile( const QString& FilePath, QString* ErrorMessage )
//{
//    QFile file( FilePath );
//    if( !file.open( QIODevice::ReadOnly ) )
//    {
//        if( ErrorMessage )
//            *ErrorMessage = file.errorString();
//        return false;
//    }
//
//    const QByteArray contents = file.readAll();
//    if( file.error() != QFileDevice::NoError )
//    {
//        if( ErrorMessage )
//            *ErrorMessage = file.errorString();
//        return false;
//    }
//
//    setFilePath( FilePath );
//    if( m_scintilla )
//        m_scintilla->SetText( contents );
//
//    emit modificationChanged( false );
//    return true;
//}
//
//bool QBaseEditor::SaveFile( QString* ErrorMessage )
//{
//    if( m_filePath.isEmpty() )
//    {
//        if( ErrorMessage )
//            *ErrorMessage = tr( "저장할 파일 경로가 없습니다." );
//        return false;
//    }
//
//    return SaveFileAs( m_filePath, ErrorMessage );
//}
//
//bool QBaseEditor::SaveFileAs( const QString& FilePath, QString* ErrorMessage )
//{
//    QFile file( FilePath );
//    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
//    {
//        if( ErrorMessage )
//            *ErrorMessage = file.errorString();
//        return false;
//    }
//
//    const QByteArray contents = m_scintilla ? m_scintilla->Text() : QByteArray();
//    if( file.write( contents ) != contents.size() )
//    {
//        if( ErrorMessage )
//            *ErrorMessage = file.errorString();
//        return false;
//    }
//
//    setFilePath( FilePath );
//    if( m_scintilla )
//        m_scintilla->Send( SCI_SETSAVEPOINT );
//
//    emit modificationChanged( false );
//    return true;
//}
//
//QString QBaseEditor::EditorType() const
//{
//    return QStringLiteral( "QBaseEditor" );
//}
//
//bool QBaseEditor::IsAutoCompletionAvailable() const
//{
//    return m_autoCompletionAvailable;
//}
//
//bool QBaseEditor::IsPreviewAvailable() const
//{
//    return m_previewAvailable;
//}
//
//bool QBaseEditor::IsOutlineAvailable() const
//{
//    return m_outlineAvailable;
//}
//
//bool QBaseEditor::IsDiagnosticsAvailable() const
//{
//    return m_diagnosticsAvailable;
//}
//
//void QBaseEditor::SetRulerVisible( bool Visible )
//{
//    if( m_rulerWidget == nullptr )
//        return;
//
//    m_rulerWidget->setVisible( Visible );
//    RefreshRuler();
//}
//
//bool QBaseEditor::IsRulerVisible() const
//{
//    return m_rulerWidget != nullptr && m_rulerWidget->isVisible();
//}
//
//void QBaseEditor::RefreshRuler()
//{
//    if( m_rulerWidget )
//        m_rulerWidget->update();
//}
//
//void QBaseEditor::SetReadOnly( bool ReadOnly )
//{
//    if( m_scintilla )
//        m_scintilla->SetReadOnly( ReadOnly );
//}
//
//bool QBaseEditor::IsReadOnly() const
//{
//    return m_scintilla ? m_scintilla->IsReadOnly() : false;
//}
//
//void QBaseEditor::ShowFindBar( bool replaceMode )
//{
//    if( !m_findWidget )
//        return;
//
//    // 포커스 이동 전에 선택 범위 저장
//    m_isSearching = true; // 설정 중 선택 변경 무시
//    if( m_scintilla )
//    {
//        const int selStart = m_scintilla->SelectionStartPos();
//        const int selEnd = m_scintilla->SelectionEndPos();
//        if( selStart != selEnd )
//        {
//            m_findWidget->setSelectionRange( selStart, selEnd );
//            m_findWidget->setSearchInSelectionEnabled( true );
//            // 시그널 차단하여 optionsChanged → performSearch 연쇄 방지
//            const bool blocked = m_findWidget->blockSignals( true );
//            m_findWidget->setSearchInSelectionChecked( true );
//            m_findWidget->blockSignals( blocked );
//        }
//        else
//        {
//            m_findWidget->setSearchInSelectionEnabled( false );
//        }
//    }
//
//    m_findWidget->setReplaceMode( replaceMode );
//    m_findWidget->setVisible( true );
//    m_findWidget->focusSearchField();
//    m_isSearching = false;
//    updateMatchCount();
//}
//
//void QBaseEditor::HideFindBar()
//{
//    if( !m_findWidget )
//        return;
//
//    m_findWidget->setVisible( false );
//    if( m_searchDebounceTimer )
//        m_searchDebounceTimer->stop();
//    clearCurrentMatchHighlight();
//    clearExcludedRanges();
//
//    // 에디터로 포커스 반환
//    if( m_scintilla && m_scintilla->Editor() )
//        m_scintilla->Editor()->setFocus();
//}
//
//void QBaseEditor::FindText( const QString& text, bool regex, bool caseSensitive, bool wholeWords, bool forward )
//{
//    if( !m_scintilla || text.isEmpty() )
//        return;
//
//    m_searchOptions.regex = regex;
//    m_searchOptions.caseSensitive = caseSensitive;
//    m_searchOptions.wholeWords = wholeWords;
//    m_searchOptions.forward = forward;
//
//    m_scintilla->FindFirst( text, regex, caseSensitive, wholeWords, true, forward );
//}
//
//void QBaseEditor::ReplaceText( const QString& find, const QString& replace, bool regex, bool caseSensitive, bool wholeWords, bool forward )
//{
//    if( !m_scintilla || find.isEmpty() )
//        return;
//
//    m_searchOptions.regex = regex;
//    m_searchOptions.caseSensitive = caseSensitive;
//    m_searchOptions.wholeWords = wholeWords;
//    m_searchOptions.forward = forward;
//
//    if( matchesCurrentSelection( m_scintilla, find, regex, caseSensitive ) )
//    {
//        m_scintilla->Replace( replace );
//        moveCursorPastSelection( m_scintilla, forward );
//        return;
//    }
//
//    if( m_scintilla->FindFirst( find, regex, caseSensitive, wholeWords, true, forward ) )
//    {
//        m_scintilla->Replace( replace );
//        moveCursorPastSelection( m_scintilla, forward );
//    }
//}
//
//void QBaseEditor::ReplaceAll( const QString& find, const QString& replace, bool regex, bool caseSensitive, bool wholeWords, bool forward )
//{
//    if( !m_scintilla || find.isEmpty() )
//        return;
//
//    m_searchOptions.regex = regex;
//    m_searchOptions.caseSensitive = caseSensitive;
//    m_searchOptions.wholeWords = wholeWords;
//    m_searchOptions.forward = forward;
//
//    moveCursorToSearchBoundary( m_scintilla, forward );
//    while( m_scintilla->FindFirst( find, regex, caseSensitive, wholeWords, false, forward ) )
//    {
//        m_scintilla->Replace( replace );
//        moveCursorPastSelection( m_scintilla, forward );
//    }
//}
//
//void QBaseEditor::SetAutoCompletionAvailable( bool Available )
//{
//    m_autoCompletionAvailable = Available;
//}
//
//void QBaseEditor::SetPreviewAvailable( bool Available )
//{
//    m_previewAvailable = Available;
//}
//
//void QBaseEditor::SetOutlineAvailable( bool Available )
//{
//    m_outlineAvailable = Available;
//}
//
//void QBaseEditor::SetDiagnosticsAvailable( bool Available )
//{
//    m_diagnosticsAvailable = Available;
//}
//
//QString QBaseEditor::normalizeFilePath( const QString& FilePath )
//{
//    const QFileInfo info( FilePath );
//    const QString canonical = info.canonicalFilePath();
//    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
//}
//
//void QBaseEditor::setFilePath( const QString& FilePath )
//{
//    const QString normalized = normalizeFilePath( FilePath );
//    const QString absolute = QFileInfo( FilePath ).absoluteFilePath();
//
//    if( m_filePath == absolute && m_normalizedFilePath == normalized )
//        return;
//
//    m_filePath = absolute;
//    m_normalizedFilePath = normalized;
//    emit filePathChanged( m_filePath );
//}
//
//void QBaseEditor::setupFindWidget()
//{
//    m_findWidget = new FindReplaceWidget( this );
//    m_findWidget->setVisible( false );
//
//    if( auto* vbox = qobject_cast< QVBoxLayout* >( layout() ) )
//    {
//        // 눈금자(인덱스 0) 다음에 삽입
//        vbox->insertWidget( 1, m_findWidget );
//    }
//    else if( layout() )
//    {
//        layout()->addWidget( m_findWidget );
//    }
//
//    connectFindWidgetSignals();
//}
//
//void QBaseEditor::connectFindWidgetSignals()
//{
//    // 검색 디바운스 타이머 (입력 후 400ms 대기)
//    m_searchDebounceTimer = new QTimer( this );
//    m_searchDebounceTimer->setSingleShot( true );
//    m_searchDebounceTimer->setInterval( 400 );
//    connect( m_searchDebounceTimer, &QTimer::timeout, this, &QBaseEditor::performSearch );
//
//    connect( m_findWidget, &FindReplaceWidget::findTextChanged, this, [this]( const QString& ) {
//        clearExcludedRanges();
//        m_searchDebounceTimer->start(); // 디바운스: 입력 멈춘 후 400ms 뒤 검색
//    } );
//    connect( m_findWidget, &FindReplaceWidget::findNext, this, &QBaseEditor::performFindNext );
//    connect( m_findWidget, &FindReplaceWidget::findPrev, this, &QBaseEditor::performFindPrev );
//    connect( m_findWidget, &FindReplaceWidget::replaceRequested, this, &QBaseEditor::performReplace );
//    connect( m_findWidget, &FindReplaceWidget::replaceAllRequested, this, &QBaseEditor::performReplaceAll );
//    connect( m_findWidget, &FindReplaceWidget::excludeRequested, this, &QBaseEditor::performExclude );
//    connect( m_findWidget, &FindReplaceWidget::closed, this, &QBaseEditor::HideFindBar );
//    connect( m_findWidget, &FindReplaceWidget::optionsChanged, this, [this] {
//        clearExcludedRanges();
//        performSearch();
//    } );
//}
//
//void QBaseEditor::performSearch()
//{
//    if( !m_scintilla || !m_findWidget )
//        return;
//
//    const QString text = m_findWidget->searchText();
//    if( text.isEmpty() )
//    {
//        clearCurrentMatchHighlight();
//        m_searchResumePos = -1;
//        m_findWidget->setMatchCount( 0 );
//        return;
//    }
//
//    m_isSearching = true;
//    clearCurrentMatchHighlight();
//    m_searchResumePos = -1;
//
//    updateMatchCount();
//
//    // 자동 스크롤 옵션이 켜져 있으면 첫 번째 매치로 이동
//    if( m_findWidget->isAutoScrollToFirst() )
//    {
//        if( m_findWidget->isSearchInSelection() )
//        {
//            findInSelectionRange( true, true, true );
//        }
//        else
//        {
//            findInRange( 0, m_scintilla->DocumentLength(), true, true, true );
//        }
//    }
//
//    // updateUi 시그널이 이벤트 루프에서 처리될 수 있으므로 지연 해제
//    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
//}
//
//void QBaseEditor::performFindNext()
//{
//    if( !m_findWidget || !m_scintilla || m_findWidget->searchText().isEmpty() )
//        return;
//
//    m_isSearching = true;
//
//    if( m_findWidget->isSearchInSelection() )
//    {
//        findInSelectionRange( true, true );
//    }
//    else
//    {
//        findInRange( 0, m_scintilla->DocumentLength(), true, true );
//    }
//    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
//}
//
//void QBaseEditor::performFindPrev()
//{
//    if( !m_findWidget || !m_scintilla || m_findWidget->searchText().isEmpty() )
//        return;
//
//    m_isSearching = true;
//
//    if( m_findWidget->isSearchInSelection() )
//    {
//        findInSelectionRange( false, true );
//    }
//    else
//    {
//        findInRange( 0, m_scintilla->DocumentLength(), false, true );
//    }
//    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
//}
//
//void QBaseEditor::performReplace()
//{
//    if( !m_findWidget || !m_scintilla )
//        return;
//
//    if( m_findWidget->isSearchInSelection() )
//    {
//        if( !hasCurrentMatch() )
//        {
//            if( !findInSelectionRange( true, true, true ) )
//                return;
//        }
//
//        const QString searchText = m_findWidget->searchText();
//        if( searchText.isEmpty() )
//            return;
//
//        const int flags = ( m_findWidget->isCaseSensitive() ? 0x04 : 0 )
//            | ( m_findWidget->isWholeWord() ? 0x02 : 0 );
//        const int matchStart = m_currentMatchStart;
//        const int matchLength = m_currentMatchLength;
//        const int delta = m_findWidget->replaceText().size() - searchText.size();
//
//        m_scintilla->SetSearchTargetRange( matchStart, matchStart + matchLength );
//        if( m_scintilla->SearchInTarget( searchText, flags ) >= 0 )
//            m_scintilla->ReplaceInTarget( m_findWidget->replaceText() );
//
//        const int selectionStart = m_findWidget->selectionRangeStart();
//        const int selectionEnd = m_findWidget->selectionRangeEnd();
//        if( matchStart < selectionEnd )
//            m_findWidget->setSelectionRange( selectionStart, selectionEnd + delta );
//
//        clearCurrentMatchHighlight();
//        updateMatchCount();
//        performSearch();
//        return;
//    }
//
//    ReplaceText( m_findWidget->searchText(),
//                m_findWidget->replaceText(),
//                false,
//                m_findWidget->isCaseSensitive(),
//                m_findWidget->isWholeWord(),
//                true );
//    updateMatchCount();
//}
//
//void QBaseEditor::performReplaceAll()
//{
//    if( !m_findWidget || !m_scintilla )
//        return;
//
//    const QString searchStr = m_findWidget->searchText();
//    const QString replaceStr = m_findWidget->replaceText();
//    if( searchStr.isEmpty() )
//        return;
//
//    const bool caseSensitive = m_findWidget->isCaseSensitive();
//    const bool wholeWord = m_findWidget->isWholeWord();
//
//    // 선택 영역에서 검색 모드
//    const bool inSelection = m_findWidget->isSearchInSelection();
//    const int rangeStart = inSelection ? m_findWidget->selectionRangeStart() : 0;
//    const int rangeEnd = inSelection ? m_findWidget->selectionRangeEnd() : m_scintilla->DocumentLength();
//
//    int flags = 0;
//    if( caseSensitive ) flags |= 0x04; // SCFIND_MATCHCASE
//    if( wholeWord )     flags |= 0x02; // SCFIND_WHOLEWORD
//
//    // 제외된 범위를 건너뛰며 역순으로 치환 (오프셋 유지 위해)
//    // 먼저 모든 매치 위치를 수집
//    QVector<QPair<int, int>> matches;
//    int pos = rangeStart;
//    while( pos < rangeEnd )
//    {
//        m_scintilla->SetSearchTargetRange( pos, rangeEnd );
//        int found = m_scintilla->SearchInTarget( searchStr, flags );
//        if( found < 0 ) break;
//        int matchEnd = m_scintilla->TargetEnd();
//        int matchLen = matchEnd - found;
//        if( matchLen <= 0 ) { pos = found + 1; continue; }
//
//        // 제외 목록에 있는지 확인
//        bool excluded = false;
//        for( const auto& excl : m_excludedRanges )
//        {
//            if( found == excl.first && matchLen == excl.second )
//            {
//                excluded = true;
//                break;
//            }
//        }
//        if( !excluded )
//        {
//            matches.append( { found, matchLen } );
//        }
//        pos = matchEnd;
//    }
//
//    // 역순으로 치환
//    for( int i = matches.size() - 1; i >= 0; --i )
//    {
//        m_scintilla->SetSearchTargetRange( matches[ i ].first, matches[ i ].first + matches[ i ].second );
//        m_scintilla->SearchInTarget( searchStr, flags );
//        m_scintilla->ReplaceInTarget( replaceStr );
//    }
//
//    clearCurrentMatchHighlight();
//    clearExcludedRanges();
//    updateMatchCount();
//}
//
//void QBaseEditor::performExclude()
//{
//    if( !m_scintilla || !m_findWidget )
//        return;
//
//    const bool searchInSelection = m_findWidget->isSearchInSelection();
//
//    int matchStart = -1;
//    int len = 0;
//    if( hasCurrentMatch() )
//    {
//        matchStart = m_currentMatchStart;
//        len = m_currentMatchLength;
//    }
//    else
//    {
//        const int selStart = m_scintilla->SelectionStartPos();
//        const int selEnd = m_scintilla->SelectionEndPos();
//        if( selStart == selEnd )
//            return;
//        matchStart = selStart;
//        len = selEnd - selStart;
//    }
//
//    if( len <= 0 )
//        return;
//
//    if( isExcludedRange( matchStart, len ) )
//    {
//        m_searchResumePos = qMin( m_scintilla->DocumentLength(), matchStart + len );
//        if( !searchInSelection )
//            m_scintilla->SetSelectionByPos( m_searchResumePos, m_searchResumePos );
//        clearCurrentMatchHighlight();
//        return;
//    }
//
//    m_excludedRanges.append( { matchStart, len } );
//
//    // Indicator로 시각 마킹 (더 진한 배경 + 박스 + 취소선)
//    m_scintilla->SetIndicatorStyle( kExcludeBackgroundIndicatorId, 8, QColor( 128, 72, 72, 150 ) ); // INDIC_STRAIGHTBOX
//    m_scintilla->ApplyIndicator( kExcludeBackgroundIndicatorId, matchStart, len );
//    m_scintilla->SetIndicatorStyle( kExcludeBorderIndicatorId, 7, QColor( 186, 110, 110, 170 ) ); // INDIC_ROUNDBOX
//    m_scintilla->ApplyIndicator( kExcludeBorderIndicatorId, matchStart, len );
//    m_scintilla->SetIndicatorStyle( kExcludeStrikeIndicatorId, 5, QColor( 210, 210, 210, 255 ) ); // INDIC_STRIKE
//    m_scintilla->ApplyIndicator( kExcludeStrikeIndicatorId, matchStart, len );
//    m_searchResumePos = qMin( m_scintilla->DocumentLength(), matchStart + len );
//    if( !searchInSelection )
//        m_scintilla->SetSelectionByPos( m_searchResumePos, m_searchResumePos );
//    clearCurrentMatchHighlight();
//    updateExcludedCount();
//    updateMatchCount();
//}
//
//void QBaseEditor::clearExcludedRanges()
//{
//    if( !m_scintilla )
//        return;
//
//    if( !m_excludedRanges.isEmpty() )
//    {
//        m_scintilla->ClearAllIndicator( kExcludeBackgroundIndicatorId );
//        m_scintilla->ClearAllIndicator( kExcludeBorderIndicatorId );
//        m_scintilla->ClearAllIndicator( kExcludeStrikeIndicatorId );
//        m_excludedRanges.clear();
//    }
//
//    m_searchResumePos = -1;
//    updateExcludedCount();
//}
//
//void QBaseEditor::updateExcludedCount()
//{
//    if( m_findWidget )
//        m_findWidget->setExcludedCount( m_excludedRanges.size() );
//}
//
//void QBaseEditor::clearCurrentMatchHighlight()
//{
//    if( !m_scintilla )
//        return;
//
//    if( m_currentMatchStart >= 0 && m_currentMatchLength > 0 )
//        m_scintilla->ClearIndicator( kCurrentMatchIndicatorId, m_currentMatchStart, m_currentMatchLength );
//
//    m_currentMatchStart = -1;
//    m_currentMatchLength = 0;
//}
//
//void QBaseEditor::setCurrentMatchHighlight( int startPos, int length, bool scrollToMatch )
//{
//    if( !m_scintilla || length <= 0 )
//        return;
//
//    clearCurrentMatchHighlight();
//    m_scintilla->SetIndicatorStyle( kCurrentMatchIndicatorId, 7, QColor( 255, 215, 0, 110 ) ); // INDIC_ROUNDBOX
//    m_scintilla->ApplyIndicator( kCurrentMatchIndicatorId, startPos, length );
//    m_currentMatchStart = startPos;
//    m_currentMatchLength = length;
//
//    if( scrollToMatch )
//        m_scintilla->ScrollRangeToView( startPos, startPos + length );
//}
//
//int QBaseEditor::currentSearchFlags() const
//{
//    if( !m_findWidget )
//        return 0;
//
//    int flags = 0;
//    if( m_findWidget->isCaseSensitive() )
//        flags |= 0x04;
//    if( m_findWidget->isWholeWord() )
//        flags |= 0x02;
//    return flags;
//}
//
//bool QBaseEditor::isExcludedRange( int startPos, int length ) const
//{
//    for( const auto& excludedRange : m_excludedRanges )
//    {
//        if( excludedRange.first == startPos && excludedRange.second == length )
//            return true;
//    }
//    return false;
//}
//
//bool QBaseEditor::findInRange( int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary )
//{
//    if( !m_scintilla || !m_findWidget )
//        return false;
//
//    const QString text = m_findWidget->searchText();
//    const bool searchInSelection = m_findWidget->isSearchInSelection();
//    if( text.isEmpty() )
//    {
//        clearCurrentMatchHighlight();
//        m_searchResumePos = -1;
//        return false;
//    }
//
//    if( rangeStart >= rangeEnd )
//    {
//        clearCurrentMatchHighlight();
//        return false;
//    }
//
//    const int flags = currentSearchFlags();
//    const auto clampToRange = [rangeStart, rangeEnd]( int pos ) {
//        return qBound( rangeStart, pos, rangeEnd );
//        };
//
//    auto searchOnce = [&]( int startPos, int endPos ) -> bool {
//        int nextStart = startPos;
//
//        while( forward ? ( nextStart < endPos ) : ( nextStart > endPos ) )
//        {
//            m_scintilla->SetSearchTargetRange( nextStart, endPos );
//            const int found = m_scintilla->SearchInTarget( text, flags );
//            if( found < 0 )
//                return false;
//
//            const int matchEnd = m_scintilla->TargetEnd();
//            const int matchLength = matchEnd - found;
//            if( matchLength <= 0 )
//            {
//                nextStart = forward ? qMin( endPos, found + 1 )
//                    : qMax( endPos, found - 1 );
//                continue;
//            }
//
//            if( isExcludedRange( found, matchLength ) )
//            {
//                nextStart = forward ? qMin( rangeEnd, found + qMax( 1, matchLength ) )
//                    : qMax( rangeStart, found - 1 );
//                continue;
//            }
//
//            if( !searchInSelection )
//                m_scintilla->SetSelectionByPos( found, matchEnd );
//            setCurrentMatchHighlight( found, matchLength, true );
//            m_searchResumePos = -1;
//            return true;
//        }
//
//        return false;
//        };
//
//    const int selectionAnchor = forward ? m_scintilla->SelectionEndPos()
//        : m_scintilla->SelectionStartPos();
//    int searchFrom = forward ? rangeStart : rangeEnd;
//    if( !fromRangeBoundary )
//    {
//        if( hasCurrentMatch() )
//        {
//            searchFrom = forward ? qMin( rangeEnd, m_currentMatchStart + qMax( 1, m_currentMatchLength ) )
//                : qMax( rangeStart, m_currentMatchStart - 1 );
//        }
//        else if( m_searchResumePos >= 0 )
//        {
//            searchFrom = forward ? clampToRange( m_searchResumePos )
//                : clampToRange( m_searchResumePos - 1 );
//        }
//        else
//        {
//            searchFrom = searchInSelection
//                ? ( forward ? rangeStart : rangeEnd )
//                : clampToRange( selectionAnchor );
//        }
//    }
//
//    if( forward )
//    {
//        if( searchOnce( searchFrom, rangeEnd ) )
//            return true;
//        if( wrap && searchFrom > rangeStart && searchOnce( rangeStart, searchFrom ) )
//            return true;
//    }
//    else
//    {
//        if( searchOnce( searchFrom, rangeStart ) )
//            return true;
//        if( wrap && searchFrom < rangeEnd && searchOnce( rangeEnd, searchFrom ) )
//            return true;
//    }
//
//    clearCurrentMatchHighlight();
//    m_searchResumePos = -1;
//    return false;
//}
//
//bool QBaseEditor::findInSelectionRange( bool forward, bool wrap, bool fromRangeBoundary )
//{
//    if( !m_scintilla || !m_findWidget || !m_findWidget->isSearchInSelection() )
//        return false;
//
//    const int rangeStart = m_findWidget->selectionRangeStart();
//    const int rangeEnd = m_findWidget->selectionRangeEnd();
//    return findInRange( rangeStart, rangeEnd, forward, wrap, fromRangeBoundary );
//}
//
//bool QBaseEditor::hasCurrentMatch() const
//{
//    return m_currentMatchStart >= 0 && m_currentMatchLength > 0;
//}
//
//void QBaseEditor::updateMatchCount()
//{
//    if( !m_findWidget || !m_scintilla )
//        return;
//
//    const QString text = m_findWidget->searchText();
//    if( text.isEmpty() )
//    {
//        m_findWidget->setMatchCount( 0 );
//        m_findWidget->setExcludedCount( 0 );
//        return;
//    }
//
//    int count = 0;
//    if( m_findWidget->isSearchInSelection() )
//    {
//        count = m_scintilla->CountMatchesInRange( text, false,
//                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord(),
//                    m_findWidget->selectionRangeStart(), m_findWidget->selectionRangeEnd() );
//    }
//    else
//    {
//        count = m_scintilla->CountMatches( text, false, m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord() );
//    }
//    m_findWidget->setMatchCount( count );
//    updateExcludedCount();
//}
