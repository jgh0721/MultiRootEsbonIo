#include "stdafx.h"
#include "ScintillaEdit.hpp"

#include "core/solThemeManager.hpp"

#include <Scintilla.h>
#include <ILexer.h>

///////////////////////////////////////////////////////////////////////////////
///

namespace nsDetail
{
    constexpr sptr_t kInvalidPosition = static_cast<sptr_t>(-1);
    constexpr int kBraceHighlightIndicatorId = 24;
    constexpr int kBraceBadLightIndicatorId = 25;

    bool isBraceCharacter( const sptr_t ch )
    {
        switch( static_cast< char >( ch ) )
        {
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
                return true;
            default:
                return false;
        }
    }

    sptr_t positionFromLineIndex( ScintillaEditBase* editor, const int line, const int index )
    {
        if( !editor )
            return 0;

        const sptr_t lineCount        = qMax< sptr_t >( 1, editor->send( SCI_GETLINECOUNT ) );
        const sptr_t safeLine         = qBound< sptr_t >( 0, static_cast< sptr_t >( line ), lineCount - 1 );
        const sptr_t linePosition     = editor->send( SCI_POSITIONFROMLINE, safeLine );
        const sptr_t safeIndex        = qMax< sptr_t >( 0, static_cast< sptr_t >( index ) );
        const sptr_t documentPosition = editor->send( SCI_POSITIONRELATIVECODEUNITS, linePosition, safeIndex );
        const sptr_t lineEnd = editor->send( SCI_GETLINEENDPOSITION, safeLine );
        return qBound< sptr_t >( linePosition, documentPosition, lineEnd );
    }

    void positionToLineIndex( ScintillaEditBase* editor, const sptr_t position, int& line, int& index )
    {
        if( !editor )
        {
            line  = 0;
            index = 0;
            return;
        }

        const sptr_t safePosition = qBound< sptr_t >( 0, position, editor->send( SCI_GETLENGTH ) );
        line = static_cast< int >( editor->send( SCI_LINEFROMPOSITION, safePosition ) );
        const sptr_t linePosition = editor->send( SCI_POSITIONFROMLINE, line );
        index = static_cast< int >( editor->send( SCI_COUNTCODEUNITS, linePosition, safePosition ) );
    }
} // nsDetail

///////////////////////////////////////////////////////////////////////////////
///

QScintillaEdit::QScintillaEdit( QWidget* EditorParent, QObject* Parent )
    : QObject( Parent )
    , m_editor( new ScintillaEditBase( EditorParent ) )
    , m_braceMatchingEnabled( true )
{
    m_editor->setObjectName( QStringLiteral( "scintillaEditor" ) );
    m_editor->send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    m_editor->send( SCI_SETUNDOCOLLECTION, 1 );
    m_editor->send( SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER );
    m_editor->send( SCI_SETMARGINWIDTHN, 0, 48 );
    m_editor->send( SCI_SETMARGINWIDTHN, 1, 0 );
    m_editor->send( SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH );
    m_editor->send( SCI_SETCARETLINEVISIBLE, 1 );
    m_editor->send( SCI_SETCARETLINEBACK, 0x202020 );

    const QFont fixedFont = QFontDatabase::systemFont( QFontDatabase::FixedFont );
    const QByteArray family = fixedFont.family().toUtf8();
    m_editor->send( SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast< sptr_t >( family.constData() ) );
    m_editor->send( SCI_STYLESETSIZE, STYLE_DEFAULT, fixedFont.pointSize() > 0 ? fixedFont.pointSize() : 10 );
    m_editor->send( SCI_STYLECLEARALL );

    connect( m_editor, &ScintillaEditBase::savePointChanged, this, &QScintillaEdit::modificationChanged );
    connect( m_editor, &ScintillaEditBase::notifyChange, this, [this] {
        emit textChanged();
    } );
    connect( m_editor, &ScintillaEditBase::linesAdded, this, [this]( Scintilla::Position ) {
        emit linesChanged();
    } );
    connect( m_editor, &ScintillaEditBase::modified, this, [this]( Scintilla::ModificationFlags, Scintilla::Position,
                                                                  Scintilla::Position, Scintilla::Position linesAdded,
                                                                  const QByteArray&, Scintilla::Position,
                                                                  Scintilla::FoldLevel, Scintilla::FoldLevel ) {
        if( linesAdded != 0 )
            emit linesChanged();
    } );
    connect( m_editor, &ScintillaEditBase::updateUi, this, [this]( Scintilla::Update ) {
        updateBraceHighlight();
        emitCursorAndSelectionState();
    } );
}

