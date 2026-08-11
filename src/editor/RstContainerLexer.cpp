// Qt 비의존 순수 모듈이다. stdafx.h 를 포함하지 않으므로 테스트 타깃에서
// Qt Widgets/WebEngine 없이 그대로 컴파일할 수 있다.
#include "RstContainerLexer.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>

namespace mrst::rst {
namespace {

const std::string kTitleChars = "=-`:.~'^\"_*+#!$%&()/<>@\\{|}";

const std::regex kDirectiveRegex( R"(^(\s*\.\.\s+)([a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?)(::)(.*))" );
const std::regex kRoleRegex( R"(:([a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?):`([^`]*)`)" );
const std::regex kInlineLiteralRegex( R"(``(.+?)``)" );
const std::regex kStrongRegex( R"(\*\*(.+?)\*\*)" );
const std::regex kEmphasisRegex( R"((^|[^*])\*([^*]+)\*(?!\*))" );
const std::regex kHyperlinkRegex( R"(`[^`]+`_|[a-zA-Z0-9]+_(?!_))" );
const std::regex kSubstitutionRegex( R"(\|[^|]+\|)" );
const std::regex kFieldRegex( R"(^(\s*):([^:]+):(.*))" );
const std::regex kExplicitMarkupRegex( R"(^\s*\.\.\s+(?![a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?::))" );
const std::regex kTransitionRegex( R"(^([=\-`:.~'\"_*+#]{4,})\s*$)" );
const std::regex kSnippetPlaceholderRegex( R"(\$\{[^}]*\})" );

[[nodiscard]] std::string trim( std::string value )
{
    const auto first = std::find_if_not( value.begin(), value.end(),
                                        []( unsigned char ch ) { return std::isspace( ch ) != 0; } );
    const auto last = std::find_if_not( value.rbegin(), value.rend(),
                                       []( unsigned char ch ) { return std::isspace( ch ) != 0; } ).base();
    if( first >= last )
        return {};

    return std::string( first, last );
}

[[nodiscard]] std::string firstLine( std::string_view value )
{
    const std::size_t pos = value.find( '\n' );
    return std::string( pos == std::string_view::npos ? value : value.substr( 0, pos ) );
}

[[nodiscard]] bool startsWith( std::string_view value, std::string_view prefix )
{
    return value.size() >= prefix.size() && value.substr( 0, prefix.size() ) == prefix;
}

[[nodiscard]] bool endsWith( std::string_view value, std::string_view suffix )
{
    return value.size() >= suffix.size() && value.substr( value.size() - suffix.size() ) == suffix;
}

[[nodiscard]] std::string toLower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(),
                   []( unsigned char ch ) { return static_cast< char >( std::tolower( ch ) ); } );
    return value;
}

[[nodiscard]] std::string stripColons( std::string value )
{
    while( !value.empty() && value.front() == ':' )
        value.erase( value.begin() );
    while( !value.empty() && value.back() == ':' )
        value.pop_back();

    return value;
}

/// UTF-8 문자(코드포인트) 수. 후속 바이트(10xxxxxx)는 세지 않는다.
[[nodiscard]] std::size_t utf8Length( std::string_view value )
{
    std::size_t count = 0;
    for( const char raw : value )
    {
        if( ( static_cast< unsigned char >( raw ) & 0xC0 ) != 0x80 )
            ++count;
    }
    return count;
}

/// adornmentLine 이 titleLine 의 제목 장식(윗줄/밑줄)인가.
[[nodiscard]] bool isTitleAdornmentFor( const std::string& adornmentLine, const std::string& titleLine )
{
    if( adornmentLine.empty() || !std::regex_match( adornmentLine, kTransitionRegex ) )
        return false;

    const std::string adornment = trim( adornmentLine );
    const std::string title = trim( titleLine );
    if( adornment.empty() || title.empty() )
        return false;
    if( kTitleChars.find( adornment.front() ) == std::string::npos )
        return false;

    // 길이는 반드시 **문자 수**로 비교한다. 바이트로 재면 글자당 3바이트인
    // 한글 제목은 장식이 늘 짧아 보여서 제목으로 인식되지 않는다.
    return utf8Length( adornment ) >= utf8Length( title );
}

