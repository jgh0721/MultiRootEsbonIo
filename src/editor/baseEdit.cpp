#include "stdafx.h"
#include "baseEdit.hpp"
#include "FindReplaceWidget.hpp"

namespace
{
    constexpr int kRulerHeight = 24;
    constexpr int kRulerMajorStep = 10;
    constexpr int kRulerMinorStep = 5;

    class TextRulerWidget final : public QWidget
    {
    public:
        explicit TextRulerWidget( BaseEdit* Owner )
            : QWidget( Owner )
            , m_owner( Owner )
        {
            setFixedHeight( kRulerHeight );
            setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
        }

    protected:
        void paintEvent( QPaintEvent* Event ) override
        {
            QWidget::paintEvent( Event );

            QPainter painter( this );
            painter.fillRect( rect(), palette().window() );
            painter.setPen( palette().mid().color() );
            painter.drawLine( rect().bottomLeft(), rect().bottomRight() );

            const QScintillaEdit* scintilla = m_owner ? m_owner->Scintilla() : nullptr;
            const int leftMarginWidth = scintilla ? scintilla->LeftMarginWidth() : 0;
            const QWidget* editorWidget = scintilla ? scintilla->Editor() : nullptr;
            const QFontMetrics metrics( editorWidget ? editorWidget->font() : font() );
            const int charWidth = qMax( 1, metrics.horizontalAdvance( QLatin1Char( '0' ) ) );
            const int xOffset = scintilla ? static_cast< int >( scintilla->Send( SCI_GETXOFFSET ) ) : 0;

            painter.setPen( palette().text().color() );
            for( int column = 0; ; ++column )
            {
                const int x = leftMarginWidth + column * charWidth - xOffset;
                if( x >= width() )
                    break;
                if( x < leftMarginWidth - charWidth )
                    continue;

                const bool major = column % kRulerMajorStep == 0;
                const bool minor = column % kRulerMinorStep == 0;
                const int tickTop = major ? 4 : ( minor ? 10 : 14 );
                painter.drawLine( x, tickTop, x, height() - 3 );

                if( major )
                    painter.drawText( x + 2, 2, 40, 12, Qt::AlignLeft | Qt::AlignVCenter, QString::number( column ) );
            }
        }

    private:
        BaseEdit* m_owner = nullptr;
    };
}

bool matchesCurrentSelection( QScintillaEdit* editor,                             const QString& findText,                             const bool regex,                             const bool caseSensitive )
{
    if( !editor || regex || !editor->HasSelectedText() )
        return false;

    return editor->SelectedText().compare( findText,
                                          caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive ) == 0;
}

void moveCursorPastSelection( QScintillaEdit* editor, bool forward )
{
    if( !editor )
        return;

    int lineFrom = 0;
    int indexFrom = 0;
    int lineTo = 0;
    int indexTo = 0;
    editor->GetSelectionRange( lineFrom, indexFrom, lineTo, indexTo );
    editor->SetCursorPosition( forward ? lineTo : lineFrom,
                              forward ? indexTo : indexFrom );
}

void moveCursorToSearchBoundary( QScintillaEdit* editor, bool forward )
{
    if( !editor )
        return;

    if( forward )
    {
        editor->SetCursorPosition( 0, 0 );
        return;
    }

    editor->SetCursorPosition( qMax( 0, editor->GetLineCount() - 1 ), std::numeric_limits<int>::max() );
}

