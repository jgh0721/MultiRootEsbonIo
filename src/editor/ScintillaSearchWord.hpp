#pragma once

#include <QByteArray>
#include <QString>
#include <ScintillaEditBase.h>
#include <Scintilla.h>

namespace mrst::sci {

/// Use the editor's word boundaries and UTF-8 positions without moving its caret.
inline QString wordAtCaret( ScintillaEditBase& editor )
{
    const sptr_t caret = editor.send( SCI_GETCURRENTPOS );
    const sptr_t start = editor.send( SCI_WORDSTARTPOSITION, caret, 1 );
    const sptr_t end = editor.send( SCI_WORDENDPOSITION, caret, 1 );
    if( end <= start )
        return {};
    QByteArray bytes( static_cast<qsizetype>( end - start ) + 1, Qt::Uninitialized );
    Sci_TextRangeFull range{};
    range.chrg.cpMin = start;
    range.chrg.cpMax = end;
    range.lpstrText = bytes.data();
    editor.send( SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>( &range ) );
    return QString::fromUtf8( bytes.constData(), end - start );
}

} // namespace mrst::sci