/// 윗줄과 밑줄이 같은 문자로 그려졌는가. reST 는 짝이 맞아야 제목으로 본다.
[[nodiscard]] bool sameAdornmentChar( const std::string& first, const std::string& second )
{
    const std::string left = trim( first );
    const std::string right = trim( second );
    return !left.empty() && !right.empty() && left.front() == right.front();
}

[[nodiscard]] std::vector< std::string > splitLines( const std::string& text )
{
    std::vector< std::string > lines;
    std::size_t start = 0;
    while( true )
    {
        const std::size_t pos = text.find( '\n', start );
        if( pos == std::string::npos )
        {
            lines.push_back( text.substr( start ) );
            break;
        }
        lines.push_back( text.substr( start, pos - start ) );
        start = pos + 1;
    }
    return lines;
}

/// std::regex 를 std::string 위에서 돌리면 match.position() 이 이미 바이트
/// 오프셋이다. (Python 원본은 str 위에서 돌아 문자 오프셋이 나오므로 바이트로
/// 환산하는 단계가 필요했지만, 여기서는 그 환산이 오히려 비ASCII 앞선 줄에서
/// 오프셋을 밀어버린다.)
void appendMatchSpans( const std::string& line,
                      const std::regex& pattern,
                      int style,
                      std::vector< Span >& matches,
                      bool emphasisWorkaround = false )
{
    for( std::sregex_iterator it( line.begin(), line.end(), pattern ), end; it != end; ++it )
    {
        const std::smatch& match = *it;
        std::size_t start = static_cast< std::size_t >( match.position( 0 ) );
        std::size_t length = static_cast< std::size_t >( match.length( 0 ) );
        if( emphasisWorkaround && match.length( 1 ) == 1 )
        {
            // std::regex 에는 Python 의 고정폭 negative lookbehind
            // (?<!\*)\*([^*]+)\*(?!\*) 가 없다. 선행 non-star 한 글자를 선택적으로
            // 매치한 뒤 스타일 범위에서 빼는 방식으로 대체한다.
            start += 1;
            length -= 1;
        }
        matches.push_back( { start, start + length, style } );
    }
}

}  // namespace

int RstMetadataCache::directiveStyle( const std::string& name ) const
{
    if( !directivesPopulated )
        return STYLE_DIRECTIVE_UNKNOWN;

    return directives.contains( name ) ? STYLE_DIRECTIVE_VALID : STYLE_DIRECTIVE_INVALID;
}

int RstMetadataCache::roleStyle( const std::string& name ) const
{
    if( !rolesPopulated )
        return STYLE_ROLE_UNKNOWN;

    return roles.contains( name ) ? STYLE_ROLE_VALID : STYLE_ROLE_INVALID;
}

void RstMetadataCache::updateFromCompletion( const std::vector< CompletionEntry >& entries )
{
    for( const CompletionEntry& entry : entries )
    {
        const std::string detailLower = toLower( entry.detail );
        const std::string insertTrimmed = trim( entry.insertText );
        if( endsWith( insertTrimmed, "::" ) || detailLower.find( "directive" ) != std::string::npos )
        {
            const std::string name = extractDirectiveName( entry.label, entry.insertText );
            if( !name.empty() )
            {
                directives.insert( name );
                directivesPopulated = true;
            }
        }
        else if( startsWith( entry.label, ":" ) && endsWith( entry.label, ":" ) )
        {
            const std::string name = stripColons( entry.label );
            if( !name.empty() )
            {
                roles.insert( name );
                rolesPopulated = true;
            }
        }
        else if( detailLower.find( "role" ) != std::string::npos )
        {
            const std::string name = stripColons( entry.label );
            if( !name.empty() )
            {
                roles.insert( name );
                rolesPopulated = true;
            }
        }
    }
}

std::string extractDirectiveName( const std::string& label, const std::string& insertText )
{
    std::string text = trim( firstLine( insertText ) );
    if( startsWith( text, ".." ) )
        text = trim( text.substr( 2 ) );

    // 스니펫 placeholder 를 "::" 보다 먼저 제거해야 한다. Python 원본은 순서가
    // 반대라서 "image:: ${1:path}" 가 "image::" 로 남고, 그 이름이 캐시에 들어가면
    // 정작 "image" directive 가 INVALID(빨강)로 표시된다.
    text = trim( std::regex_replace( text, kSnippetPlaceholderRegex, "" ) );
    if( endsWith( text, "::" ) )
        text = trim( text.substr( 0, text.size() - 2 ) );

    if( !text.empty() )
        return text;

    std::string fallback = trim( firstLine( label ) );
    if( startsWith( fallback, ".." ) )
        fallback = trim( fallback.substr( 2 ) );
    if( endsWith( fallback, "::" ) )
        fallback = trim( fallback.substr( 0, fallback.size() - 2 ) );

    return fallback;
}