QScintillaEdit::~QScintillaEdit() = default;

QWidget* QScintillaEdit::Editor() const
{
    return m_editor;
}

ScintillaEditBase* QScintillaEdit::ScintillaWidget() const
{
    return m_editor;
}

sptr_t QScintillaEdit::Send( unsigned int Message, uptr_t WParam, sptr_t LParam ) const
{
    return m_editor ? m_editor->send( Message, WParam, LParam ) : 0;
}

sptr_t QScintillaEdit::Sends( unsigned int Message, uptr_t WParam, const char* Text ) const
{
    return m_editor ? m_editor->sends( Message, WParam, Text ) : 0;
}

void QScintillaEdit::SetText( const QByteArray& Text )
{
    if( !m_editor )
        return;

    m_editor->send( SCI_SETTEXT, 0, reinterpret_cast< sptr_t >( Text.constData() ) );
    m_editor->send( SCI_EMPTYUNDOBUFFER );
    m_editor->send( SCI_SETSAVEPOINT );
    updateBraceHighlight();
    emitCursorAndSelectionState();
}

QByteArray QScintillaEdit::Text() const
{
    if( !m_editor )
        return {};

    const sptr_t length = m_editor->send( SCI_GETTEXTLENGTH );
    if( length <= 0 )
        return {};

    QByteArray buffer( static_cast< int >( length ) + 1, Qt::Uninitialized );
    m_editor->send( SCI_GETTEXT, static_cast< uptr_t >( buffer.size() ), reinterpret_cast< sptr_t >( buffer.data() ) );
    buffer.resize( static_cast< int >( length ) );
    return buffer;
}

void QScintillaEdit::Copy()
{
    if( m_editor )
        (void)m_editor->send( SCI_COPY );
}

int QScintillaEdit::GetLineCount() const
{
    return m_editor ? static_cast< int >( m_editor->send( SCI_GETLINECOUNT ) ) : 0;
}

void QScintillaEdit::SetReadOnly( bool ReadOnly )
{
    if( m_editor == nullptr )
        return;

    m_editor->send( SCI_SETREADONLY, ReadOnly ? 1 : 0 );
}

bool QScintillaEdit::IsReadOnly() const
{
    return m_editor ? ( m_editor->send( SCI_GETREADONLY ) != 0 ) : false;
}


bool QScintillaEdit::HasSelectedText() const
{
    return m_editor ? ( m_editor->send( SCI_GETSELECTIONEMPTY ) == 0 ) : false;
}

QString QScintillaEdit::SelectedText() const
{
    if( !m_editor || !HasSelectedText() )
        return {};

    const sptr_t lengthWithTerminator = m_editor->send( SCI_GETSELTEXT );
    if( lengthWithTerminator <= 1 )
        return {};

    QByteArray buffer( static_cast< int >( lengthWithTerminator ), Qt::Uninitialized );
    m_editor->send( SCI_GETSELTEXT, 0, reinterpret_cast< sptr_t >( buffer.data() ) );
    return QString::fromUtf8( buffer.constData() );
}