BaseEdit::BaseEdit( QWidget* Parent )
    : QWidget( Parent )
    , m_scintilla( new QScintillaEdit( this, this ) )
{
    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 0 );
    m_rulerWidget = new TextRulerWidget( this );
    layout->addWidget( m_rulerWidget );
    layout->addWidget( m_scintilla->Editor() );

    // 찾기/바꾸기 위젯 (눈금자 아래, 에디터 위에 배치)
    setupFindWidget();

    // 단축키 액션 등록
    auto* findAction = new QAction( this );
    findAction->setObjectName( QStringLiteral( "text.find" ) );
    findAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_F ) );
    connect( findAction, &QAction::triggered, this, [this] {
        if( m_findWidget && m_findWidget->isVisible() && !m_findWidget->isReplaceMode() )
        {
            HideFindBar();
        }
        else
        {
            ShowFindBar( false );
        }
    } );
    addAction( findAction );


    auto* replaceAction = new QAction( this );
    replaceAction->setObjectName( QStringLiteral( "text.replace" ) );
    replaceAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_H ) );
    connect( replaceAction, &QAction::triggered, this, [this] {
        if( m_findWidget && m_findWidget->isVisible() && m_findWidget->isReplaceMode() )
        {
            HideFindBar();
        }
        else
        {
            ShowFindBar( true );
        }
    } );
    addAction( replaceAction );

    auto* findNextAction = new QAction( this );
    findNextAction->setObjectName( QStringLiteral( "text.findNext" ) );
    findNextAction->setShortcut( QKeySequence( Qt::Key_F3 ) );
    connect( findNextAction, &QAction::triggered, this, &BaseEdit::performFindNext );
    addAction( findNextAction );

    auto* findPrevAction = new QAction( this );
    findPrevAction->setObjectName( QStringLiteral( "text.findPrev" ) );
    findPrevAction->setShortcut( QKeySequence( Qt::SHIFT | Qt::Key_F3 ) );
    connect( findPrevAction, &QAction::triggered, this, &BaseEdit::performFindPrev );
    addAction( findPrevAction );

    connect( m_scintilla, &QScintillaEdit::modificationChanged, this, &BaseEdit::modificationChanged );
    connect( m_scintilla, &QScintillaEdit::cursorPositionChanged, this, &BaseEdit::cursorPositionChanged );
    connect( m_scintilla, &QScintillaEdit::linesChanged, this, &BaseEdit::linesChanged );
    connect( m_scintilla, &QScintillaEdit::textChanged, this, &BaseEdit::textChanged );
    connect( m_scintilla, &QScintillaEdit::selectionChanged, this, &BaseEdit::selectionChanged );
    connect( m_scintilla, &QScintillaEdit::cursorPositionChanged, this, [this]( int, int ) { RefreshRuler(); } );
    connect( m_scintilla, &QScintillaEdit::linesChanged, this, [this] { RefreshRuler(); } );

    SetRulerVisible( QSettings().value( QStringLiteral( "TextViewer/ShowRulerWidget" ), true ).toBool() );
}

BaseEdit::~BaseEdit() = default;

QScintillaEdit* BaseEdit::Scintilla() const
{
    return m_scintilla;
}

QWidget* BaseEdit::EditorWidget() const
{
    return m_scintilla ? m_scintilla->Editor() : nullptr;
}

QString BaseEdit::FilePath() const
{
    return m_filePath;
}

QString BaseEdit::NormalizedFilePath() const
{
    return m_normalizedFilePath;
}

QString BaseEdit::DisplayName() const
{
    if( m_filePath.isEmpty() )
        return tr( "Untitled" );

    const QString fileName = QFileInfo( m_filePath ).fileName();
    return fileName.isEmpty() ? m_filePath : fileName;
}

bool BaseEdit::LoadFile( const QString& FilePath, QString* ErrorMessage )
{
    QFile file( FilePath );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    const QByteArray contents = file.readAll();
    if( file.error() != QFileDevice::NoError )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    setFilePath( FilePath );
    if( m_scintilla )
        m_scintilla->SetText( contents );

    emit modificationChanged( false );
    return true;
}

bool BaseEdit::SaveFile( QString* ErrorMessage )
{
    if( m_filePath.isEmpty() )
    {
        if( ErrorMessage )
            *ErrorMessage = tr( "저장할 파일 경로가 없습니다." );
        return false;
    }

    return SaveFileAs( m_filePath, ErrorMessage );
}

