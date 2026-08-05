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

struct Context
{
    ContextKind                         kind = ContextKind::None;
    QString                             prefix;          ///< 이미 입력된 부분
    QString                             directiveName;   ///< DirectiveOption 일 때 어떤 directive 인지
    /// 완성 항목을 넣을 때 지워야 할 글자 수. prefix 길이와 같지만
    /// 컨텍스트에 따라 달라질 수 있어 따로 둔다.
    int                                 replaceLength = 0;
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

}  // namespace mrst::rstcomplete