bool QScintillaEdit::GetSelectionRange( int& lineFrom, int& indexFrom, int& lineTo, int& indexTo ) const
{
    if( !m_editor )
    {
        lineFrom  = 0;
        indexFrom = 0;
        lineTo    = 0;
        indexTo   = 0;
        return false;
    }

    const sptr_t selectionStart = m_editor->send( SCI_GETSELECTIONSTART );
    const sptr_t selectionEnd   = m_editor->send( SCI_GETSELECTIONEND );
    nsDetail::positionToLineIndex( m_editor, selectionStart, lineFrom, indexFrom );
    nsDetail::positionToLineIndex( m_editor, selectionEnd, lineTo, indexTo );
    return selectionStart != selectionEnd;
}

void QScintillaEdit::SetSelectionRange( int lineFrom, int indexFrom, int lineTo, int indexTo )
{
    if( !m_editor )
        return;

    const sptr_t start = nsDetail::positionFromLineIndex( m_editor, lineFrom, indexFrom );
    const sptr_t end   = nsDetail::positionFromLineIndex( m_editor, lineTo, indexTo );
    m_editor->send( SCI_SETSEL, start, end );
    updateBraceHighlight();
    emitCursorAndSelectionState();
}

void QScintillaEdit::SetSelectionByPos( int startPos, int endPos )
{
    if( !m_editor ) return;
    m_editor->send( SCI_SETSEL, startPos, endPos );
    updateBraceHighlight();
    emitCursorAndSelectionState();
}

int QScintillaEdit::SelectionStartPos() const
{
    if( !m_editor ) return 0;
    return static_cast< int >( m_editor->send( SCI_GETSELECTIONSTART ) );
}

int QScintillaEdit::SelectionEndPos() const
{
    if( !m_editor ) return 0;
    return static_cast< int >( m_editor->send( SCI_GETSELECTIONEND ) );
}

void QScintillaEdit::SetCursorPosition( int line, int index )
{
    if( !m_editor )
        return;

    const sptr_t maxLine = qMax<sptr_t>( 0, static_cast< sptr_t >( GetLineCount() - 1 ) );
    const sptr_t safeLine = qMax<sptr_t>( 0, qMin<sptr_t>( static_cast< sptr_t >( line ), maxLine ) );
    const sptr_t safeIndex = qMax<sptr_t>( 0, static_cast< sptr_t >( index ) );
    const sptr_t position = nsDetail::positionFromLineIndex( m_editor, static_cast< int >( safeLine ), static_cast< int >( safeIndex ) );
    m_editor->send( SCI_SETEMPTYSELECTION, position );
    m_editor->send( SCI_SCROLLCARET );
    updateBraceHighlight();
    emitCursorAndSelectionState();
}

bool QScintillaEdit::FindFirst( const QString& text, bool regex, bool caseSensitive, bool wholeWords, bool wrap, bool forward )
{
    if( !m_editor || text.isEmpty() )
        return false;

    int flags = 0;
    if( caseSensitive )
        flags |= SCFIND_MATCHCASE;
    if( wholeWords )
        flags |= SCFIND_WHOLEWORD;
    if( regex )
        flags |= SCFIND_CXX11REGEX;

    int lineFrom = 0;
    int indexFrom = 0;
    int lineTo = 0;
    int indexTo = 0;
    if( GetSelectionRange( lineFrom, indexFrom, lineTo, indexTo ) )
        SetSelectionRange( forward ? lineTo : lineFrom,
                      forward ? indexTo : indexFrom,
                      forward ? lineTo : lineFrom,
                      forward ? indexTo : indexFrom );
    else
        m_editor->send( SCI_SETEMPTYSELECTION, m_editor->send( SCI_GETCURRENTPOS ) );

    const QByteArray needle = text.toUtf8();
    m_editor->send( SCI_SETSEARCHFLAGS, flags );
    m_editor->send( SCI_SEARCHANCHOR );

    const int message = forward ? SCI_SEARCHNEXT : SCI_SEARCHPREV;
    if( m_editor->sends( message, flags, needle.constData() ) != -1 )
    {
        m_editor->send( SCI_SCROLLCARET );
        return true;
    }

    if( !wrap )
        return false;

    const sptr_t wrapPosition = forward ? 0 : m_editor->send( SCI_GETLENGTH );
    m_editor->send( SCI_SETEMPTYSELECTION, wrapPosition );
    m_editor->send( SCI_SEARCHANCHOR );
    if( m_editor->sends( message, flags, needle.constData() ) == -1 )
        return false;

    m_editor->send( SCI_SCROLLCARET );
    return true;
}

