#include "stdafx.h"
#include "solRstOfflineCompletions.hpp"

#include <QObject>
#include <QRegularExpression>

namespace mrst::rstcomplete {
namespace {

// LSP CompletionItemKind 중 쓰는 것만.
constexpr int kKindClass = 7;
constexpr int kKindProperty = 10;
constexpr int kKindKeyword = 14;
constexpr int kKindFile = 17;
constexpr int kKindReference = 18;

struct DirectiveSpec
{
    const char* name;
    const char* detail;
    const char* options;   ///< 공백으로 구분
};

/// docutils + Sphinx 기본 directive. Esbonio 가 응답하기 전까지 쓰인다.
const QVector< DirectiveSpec >& directiveTable()
{
    static const QVector< DirectiveSpec > table{
        { "note",         "참고 admonition",        "class name" },
        { "warning",      "경고 admonition",        "class name" },
        { "tip",          "팁 admonition",          "class name" },
        { "important",    "중요 admonition",        "class name" },
        { "caution",      "주의 admonition",        "class name" },
        { "danger",       "위험 admonition",        "class name" },
        { "attention",    "주목 admonition",        "class name" },
        { "error",        "오류 admonition",        "class name" },
        { "hint",         "힌트 admonition",        "class name" },
        { "admonition",   "제목을 직접 쓰는 admonition", "class name" },
        { "code-block",   "구문 강조 코드 블록",
          "linenos lineno-start emphasize-lines caption name dedent force class" },
        { "code",         "코드 블록 (docutils)",   "number-lines class name" },
        { "literalinclude", "파일 내용 삽입",
          "language linenos lines start-after end-before emphasize-lines caption "
          "dedent tab-width encoding pyobject diff class name" },
        { "include",      "다른 reST 파일 삽입",
          "start-line end-line start-after end-before literal code encoding tab-width" },
        { "image",        "이미지",                 "alt height width scale align target class name" },
        { "figure",       "캡션이 있는 이미지",
          "alt height width scale align target figwidth figclass class name" },
        { "toctree",      "목차 트리",
          "maxdepth caption numbered titlesonly glob hidden includehidden reversed name" },
        { "table",        "제목이 있는 표",         "widths width align class name" },
        { "list-table",   "리스트로 쓰는 표",
          "header-rows stub-columns widths width align class name" },
        { "csv-table",    "CSV 로 쓰는 표",
          "header header-rows stub-columns widths file url encoding delim quote escape class name" },
        { "math",         "수식 블록",              "label nowrap class name" },
        { "rubric",       "제목처럼 보이는 단락",   "class name" },
        { "topic",        "주제 블록",              "class name" },
        { "sidebar",      "사이드바",               "subtitle class name" },
        { "parsed-literal", "마크업이 해석되는 리터럴", "class name" },
        { "epigraph",     "인용구",                 "class" },
        { "highlights",   "요약 인용",              "class" },
        { "pull-quote",   "발췌 인용",              "class" },
        { "compound",     "복합 단락",              "class name" },
        { "container",    "임의 컨테이너",          "name" },
        { "raw",          "가공하지 않은 출력",     "file url encoding class name" },
        { "replace",      "치환 정의 본문",         "" },
        { "unicode",      "유니코드 치환",          "trim ltrim rtrim" },
        { "date",         "날짜 치환",              "" },
        { "contents",     "문서 내 목차",           "depth local backlinks class name" },
        { "sectnum",      "섹션 번호 매기기",       "depth start prefix suffix" },
        { "index",        "색인 항목",              "name" },
        { "only",         "조건부 출력",            "" },
        { "versionadded", "추가된 버전",            "" },
        { "versionchanged", "변경된 버전",          "" },
        { "deprecated",   "폐지 예정",              "" },
        { "seealso",      "참고 자료",              "class name" },
        { "centered",     "가운데 정렬 (구식)",     "" },
        { "highlight",    "이후 코드 블록 언어 지정", "linenothreshold force" },
    };
    return table;
}

struct RoleSpec
{
    const char* name;
    const char* detail;
};

const QVector< RoleSpec >& roleTable()
{
    static const QVector< RoleSpec > table{
        { "ref",       "레이블 참조" },
        { "doc",       "문서 참조" },
        { "download",  "파일 내려받기 링크" },
        { "numref",    "번호로 참조" },
        { "term",      "용어집 항목" },
        { "abbr",      "약어" },
        { "command",   "명령어" },
        { "file",      "파일 경로" },
        { "guilabel",  "GUI 레이블" },
        { "kbd",       "키 입력" },
        { "menuselection", "메뉴 경로" },
        { "mailheader", "메일 헤더" },
        { "manpage",   "man 페이지" },
        { "program",   "프로그램 이름" },
        { "samp",      "치환 가능한 코드" },
        { "math",      "인라인 수식" },
        { "eq",        "수식 참조" },
        { "envvar",    "환경 변수" },
        { "option",    "명령행 옵션" },
        { "regexp",    "정규식" },
        { "subscript", "아래 첨자" },
        { "superscript", "위 첨자" },
        { "title-reference", "제목 참조" },
        { "pep",       "PEP 참조" },
        { "rfc",       "RFC 참조" },
        { "emphasis",  "강조" },
        { "strong",    "굵게" },
        { "literal",   "리터럴" },
        { "code",      "코드" },
    };
    return table;
}

/// 경로 인자를 받는 directive (경로 완성 대상).
bool takesPathArgument( const QString& name )
{
    static const QStringList names{ QStringLiteral( "image" ), QStringLiteral( "figure" ),
                                   QStringLiteral( "include" ), QStringLiteral( "literalinclude" ),
                                   QStringLiteral( "raw" ), QStringLiteral( "csv-table" ) };
    return names.contains( name );
}

QStringList optionsFor( const QString& directiveName )
{
    for( const DirectiveSpec& spec : directiveTable() )
    {
        if( QLatin1String( spec.name ) == directiveName )
        {
            const QString options = QString::fromLatin1( spec.options );
            return options.isEmpty() ? QStringList{}
                                     : options.split( QLatin1Char( ' ' ), Qt::SkipEmptyParts );
        }
    }
    return {};
}

/// 줄 앞쪽 공백 문자 수.
int leadingIndent( const QString& line )
{
    int index = 0;
    while( index < line.length() && line.at( index ).isSpace() )
        ++index;
    return index;
}

/// 커서가 directive 블록 안(들여쓰기된 본문)이라면 그 directive 이름.
///
/// previousLines 는 역순이다 (바로 앞 줄이 [0]). 위로 거슬러 올라가다가
/// 처음 만나는 "더 얕게 들여쓴 줄" 이 directive 면 그 안에 있는 것이고,
/// 아니면 블록 밖이다.
QString enclosingDirective( const QStringList& previousLines, const int currentIndent )
{
    static const QRegularExpression directiveRe(
        QStringLiteral( R"(^(\s*)\.\.\s+([a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?)::)" ) );

    for( const QString& line : previousLines )
    {
        if( line.trimmed().isEmpty() )
            continue;

        const int indent = leadingIndent( line );
        if( indent >= currentIndent )
            continue;   // 같은 블록 안의 다른 줄. 계속 위로 본다.

        // 더 얕게 들여쓴 첫 줄. 이게 directive 여야 우리가 그 블록 안이다.
        const QRegularExpressionMatch match = directiveRe.match( line );
        return match.hasMatch() ? match.captured( 2 ) : QString{};
    }
    return {};
}

}  // namespace

QStringList knownDirectives()
{
    QStringList names;
    for( const DirectiveSpec& spec : directiveTable() )
    {
        const QString name = QString::fromLatin1( spec.name );
        if( !names.contains( name ) )
            names << name;
    }
    return names;
}

QStringList knownRoles()
{
    QStringList names;
    for( const RoleSpec& spec : roleTable() )
        names << QString::fromLatin1( spec.name );
    return names;
}

Context detectContext( const QString& lineText, const int column, const QStringList& previousLines )
{
    Context context;

    const int caret = qBound( 0, column - 1, static_cast< int >( lineText.length() ) );
    const QString before = lineText.left( caret );

    // ".. dir" / ".. dir::" — directive 이름
    static const QRegularExpression directiveRe( QStringLiteral( R"(^(\s*)\.\.\s+([a-zA-Z0-9_.-]*)$)" ) );
    if( const QRegularExpressionMatch match = directiveRe.match( before ); match.hasMatch() )
    {
        context.kind = ContextKind::Directive;
        context.prefix = match.captured( 2 );
        context.replaceLength = context.prefix.length();
        return context;
    }

    // 경로 인자: ".. image:: pa"
    static const QRegularExpression pathRe(
        QStringLiteral( R"(^\s*\.\.\s+([a-zA-Z0-9_.-]+)::\s+(\S*)$)" ) );
    if( const QRegularExpressionMatch match = pathRe.match( before ); match.hasMatch() )
    {
        if( takesPathArgument( match.captured( 1 ) ) )
        {
            context.kind = ContextKind::Path;
            context.directiveName = match.captured( 1 );
            context.prefix = match.captured( 2 );
            context.replaceLength = context.prefix.length();
            return context;
        }
    }

    // directive 블록 안의 옵션: 들여쓰기 + ":opt"
    static const QRegularExpression optionRe( QStringLiteral( R"(^(\s+):([a-zA-Z0-9_-]*)$)" ) );
    if( const QRegularExpressionMatch match = optionRe.match( before ); match.hasMatch() )
    {
        const QString owner = enclosingDirective( previousLines, match.captured( 1 ).length() );
        if( !owner.isEmpty() )
        {
            context.kind = ContextKind::DirectiveOption;
            context.directiveName = owner;
            context.prefix = match.captured( 2 );
            context.replaceLength = context.prefix.length();
            return context;
        }
    }

    // :ref:`tar` — 참조 대상
    static const QRegularExpression targetRe(
        QStringLiteral( R"(:([a-zA-Z0-9_.-]+):`([^`]*)$)" ) );
    if( const QRegularExpressionMatch match = targetRe.match( before ); match.hasMatch() )
    {
        context.kind = ContextKind::RoleTarget;
        context.directiveName = match.captured( 1 );
        context.prefix = match.captured( 2 );
        context.replaceLength = context.prefix.length();
        return context;
    }

    // 인라인 role 이름: ":re"
    // 줄 맨 앞의 ":opt:" 필드는 위에서 이미 걸러졌다.
    static const QRegularExpression roleRe( QStringLiteral( R"((?:^|[\s(\[{])(:[a-zA-Z0-9_.-]*)$)" ) );
    if( const QRegularExpressionMatch match = roleRe.match( before ); match.hasMatch() )
    {
        context.kind = ContextKind::Role;
        context.prefix = match.captured( 1 ).mid( 1 );   // 앞의 ':' 제외
        context.replaceLength = context.prefix.length();
        return context;
    }

    return context;
}

QVector< Item > candidatesFor( const Context& context )
{
    QVector< Item > items;

    switch( context.kind )
    {
        case ContextKind::Directive:
        {
            QStringList seen;
            for( const DirectiveSpec& spec : directiveTable() )
            {
                const QString name = QString::fromLatin1( spec.name );
                if( seen.contains( name ) )
                    continue;
                seen << name;
                items.push_back( { name, name + QStringLiteral( ":: " ),
                                  QString::fromLatin1( spec.detail ), kKindClass } );
            }
            break;
        }
        case ContextKind::Role:
            for( const RoleSpec& spec : roleTable() )
            {
                const QString name = QString::fromLatin1( spec.name );
                items.push_back( { name, name + QStringLiteral( ":`" ),
                                  QString::fromLatin1( spec.detail ), kKindKeyword } );
            }
            break;

        case ContextKind::DirectiveOption:
            for( const QString& option : optionsFor( context.directiveName ) )
            {
                items.push_back( { option, option + QStringLiteral( ": " ),
                                  QObject::tr( "%1 옵션" ).arg( context.directiveName ),
                                  kKindProperty } );
            }
            break;

        case ContextKind::RoleTarget:
            // 실제 대상 수집은 LSP 나 워크스페이스 스캔이 맡는다.
            // 오프라인 표에는 후보가 없다.
            break;

        case ContextKind::Path:
            // 경로 후보는 호출 측이 파일 시스템에서 채운다.
            break;

        case ContextKind::None:
            break;
    }

    // 이미 입력한 접두로 거른다.
    if( !context.prefix.isEmpty() )
    {
        QVector< Item > filtered;
        for( const Item& item : items )
        {
            if( item.label.startsWith( context.prefix, Qt::CaseInsensitive ) )
                filtered.push_back( item );
        }
        return filtered;
    }

    return items;
}

}  // namespace mrst::rstcomplete
