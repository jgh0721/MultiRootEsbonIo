#include "stdafx.h"
#include "core/solRstPathIndex.hpp"

#include "core/solFileKinds.hpp"
#include "utils/solBackgroundWork.hpp"

#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace mrst {

namespace {

/// 저장할 때마다 전 트리를 다시 훑지 않도록 하는 최소 간격.
constexpr qint64 kRescanThrottleMs = 10'000;

}   // namespace

QStringList scanPathIndex( const QString& root, const int limit )
{
    QStringList paths;
    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() || !QFileInfo( base ).isDir() )
        return paths;

    // QDirIterator::Subdirectories 를 쓰지 않는다. 그것은 다 훑은 뒤에 걸러내는
    // 방식이라 .git 과 .venv 안까지 전부 들어간다. 실사용 워크스페이스에서
    // 15,592 개를 훑어 1,351 개를 남기게 된다 — 11배를 헛도는 셈이다.
    // 명시적인 스택으로 가지치기를 하면 들어가지 않는다.
    QStringList pending{ base };
    while( !pending.isEmpty() && paths.size() < limit )
    {
        if( isShuttingDown() )
            return {};

        const QString directory = pending.takeLast();
        const QFileInfoList entries =
            QDir( directory ).entryInfoList( QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot );

        for( const QFileInfo& entry : entries )
        {
            const QString name = entry.fileName();
            if( name.startsWith( QLatin1Char( '.' ) ) )
                continue;

            if( entry.isDir() )
            {
                if( !filekinds::isExcludedDirectoryName( name ) )
                    pending << entry.absoluteFilePath();
                continue;
            }

            paths << QDir( base ).relativeFilePath( entry.absoluteFilePath() );
            if( paths.size() >= limit )
                break;
        }
    }

    paths.sort( Qt::CaseInsensitive );
    return paths;
}

PathIndex::PathIndex( QObject* parent )
    : QObject( parent )
{
}

bool PathIndex::isReadyFor( const QString& root ) const
{
    return !root.isEmpty() && indexedRoot_ == QDir::cleanPath( root );
}

void PathIndex::ensure( const QString& root )
{
    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() || indexedRoot_ == base || scanningRoot_ == base )
        return;
    startScan( base );
}

void PathIndex::invalidate( const QString& root )
{
    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() || scanningRoot_ == base )
        return;
    if( sinceLastScan_.isValid() && sinceLastScan_.elapsed() < kRescanThrottleMs )
        return;
    startScan( base );
}

void PathIndex::startScan( const QString& root )
{
    scanningRoot_ = root;
    const quint64 generation = ++generation_;
    QPointer< PathIndex > guard( this );

    // 프로젝트 개요·용어집과 같은 관용구다. 결과만 GUI 스레드로 되돌린다.
    QThreadPool::globalInstance()->start( [ guard, root, generation ] {
        QStringList found = scanPathIndex( root );
        if( guard.isNull() || isShuttingDown() )
            return;
        QMetaObject::invokeMethod(
            guard,
            [ guard, root, found = std::move( found ), generation ]() mutable {
                if( guard.isNull() )
                    return;
                guard->apply( root, std::move( found ), generation );
            },
            Qt::QueuedConnection );
    } );
}

void PathIndex::apply( const QString& root, QStringList paths, const quint64 generation )
{
    if( generation != generation_ )
        return;   // 그사이 다른 루트를 훑기 시작했다

    scanningRoot_.clear();
    indexedRoot_ = root;
    paths_ = std::move( paths );
    sinceLastScan_.start();

    emit logMessage( tr( "경로 인덱스: %1 개 [%2]" ).arg( paths_.size() ).arg( root ) );
    emit ready( root, static_cast< int >( paths_.size() ) );
}

}   // namespace mrst
