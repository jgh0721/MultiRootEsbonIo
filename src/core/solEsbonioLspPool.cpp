#include "stdafx.h"
#include "solEsbonioLspPool.hpp"

namespace mrst {

LspServerPool::LspServerPool( QObject* parent )
    : QObject( parent )
{
}

LspServerPool::~LspServerPool()
{
    stopAll();
}

void LspServerPool::setMaxProcesses( const int count )
{
    const int clamped = qBound( 1, count, 8 );
    if( maxProcesses_ == clamped )
        return;

    maxProcesses_ = clamped;
    evictIfNeeded();   // 설정을 줄이면 즉시 반영한다
}

int LspServerPool::maxProcesses() const
{
    return maxProcesses_;
}

void LspServerPool::setPythonPaths( const QString& pythonExe, const QString& sphinxBuildExe )
{
    pythonExe_ = pythonExe;
    sphinxBuildExe_ = sphinxBuildExe;
}

void LspServerPool::setPinnedProject( const QString& projectId )
{
    pinnedProjectId_ = projectId;
}

LspClient* LspServerPool::clientFor( const QString& projectId ) const
{
    return clients_.value( projectId, nullptr );
}

QStringList LspServerPool::runningProjectIds() const
{
    QStringList ids;
    for( auto it = clients_.constBegin(); it != clients_.constEnd(); ++it )
    {
        if( it.value() != nullptr && it.value()->isRunning() )
            ids << it.key();
    }
    return ids;
}

void LspServerPool::touch( const QString& projectId )
{
    lruOrder_.removeAll( projectId );
    lruOrder_.append( projectId );   // back = 가장 최근
}

void LspServerPool::evictIfNeeded()
{
    while( clients_.size() > maxProcesses_ )
    {
        // 앞에서부터(가장 오래된 것부터) 후보를 찾되 pin 된 것은 건너뛴다.
        QString victim;
        for( const QString& candidate : lruOrder_ )
        {
            if( candidate != pinnedProjectId_ )
            {
                victim = candidate;
                break;
            }
        }

        if( victim.isEmpty() )
        {
            // 전부 pin 이면 캡을 넘겨서라도 유지한다. 스래싱보다 낫다.
            emit logMessage( pinnedProjectId_,
                            tr( "최대 프로세스 수를 넘었지만 축출할 대상이 없습니다." ) );
            return;
        }

        stopProject( victim );
    }
}

LspClient* LspServerPool::activate( const SphinxProject& project )
{
    const QString projectId = QString::fromStdWString( project.projectId );
    if( projectId.isEmpty() || pythonExe_.isEmpty() )
        return nullptr;

    if( LspClient* existing = clients_.value( projectId, nullptr ) )
    {
        touch( projectId );
        return existing;
    }

    // 새로 띄우기 전에 자리를 만든다. pin 은 setPinnedProject 로 이미
    // 지정돼 있어야 한다.
    auto* client = new LspClient( this );
    clients_.insert( projectId, client );
    touch( projectId );
    evictIfNeeded();

    // 축출 과정에서 자기 자신이 사라졌다면(캡이 0에 가까운 이상 상황) 중단.
    if( !clients_.contains( projectId ) )
        return nullptr;

    connect( client, &LspClient::logMessage, this, [this, projectId]( const QString& text ) {
        emit logMessage( projectId, text );
    } );
    connect( client, &LspClient::diagnosticsReady, this,
            [this, projectId]( const QString& source, const QVector< DiagnosticEntry >& entries ) {
                emit diagnosticsReady( projectId, source, entries );
            } );
    connect( client, &LspClient::completionsReady, this,
            [this, projectId]( const QList< LspCompletionItem >& items ) {
                emit completionsReady( projectId, items );
            } );
    connect( client, &LspClient::documentSymbolsReady, this,
            [this, projectId]( const QString& path, const QList< LspDocumentSymbol >& symbols ) {
                emit documentSymbolsReady( projectId, path, symbols );
            } );
    connect( client, &LspClient::serverNotification, this,
            [this, projectId]( const QString& method, const QJsonObject& params ) {
                emit serverNotification( projectId, method, params );
            } );

    client->setHtmlStyleOverride( confDeclaresEmptyHtmlStyle( project.confPath ) );
    client->start( project, pythonExe_, sphinxBuildExe_ );

    emit projectSpawned( projectId );
    return client;
}

void LspServerPool::stopProject( const QString& projectId )
{
    LspClient* client = clients_.take( projectId );
    lruOrder_.removeAll( projectId );
    if( client == nullptr )
        return;

    client->stop();
    client->deleteLater();
    emit projectEvicted( projectId );
}

void LspServerPool::stopAll()
{
    const QStringList ids = clients_.keys();
    for( const QString& projectId : ids )
    {
        LspClient* client = clients_.take( projectId );
        if( client == nullptr )
            continue;
        client->stop();
        delete client;   // 종료 경로라 deleteLater 를 기다릴 수 없다
    }
    lruOrder_.clear();
}

}  // namespace mrst