void QScintillaEdit::Replace( const QString& replacement )
{
    if( !m_editor )
        return;

    const QByteArray utf8 = replacement.toUtf8();
    m_editor->sends( SCI_REPLACESEL, 0, utf8.constData() );
}

int QScintillaEdit::CountMatches( const QString& text, bool regex, bool caseSensitive, bool wholeWords )
{
    if( !m_editor || text.isEmpty() )
        return 0;

    return CountMatchesInRange( text, regex, caseSensitive, wholeWords,
                                0, static_cast< int >( m_editor->send( SCI_GETLENGTH ) ) );
}

int QScintillaEdit::CountMatchesInRange( const QString& text, bool regex, bool caseSensitive, bool wholeWords, int startPos, int endPos )
{
    if( !m_editor || text.isEmpty() || startPos >= endPos )
        return 0;

    int flags = 0;
    if( caseSensitive ) flags |= SCFIND_MATCHCASE;
    if( wholeWords )    flags |= SCFIND_WHOLEWORD;
    if( regex )         flags |= SCFIND_CXX11REGEX;

    const QByteArray needle = text.toUtf8();
    int count = 0;
    int searchStart = startPos;

    while( searchStart < endPos )
    {
        m_editor->send( SCI_SETTARGETSTART, searchStart );
        m_editor->send( SCI_SETTARGETEND, endPos );
        m_editor->send( SCI_SETSEARCHFLAGS, flags );
        const sptr_t pos = m_editor->sends( SCI_SEARCHINTARGET,
                                           static_cast< sptr_t >( needle.size() ),
                                           needle.constData() );
        if( pos < 0 )
            break;
        ++count;
        const int matchEnd = static_cast< int >( m_editor->send( SCI_GETTARGETEND ) );
        searchStart = qMax( matchEnd, searchStart + 1 );
    }
    return count;
}
void QScintillaEdit::SetSearchTargetRange( int startPos, int endPos )
{
    if( !m_editor ) return;
    m_editor->send( SCI_SETTARGETSTART, startPos );
    m_editor->send( SCI_SETTARGETEND, endPos );
}

int QScintillaEdit::SearchInTarget( const QString& text, int flags )
{
    if( !m_editor ) 
        return -1;
    m_editor->send( SCI_SETSEARCHFLAGS, flags );
    const QByteArray utf8 = text.toUtf8();
    return static_cast< int >( m_editor->sends( SCI_SEARCHINTARGET, utf8.size(), utf8.constData() ) );
}

int QScintillaEdit::TargetEnd() const
{
    if( !m_editor ) 
        return -1;
    return static_cast< int >( m_editor->send( SCI_GETTARGETEND ) );
}

void QScintillaEdit::ReplaceInTarget( const QString& text )
{
    if( !m_editor ) 
        return;
    const QByteArray utf8 = text.toUtf8();
    m_editor->sends( SCI_REPLACETARGET, utf8.size(), utf8.constData() );
}

int QScintillaEdit::DocumentLength() const
{
    if( !m_editor ) 
        return 0;
    return static_cast< int >( m_editor->send( SCI_GETLENGTH ) );
}

int QScintillaEdit::RowHeight( int Line ) const
{
    return m_editor ? static_cast< int >( m_editor->send( SCI_TEXTHEIGHT, Line ) ) : 0;
}

