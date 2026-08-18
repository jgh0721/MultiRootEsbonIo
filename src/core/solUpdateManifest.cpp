#include "stdafx.h"
#include "core/solUpdateManifest.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>
#include <QVersionNumber>

namespace mrst {

namespace {

constexpr auto kManifestOwner = "jgh0721";
constexpr auto kManifestRepo  = "MultiRootEsbonIo";
constexpr auto kManifestName  = "update-manifest.json";

/// 이 빌드가 받아야 할 배포 파일의 조건. 지금은 조합이 하나뿐이지만
/// assets[] 에서 골라내는 기준을 한 곳에 모아 둔다.
constexpr auto kTargetPlatform = "windows";
constexpr auto kTargetArch     = "x64";
constexpr auto kTargetKind     = "zip";

/// 최상위 항목 이름으로 쓸 수 있는가.
///
/// rootDir 과 removals 는 파일 시스템 경로를 만드는 데 쓰인다. 경로 구분자나
/// 상위 이동이 섞이면 앱 디렉터리 밖을 건드릴 수 있으므로 이름 한 조각만
/// 허용한다. (압축 해제 쪽은 bsdtar 기본 동작이 한 번 더 막아 준다.)
[[nodiscard]] bool isSafeEntryName( const QString& name )
{
    if( name.isEmpty() || name.size() > 255 )
        return false;
    if( name == QStringLiteral( "." ) || name == QStringLiteral( ".." ) )
        return false;
    if( name.contains( QLatin1Char( '/' ) ) || name.contains( QLatin1Char( '\\' ) ) )
        return false;
    if( name.contains( QLatin1Char( ':' ) ) )
        return false;
    return true;
}

[[nodiscard]] bool isSha256Hex( const QString& text )
{
    if( text.size() != 64 )
        return false;
    // isxdigit() 에 넘기지 않는다. 비ASCII 문자의 toLatin1() 은 음수가 될 수
    // 있고, 그 값을 <cctype> 함수에 넘기는 것은 정의되지 않은 동작이다.
    for( const QChar ch : text )
    {
        const char c = ch.toLatin1();
        const bool hex = ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
        if( !hex )
            return false;
    }
    return true;
}

void setError( QString* out, const QString& message )
{
    if( out != nullptr )
        *out = message;
}

}  // namespace

// ── UpdateAsset ───────────────────────────────────────────

bool UpdateAsset::isValid() const
{
    // 호스트 검증은 여기서 하지 않는다. 허용 호스트는 사용자가 지정한
    // manifestUrl 에 따라 달라지므로 파싱 단계에서 판단한다.
    return url.isValid()
        && isSafeEntryName( rootDir )
        && size >= kMinAssetSize && size <= kMaxAssetSize
        && isSha256Hex( sha256 )
        && entryCount > 0;
}

// ── UpdateInfo ────────────────────────────────────────────

bool UpdateInfo::isValid() const
{
    return schema > 0 && schema <= kSupportedManifestSchema
        && !product.isEmpty()
        && isVersionParsable( version )
        && asset.isValid();
}

bool UpdateInfo::isNewerThan( const QString& currentVersion ) const
{
    // 판단 근거가 없으면 업데이트하지 않는다.
    if( !isVersionParsable( version ) || !isVersionParsable( currentVersion ) )
        return false;
    return compareVersions( version, currentVersion ) > 0;
}

bool UpdateInfo::isSkipped( const QString& skippedVersion ) const
{
    if( skippedVersion.isEmpty() || !isVersionParsable( skippedVersion ) )
        return false;
    // 문자열 동등 비교로 하면 "0.3" 을 건너뛴 뒤 매니페스트가 "0.3.0.0" 으로
    // 오면 또 뜬다. <= 로 두면 0.3 은 계속 침묵하고 0.3.1 은 다시 알린다.
    return compareVersions( version, skippedVersion ) <= 0;
}

bool UpdateInfo::isUpgradableFrom( const QString& currentVersion ) const
{
    // 하한을 적지 않은 매니페스트는 제약이 없다는 뜻이다.
    if( minimumFromVersion.isEmpty() )
        return true;
    if( !isVersionParsable( minimumFromVersion ) || !isVersionParsable( currentVersion ) )
        return false;
    return compareVersions( currentVersion, minimumFromVersion ) >= 0;
}

// ── 버전 비교 ─────────────────────────────────────────────

int compareVersions( const QString& lhs, const QString& rhs )
{
    const QVersionNumber left  = QVersionNumber::fromString( lhs ).normalized();
    const QVersionNumber right = QVersionNumber::fromString( rhs ).normalized();
    if( left.isNull() || right.isNull() )
        return 0;
    return QVersionNumber::compare( left, right );
}

bool isVersionParsable( const QString& text )
{
    return !QVersionNumber::fromString( text ).normalized().isNull();
}

// ── URL ───────────────────────────────────────────────────

bool isAllowedAssetHost( const QUrl& url, const QString& extraAllowedHost )
{
    const QString host = url.host().toLower();

    // 사용자가 매니페스트 위치를 직접 지정한 경우, 그 호스트는 스킴 제약 없이
    // 허용한다. 개발 중에 http 로 띄운 로컬 서버를 쓸 수 있어야 한다.
    if( !extraAllowedHost.isEmpty() && host == extraAllowedHost.toLower() )
        return true;

    if( url.scheme() != QStringLiteral( "https" ) )
        return false;

    if( host == QStringLiteral( "github.com" ) )
        return true;
    // 릴리스 에셋은 objects.githubusercontent.com 등으로 302 된다. 리다이렉트를
    // 따라가는 쪽은 Qt 가 검사하지만, 매니페스트가 그 호스트를 직접 적어 두는
    // 경우도 허용한다.
    return host.endsWith( QStringLiteral( ".githubusercontent.com" ) );
}

QUrl defaultManifestUrl()
{
    return QUrl( QStringLiteral( "https://github.com/%1/%2/releases/latest/download/%3" )
                    .arg( QLatin1String( kManifestOwner ),
                          QLatin1String( kManifestRepo ),
                          QLatin1String( kManifestName ) ) );
}

QUrl repositoryUrl()
{
    return QUrl( QStringLiteral( "https://github.com/%1/%2" )
                    .arg( QLatin1String( kManifestOwner ), QLatin1String( kManifestRepo ) ) );
}

QUrl withCacheBuster( const QUrl& url, const qint64 epochSeconds )
{
    QUrl result = url;
    QUrlQuery query( result );
    query.removeQueryItem( QStringLiteral( "t" ) );
    query.addQueryItem( QStringLiteral( "t" ), QString::number( epochSeconds ) );
    result.setQuery( query );
    return result;
}

// ── 파싱 ──────────────────────────────────────────────────

UpdateInfo parseUpdateManifest( const QByteArray& json, const QString& expectedProduct,
                                QString* errorMessage, const QString& extraAllowedHost )
{
    setError( errorMessage, QString() );

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson( json, &parseError );
    if( parseError.error != QJsonParseError::NoError || !document.isObject() )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "매니페스트를 읽을 수 없습니다: %1" )
                                   .arg( parseError.errorString() ) );
        return {};
    }

    const QJsonObject root = document.object();

    UpdateInfo info;
    info.schema = root.value( QStringLiteral( "schema" ) ).toInt();
    if( info.schema <= 0 )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "매니페스트에 schema 가 없습니다." ) );
        return {};
    }
    if( info.schema > kSupportedManifestSchema )
    {
        // 이 앱이 모르는 형식이다. 조용히 물러난다 — 추측해서 설치하는 것보다
        // 업데이트하지 않는 편이 항상 낫다.
        setError( errorMessage,
                 QCoreApplication::translate( "UpdateManifest",
                                              "이 버전이 모르는 매니페스트 형식입니다 "
                                              "(schema %1). 업데이트를 건너뜁니다." )
                     .arg( info.schema ) );
        return {};
    }

    info.product = root.value( QStringLiteral( "product" ) ).toString();
    if( info.product.isEmpty() )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "매니페스트에 product 가 없습니다." ) );
        return {};
    }
    if( !expectedProduct.isEmpty() && info.product != expectedProduct )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "다른 제품의 매니페스트입니다 (%1)." )
                                   .arg( info.product ) );
        return {};
    }

    info.version = root.value( QStringLiteral( "version" ) ).toString().trimmed();
    if( !isVersionParsable( info.version ) )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "매니페스트의 version 을 해석할 수 없습니다 (%1)." )
                                   .arg( info.version ) );
        return {};
    }

    info.tag                = root.value( QStringLiteral( "tag" ) ).toString();
    info.commit             = root.value( QStringLiteral( "commit" ) ).toString();
    info.mandatory          = root.value( QStringLiteral( "mandatory" ) ).toBool( false );
    info.minimumFromVersion = root.value( QStringLiteral( "minimumFromVersion" ) ).toString().trimmed();
    info.releasedAt         = QDateTime::fromString(
        root.value( QStringLiteral( "releasedAt" ) ).toString(), Qt::ISODate );

    const QUrl notesUrl( root.value( QStringLiteral( "notesUrl" ) ).toString() );
    // 노트 링크는 사용자를 브라우저로 보내는 값이다. 허용 호스트가 아니면 그냥 버린다.
    if( notesUrl.isValid() && isAllowedAssetHost( notesUrl, extraAllowedHost ) )
        info.notesUrl = notesUrl;

    // ── assets[] 에서 이 플랫폼용 파일을 고른다 ──
    const QJsonArray assets = root.value( QStringLiteral( "assets" ) ).toArray();
    if( assets.isEmpty() )
    {
        setError( errorMessage, QCoreApplication::translate( "UpdateManifest", "매니페스트에 assets 가 없습니다." ) );
        return {};
    }

    for( const QJsonValue& value : assets )
    {
        const QJsonObject object = value.toObject();
        if( object.value( QStringLiteral( "platform" ) ).toString() != QLatin1String( kTargetPlatform ) )
            continue;
        if( object.value( QStringLiteral( "arch" ) ).toString() != QLatin1String( kTargetArch ) )
            continue;
        if( object.value( QStringLiteral( "kind" ) ).toString() != QLatin1String( kTargetKind ) )
            continue;

        UpdateAsset asset;
        asset.id         = object.value( QStringLiteral( "id" ) ).toString();
        asset.kind       = object.value( QStringLiteral( "kind" ) ).toString();
        asset.platform   = object.value( QStringLiteral( "platform" ) ).toString();
        asset.arch       = object.value( QStringLiteral( "arch" ) ).toString();
        asset.url        = QUrl( object.value( QStringLiteral( "url" ) ).toString() );
        asset.rootDir    = object.value( QStringLiteral( "rootDir" ) ).toString();
        asset.size       = static_cast< qint64 >( object.value( QStringLiteral( "size" ) ).toDouble() );
        asset.sha256     = object.value( QStringLiteral( "sha256" ) ).toString().toLower();
        asset.entryCount = object.value( QStringLiteral( "entryCount" ) ).toInt();

        // 위조된 매니페스트가 임의의 서버를 가리키지 못하게 여기서 막는다.
        if( !isAllowedAssetHost( asset.url, extraAllowedHost ) )
            continue;

        // id 가 "app" 인 것을 우선한다. 나중에 델타 패치가 추가되면 그때
        // 고르는 규칙이 여기서 갈린다.
        if( asset.isValid() && ( !info.asset.isValid() || asset.id == QStringLiteral( "app" ) ) )
            info.asset = asset;
    }

    if( !info.asset.isValid() )
    {
        setError( errorMessage,
                 QCoreApplication::translate( "UpdateManifest",
                                              "이 플랫폼(%1/%2)에 쓸 수 있는 배포 파일이 "
                                              "매니페스트에 없습니다." )
                     .arg( QLatin1String( kTargetPlatform ), QLatin1String( kTargetArch ) ) );
        return {};
    }

    // ── removals ──
    // 안전하지 않은 이름은 조용히 버린다. 이 목록이 조금 부실한 것은
    // "구버전 파일 하나가 남는" 정도의 문제이고, 파싱 전체를 실패시켜
    // 업데이트를 막을 만한 사안이 아니다.
    const QJsonArray removals = root.value( QStringLiteral( "removals" ) ).toArray();
    for( const QJsonValue& value : removals )
    {
        const QString name = value.toString();
        if( isSafeEntryName( name ) )
            info.removals.append( name );
    }

    return info;
}

// ── 주기 판정 ─────────────────────────────────────────────

bool isCheckDue( const QDateTime& lastCheckedAt, const int intervalDays, const QDateTime& now )
{
    if( clampCheckIntervalDays( intervalDays ) <= 0 )
        return false;                       // 0 = 확인하지 않음
    if( !lastCheckedAt.isValid() )
        return true;                        // 한 번도 점검한 적이 없다
    if( lastCheckedAt > now )
        return true;                        // 시계가 되돌아갔다. 지금 점검한다
    return lastCheckedAt.daysTo( now ) >= clampCheckIntervalDays( intervalDays );
}

int clampCheckIntervalDays( const int days )
{
    return qBound( 0, days, kMaxCheckIntervalDays );
}

}  // namespace mrst