RstContainerLexer::RstContainerLexer( RstMetadataCache cache )
    : cache_( std::move( cache ) )
{
}

const RstMetadataCache& RstContainerLexer::metadataCache() const
{
    return cache_;
}

RstMetadataCache& RstContainerLexer::metadataCache()
{
    return cache_;
}

std::vector< Span > RstContainerLexer::tokenizeLine( const std::string& line,
                                                    const std::string& previousLine,
                                                    const std::string& nextLine,
                                                    const std::string& lineAfterNext ) const
{
    const std::size_t lineByteLen = line.size();

    // 다음 줄이 밑줄이면 이 줄은 제목이다.
    if( isTitleAdornmentFor( nextLine, line ) )
        return { { 0, lineByteLen, STYLE_TITLE } };

    if( std::regex_match( line, kTransitionRegex ) )
    {
        // 앞 줄에 내용이 있으면 제목의 밑줄이다.
        if( !trim( previousLine ).empty() )
            return { { 0, lineByteLen, STYLE_TITLE } };

        // 윗줄/아랫줄로 감싼 제목의 윗줄도 제목의 일부다.
        //     ########
        //     제목
        //     ########
        // 두 장식이 같은 문자여야 하고, 그 사이 줄이 제목이어야 한다.
        if( isTitleAdornmentFor( lineAfterNext, nextLine ) && sameAdornmentChar( line, lineAfterNext ) )
            return { { 0, lineByteLen, STYLE_TITLE } };

        // 그 밖에는 단독 구분선이다.
        return { { 0, lineByteLen, STYLE_TRANSITION } };
    }

    if( std::regex_match( line, kDirectiveRegex ) )
        return styleDirectiveLine( line );

    if( std::regex_search( line, kExplicitMarkupRegex ) )
        return { { 0, lineByteLen, STYLE_COMMENT } };

    if( std::regex_match( line, kFieldRegex ) )
        return styleFieldLine( line );

    return styleInline( line );
}

std::vector< Span > RstContainerLexer::styleText( const std::string& utf8Text ) const
{
    const std::vector< std::string > lines = splitLines( utf8Text );
    std::vector< Span > result;
    std::size_t base = 0;

    for( std::size_t i = 0; i < lines.size(); ++i )
    {
        const std::string& line = lines[ i ];
        // 마지막 줄을 제외하면 개행 1바이트가 줄 길이에 포함된다.
        const std::size_t lineByteLen = line.size() + ( i + 1 < lines.size() ? 1 : 0 );
        if( lineByteLen == 0 )
            continue;

        std::vector< Span > spans = tokenizeLine( line,
                                                 i > 0 ? lines[ i - 1 ] : std::string{},
                                                 i + 1 < lines.size() ? lines[ i + 1 ] : std::string{},
                                                 i + 2 < lines.size() ? lines[ i + 2 ] : std::string{} );
        if( spans.empty() )
        {
            result.push_back( { base, base + lineByteLen, STYLE_DEFAULT } );
        }
        else
        {
            std::sort( spans.begin(), spans.end(),
                      []( const Span& left, const Span& right ) { return left.start < right.start; } );
            std::size_t pos = 0;
            for( const Span& span : spans )
            {
                if( span.start > pos )
                    result.push_back( { base + pos, base + span.start, STYLE_DEFAULT } );
                if( span.end > span.start )
                    result.push_back( { base + span.start, base + span.end, span.style } );
                pos = span.end;
            }
            if( pos < lineByteLen )
                result.push_back( { base + pos, base + lineByteLen, STYLE_DEFAULT } );
        }
        base += lineByteLen;
    }

    return result;
}

