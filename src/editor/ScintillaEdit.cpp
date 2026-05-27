#include "stdafx.h"
#include "ScintillaEdit.hpp"

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

QWidget* QScintillaEdit::Editor() const
{
    return m_editor;
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
