#pragma once

// reStructuredText 구조 판정의 단일 출처.
//
// 예전에는 같은 규칙이 다섯 곳에 서로 다르게 구현되어 있었다 — 렉서, 접기, 개요,
// 자동완성 문맥, 치환/용어집 인덱스. 섹션 제목 판정만 해도 장식 문자 집합·동질성·
// 최소 길이·들여쓰기·길이 비교 단위 다섯 축이 전부 갈라져 있었고, 그 차이가 실제
// 문서에서 발현했다(개요에는 나오는데 화면에는 색이 없는 섹션).
//
// 이 모듈은 Qt 에 의존하지 않는다. 같은 디렉터리의 MarkdownStructure 와 같은 자격이며
// mrst_tests 에서 그대로 컴파일된다.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace mrst::rst {

// ── 문자 분류 ────────────────────────────────────────────

/// directive·role 이름에 쓸 수 있는 문자. docutils 는 `+` 도 허용한다.
[[nodiscard]] bool isNameChar( char ch ) noexcept;

/// 섹션 장식과 구분선에 쓸 수 있는 문자.
///
/// docutils 의 `[!-/:-@[-`{-~]` 그대로다 — ASCII 문장부호 32자 전부.
/// 예전 렉서는 12자, 개요는 14자를 임의로 골라 썼고 그래서 서로 갈라졌다.
[[nodiscard]] bool isAdornmentChar( char ch ) noexcept;

/// 줄 안에서 공백으로 치는 문자. 개행은 줄에 들어오지 않는다.
[[nodiscard]] bool isInlineSpace( char ch ) noexcept;

/// pos 에서 시작하는 `name` 또는 `domain:name` 의 끝 오프셋. 없으면 pos.
[[nodiscard]] std::size_t scanQualifiedName( std::string_view line, std::size_t pos ) noexcept;

// ── 줄 단위 원시 함수 ────────────────────────────────────

/// 줄 앞 공백의 시각적 폭. 탭은 docutils 와 같이 8칸으로 편다.
[[nodiscard]] int indentWidth( std::string_view line ) noexcept;

[[nodiscard]] bool isBlank( std::string_view line ) noexcept;

/// 앞뒤 공백을 뗀 view. 복사하지 않는다.
[[nodiscard]] std::string_view trimView( std::string_view line ) noexcept;

/// UTF-8 문자(코드포인트) 수.
[[nodiscard]] std::size_t utf8Length( std::string_view value ) noexcept;

/// 화면에 차지하는 칸 수. 동아시아 문자(W/F)는 두 칸이다.
///
/// docutils 가 제목과 장식 길이를 비교할 때 쓰는 단위이며, 예전 구현들은 이것을
/// 코드포인트 수(렉서)나 UTF-16 코드 유닛(개요)으로 재서 한글 제목에서 어긋났다.
/// `제목` 은 두 글자지만 네 칸이므로 `==` 로는 밑줄이 되지 않는다.
[[nodiscard]] std::size_t columnWidth( std::string_view value ) noexcept;

// ── 줄 경계 색인 ─────────────────────────────────────────

/// 줄 경계만 유지한다. 본문을 복사하지 않는다.
///
/// line() 은 개행과 말미 CR 을 뗀 view 를 낸다 — CR 처리를 여기 한 곳으로 모은다.
class LineIndex
{
public:
    LineIndex() = default;
    explicit LineIndex( std::string_view text );

    void reset( std::string_view text );

    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] std::size_t      size() const noexcept { return starts_.size(); }

    /// 범위 밖이면 빈 view.
    [[nodiscard]] std::string_view line( std::size_t index ) const noexcept;
    /// 줄의 첫 바이트 오프셋. 범위 밖이면 text().size().
    [[nodiscard]] std::size_t byteStart( std::size_t index ) const noexcept;
    /// 개행을 포함한 줄 길이.
    [[nodiscard]] std::size_t byteLength( std::size_t index ) const noexcept;
    /// 바이트 오프셋이 속한 줄 번호.
    [[nodiscard]] std::size_t lineAt( std::size_t byteOffset ) const noexcept;

private:
    std::string_view             text_;
    std::vector< std::uint32_t > starts_;
};

// ── 구성 요소 인식 ───────────────────────────────────────

/// `.. |sub| name:: rest` 의 각 구간 끝 오프셋.
struct DirectiveParts
{
    std::uint32_t prefixEnd = 0;   ///< ".." 와 뒤 공백의 끝
    std::uint32_t subStart  = 0;   ///< "|sub|" 시작. 없으면 prefixEnd
    std::uint32_t subEnd    = 0;   ///< 없으면 prefixEnd
    std::uint32_t nameStart = 0;
    std::uint32_t nameEnd   = 0;
    std::uint32_t colonsEnd = 0;   ///< "::" 의 끝

    [[nodiscard]] bool hasSubstitution() const noexcept { return subEnd > subStart; }
};

