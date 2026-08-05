#include "stdafx.h"
#include "solSphinxDiagnosticsStore.hpp"

#include <QFileInfo>

namespace mrst {

DiagnosticsStore::DiagnosticsStore( QObject* parent )
    : QObject( parent )
{
}

QString DiagnosticsStore::normalizeKey( const QString& path )
{
    if( path.isEmpty() )
        return {};
    // Windows 는 대소문자를 구분하지 않으므로 키를 접어서 맞춘다.
    return QFileInfo( path ).absoluteFilePath().toCaseFolded();
}

void DiagnosticsStore::replaceSource( const QString& source, const QVector< DiagnosticEntry >& entries )
{
    QHash< QString, QVector< DiagnosticEntry > > grouped;
    for( const DiagnosticEntry& entry : entries )
        grouped[ normalizeKey( entry.path ) ].push_back( entry );

    // 이번에 사라진 파일들도 알려야 편집기 스퀴글이 지워진다.
    QSet< QString > touched;
    const auto previous = bySource_.value( source );
    for( auto it = previous.constBegin(); it != previous.constEnd(); ++it )
        touched.insert( it.key() );
    for( auto it = grouped.constBegin(); it != grouped.constEnd(); ++it )
        touched.insert( it.key() );

    bySource_[ source ] = std::move( grouped );

    emit changed();
    for( const QString& key : touched )
        emit pathChanged( key );
}

void DiagnosticsStore::replaceSourceForPath( const QString& source, const QString& path,
                                            const QVector< DiagnosticEntry >& entries )
{
    const QString key = normalizeKey( path );
    if( key.isEmpty() )
        return;

    if( entries.isEmpty() )
        bySource_[ source ].remove( key );
    else
        bySource_[ source ][ key ] = entries;

    emit changed();
    emit pathChanged( key );
}

void DiagnosticsStore::clearSource( const QString& source )
{
    if( !bySource_.contains( source ) )
        return;

    const auto removed = bySource_.take( source );
    emit changed();
    for( auto it = removed.constBegin(); it != removed.constEnd(); ++it )
        emit pathChanged( it.key() );
}

void DiagnosticsStore::clear()
{
    bySource_.clear();
    emit changed();
}

QVector< DiagnosticEntry > DiagnosticsStore::all() const
{
    QVector< DiagnosticEntry > merged;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
    {
        for( auto path = source.value().constBegin(); path != source.value().constEnd(); ++path )
            merged += path.value();
    }
    return deduplicateDiagnostics( merged );
}

QVector< DiagnosticEntry > DiagnosticsStore::forPath( const QString& path ) const
{
    const QString key = normalizeKey( path );
    QVector< DiagnosticEntry > merged;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
        merged += source.value().value( key );

    return deduplicateDiagnostics( merged );
}

int DiagnosticsStore::count() const
{
    int total = 0;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
    {
        for( auto path = source.value().constBegin(); path != source.value().constEnd(); ++path )
            total += static_cast< int >( path.value().size() );
    }
    return total;
}

}  // namespace mrst
