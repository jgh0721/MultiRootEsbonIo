#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace mrst::rst {

/// Scintilla 스타일 ID. Lexilla 에는 reStructuredText 렉서가 없어서 컨테이너
/// 렉싱(ILexer 를 비우고 SCN_STYLENEEDED 를 직접 처리)으로 칠한다.
enum Style : int
{
    STYLE_DEFAULT = 0,
    STYLE_COMMENT = 1,
    STYLE_TITLE = 2,
    STYLE_TRANSITION = 3,
    STYLE_DIRECTIVE_VALID = 4,
    STYLE_DIRECTIVE_INVALID = 5,
    STYLE_DIRECTIVE_UNKNOWN = 6,
    STYLE_ROLE_VALID = 7,
    STYLE_ROLE_INVALID = 8,
    STYLE_ROLE_UNKNOWN = 9,
    STYLE_LITERAL = 10,
    STYLE_EMPHASIS = 11,
    STYLE_STRONG = 12,
    STYLE_INTERPRETED = 13,
    STYLE_INLINE_LITERAL = 14,
    STYLE_HYPERLINK = 15,
    STYLE_SUBSTITUTION = 16,
    STYLE_FIELD_NAME = 17,
    STYLE_EXPLICIT_MARKUP = 18,

    STYLE_COUNT = 19,
};

struct CompletionEntry
{
    std::string label;
    std::string insertText;
    std::string detail;
};

/// Esbonio 자동완성 결과에서 수확한 directive/role 이름.
///
/// 3-state 로 동작한다: 캐시가 비어 있으면 UNKNOWN, 채워진 뒤에는 목록에
/// 있으면 VALID, 없으면 INVALID. 덕분에 LSP 가 준비되기 전에 멀쩡한 문서가
/// 빨갛게 물드는 일이 없다.
class RstMetadataCache
{
public:
    std::set< std::string > directives;
    std::set< std::string > roles;
    bool directivesPopulated = false;
    bool rolesPopulated = false;

    [[nodiscard]] int directiveStyle( const std::string& name ) const;
    [[nodiscard]] int roleStyle( const std::string& name ) const;
    void updateFromCompletion( const std::vector< CompletionEntry >& entries );
};

/// UTF-8 바이트 오프셋 기준 구간. Scintilla 위치와 단위가 같다.
struct Span
{
    std::size_t start = 0;
    std::size_t end = 0;
    int style = STYLE_DEFAULT;
};

[[nodiscard]] std::string extractDirectiveName( const std::string& label, const std::string& insertText );

class RstContainerLexer
{
public:
    explicit RstContainerLexer( RstMetadataCache cache = {} );

    [[nodiscard]] const RstMetadataCache& metadataCache() const;
    [[nodiscard]] RstMetadataCache& metadataCache();

    /// lineAfterNext 는 윗줄/아랫줄로 감싼 제목(overline)을 알아보는 데 쓴다.
    ///     ########
    ///     제목
    ///     ########
    /// 첫 줄만 보면 단독 구분선과 구별할 수 없어 두 줄 아래까지 필요하다.
    [[nodiscard]] std::vector< Span > tokenizeLine( const std::string& line,
                                                    const std::string& previousLine,
                                                    const std::string& nextLine,
                                                    const std::string& lineAfterNext = {} ) const;

    [[nodiscard]] std::vector< Span > styleText( const std::string& utf8Text ) const;

    /// 바이트당 스타일 1개. 길이는 항상 utf8Text.size() 와 같다 (Scintilla
    /// SCI_SETSTYLINGEX 가 바이트 단위로 요구하기 때문).
    [[nodiscard]] std::vector< unsigned char > styleBytes( const std::string& utf8Text ) const;

private:
    RstMetadataCache cache_;

    [[nodiscard]] std::vector< Span > styleDirectiveLine( const std::string& line ) const;
    [[nodiscard]] std::vector< Span > styleFieldLine( const std::string& line ) const;
    [[nodiscard]] std::vector< Span > styleInline( const std::string& line ) const;
};

}  // namespace mrst::rst