int QScintillaEdit::LeftMarginWidth() const
{
    if( !m_editor )
        return 0;

    // 모든 마진(줄 번호, 기호 등) 폭 합산
    int total = 0;
    for( int i = 0; i < 5; ++i )
        total += static_cast< int >( m_editor->send( SCI_GETMARGINWIDTHN, i ) );

    // 텍스트 영역 왼쪽 패딩 (텍스트 시작 위치까지의 추가 간격)
    total += static_cast< int >( m_editor->send( SCI_GETMARGINLEFT ) );
    return total;
}

void QScintillaEdit::SetLingEnding( Scintilla::EndOfLine Type, bool ConvertExisting )
{
    if( !m_editor )
        return;

    m_editor->send( SCI_SETEOLMODE, static_cast<sptr_t>( Type ) );
    if (ConvertExisting)
        m_editor->send( SCI_CONVERTEOLS, static_cast<sptr_t>( Type ) );
}

int QScintillaEdit::ChangeHistoryFlags() const
{
    return m_editor ? static_cast< int >( m_editor->send( SCI_GETCHANGEHISTORY ) ) : 0;
}

void QScintillaEdit::SetIndicatorStyle( int indicatorId, int style, const QColor& color )
{
    if( !m_editor )
        return;
    m_editor->send( SCI_INDICSETSTYLE, indicatorId, style );
    const int sciColor = color.red() | ( color.green() << 8 ) | ( color.blue() << 16 );
    m_editor->send( SCI_INDICSETFORE, indicatorId, sciColor );
    m_editor->send( SCI_INDICSETALPHA, indicatorId, color.alpha() );
    m_editor->send( SCI_INDICSETOUTLINEALPHA, indicatorId, qMax( color.alpha(), 160 ) );
}

void QScintillaEdit::ApplyIndicator( int indicatorId, int startPos, int length )
{
    if( !m_editor || length <= 0 )
        return;
    m_editor->send( SCI_SETINDICATORCURRENT, indicatorId );
    m_editor->send( SCI_INDICATORFILLRANGE, startPos, length );
}

void QScintillaEdit::ClearIndicator( int indicatorId, int startPos, int length )
{
    if( !m_editor || length <= 0 )
        return;
    m_editor->send( SCI_SETINDICATORCURRENT, indicatorId );
    m_editor->send( SCI_INDICATORCLEARRANGE, startPos, length );
}

void QScintillaEdit::ClearAllIndicator( int indicatorId )
{
    if( !m_editor )
        return;
    const int docLen = static_cast< int >( m_editor->send( SCI_GETLENGTH ) );
    ClearIndicator( indicatorId, 0, docLen );
}

