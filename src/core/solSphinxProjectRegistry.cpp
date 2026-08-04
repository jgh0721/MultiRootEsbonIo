#include "stdafx.h"
#include "solSphinxProjectRegistry.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThreadPool>

namespace mrst {
namespace fs = std::filesystem;

namespace {

constexpr int kCacheSchemaVersion = 1;

QJsonObject projectToJson( const SphinxProject& project )
{
    return QJsonObject{
        {QStringLiteral( "projectId" ), QString::fromStdWString( project.projectId )},
        {QStringLiteral( "rootPath" ), toQString( project.rootPath )},
        {QStringLiteral( "confPath" ), toQString( project.confPath )},
        {QStringLiteral( "sourcePath" ), toQString( project.sourcePath )},
        {QStringLiteral( "rootDoc" ), QString::fromStdString( project.rootDoc )},
        {QStringLiteral( "buildPath" ), toQString( project.buildPath )},
    };
}

/// conf.py 가 여전히 존재할 때만 프로젝트를 복원한다.
bool projectFromJson( const QJsonObject& object, SphinxProject& out )
{
    const QString confPath = object.value( QStringLiteral( "confPath" ) ).toString();
    if( confPath.isEmpty() || !QFileInfo::exists( confPath ) )
        return false;

    out.projectId  = object.value( QStringLiteral( "projectId" ) ).toString().toStdWString();
    out.rootPath   = toPath( object.value( QStringLiteral( "rootPath" ) ).toString() );
    out.confPath   = toPath( confPath );
    out.sourcePath = toPath( object.value( QStringLiteral( "sourcePath" ) ).toString() );
    out.rootDoc    = object.value( QStringLiteral( "rootDoc" ) ).toString( QStringLiteral( "index" ) ).toStdString();
    out.buildPath  = toPath( object.value( QStringLiteral( "buildPath" ) ).toString() );
    return !out.rootPath.empty();
}

}  // namespace

ProjectRegistry::ProjectRegistry( QObject* parent )
    : QObject( parent )
{
}

ProjectRegistry::~ProjectRegistry() = default;

void ProjectRegistry::setWorkspaceRoot( const QString& root )
{
    const QString normalized = root.isEmpty() ? QString{} : QFileInfo( root ).absoluteFilePath();
    if( normalized == workspaceRoot_ )
        return;

    workspaceRoot_ = normalized;
    projects_.clear();
    ++generation_;   // 이전 워크스페이스의 스캔 결과가 뒤늦게 도착해도 무시된다.
}

QString ProjectRegistry::workspaceRoot() const
{
    return workspaceRoot_;
}

void ProjectRegistry::setScannerSettings( ScannerSettings settings )
{
    settings_ = std::move( settings );
}

const ScannerSettings& ProjectRegistry::scannerSettings() const
{
    return settings_;
}

bool ProjectRegistry::isScanning() const
{
    return scanning_.load();
}

const std::vector< SphinxProject >& ProjectRegistry::projects() const
{
    return projects_;
}

const SphinxProject* ProjectRegistry::resolveForFile( const QString& filePath ) const
{
    if( filePath.isEmpty() || projects_.empty() )
        return nullptr;

    return resolveProjectForFile( toPath( QFileInfo( filePath ).absoluteFilePath() ), projects_ );
}

const SphinxProject* ProjectRegistry::findById( const QString& projectId ) const
{
    if( projectId.isEmpty() )
        return nullptr;

    for( const SphinxProject& project : projects_ )
    {
        if( QString::fromStdWString( project.projectId ) == projectId )
            return &project;
    }
    return nullptr;
}

QString ProjectRegistry::cacheFilePath() const
{
    if( workspaceRoot_.isEmpty() )
        return {};

    return QDir( workspaceRoot_ ).filePath( QStringLiteral( ".multiroot/projects.json" ) );
}

bool ProjectRegistry::loadCache()
{
    const QString path = cacheFilePath();
    if( path.isEmpty() )
        return false;

    QFile file( path );
    if( !file.open( QIODevice::ReadOnly ) )
        return false;

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );
    file.close();
    if( !document.isObject() )
        return false;

    const QJsonObject root = document.object();
    if( root.value( QStringLiteral( "version" ) ).toInt() != kCacheSchemaVersion )
        return false;

    std::vector< SphinxProject > restored;
    const QJsonArray entries = root.value( QStringLiteral( "projects" ) ).toArray();
    restored.reserve( static_cast< std::size_t >( entries.size() ) );
    for( const QJsonValue& value : entries )
    {
        SphinxProject project;
        if( value.isObject() && projectFromJson( value.toObject(), project ) )
            restored.push_back( std::move( project ) );
    }

    if( restored.empty() )
        return false;

    projects_ = std::move( restored );
    emit logMessage( tr( "프로젝트 캐시에서 %1개를 복원했습니다." ).arg( projects_.size() ) );
    return true;
}

void ProjectRegistry::saveCache() const
{
    const QString path = cacheFilePath();
    if( path.isEmpty() )
        return;

    QDir().mkpath( QFileInfo( path ).absolutePath() );

    QJsonArray entries;
    for( const SphinxProject& project : projects_ )
        entries.append( projectToJson( project ) );

    const QJsonObject root{
        {QStringLiteral( "version" ), kCacheSchemaVersion},
        {QStringLiteral( "workspaceRoot" ), workspaceRoot_},
        {QStringLiteral( "projects" ), entries},
    };

    QFile file( path );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        return;

    file.write( QJsonDocument( root ).toJson( QJsonDocument::Indented ) );
    file.close();
}

void ProjectRegistry::rescanAsync()
{
    if( workspaceRoot_.isEmpty() || scanning_.exchange( true ) )
        return;

    const quint64 generation   = ++generation_;
    const fs::path root        = toPath( workspaceRoot_ );
    const ScannerSettings copy = settings_;
    QPointer< ProjectRegistry > guard( this );

    emit scanStarted();

    QThreadPool::globalInstance()->start( [guard, root, copy, generation]() {
        std::vector< SphinxProject > scanned;
        try
        {
            scanned = ProjectScanner( root, copy ).scan();
        }
        catch( const std::exception& )
        {
            // 스캔 도중 파일시스템 예외가 나도 빈 결과로 마무리한다.
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, scanned = std::move( scanned ), generation]() mutable {
                if( guard )
                    guard->applyScanResult( std::move( scanned ), generation );
            },
            Qt::QueuedConnection );
    } );
}

void ProjectRegistry::applyScanResult( std::vector< SphinxProject > scanned, quint64 generation )
{
    scanning_.store( false );

    // 워크스페이스가 바뀌었거나 더 최신 스캔이 시작된 결과는 버린다.
    if( generation != generation_ )
        return;

    projects_ = std::move( scanned );
    saveCache();
    emit scanFinished( static_cast< int >( projects_.size() ) );
}

}  // namespace mrst
