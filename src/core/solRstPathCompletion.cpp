#include "stdafx.h"
#include "core/solRstPathCompletion.hpp"

#include "core/solFileKinds.hpp"

#include <QHash>

namespace mrst::rstpath {

namespace {

QStringList delimitedExtensions()
{
    return { QStringLiteral( "csv" ), QStringLiteral( "txt" ), QStringLiteral( "tsv" ) };
}

QStringList graphExtensions()
{
    return { QStringLiteral( "dot" ), QStringLiteral( "gv" ) };
}

/// `.. name:: <인자>` 가 경로인 directive.
///
/// 근거 (docutils 0.21 / Sphinx 8):
///   image           images.py 가 directives.uri() 로 변환한다
///   figure          Figure 는 Image 를 상속하므로 같은 변환
///   include         sphinx/directives/other.py 가 env.relfn2path() 를 부른다
///   literalinclude  sphinx/directives/code.py 도 같은 호출
///   graphviz        sphinx/ext/graphviz.py, final_argument_whitespace=false
///
/// **raw 와 csv-table 은 여기 없다.** raw 의 인자는 포맷 이름(html/latex)이고
/// csv-table 의 인자는 표 제목이다. 둘 다 경로는 :file: 옵션에 있다. 예전
/// takesPathArgument() 는 이 둘을 경로로 오인해서 표 제목 자리에 파일 목록을 띄웠다.
const QHash< QString, Slot >& argumentSlots()
{
    static const QHash< QString, Slot > table{
        { QStringLiteral( "image" ),
         Slot{ Shape::FilePath, Spaces::Escape, filekinds::imageExtensions(), {} } },
        { QStringLiteral( "figure" ),
         Slot{ Shape::FilePath, Spaces::Escape, filekinds::imageExtensions(), {} } },
        { QStringLiteral( "include" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::documentExtensions() } },
        { QStringLiteral( "literalinclude" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
        { QStringLiteral( "graphviz" ),
         Slot{ Shape::FilePath, Spaces::Forbidden, graphExtensions(), {} } },
    };
    return table;
}

/// "<directive>|<option>" -> 슬롯.
const QHash< QString, Slot >& optionSlots()
{
    static const QHash< QString, Slot > table{
        // 원본 크기 이미지나 다른 문서로 거는 링크. URL 일 수도 있어서 거르지
        // 않고 순위만 올린다.
        { QStringLiteral( "image|target" ),
         Slot{ Shape::FilePath, Spaces::Escape, {},
              filekinds::imageExtensions() + filekinds::documentExtensions() } },
        { QStringLiteral( "figure|target" ),
         Slot{ Shape::FilePath, Spaces::Escape, {},
              filekinds::imageExtensions() + filekinds::documentExtensions() } },
        // docutils tables.py 가 directives.path 로 받는다.
        { QStringLiteral( "csv-table|file" ),
         Slot{ Shape::FilePath, Spaces::Preserve, delimitedExtensions(), {} } },
        // docutils misc.py 가 directives.path 로 받는다.
        { QStringLiteral( "raw|file" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
        // sphinx code.py 가 relfn2path( options["diff"] ) 를 부른다.
        { QStringLiteral( "literalinclude|diff" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
    };
    return table;
}

/// directive 본문의 각 줄이 경로인 경우.
const QHash< QString, Slot >& bodySlots()
{
    static const QHash< QString, Slot > table{
        // sphinx/directives/other.py 는 접미사를 뗀 뒤 docname_join 한다.
        { QStringLiteral( "toctree" ),
         Slot{ Shape::DocName, Spaces::Forbidden, filekinds::documentExtensions(), {} } },
    };
    return table;
}

const QHash< QString, Slot >& roleTargetSlots()
{
    static const QHash< QString, Slot > table{
        // sphinx 의 download 롤도 relfn2path 를 탄다. 무엇이든 내려받을 수 있다.
        { QStringLiteral( "download" ), Slot{ Shape::FilePath, Spaces::Preserve, {}, {} } },
        { QStringLiteral( "doc" ),
         Slot{ Shape::DocName, Spaces::Forbidden, filekinds::documentExtensions(), {} } },
    };
    return table;
}

const Slot* lookup( const QHash< QString, Slot >& table, const QString& key )
{
    const auto it = table.constFind( key );
    return it == table.constEnd() ? nullptr : &it.value();
}

}   // namespace

const Slot* slotForArgument( const QString& directiveName )
{
    return lookup( argumentSlots(), directiveName );
}

const Slot* slotForOption( const QString& directiveName, const QString& optionName )
{
    return lookup( optionSlots(), directiveName + QLatin1Char( '|' ) + optionName );
}

const Slot* slotForBody( const QString& directiveName )
{
    return lookup( bodySlots(), directiveName );
}

const Slot* slotForRoleTarget( const QString& roleName )
{
    return lookup( roleTargetSlots(), roleName );
}

const Slot* slotFor( const rstcomplete::Context& context )
{
    if( context.kind != rstcomplete::ContextKind::Path )
        return nullptr;

    switch( context.pathSite )
    {
        case rstcomplete::PathSlotSite::Argument:
            return slotForArgument( context.directiveName );
        case rstcomplete::PathSlotSite::Option:
            return slotForOption( context.directiveName, context.optionName );
        case rstcomplete::PathSlotSite::Body:
            return slotForBody( context.directiveName );
        case rstcomplete::PathSlotSite::RoleTarget:
            return slotForRoleTarget( context.directiveName );
        case rstcomplete::PathSlotSite::None:
            break;
    }
    return nullptr;
}

TypedPath splitTypedPath( const QString& typed )
{
    TypedPath result;
    result.fromSourceRoot = typed.startsWith( QLatin1Char( '/' ) )
                            || typed.startsWith( QLatin1Char( '\\' ) );

    QString decoded;
    decoded.reserve( typed.length() );
    int lastSeparator = -1;

    for( int index = 0; index < typed.length(); ++index )
    {
        const QChar ch = typed.at( index );

        // reST 에서 백슬래시+공백은 이스케이프된 공백이지 경로 구분자가 아니다.
        // 이걸 먼저 보지 않으면 image 인자의 "my\ photo.png" 가 두 조각으로 잘린다.
        if( ch == QLatin1Char( '\\' ) && index + 1 < typed.length()
            && typed.at( index + 1 ) == QLatin1Char( ' ' ) )
        {
            decoded.append( QLatin1Char( ' ' ) );
            ++index;
            continue;
        }

        if( ch == QLatin1Char( '/' ) || ch == QLatin1Char( '\\' ) )
        {
            decoded.append( QLatin1Char( '/' ) );
            lastSeparator = static_cast< int >( decoded.length() ) - 1;
            continue;
        }

        decoded.append( ch );
    }

    if( lastSeparator < 0 )
    {
        result.name = decoded;
        return result;
    }

    result.directory = decoded.left( lastSeparator );   // 끝의 '/' 는 뗀다
    result.name = decoded.mid( lastSeparator + 1 );
    return result;
}

QString encodeForInsertion( const QString& relativePath, const Slot& slot )
{
    QString text = relativePath;
    text.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );

    if( slot.spaces == Spaces::Escape )
    {
        // docutils 의 uri() 는 이스케이프하지 않은 공백을 통째로 지운다.
        // 백슬래시를 앞에 붙여야 실제 공백이 있는 파일을 가리킬 수 있다.
        // %20 은 답이 아니다 - relfn2path 는 퍼센트 디코딩을 하지 않는다.
        text.replace( QLatin1Char( ' ' ), QStringLiteral( "\\ " ) );
    }
    return text;
}

bool acceptsFileName( const QString& fileName, const Slot& slot )
{
    if( slot.spaces == Spaces::Forbidden && fileName.contains( QLatin1Char( ' ' ) ) )
        return false;
    if( slot.accepted.isEmpty() )
        return true;
    return filekinds::hasExtension( fileName, slot.accepted );
}

QString stripDocumentSuffix( const QString& fileName )
{
    for( const QString& suffix : filekinds::documentExtensions() )
    {
        const QString dotted = QLatin1Char( '.' ) + suffix;
        if( fileName.endsWith( dotted, Qt::CaseInsensitive ) )
            return fileName.left( fileName.length() - dotted.length() );
    }
    return fileName;
}

}   // namespace mrst::rstpath