/// `:name: rest` 의 각 구간.
struct FieldParts
{
    std::uint32_t indentEnd = 0;
    std::uint32_t nameStart = 0;   ///< 여는 ':' 다음
    std::uint32_t nameEnd   = 0;   ///< 닫는 ':' 앞
    std::uint32_t markerEnd = 0;   ///< 닫는 ':' 다음
};

[[nodiscard]] std::optional< DirectiveParts > parseDirective( std::string_view line ) noexcept;

/// docutils 의 field marker 는 닫는 콜론 뒤에 공백 또는 줄끝을 요구한다.
/// 그 조건이 없으면 줄머리의 `` :ref:`x` `` 가 필드로 오인된다.
[[nodiscard]] std::optional< FieldParts > parseField( std::string_view line ) noexcept;

/// `.. ` 로 시작하되 directive 가 아닌 줄(주석·하이퍼링크 대상·각주 등).
[[nodiscard]] bool hasExplicitMarkupPrefix( std::string_view line ) noexcept;

/// 0열에서 시작하는 **같은 장식 문자의 연속**이고 나머지가 공백인가.
/// 맞으면 그 문자를, 아니면 '\0' 을 낸다. 길이는 outLength 에 담는다.
///
/// 들여쓴 줄은 장식으로 보지 않는다. 허용하면 code-block 본문의 `////`·`----` 가
/// 전부 장식이 되는데, 리터럴 블록 추적이 없는 지금은 구별할 수단이 없다.
[[nodiscard]] char adornmentRun( std::string_view line, std::size_t* outLength = nullptr ) noexcept;

/// 홀로 선 구분선인가. docutils 는 4자 이상을 요구한다.
[[nodiscard]] bool isTransitionLine( std::string_view line ) noexcept;

// ── 섹션 제목 ────────────────────────────────────────────

struct TitleRun
{
    std::uint32_t    firstLine = 0;   ///< 윗줄이 있으면 제목 글자 줄 - 1
    std::uint32_t    textLine  = 0;   ///< 제목 글자가 있는 줄
    std::uint32_t    lastLine  = 0;   ///< 밑줄
    char             adornChar = '\0';
    bool             overlined = false;
    std::string_view text;            ///< trim 한 제목 글자

    [[nodiscard]] std::uint32_t lineSpan() const noexcept { return lastLine - firstLine + 1; }
};

/// index 에서 시작하는 제목 후보 묶음.
///
/// docutils 는 윗줄과 아랫줄이 어긋나도 **세 줄을 통째로 소비하고** 오류를 낸다.
/// 그래서 소비 길이와 제목 여부를 따로 돌려준다 — 문서를 앞에서부터 훑는 쪽은
/// span 만큼 건너뛰어야 가운데 줄이 뒤늦게 밑줄형 제목으로 잡히는 일이 없다.
struct TitleScan
{
    std::uint32_t             span = 0;   ///< 0 이면 이 줄에서 시작하는 묶음이 없다
    std::optional< TitleRun > title;      ///< 비어 있으면 소비만 하고 제목은 아니다
};

/// docutils 규칙 그대로다(parsers/rst/states.py 의 Text.underline, Line.text).
///  * 장식은 같은 문장부호의 연속이고 0열에서 시작한다.
///  * 윗줄이 있으면 **아랫줄과 문자·길이가 정확히 같아야** 한다.
///  * `columnWidth(제목) > len(장식)` 이면서 `len(장식) < 4` 면 제목이 아니다.
///    (넘되 4 이상이면 docutils 는 경고만 하고 섹션으로 인정한다.)
[[nodiscard]] TitleScan titleScanAt( const LineIndex& lines, std::size_t index );

/// 제목만 필요한 곳을 위한 얇은 질의. 소비 길이는 버린다.
[[nodiscard]] std::optional< TitleRun > titleAt( const LineIndex& lines, std::size_t index );

// ── 인라인 마크업 ────────────────────────────────────────

enum class InlineKind
{
    Literal,        ///< ``code``
    Strong,         ///< **strong**
    Emphasis,       ///< *em*
    Substitution,   ///< |name|
    Hyperlink,      ///< `text`_ 또는 word_
    Role,           ///< :name:`target`
    Interpreted,    ///< `text`
};

struct InlineToken
{
    std::uint32_t start     = 0;
    std::uint32_t end       = 0;
    InlineKind    kind      = InlineKind::Literal;
    std::uint32_t nameStart = 0;   ///< Role 일 때 이름 구간
    std::uint32_t nameEnd   = 0;
};

/// 한 줄의 인라인 마크업을 좌→우로 한 번 훑는다.
///
/// reST 는 인라인 마크업의 중첩을 금지하므로(예외는 role 의 백틱 안쪽이고 그 내부는
/// 해석하지 않는다) 여는 문자열을 만나면 닫는 문자열은 정방향 탐색 한 번으로
/// 확정된다. 델리미터 스택이 필요하지 않다.
///
/// 산출은 겹치지 않고 start 오름차순이다. out 은 append 되며 비워지지 않는다.
void scanInline( std::string_view line, std::vector< InlineToken >& out );

}   // namespace mrst::rst
