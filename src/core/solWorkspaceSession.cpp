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
        { QStringLiteral( "sideSplitterSizes" ), toJsonArray( session.sideSplitterSizes ) },
        { QStringLiteral( "contentSplitterSizes" ), toJsonArray( session.contentSplitterSizes ) },
        { QStringLiteral( "previewSplitterSizes" ), toJsonArray( session.previewSplitterSizes ) },
    };
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

    session.sideSplitterSizes = toIntList( object.value( QStringLiteral( "sideSplitterSizes" ) ) );
    session.contentSplitterSizes = toIntList( object.value( QStringLiteral( "contentSplitterSizes" ) ) );
    session.previewSplitterSizes = toIntList( object.value( QStringLiteral( "previewSplitterSizes" ) ) );
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
