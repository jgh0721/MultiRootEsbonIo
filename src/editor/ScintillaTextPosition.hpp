#pragma once

#include <ScintillaEditBase.h>
#include <Scintilla.h>
#include <algorithm>

namespace mrst::sci {

struct Utf16LineColumn
{
    int line = 0;
    int column = 0;
};

/// LSP와 QString이 쓰는 UTF-16 위치. 탭을 펼친 SCI_GETCOLUMN과 구별한다.
inline Utf16LineColumn utf16LineColumn(ScintillaEditBase& editor, sptr_t position)
{
    const sptr_t safe = std::clamp(position, sptr_t(0), editor.send(SCI_GETLENGTH));
    const sptr_t line = editor.send(SCI_LINEFROMPOSITION, safe);
    const sptr_t first = editor.send(SCI_POSITIONFROMLINE, line);
    return {static_cast<int>(line), static_cast<int>(editor.send(SCI_COUNTCODEUNITS, first, safe))};
}

inline sptr_t positionFromUtf16LineColumn(ScintillaEditBase& editor, int line, int column)
{
    const sptr_t count = (std::max)(sptr_t(1), editor.send(SCI_GETLINECOUNT));
    const sptr_t safeLine = std::clamp(sptr_t(line), sptr_t(0), count - 1);
    const sptr_t first = editor.send(SCI_POSITIONFROMLINE, safeLine);
    const sptr_t position = editor.send(SCI_POSITIONRELATIVECODEUNITS, first, (std::max)(0, column));
    return std::clamp(position, first, editor.send(SCI_GETLINEENDPOSITION, safeLine));
}

} // namespace mrst::sci
