#include "stdafx.h"
#include "solRstOfflineCompletions.hpp"

#include <QCoreApplication>
#include <QObject>
#include <QRegularExpression>
#include <QSet>

namespace mrst::rstcomplete {
namespace {

// LSP CompletionItemKind 중 쓰는 것만.
constexpr int kKindClass = 7;
constexpr int kKindProperty = 10;
constexpr int kKindKeyword = 14;
constexpr int kKindFile = 17;
constexpr int kKindReference = 18;

/// detail 은 자동완성 팝업에 그대로 보이는 문구라 번역 대상이다.
///
/// 정적 표라 tr() 을 쓸 수 없다 — tr() 은 실행 시점에 평가돼야 하는데 여기는
/// 표의 초기화 목록이다. QT_TRANSLATE_NOOP 로 lupdate 에만 알려 두고, 실제
/// 번역은 소비 지점에서 QCoreApplication::translate() 로 한다. translate() 는
/// 소스 문자열을 UTF-8 로 읽고 번역을 못 찾으면 원문을 그대로 돌려주므로,
/// 한국어 동작은 예전 QString::fromUtf8() 과 바이트 단위로 같다.
///
/// ⚠ 컨텍스트 "RstOfflineCompletions" 는 NOOP 과 translate() 양쪽이 글자 하나까지
///   같아야 한다. 다르면 lupdate 는 항목을 만들지만 실행 시점에는 못 찾아
///   조용히 원문이 나온다. QT_TRANSLATE_NOOP 은 컨텍스트를 리터럴로만 받으므로
///   상수로 뺄 수 없다 — 그래서 양쪽에 그대로 적는다.
///
/// options 는 ASCII 옵션명 목록이라 번역하지 않는다.
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
        { "note",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "참고 admonition" ),        "class name" },
        { "warning",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "경고 admonition" ),        "class name" },
        { "tip",          QT_TRANSLATE_NOOP( "RstOfflineCompletions", "팁 admonition" ),          "class name" },
        { "important",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "중요 admonition" ),        "class name" },
        { "caution",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주의 admonition" ),        "class name" },
        { "danger",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "위험 admonition" ),        "class name" },
        { "attention",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주목 admonition" ),        "class name" },
        { "error",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "오류 admonition" ),        "class name" },
        { "hint",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "힌트 admonition" ),        "class name" },
        { "admonition",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목을 직접 쓰는 admonition" ), "class name" },
        { "code-block",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "구문 강조 코드 블록" ),
          "linenos lineno-start emphasize-lines caption name dedent force class" },
        { "code",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "코드 블록 (docutils)" ),   "number-lines class name" },
        { "literalinclude", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 내용 삽입" ),
          "language linenos lines start-after end-before emphasize-lines caption "
          "dedent tab-width encoding pyobject diff class name" },
        { "include",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "다른 reST 파일 삽입" ),
          "start-line end-line start-after end-before literal code encoding tab-width" },
        { "image",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "이미지" ),                 "alt height width scale align target class name" },
        { "figure",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "캡션이 있는 이미지" ),
          "alt height width scale align target figwidth figclass class name" },
        { "toctree",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "목차 트리" ),
          "maxdepth caption numbered titlesonly glob hidden includehidden reversed name" },
        { "table",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목이 있는 표" ),         "widths width align class name" },
        { "list-table",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "리스트로 쓰는 표" ),
          "header-rows stub-columns widths width align class name" },
        { "csv-table",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "CSV 로 쓰는 표" ),
          "header header-rows stub-columns widths file url encoding delim quote escape class name" },
        { "math",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "수식 블록" ),              "label nowrap class name" },
        { "rubric",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목처럼 보이는 단락" ),   "class name" },
        { "topic",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주제 블록" ),              "class name" },
        { "sidebar",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "사이드바" ),               "subtitle class name" },
        { "parsed-literal", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "마크업이 해석되는 리터럴" ), "class name" },
        { "epigraph",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "인용구" ),                 "class" },
        { "highlights",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "요약 인용" ),              "class" },
        { "pull-quote",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "발췌 인용" ),              "class" },
        { "compound",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "복합 단락" ),              "class name" },
        { "container",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "임의 컨테이너" ),          "name" },
        { "raw",          QT_TRANSLATE_NOOP( "RstOfflineCompletions", "가공하지 않은 출력" ),     "file url encoding class name" },
        { "replace",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "치환 정의 본문" ),         "" },
        { "unicode",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "유니코드 치환" ),          "trim ltrim rtrim" },
        { "date",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "날짜 치환" ),              "" },
        { "contents",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "문서 내 목차" ),           "depth local backlinks class name" },
        { "sectnum",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "섹션 번호 매기기" ),       "depth start prefix suffix" },
        { "index",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "색인 항목" ),              "name" },
        { "only",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "조건부 출력" ),            "" },
        { "versionadded", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "추가된 버전" ),            "" },
        { "versionchanged", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "변경된 버전" ),          "" },
        { "deprecated",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "폐지 예정" ),              "" },
        { "seealso",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "참고 자료" ),              "class name" },
        { "centered",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "가운데 정렬 (구식)" ),     "" },
        { "highlight",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "이후 코드 블록 언어 지정" ), "linenothreshold force" },
    };
    return table;
}

