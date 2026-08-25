#include "stdafx.h"
#include "solRecentItems.hpp"

#include <QDir>

namespace mrst {
namespace {

/// 경로 비교 규칙. Windows 의 파일 시스템은 대소문자를 구분하지 않는다.
constexpr Qt::CaseSensitivity kPathCompare =
#ifdef Q_OS_WIN
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

/// 목록에 넣고 비교할 때 쓰는 표기. 화면에도 이 값이 그대로 나간다.
QString normalizedEntry( const QString& path )
{
    if( path.isEmpty() )
        return {};
    return QDir::toNativeSeparators( QDir::cleanPath( path ) );
}

}  // namespace

QStringList prependRecentEntry( const QStringList& entries, const QString& entry, const int maximum )
{
    const QString normalized = normalizedEntry( entry );
    if( normalized.isEmpty() || maximum <= 0 )
        return entries;

    QStringList result;
    result.reserve( qMin( entries.size() + 1, maximum ) );
    result << normalized;

    for( const QString& existing : entries )
    {
        if( result.size() >= maximum )
            break;

        const QString candidate = normalizedEntry( existing );
        if( candidate.isEmpty() || candidate.compare( normalized, kPathCompare ) == 0 )
            continue;
        result << candidate;
    }

    return result;
}

QStringList removeRecentEntry( const QStringList& entries, const QString& entry )
{
    const QString normalized = normalizedEntry( entry );
    if( normalized.isEmpty() )
        return entries;

    QStringList result;
    result.reserve( entries.size() );
    for( const QString& existing : entries )
    {
        const QString candidate = normalizedEntry( existing );
        if( candidate.isEmpty() || candidate.compare( normalized, kPathCompare ) == 0 )
            continue;
        result << candidate;
    }

    return result;
}

}  // namespace mrst