bool BaseEdit::SaveFileAs( const QString& FilePath, QString* ErrorMessage )
{
    QFile file( FilePath );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    const QByteArray contents = m_scintilla ? m_scintilla->Text() : QByteArray();
    if( file.write( contents ) != contents.size() )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    setFilePath( FilePath );
    if( m_scintilla )
        m_scintilla->Send( SCI_SETSAVEPOINT );

    emit modificationChanged( false );
    return true;
}

QString BaseEdit::EditorType() const
{
    return QStringLiteral( "BaseEdit" );
}

bool BaseEdit::IsAutoCompletionAvailable() const
{
    return m_autoCompletionAvailable;
}

bool BaseEdit::IsPreviewAvailable() const
{
    return m_previewAvailable;
}

bool BaseEdit::IsOutlineAvailable() const
{
    return m_outlineAvailable;
}

bool BaseEdit::IsDiagnosticsAvailable() const
{
    return m_diagnosticsAvailable;
}

void BaseEdit::SetRulerVisible( bool Visible )
{
    if( m_rulerWidget == nullptr )
        return;

    m_rulerWidget->setVisible( Visible );
    RefreshRuler();
}

bool BaseEdit::IsRulerVisible() const
{
    return m_rulerWidget != nullptr && m_rulerWidget->isVisible();
}

void BaseEdit::RefreshRuler()
{
    if( m_rulerWidget )
        m_rulerWidget->update();
}

void BaseEdit::SetReadOnly( bool ReadOnly )
{
    if( m_scintilla )
        m_scintilla->SetReadOnly( ReadOnly );
}

bool BaseEdit::IsReadOnly() const
{
    return m_scintilla ? m_scintilla->IsReadOnly() : false;
}

