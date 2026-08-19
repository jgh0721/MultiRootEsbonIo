#pragma once

#include "core/solRstOfflineCompletions.hpp"

#include <QString>
#include <QStringList>

namespace mrst::rstpath {

/// 경로 인자의 문법 형태.
enum class Shape
{
    FilePath,   ///< 확장자까지 그대로 쓴다
    DocName,    ///< 확장자를 뗀 docname (toctree 항목, :doc: 대상)
};

/// 인자 안에서 공백을 어떻게 다뤄야 하는가.
///
/// docutils 가 그 자리를 무엇으로 변환하느냐에 달렸다. 세 갈래가 실제로 다르다.
enum class Spaces
{
    /// `directives.path()` — 줄 안 공백을 그대로 보존한다.
    /// (include, literalinclude, :file:, :download:)
    Preserve,
    /// `directives.uri()` — 이스케이프하지 않은 공백을 **전부 지운다**.
    /// `\ ` 로 이스케이프해야 진짜 공백으로 살아난다. (image, figure, :target:)
    ///
    /// `%20` 은 답이 아니다. Sphinx 의 `relfn2path` 는 퍼센트 디코딩을 하지 않아
    /// 존재하지 않는 파일을 가리키게 된다.
    Escape,
    /// 인자에 공백이 올 수 없다. 공백이 든 후보는 아예 내지 않는다.
    /// (toctree 항목, graphviz, :doc:)
    Forbidden,
};

/// 경로 하나가 놓일 수 있는 자리의 규칙.
struct Slot
{
    Shape                       shape = Shape::FilePath;
    Spaces                      spaces = Spaces::Preserve;
    /// 후보로 받아들일 확장자. **비어 있으면 무엇이든 받는다** —
    /// literalinclude 나 :download: 는 실제로 무엇이든 받기 때문이다.
    QStringList                 accepted;
    /// 받아들이되 목록 위로 올릴 확장자. 거르지는 않는다.
    QStringList                 preferred;
};

// ── 슬롯 표 ───────────────────────────────────────────────
// 아래 넷은 docutils/Sphinx 실제 구현을 보고 만든 것이다. 특히 `raw` 의 인자는
// 포맷 이름이고 `csv-table` 의 인자는 표 제목이다 — 둘 다 경로가 아니다.

/// `.. name:: <인자>` 의 인자.
[[nodiscard]] const Slot* slotForArgument( const QString& directiveName );
/// directive 블록 안 `   :option: <값>` 의 값.
[[nodiscard]] const Slot* slotForOption( const QString& directiveName, const QString& optionName );
/// directive 본문의 각 줄 (지금은 toctree 뿐).
[[nodiscard]] const Slot* slotForBody( const QString& directiveName );
/// `` :role:`대상` `` 의 대상.
[[nodiscard]] const Slot* slotForRoleTarget( const QString& roleName );
/// 완성 컨텍스트에 해당하는 슬롯. 경로 컨텍스트가 아니면 nullptr.
[[nodiscard]] const Slot* slotFor( const rstcomplete::Context& context );

// ── 문자열 다루기 ─────────────────────────────────────────

/// 입력된 경로를 디렉터리 부분과 마지막 조각으로 가른 결과.
///
/// 둘 다 **디코딩된** 값이다 (`\ ` 는 공백으로 되돌아간다). 문서에 다시 넣을
/// 때는 `encodeForInsertion()` 을 거친다.
struct TypedPath
{
    QString                     directory;        ///< 마지막 구분자까지. 끝에 '/' 는 없다
    QString                     name;             ///< 마지막 구분자 뒤. 팝업 필터에 쓸 조각
    bool                        fromSourceRoot = false;   ///< '/' 로 시작했다 (Sphinx srcdir 기준)
};

/// 구분자는 `/` 와 `\` 둘 다 받는다. 단 `\` 뒤에 공백이 오면 구분자가 아니라
/// reST 이스케이프다 — 그 경우 공백 하나로 디코딩한다.
[[nodiscard]] TypedPath splitTypedPath( const QString& typed );

/// 문서 기준 상대 경로를 슬롯 규칙에 맞춰 문서에 넣을 문자열로 바꾼다.
/// 구분자는 언제나 `/` 다.
[[nodiscard]] QString  encodeForInsertion( const QString& relativePath, const Slot& slot );

/// 슬롯이 이 파일 이름을 후보로 받아들이는가 (확장자 + 공백 규칙).
[[nodiscard]] bool     acceptsFileName( const QString& fileName, const Slot& slot );

/// docname 으로 쓸 수 있게 Sphinx 소스 접미사만 뗀다.
///
/// `QFileInfo::completeBaseName()` 을 쓰면 안 되는 것은 아니지만 의미가 다르다.
/// Sphinx 는 등록된 접미사만 `removesuffix` 하므로 `api.v2.rst` 는 `api.v2` 여야
/// 하고 `api` 가 되면 안 된다. 여기서는 그 규칙을 명시적으로 흉내 낸다.
[[nodiscard]] QString  stripDocumentSuffix( const QString& fileName );

}   // namespace mrst::rstpath