/// detail 은 DirectiveSpec 과 같다 — QT_TRANSLATE_NOOP 로 표시하고 소비 지점에서
/// QCoreApplication::translate() 로 옮긴다.
struct RoleSpec
{
    const char* name;
    const char* detail;
};

const QVector< RoleSpec >& roleTable()
{
    static const QVector< RoleSpec > table{
        { "ref",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "레이블 참조" ) },
        { "doc",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "문서 참조" ) },
        { "download",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 내려받기 링크" ) },
        { "numref",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "번호로 참조" ) },
        { "term",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "용어집 항목" ) },
        { "abbr",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "약어" ) },
        { "command",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "명령어" ) },
        { "file",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 경로" ) },
        { "guilabel",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "GUI 레이블" ) },
        { "kbd",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "키 입력" ) },
        { "menuselection", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "메뉴 경로" ) },
        { "mailheader", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "메일 헤더" ) },
        { "manpage",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "man 페이지" ) },
        { "program",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "프로그램 이름" ) },
        { "samp",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "치환 가능한 코드" ) },
        { "math",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "인라인 수식" ) },
        { "eq",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "수식 참조" ) },
        { "envvar",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "환경 변수" ) },
        { "option",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "명령행 옵션" ) },
        { "regexp",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "정규식" ) },
        { "subscript", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "아래 첨자" ) },
        { "superscript", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "위 첨자" ) },
        { "title-reference", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목 참조" ) },
        { "pep",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "PEP 참조" ) },
        { "rfc",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "RFC 참조" ) },
        { "emphasis",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "강조" ) },
        { "strong",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "굵게" ) },
        { "literal",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "리터럴" ) },
        { "code",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "코드" ) },
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
                                  QCoreApplication::translate( "RstOfflineCompletions", spec.detail ),
                                  kKindClass } );
            }
            break;
        }
        case ContextKind::Role:
            for( const RoleSpec& spec : roleTable() )
            {
                const QString name = QString::fromLatin1( spec.name );
                items.push_back( { name, name + QStringLiteral( ":`" ),
                                  QCoreApplication::translate( "RstOfflineCompletions", spec.detail ),
                                  kKindKeyword } );
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

QVector< Item > normalizeLspItems( QVector< Item > items, const QString& lineText, const int column )
{
    const int caret = qBound( 0, column - 1, static_cast< int >( lineText.length() ) );
    const QString before = lineText.left( caret );
    // 앞쪽만 잘라야 한다. trimmed() 로 뒤까지 자르면 정작 문제가 되는 상황인
    // ".. " (뒤에 공백 하나) 가 ".." 이 되어 컨텍스트 판정을 놓친다.
    const QStringView stripped = QStringView( before ).mid( leadingIndent( before ) );
    if( !stripped.startsWith( QStringLiteral( ".. " ) ) )
        return items;

    for( Item& item : items )
    {
        // QString::trimmed() 는 뒤쪽도 자른다. 여기서는 앞만 잘라야 한다 —
        // "code-block:: " 의 끝 공백은 커서를 인자 자리로 보내는 의미가 있다.
        qsizetype leading = 0;
        while( leading < item.insertText.length() && item.insertText.at( leading ).isSpace() )
            ++leading;
        item.insertText = item.insertText.mid( leading );
    }
    return items;
}

QVector< Item > finalizeItems( QVector< Item > items, const int limit )
{
    QVector< Item > result;
    result.reserve( qMin( items.size(), static_cast< qsizetype >( limit ) ) );

    QSet< QString > seen;
    for( Item& item : items )
    {
        // Esbonio 는 스니펫 경계에 \x01 을 남기기도 한다. 그대로 넣으면 문서에 박힌다.
        item.insertText.remove( QChar( 0x0001 ) );
        if( item.insertText.isEmpty() || seen.contains( item.insertText ) )
            continue;

        seen.insert( item.insertText );
        result.push_back( item );
        if( result.size() >= limit )
            break;
    }
    return result;
}

QVector< Item > mergeItems( QVector< Item > primary, const QVector< Item >& additional )
{
    QSet< QString > seen;
    seen.reserve( primary.size() );
    for( const Item& item : primary )
        seen.insert( item.insertText );

    for( const Item& item : additional )
    {
        if( seen.contains( item.insertText ) )
            continue;
        seen.insert( item.insertText );
        primary.push_back( item );
    }
    return primary;
}

}  // namespace mrst::rstcomplete