void QScintillaEdit::ApplyThemeColors( bool dark )
{
    if( !m_editor )
        return;

    Q_UNUSED( dark );
    auto colourToSci = []( const QColor& color ) -> sptr_t {
        return static_cast< sptr_t >( color.red() | ( color.green() << 8 ) | ( color.blue() << 16 ) );
        };

    auto& theme = ThemeManager::instance();
    const QColor bg = theme.color( QStringLiteral( "text.background" ) );
    const QColor fg = theme.color( QStringLiteral( "text.foreground" ) );
    const QColor marginBg = theme.color( QStringLiteral( "text.marginBackground" ) );
    const QColor marginFg = theme.color( QStringLiteral( "text.marginForeground" ) );
    const QColor selection = theme.color( QStringLiteral( "text.selection" ) );
    const QColor selectionForeground = theme.color( QStringLiteral( "text.selectionForeground" ) );
    const QColor caretLine = theme.color( QStringLiteral( "text.currentLine" ) );
    const QColor caret = theme.color( QStringLiteral( "text.caret" ) );
    const QColor foldMarker = theme.color( QStringLiteral( "text.foldMarker" ) );
    const QColor indentGuide = theme.color( QStringLiteral( "text.indentGuide" ) );

    m_editor->send( SCI_STYLESETFORE, STYLE_DEFAULT, colourToSci( fg ) );
    m_editor->send( SCI_STYLESETBACK, STYLE_DEFAULT, colourToSci( bg ) );
    m_editor->send( SCI_STYLECLEARALL );
    m_editor->send( SCI_STYLESETFORE, STYLE_INDENTGUIDE, colourToSci( indentGuide ) );
    m_editor->send( SCI_STYLESETBACK, STYLE_INDENTGUIDE, colourToSci( bg ) );

    m_editor->send( SCI_STYLESETFORE, STYLE_LINENUMBER, colourToSci( marginFg ) );
    m_editor->send( SCI_STYLESETBACK, STYLE_LINENUMBER, colourToSci( marginBg ) );

    m_editor->send( SCI_SETCARETFORE, colourToSci( caret ) );
    m_editor->send( SCI_SETCARETLINEVISIBLEALWAYS, 1 );
    m_editor->send( SCI_SETCARETLINEBACK, colourToSci( caretLine ) );

    m_editor->send( SCI_SETSELBACK, 1, colourToSci( selection ) );
    m_editor->send( SCI_SETSELECTIONLAYER, SC_LAYER_UNDER_TEXT );
    m_editor->send( SCI_SETSELALPHA, selection.alpha() > 0 ? selection.alpha() : 112 );
    m_editor->send( SCI_SETSELFORE, 0, colourToSci( selectionForeground ) );

    m_editor->send( SCI_SETFOLDMARGINCOLOUR, 1, colourToSci( bg ) );
    m_editor->send( SCI_SETFOLDMARGINHICOLOUR, 1, colourToSci( bg ) );
    for( int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker )
    {
        m_editor->send( SCI_MARKERSETFORE, marker, colourToSci( bg ) );
        m_editor->send( SCI_MARKERSETBACK, marker, colourToSci( foldMarker ) );
    }

    // 현재 언어의 구문 강조 색상 적용
    m_darkTheme = ThemeManager::instance().currentTheme() == ThemeManager::Dark;
    configureBraceHighlightIndicators();
    applySyntaxStyles( m_darkTheme );
    m_editor->update();
}

void QScintillaEdit::ScrollRangeToView( int startPos, int endPos )
{
    if( !m_editor )
        return;

    const sptr_t start = qMax<sptr_t>( 0, static_cast< sptr_t >( startPos ) );
    const sptr_t end = qMax( start, static_cast< sptr_t >( endPos ) );
    m_editor->send( SCI_SCROLLRANGE, start, end );
}

void QScintillaEdit::configureBraceHighlightIndicators()
{
    if( !m_editor )
        return;

    auto colourToSci = []( const QColor& color ) -> sptr_t {
        return static_cast< sptr_t >( color.red() | ( color.green() << 8 ) | ( color.blue() << 16 ) );
        };

    Q_UNUSED( m_darkTheme );
    const QColor matchColor = ThemeManager::instance().color( QStringLiteral( "text.braceMatch" ) );
    const QColor badColor = ThemeManager::instance().color( QStringLiteral( "text.braceMismatch" ) );

    // 괄호 매칭은 글자를 덮지 않도록 채움 없는 outline indicator로 표시한다.
    m_editor->send( SCI_INDICSETSTYLE, nsDetail::kBraceHighlightIndicatorId, INDIC_STRAIGHTBOX );
    m_editor->send( SCI_INDICSETFORE, nsDetail::kBraceHighlightIndicatorId, colourToSci( matchColor ) );
    m_editor->send( SCI_INDICSETALPHA, nsDetail::kBraceHighlightIndicatorId, 0 );
    m_editor->send( SCI_INDICSETOUTLINEALPHA, nsDetail::kBraceHighlightIndicatorId, qMax( matchColor.alpha(), 220 ) );

    m_editor->send( SCI_INDICSETSTYLE, nsDetail::kBraceBadLightIndicatorId, INDIC_SQUIGGLE );
    m_editor->send( SCI_INDICSETFORE, nsDetail::kBraceBadLightIndicatorId, colourToSci( badColor ) );
    m_editor->send( SCI_INDICSETALPHA, nsDetail::kBraceBadLightIndicatorId, badColor.alpha() );
    m_editor->send( SCI_INDICSETOUTLINEALPHA, nsDetail::kBraceBadLightIndicatorId, badColor.alpha() );

    m_editor->send( SCI_BRACEHIGHLIGHTINDICATOR, 1, nsDetail::kBraceHighlightIndicatorId );
    m_editor->send( SCI_BRACEBADLIGHTINDICATOR, 1, nsDetail::kBraceBadLightIndicatorId );
}

