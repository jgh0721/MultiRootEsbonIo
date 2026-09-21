#pragma once

#include "ScintillaDocument.hpp"
#include "core/solAppSettings.hpp"

namespace mrst {

inline ScintillaDocument::LineEnding defaultDocumentLineEnding()
{
    const QString value = AppSettings().value( "textView/defaultLineEnding", "CRLF" ).toString();
    if( value == "LF" ) return ScintillaDocument::LF;
    if( value == "CR" ) return ScintillaDocument::CR;
    return ScintillaDocument::CRLF;
}

inline ScintillaDocument::LineEnding detectDocumentLineEnding( const QString& text )
{
    if( text.contains( QLatin1String( "\r\n" ) ) ) return ScintillaDocument::CRLF;
    if( text.contains( QLatin1Char( '\r' ) ) ) return ScintillaDocument::CR;
    if( text.contains( QLatin1Char( '\n' ) ) ) return ScintillaDocument::LF;
    return defaultDocumentLineEnding();
}

} // namespace mrst