void BaseEdit::ShowFindBar( bool replaceMode )
{
    if( !m_findWidget )
        return;

    // 포커스 이동 전에 선택 범위 저장
    m_isSearching = true; // 설정 중 선택 변경 무시
    if( m_scintilla )
    {
        const int selStart = m_scintilla->SelectionStartPos();
        const int selEnd = m_scintilla->SelectionEndPos();
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

void BaseEdit::HideFindBar()
{
    if( !m_findWidget )
        return;

    m_findWidget->setVisible( false );
    if( m_searchDebounceTimer )
        m_searchDebounceTimer->stop();
    clearCurrentMatchHighlight();
    clearExcludedRanges();

    // 에디터로 포커스 반환
    if( m_scintilla && m_scintilla->Editor() )
        m_scintilla->Editor()->setFocus();
}

void BaseEdit::FindText( const QString& text, bool regex, bool caseSensitive, bool wholeWords, bool forward )
{
    if( !m_scintilla || text.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    m_scintilla->FindFirst( text, regex, caseSensitive, wholeWords, true, forward );
}

void BaseEdit::ReplaceText( const QString& find, const QString& replace, bool regex, bool caseSensitive, bool wholeWords, bool forward )
{
    if( !m_scintilla || find.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    if( matchesCurrentSelection( m_scintilla, find, regex, caseSensitive ) )
    {
        m_scintilla->Replace( replace );
        moveCursorPastSelection( m_scintilla, forward );
        return;
    }

    if( m_scintilla->FindFirst( find, regex, caseSensitive, wholeWords, true, forward ) )
    {
        m_scintilla->Replace( replace );
        moveCursorPastSelection( m_scintilla, forward );
    }
}

void BaseEdit::ReplaceAll( const QString& find, const QString& replace, bool regex, bool caseSensitive, bool wholeWords, bool forward )
{
    if( !m_scintilla || find.isEmpty() )
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    moveCursorToSearchBoundary( m_scintilla, forward );
    while( m_scintilla->FindFirst( find, regex, caseSensitive, wholeWords, false, forward ) )
    {
        m_scintilla->Replace( replace );
        moveCursorPastSelection( m_scintilla, forward );
    }
}

void BaseEdit::SetAutoCompletionAvailable( bool Available )
{
    m_autoCompletionAvailable = Available;
}

void BaseEdit::SetPreviewAvailable( bool Available )
{
    m_previewAvailable = Available;
}

void BaseEdit::SetOutlineAvailable( bool Available )
{
    m_outlineAvailable = Available;
}

void BaseEdit::SetDiagnosticsAvailable( bool Available )
{
    m_diagnosticsAvailable = Available;
}

QString BaseEdit::normalizeFilePath( const QString& FilePath )
{
    const QFileInfo info( FilePath );
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
}

void BaseEdit::setFilePath( const QString& FilePath )
{
    const QString normalized = normalizeFilePath( FilePath );
    const QString absolute = QFileInfo( FilePath ).absoluteFilePath();

    if( m_filePath == absolute && m_normalizedFilePath == normalized )
        return;

    m_filePath = absolute;
    m_normalizedFilePath = normalized;
    emit filePathChanged( m_filePath );
}

void BaseEdit::setupFindWidget()
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

void BaseEdit::connectFindWidgetSignals()
{
    // 검색 디바운스 타이머 (입력 후 400ms 대기)
    m_searchDebounceTimer = new QTimer( this );
    m_searchDebounceTimer->setSingleShot( true );
    m_searchDebounceTimer->setInterval( 400 );
    connect( m_searchDebounceTimer, &QTimer::timeout, this, &BaseEdit::performSearch );

    connect( m_findWidget, &FindReplaceWidget::findTextChanged, this, [this]( const QString& ) {
        clearExcludedRanges();
        m_searchDebounceTimer->start(); // 디바운스: 입력 멈춘 후 400ms 뒤 검색
    } );
    connect( m_findWidget, &FindReplaceWidget::findNext, this, &BaseEdit::performFindNext );
    connect( m_findWidget, &FindReplaceWidget::findPrev, this, &BaseEdit::performFindPrev );
    connect( m_findWidget, &FindReplaceWidget::replaceRequested, this, &BaseEdit::performReplace );
    connect( m_findWidget, &FindReplaceWidget::replaceAllRequested, this, &BaseEdit::performReplaceAll );
    connect( m_findWidget, &FindReplaceWidget::excludeRequested, this, &BaseEdit::performExclude );
    connect( m_findWidget, &FindReplaceWidget::closed, this, &BaseEdit::HideFindBar );
    connect( m_findWidget, &FindReplaceWidget::optionsChanged, this, [this] {
        clearExcludedRanges();
        performSearch();
    } );
}

void BaseEdit::performSearch()
{
    if( !m_scintilla || !m_findWidget )
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
            findInRange( 0, m_scintilla->DocumentLength(), true, true, true );
        }
    }

    // updateUi 시그널이 이벤트 루프에서 처리될 수 있으므로 지연 해제
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void BaseEdit::performFindNext()
{
    if( !m_findWidget || !m_scintilla || m_findWidget->searchText().isEmpty() )
        return;

    m_isSearching = true;

    if( m_findWidget->isSearchInSelection() )
    {
        findInSelectionRange( true, true );
    }
    else
    {
        findInRange( 0, m_scintilla->DocumentLength(), true, true );
    }
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void BaseEdit::performFindPrev()
{
    if( !m_findWidget || !m_scintilla || m_findWidget->searchText().isEmpty() )
        return;

    m_isSearching = true;

    if( m_findWidget->isSearchInSelection() )
    {
        findInSelectionRange( false, true );
    }
    else
    {
        findInRange( 0, m_scintilla->DocumentLength(), false, true );
    }
    QTimer::singleShot( 0, this, [this] { m_isSearching = false; } );
}

void BaseEdit::performReplace()
{
    if( !m_findWidget || !m_scintilla )
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

        m_scintilla->SetSearchTargetRange( matchStart, matchStart + matchLength );
        if( m_scintilla->SearchInTarget( searchText, flags ) >= 0 )
            m_scintilla->ReplaceInTarget( m_findWidget->replaceText() );

        const int selectionStart = m_findWidget->selectionRangeStart();
        const int selectionEnd = m_findWidget->selectionRangeEnd();
        if( matchStart < selectionEnd )
            m_findWidget->setSelectionRange( selectionStart, selectionEnd + delta );

        clearCurrentMatchHighlight();
        updateMatchCount();
        performSearch();
        return;
    }

    ReplaceText( m_findWidget->searchText(),
                m_findWidget->replaceText(),
                false,
                m_findWidget->isCaseSensitive(),
                m_findWidget->isWholeWord(),
                true );
    updateMatchCount();
}

void BaseEdit::performReplaceAll()
{
    if( !m_findWidget || !m_scintilla )
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
    const int rangeEnd = inSelection ? m_findWidget->selectionRangeEnd() : m_scintilla->DocumentLength();

    int flags = 0;
    if( caseSensitive ) flags |= 0x04; // SCFIND_MATCHCASE
    if( wholeWord )     flags |= 0x02; // SCFIND_WHOLEWORD

    // 제외된 범위를 건너뛰며 역순으로 치환 (오프셋 유지 위해)
    // 먼저 모든 매치 위치를 수집
    QVector<QPair<int, int>> matches;
    int pos = rangeStart;
    while( pos < rangeEnd )
    {
        m_scintilla->SetSearchTargetRange( pos, rangeEnd );
        int found = m_scintilla->SearchInTarget( searchStr, flags );
        if( found < 0 ) break;
        int matchEnd = m_scintilla->TargetEnd();
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
        m_scintilla->SetSearchTargetRange( matches[ i ].first, matches[ i ].first + matches[ i ].second );
        m_scintilla->SearchInTarget( searchStr, flags );
        m_scintilla->ReplaceInTarget( replaceStr );
    }

    clearCurrentMatchHighlight();
    clearExcludedRanges();
    updateMatchCount();
}

void BaseEdit::performExclude()
{
    if( !m_scintilla || !m_findWidget )
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
        const int selStart = m_scintilla->SelectionStartPos();
        const int selEnd = m_scintilla->SelectionEndPos();
        if( selStart == selEnd )
            return;
        matchStart = selStart;
        len = selEnd - selStart;
    }

    if( len <= 0 )
        return;

    if( isExcludedRange( matchStart, len ) )
    {
        m_searchResumePos = qMin( m_scintilla->DocumentLength(), matchStart + len );
        if( !searchInSelection )
            m_scintilla->SetSelectionByPos( m_searchResumePos, m_searchResumePos );
        clearCurrentMatchHighlight();
        return;
    }

    m_excludedRanges.append( { matchStart, len } );

    // Indicator로 시각 마킹 (더 진한 배경 + 박스 + 취소선)
    m_scintilla->SetIndicatorStyle( kExcludeBackgroundIndicatorId, 8, QColor( 128, 72, 72, 150 ) ); // INDIC_STRAIGHTBOX
    m_scintilla->ApplyIndicator( kExcludeBackgroundIndicatorId, matchStart, len );
    m_scintilla->SetIndicatorStyle( kExcludeBorderIndicatorId, 7, QColor( 186, 110, 110, 170 ) ); // INDIC_ROUNDBOX
    m_scintilla->ApplyIndicator( kExcludeBorderIndicatorId, matchStart, len );
    m_scintilla->SetIndicatorStyle( kExcludeStrikeIndicatorId, 5, QColor( 210, 210, 210, 255 ) ); // INDIC_STRIKE
    m_scintilla->ApplyIndicator( kExcludeStrikeIndicatorId, matchStart, len );
    m_searchResumePos = qMin( m_scintilla->DocumentLength(), matchStart + len );
    if( !searchInSelection )
        m_scintilla->SetSelectionByPos( m_searchResumePos, m_searchResumePos );
    clearCurrentMatchHighlight();
    updateExcludedCount();
    updateMatchCount();
}

void BaseEdit::clearExcludedRanges()
{
    if( !m_scintilla )
        return;

    if( !m_excludedRanges.isEmpty() )
    {
        m_scintilla->ClearAllIndicator( kExcludeBackgroundIndicatorId );
        m_scintilla->ClearAllIndicator( kExcludeBorderIndicatorId );
        m_scintilla->ClearAllIndicator( kExcludeStrikeIndicatorId );
        m_excludedRanges.clear();
    }

    m_searchResumePos = -1;
    updateExcludedCount();
}

void BaseEdit::updateExcludedCount()
{
    if( m_findWidget )
        m_findWidget->setExcludedCount( m_excludedRanges.size() );
}

void BaseEdit::clearCurrentMatchHighlight()
{
    if( !m_scintilla )
        return;

    if( m_currentMatchStart >= 0 && m_currentMatchLength > 0 )
        m_scintilla->ClearIndicator( kCurrentMatchIndicatorId, m_currentMatchStart, m_currentMatchLength );

    m_currentMatchStart = -1;
    m_currentMatchLength = 0;
}

void BaseEdit::setCurrentMatchHighlight( int startPos, int length, bool scrollToMatch )
{
    if( !m_scintilla || length <= 0 )
        return;

    clearCurrentMatchHighlight();
    m_scintilla->SetIndicatorStyle( kCurrentMatchIndicatorId, 7, QColor( 255, 215, 0, 110 ) ); // INDIC_ROUNDBOX
    m_scintilla->ApplyIndicator( kCurrentMatchIndicatorId, startPos, length );
    m_currentMatchStart = startPos;
    m_currentMatchLength = length;

    if( scrollToMatch )
        m_scintilla->ScrollRangeToView( startPos, startPos + length );
}

int BaseEdit::currentSearchFlags() const
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

bool BaseEdit::isExcludedRange( int startPos, int length ) const
{
    for( const auto& excludedRange : m_excludedRanges )
    {
        if( excludedRange.first == startPos && excludedRange.second == length )
            return true;
    }
    return false;
}

bool BaseEdit::findInRange( int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary )
{
    if( !m_scintilla || !m_findWidget )
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
            m_scintilla->SetSearchTargetRange( nextStart, endPos );
            const int found = m_scintilla->SearchInTarget( text, flags );
            if( found < 0 )
                return false;

            const int matchEnd = m_scintilla->TargetEnd();
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
                m_scintilla->SetSelectionByPos( found, matchEnd );
            setCurrentMatchHighlight( found, matchLength, true );
            m_searchResumePos = -1;
            return true;
        }

        return false;
        };

    const int selectionAnchor = forward ? m_scintilla->SelectionEndPos()
        : m_scintilla->SelectionStartPos();
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

bool BaseEdit::findInSelectionRange( bool forward, bool wrap, bool fromRangeBoundary )
{
    if( !m_scintilla || !m_findWidget || !m_findWidget->isSearchInSelection() )
        return false;

    const int rangeStart = m_findWidget->selectionRangeStart();
    const int rangeEnd = m_findWidget->selectionRangeEnd();
    return findInRange( rangeStart, rangeEnd, forward, wrap, fromRangeBoundary );
}

bool BaseEdit::hasCurrentMatch() const
{
    return m_currentMatchStart >= 0 && m_currentMatchLength > 0;
}

void BaseEdit::updateMatchCount()
{
    if( !m_findWidget || !m_scintilla )
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
        count = m_scintilla->CountMatchesInRange( text, false,
                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord(),
                    m_findWidget->selectionRangeStart(), m_findWidget->selectionRangeEnd() );
    }
    else
    {
        count = m_scintilla->CountMatches( text, false, m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord() );
    }
    m_findWidget->setMatchCount( count );
    updateExcludedCount();
}