std::vector< unsigned char > RstContainerLexer::styleBytes( const std::string& utf8Text ) const
{
    std::vector< unsigned char > result( utf8Text.size(), static_cast< unsigned char >( STYLE_DEFAULT ) );
    for( const Span& span : styleText( utf8Text ) )
    {
        const std::size_t end = std::min( span.end, result.size() );
        for( std::size_t i = std::min( span.start, result.size() ); i < end; ++i )
            result[ i ] = static_cast< unsigned char >( span.style );
    }
    return result;
}

std::vector< Span > RstContainerLexer::styleDirectiveLine( const std::string& line ) const
{
    std::smatch match;
    if( !std::regex_match( line, match, kDirectiveRegex ) )
        return {};

    const std::string prefix = match[ 1 ].str();
    const std::string name = match[ 2 ].str();
    const std::string colons = match[ 3 ].str();
    const std::string rest = match[ 4 ].str();
    const int nameStyle = cache_.directiveStyle( name );

    std::vector< Span > spans;
    std::size_t pos = 0;
    spans.push_back( { pos, pos + prefix.size(), STYLE_EXPLICIT_MARKUP } );
    pos += prefix.size();
    spans.push_back( { pos, pos + name.size(), nameStyle } );
    pos += name.size();
    spans.push_back( { pos, pos + colons.size(), nameStyle } );
    pos += colons.size();
    if( !rest.empty() )
        spans.push_back( { pos, pos + rest.size(), STYLE_DEFAULT } );

    return spans;
}

std::vector< Span > RstContainerLexer::styleFieldLine( const std::string& line ) const
{
    std::smatch match;
    if( !std::regex_match( line, match, kFieldRegex ) )
        return {};

    const std::string indent = match[ 1 ].str();
    const std::string fieldName = match[ 2 ].str();
    const std::string rest = match[ 3 ].str();

    std::vector< Span > spans;
    if( !indent.empty() )
        spans.push_back( { 0, indent.size(), STYLE_DEFAULT } );

    const std::size_t fieldStart = indent.size();
    const std::size_t fieldEnd = fieldStart + fieldName.size() + 2;   // 앞뒤 콜론 2개
    spans.push_back( { fieldStart, fieldEnd, STYLE_FIELD_NAME } );
    if( !rest.empty() )
        spans.push_back( { fieldEnd, fieldEnd + rest.size(), STYLE_DEFAULT } );

    return spans;
}

std::vector< Span > RstContainerLexer::styleInline( const std::string& line ) const
{
    std::vector< Span > matches;
    appendMatchSpans( line, kInlineLiteralRegex, STYLE_INLINE_LITERAL, matches );
    appendMatchSpans( line, kStrongRegex, STYLE_STRONG, matches );
    appendMatchSpans( line, kEmphasisRegex, STYLE_EMPHASIS, matches, true );
    appendMatchSpans( line, kSubstitutionRegex, STYLE_SUBSTITUTION, matches );
    appendMatchSpans( line, kHyperlinkRegex, STYLE_HYPERLINK, matches );

    for( std::sregex_iterator it( line.begin(), line.end(), kRoleRegex ), end; it != end; ++it )
    {
        const std::smatch& match = *it;
        const std::string roleName = match[ 1 ].str();
        const std::size_t start = static_cast< std::size_t >( match.position( 0 ) );
        const std::size_t finish = start + static_cast< std::size_t >( match.length( 0 ) );
        matches.push_back( { start, finish, cache_.roleStyle( roleName ) } );
    }

    if( matches.empty() )
        return {};

    std::sort( matches.begin(), matches.end(), []( const Span& left, const Span& right ) {
        if( left.start == right.start )
            return left.end < right.end;
        return left.start < right.start;
    } );

    std::vector< Span > spans;
    std::size_t lastEnd = 0;
    for( const Span& match : matches )
    {
        // 앞선 매치와 겹치는 구간은 버린다 (먼저 잡힌 쪽이 이긴다).
        if( match.start < lastEnd )
            continue;
        if( match.start > lastEnd )
            spans.push_back( { lastEnd, match.start, STYLE_DEFAULT } );

        spans.push_back( match );
        lastEnd = match.end;
    }
    if( lastEnd < line.size() )
        spans.push_back( { lastEnd, line.size(), STYLE_DEFAULT } );

    return spans;
}

}  // namespace mrst::rst
