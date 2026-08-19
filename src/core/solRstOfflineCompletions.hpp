#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mrst::rstcomplete {

/// 커서 위치에서 무엇을 완성해야 하는가.
enum class ContextKind
{
    None,           ///< 완성할 것이 없다
    Directive,      ///< ".. " 뒤 — directive 이름
    Role,           ///< ":" 뒤 — role 이름
    DirectiveOption,///< directive 블록 안의 ":옵션:"
    RoleTarget,     ///< :ref:`...` 안 — 참조 대상
    Path,           ///< image/include 등의 경로 인자
};

/// Path 컨텍스트에서 경로가 문법의 어느 자리에 있는가.
///
/// 슬롯 표(rstpath)를 다시 찾을 수 있도록 detectContext 가 남겨 둔다. 표 자체를
/// 여기 두지 않는 것은 이 모듈이 파일 시스템도 Qt GUI 도 모르는 순수 판정기로
/// 남아야 하기 때문이다.
enum class PathSlotSite
{
    None,
    Argument,     ///< ".. image:: <여기>"
    Option,       ///< "   :file: <여기>"
    Body,         ///< toctree 본문의 각 줄
    RoleTarget,   ///< :download:`<여기>`
};

struct Context
{
    ContextKind                         kind = ContextKind::None;
    /// 이미 입력된 부분. **경로면 경로 전체**다 (예: "../img/lo").
    QString                             prefix;
    /// DirectiveOption/Path 면 소유 directive, RoleTarget 이면 롤 이름.
    QString                             directiveName;
    /// 옵션 값 자리일 때 그 옵션 이름 (":file:" 이면 "file").
    QString                             optionName;
    /// 완성 항목을 넣을 때 지워야 할 글자 수. prefix 길이와 같지만
    /// 컨텍스트에 따라 달라질 수 있어 따로 둔다.
    int                                 replaceLength = 0;

    PathSlotSite                        pathSite = PathSlotSite::None;

    /// 팝업의 퍼지 필터에 넘길 접두.
    ///
    /// 경로가 아니면 prefix 와 같다. 경로면 **마지막 구분자 뒤 조각**이다.
    /// 경로 후보의 라벨은 파일 이름뿐이라 "../img/lo" 전체로 거르면
    /// 부분수열 검사가 전부 실패해 목록이 통째로 사라진다.
    QString                             filterPrefix;

    /// ".. image::" 처럼 인자 앞 공백이 아직 없다. 삽입 문자열 앞에 공백을 붙인다.
    bool                                argumentNeedsSpace = false;
};

struct Item
{
    QString                             label;
    QString                             insertText;
    QString                             detail;
    int                                 kind = 0;        ///< LSP CompletionItemKind
};

/// 커서 앞 텍스트를 보고 완성 컨텍스트를 판정한다.
///
/// lineText 는 현재 줄 전체, column 은 1-based 캐럿 열.
/// previousLines 는 directive 블록 안인지 판단하는 데 쓴다 (역순: 바로 앞 줄이 [0]).
[[nodiscard]] Context detectContext( const QString& lineText, int column,
                                     const QStringList& previousLines = {} );

/// Esbonio 가 아직 응답하지 못할 때 쓰는 기본 후보.
///
/// Esbonio 2.x 는 내부 Sphinx 빌드가 끝날 때까지 completion 에 빈 결과를
/// 돌려준다. 그 수십 초 동안 타이핑을 쓸모 있게 만드는 것이 이 표의 목적이다.
[[nodiscard]] QVector< Item > candidatesFor( const Context& context );

/// 표에 들어 있는 directive / role 이름 (테스트와 메타데이터 초기화용).
[[nodiscard]] QStringList knownDirectives();
[[nodiscard]] QStringList knownRoles();

// ── LSP 응답 다듬기 ────────────────────────────────────────
// Esbonio 가 돌려주는 것을 그대로 넣으면 문서가 깨진다. 아래 셋은 순수 함수라
// 위젯 없이 검증한다.

/// Esbonio 는 ".." 직후에 발화될 것을 전제로 " image::" 처럼 **앞에 공백이 붙은**
/// insertText 를 돌려준다. 사용자가 ".. " 까지 친 뒤 완성을 부르면 "..  image::"
/// 처럼 공백이 둘이 된다. directive 컨텍스트에서만 앞 공백을 떼어낸다.
[[nodiscard]] QVector< Item > normalizeLspItems( QVector< Item > items, const QString& lineText,
                                                 int column );

/// 팝업에 올리기 전 정리: 제어 문자 제거, 빈/중복 insertText 제거, 개수 제한.
[[nodiscard]] QVector< Item > finalizeItems( QVector< Item > items, int limit = 200 );

/// primary 를 앞에 두고, additional 중 insertText 가 겹치지 않는 것만 뒤에 붙인다.
/// LSP 결과 위에 오프라인 후보를 보충할 때 쓴다.
[[nodiscard]] QVector< Item > mergeItems( QVector< Item > primary, const QVector< Item >& additional );

}  // namespace mrst::rstcomplete
