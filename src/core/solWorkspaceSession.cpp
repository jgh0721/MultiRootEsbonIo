#include "stdafx.h"
#include "solWorkspaceSession.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace mrst {
namespace {

constexpr int kSchema = 1;

QJsonArray toJsonArray( const QList< int >& values )
{
    QJsonArray array;
    for( const int value : values )
        array.append( value );
    return array;
}

QList< int > toIntList( const QJsonValue& value )
{
    QList< int > result;
    for( const QJsonValue& element : value.toArray() )
    {
        // 스플리터 크기는 음수일 수 없다. 깨진 값이 들어오면 통째로 버린다.
        if( !element.isDouble() || element.toInt() < 0 )
            return {};
        result << element.toInt();
    }
    return result;
}

}  // namespace

QString sessionFilePath( const QString& workspaceRoot )
{
    if( workspaceRoot.isEmpty() )
        return {};
    return QDir( workspaceRoot ).filePath( QStringLiteral( ".multiroot/workspace.json" ) );
}

QJsonObject sessionToJson( const WorkspaceSession& session )
{
    QJsonArray documents;
    for( const OpenDocumentState& document : session.documents )
    {
        documents.append( QJsonObject{
            { QStringLiteral( "path" ), document.path },
            { QStringLiteral( "caretLine" ), document.caretLine },
            { QStringLiteral( "caretColumn" ), document.caretColumn },
            { QStringLiteral( "firstVisibleLine" ), document.firstVisibleLine },
        } );
    }

    return QJsonObject{
        { QStringLiteral( "schema" ), kSchema },
        { QStringLiteral( "workspaceRoot" ), session.workspaceRoot },
        { QStringLiteral( "documents" ), documents },
        { QStringLiteral( "activeIndex" ), session.activeIndex },
        { QStringLiteral( "previewSplitterSizes" ), toJsonArray( session.previewSplitterSizes ) },
        { QStringLiteral( "dockLayout" ), session.dockLayout },
        { QStringLiteral( "windowGeometry" ), session.windowGeometry },
    };
}

QString activeDocumentPath( const WorkspaceSession& session )
{
    if( session.activeIndex < 0 || session.activeIndex >= session.documents.size() )
        return {};
    return session.documents.at( session.activeIndex ).path;
}

WorkspaceSession sessionFromJson( const QJsonObject& object )
{
    if( object.value( QStringLiteral( "schema" ) ).toInt() != kSchema )
        return {};

    WorkspaceSession session;
    session.schema = kSchema;
    session.workspaceRoot = object.value( QStringLiteral( "workspaceRoot" ) ).toString();

    for( const QJsonValue& value : object.value( QStringLiteral( "documents" ) ).toArray() )
    {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value( QStringLiteral( "path" ) ).toString();
        if( path.isEmpty() )
            continue;

        OpenDocumentState document;
        document.path = path;
        document.caretLine = qMax( 1, entry.value( QStringLiteral( "caretLine" ) ).toInt( 1 ) );
        document.caretColumn = qMax( 1, entry.value( QStringLiteral( "caretColumn" ) ).toInt( 1 ) );
        document.firstVisibleLine =
            qMax( 1, entry.value( QStringLiteral( "firstVisibleLine" ) ).toInt( 1 ) );
        session.documents.push_back( document );
    }

    const int activeIndex = object.value( QStringLiteral( "activeIndex" ) ).toInt( -1 );
    session.activeIndex = activeIndex < session.documents.size() ? activeIndex : -1;

    session.previewSplitterSizes = toIntList( object.value( QStringLiteral( "previewSplitterSizes" ) ) );
    // 이 키는 이 버전에서 생겼다. 없으면 빈 문자열이 되고, 그때는 기본 배치를 쓴다.
    session.dockLayout = object.value( QStringLiteral( "dockLayout" ) ).toString();
    session.windowGeometry = object.value( QStringLiteral( "windowGeometry" ) ).toString();
    return session;
}

WorkspaceSession loadWorkspaceSession( const QString& workspaceRoot )
{
    const QString path = sessionFilePath( workspaceRoot );
    if( path.isEmpty() )
        return {};

    QFile file( path );
    if( !file.open( QIODevice::ReadOnly ) )
        return {};

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &error );
    if( error.error != QJsonParseError::NoError || !document.isObject() )
        return {};

    return sessionFromJson( document.object() );
}

bool saveWorkspaceSession( const WorkspaceSession& session )
{
    const QString path = sessionFilePath( session.workspaceRoot );
    if( path.isEmpty() )
        return false;

    if( !QDir().mkpath( QFileInfo( path ).absolutePath() ) )
        return false;

    QFile file( path );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        return false;

    return file.write( QJsonDocument( sessionToJson( session ) ).toJson( QJsonDocument::Indented ) ) > 0;
}

}  // namespace mrst