void QScintillaEdit::updateLineNumberMargin( int minimumDigits )
{
    if( !m_editor )
        return;

    if( minimumDigits <= 0 )
    {
        m_editor->send( SCI_SETMARGINWIDTHN, 0, 0 );
        return;
    }

    const int digits = qMax( minimumDigits, QString::number( qMax( 1, GetLineCount() ) ).size() );
    const QFontMetrics metrics( m_editorMarginFont.resolve( m_editor->font() ) );
    const int width = metrics.horizontalAdvance( QString( digits, QLatin1Char( '9' ) ) ) + 12;
    m_editor->send( SCI_SETMARGINWIDTHN, 0, width );
}

void QScintillaEdit::emitCursorAndSelectionState()
{
    if( !m_editor )
        return;

    const sptr_t position = m_editor->send( SCI_GETCURRENTPOS );
    int          line     = 0;
    int          column   = 0;
    nsDetail::positionToLineIndex( m_editor, position, line, column );
    emit cursorPositionChanged( line, column );
    emit selectionChanged();
}

void QScintillaEdit::updateBraceHighlight()
{
    if( !m_editor )
        return;

    if( !m_braceMatchingEnabled )
    {
        m_editor->send( SCI_BRACEHIGHLIGHT, nsDetail::kInvalidPosition, nsDetail::kInvalidPosition );
        m_editor->send( SCI_BRACEBADLIGHT, nsDetail::kInvalidPosition );
        return;
    }

    const sptr_t currentPos = m_editor->send( SCI_GETCURRENTPOS );
    sptr_t       bracePos   = currentPos;
    sptr_t       braceChar  = m_editor->send( SCI_GETCHARAT, bracePos );

    if( !nsDetail::isBraceCharacter( braceChar ) && currentPos > 0 )
    {
        bracePos  = currentPos - 1;
        braceChar = m_editor->send( SCI_GETCHARAT, bracePos );
    }

    if( !nsDetail::isBraceCharacter( braceChar ) )
    {
        m_editor->send( SCI_BRACEHIGHLIGHT, nsDetail::kInvalidPosition, nsDetail::kInvalidPosition );
        m_editor->send( SCI_BRACEBADLIGHT, nsDetail::kInvalidPosition );
        return;
    }

    sptr_t matchPos = m_editor->send( SCI_BRACEMATCH, bracePos, 0 );
    if( matchPos == nsDetail::kInvalidPosition )
        matchPos = m_editor->send( SCI_BRACEMATCHNEXT, bracePos, currentPos );

    if( matchPos == nsDetail::kInvalidPosition )
    {
        m_editor->send( SCI_BRACEHIGHLIGHT, nsDetail::kInvalidPosition, nsDetail::kInvalidPosition );
        m_editor->send( SCI_BRACEBADLIGHT, bracePos );
        return;
    }

    m_editor->send( SCI_BRACEBADLIGHT, nsDetail::kInvalidPosition );
    m_editor->send( SCI_BRACEHIGHLIGHT, bracePos, matchPos );
}

void QScintillaEdit::applySyntaxStyles( bool dark )
{
}
