#include "TestRunner.hpp"

#include "core/solUpdateManifest.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

using namespace mrst;

namespace {

constexpr auto kProduct = "MultiRoot-reST-CPP";

/// 유효한 sha256 자리를 채우는 값. 실제 해시일 필요는 없고 형식만 맞으면 된다.
QString dummySha256()
{
    return QString( 64, QLatin1Char( 'a' ) );
}

QJsonObject makeAsset()
{
    QJsonObject asset;
    asset[ QStringLiteral( "id" ) ]       = QStringLiteral( "app" );
    asset[ QStringLiteral( "kind" ) ]     = QStringLiteral( "zip" );
    asset[ QStringLiteral( "platform" ) ] = QStringLiteral( "windows" );
    asset[ QStringLiteral( "arch" ) ]     = QStringLiteral( "x64" );
    asset[ QStringLiteral( "url" ) ]      = QStringLiteral(
        "https://github.com/jgh0721/MultiRootEsbonIo/releases/download/v0.2.1/"
        "MultiRoot-reST-CPP-0.2.1-win64.zip" );
    asset[ QStringLiteral( "rootDir" ) ]    = QStringLiteral( "MultiRoot-reST-CPP-0.2.1" );
    asset[ QStringLiteral( "size" ) ]       = 157286400;
    asset[ QStringLiteral( "sha256" ) ]     = dummySha256();
    asset[ QStringLiteral( "entryCount" ) ] = 82;
    return asset;
}

QJsonObject makeManifest()
{
    QJsonObject root;
    root[ QStringLiteral( "schema" ) ]             = 1;
    root[ QStringLiteral( "product" ) ]            = QLatin1String( kProduct );
    root[ QStringLiteral( "version" ) ]            = QStringLiteral( "0.2.1" );
    root[ QStringLiteral( "tag" ) ]                = QStringLiteral( "v0.2.1" );
    root[ QStringLiteral( "releasedAt" ) ]         = QStringLiteral( "2026-08-14T09:12:33Z" );
    root[ QStringLiteral( "commit" ) ]             = QStringLiteral( "4503ab3" );
    root[ QStringLiteral( "mandatory" ) ]          = false;
    root[ QStringLiteral( "minimumFromVersion" ) ] = QStringLiteral( "0.2.0" );
    root[ QStringLiteral( "notesUrl" ) ]           = QStringLiteral(
        "https://github.com/jgh0721/MultiRootEsbonIo/releases/tag/v0.2.1" );
    root[ QStringLiteral( "assets" ) ]   = QJsonArray{ makeAsset() };
    root[ QStringLiteral( "removals" ) ] = QJsonArray{};
    return root;
}

QByteArray toJson( const QJsonObject& root )
{
    return QJsonDocument( root ).toJson( QJsonDocument::Compact );
}

/// 에셋 필드 하나만 바꿔서 파싱한 결과.
UpdateInfo parseWithAssetField( const QString& key, const QJsonValue& value )
{
    QJsonObject asset = makeAsset();
    asset[ key ]      = value;
    QJsonObject root  = makeManifest();
    root[ QStringLiteral( "assets" ) ] = QJsonArray{ asset };
    return parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) );
}

}  // namespace

/// 자동 업데이트의 판단은 전부 이 순수 로직에서 나온다. 버전 비교가 틀리면
/// 같은 버전을 무한히 다시 설치하거나 새 버전을 영원히 놓치고, 매니페스트
/// 검증이 느슨하면 위조된 매니페스트가 임의의 파일을 내려받게 만든다.
class TestUpdateVersioning : public QObject
{
    Q_OBJECT

private slots:
    // 버전 비교
    void treatsTrailingZeroSegmentsAsEqual();
    void ordersVersionsNumerically();
    void refusesToJudgeUnparsableVersions();
    // UpdateInfo 판정
    void detectsNewerVersion();
    void skipSilencesSameAndOlderVersions();
    void enforcesMinimumFromVersion();
    // 매니페스트 파싱
    void parsesValidManifest();
    void rejectsFutureSchema();
    void rejectsOtherProduct();
    void rejectsNonGithubAssetHost();
    void rejectsPlainHttpAssetHost();
    void rejectsOutOfRangeSize();
    void rejectsMalformedSha256();
    void rejectsRootDirWithPathSeparator();
    void rejectsZeroEntryCount();
    void rejectsManifestWithoutMatchingAsset();
    void dropsUnsafeRemovals();
    void dropsUntrustedNotesUrl();
    void allowsExplicitlyChosenMirrorHost();
    // 주기 판정
    void neverDueWhenIntervalIsZero();
    void dueWhenNeverChecked();
    void dueWhenLastCheckIsInTheFuture();
    void respectsIntervalBoundary();
    void clampsInterval();
    // URL
    void defaultManifestUrlIsTrusted();
    void cacheBusterReplacesPreviousValue();
};

// ── 버전 비교 ─────────────────────────────────────────────

void TestUpdateVersioning::treatsTrailingZeroSegmentsAsEqual()
{
    // 0.2.0 이전 빌드는 "0.2.0.0" 을 setApplicationVersion 에 넣었고, 새
    // 매니페스트는 "0.2.0" 을 쓴다. 둘이 다르게 비교되면 같은 버전을 계속
    // 새 버전으로 착각한다.
    QCOMPARE( compareVersions( QStringLiteral( "0.2.0.0" ), QStringLiteral( "0.2.0" ) ), 0 );
    QCOMPARE( compareVersions( QStringLiteral( "0.2" ), QStringLiteral( "0.2.0.0" ) ), 0 );
}

void TestUpdateVersioning::ordersVersionsNumerically()
{
    QVERIFY( compareVersions( QStringLiteral( "0.3" ), QStringLiteral( "0.2.9" ) ) > 0 );
    QVERIFY( compareVersions( QStringLiteral( "0.2.1" ), QStringLiteral( "0.2.0.0" ) ) > 0 );
    QVERIFY( compareVersions( QStringLiteral( "0.2.0" ), QStringLiteral( "0.10.0" ) ) < 0 );
    // 문자열 비교라면 "0.10" < "0.9" 가 된다. 자리별 숫자 비교여야 한다.
    QVERIFY( compareVersions( QStringLiteral( "0.10.0" ), QStringLiteral( "0.9.0" ) ) > 0 );
}

void TestUpdateVersioning::refusesToJudgeUnparsableVersions()
{
    // 근거가 없으면 "같다" 로 둔다. 새 버전으로 기울면 엉뚱한 설치가 시작된다.
    QCOMPARE( compareVersions( QStringLiteral( "미정" ), QStringLiteral( "0.2.0" ) ), 0 );
    QCOMPARE( compareVersions( QString(), QStringLiteral( "0.2.0" ) ), 0 );
    QVERIFY( !isVersionParsable( QString() ) );
    QVERIFY( !isVersionParsable( QStringLiteral( "latest" ) ) );
    QVERIFY( isVersionParsable( QStringLiteral( "0.2.0.0" ) ) );
}

// ── UpdateInfo 판정 ───────────────────────────────────────

void TestUpdateVersioning::detectsNewerVersion()
{
    const UpdateInfo info = parseUpdateManifest( toJson( makeManifest() ), QLatin1String( kProduct ) );
    QVERIFY( info.isValid() );
    QVERIFY( info.isNewerThan( QStringLiteral( "0.2.0" ) ) );
    QVERIFY( info.isNewerThan( QStringLiteral( "0.2.0.0" ) ) );
    QVERIFY( !info.isNewerThan( QStringLiteral( "0.2.1" ) ) );
    QVERIFY( !info.isNewerThan( QStringLiteral( "0.3.0" ) ) );
    // 현재 버전을 해석할 수 없으면 업데이트하지 않는다.
    QVERIFY( !info.isNewerThan( QStringLiteral( "개발빌드" ) ) );
}

void TestUpdateVersioning::skipSilencesSameAndOlderVersions()
{
    const UpdateInfo info = parseUpdateManifest( toJson( makeManifest() ), QLatin1String( kProduct ) );

    // 0.2.1 을 건너뛰었으면 0.2.1 은 계속 침묵해야 한다. 자리 수가 달라도.
    QVERIFY( info.isSkipped( QStringLiteral( "0.2.1" ) ) );
    QVERIFY( info.isSkipped( QStringLiteral( "0.2.1.0" ) ) );
    // 더 높은 버전을 건너뛴 상태면 당연히 침묵.
    QVERIFY( info.isSkipped( QStringLiteral( "0.3.0" ) ) );
    // 예전에 0.2.0 을 건너뛴 것은 0.2.1 알림을 막지 못한다.
    QVERIFY( !info.isSkipped( QStringLiteral( "0.2.0" ) ) );
    QVERIFY( !info.isSkipped( QString() ) );
}

void TestUpdateVersioning::enforcesMinimumFromVersion()
{
    const UpdateInfo info = parseUpdateManifest( toJson( makeManifest() ), QLatin1String( kProduct ) );
    QVERIFY( info.isUpgradableFrom( QStringLiteral( "0.2.0" ) ) );
    QVERIFY( info.isUpgradableFrom( QStringLiteral( "0.2.0.0" ) ) );
    QVERIFY( !info.isUpgradableFrom( QStringLiteral( "0.1.0" ) ) );

    // 하한을 적지 않은 매니페스트는 제약이 없다.
    QJsonObject root = makeManifest();
    root.remove( QStringLiteral( "minimumFromVersion" ) );
    const UpdateInfo noFloor = parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) );
    QVERIFY( noFloor.isValid() );
    QVERIFY( noFloor.isUpgradableFrom( QStringLiteral( "0.0.1" ) ) );
}

// ── 매니페스트 파싱 ───────────────────────────────────────

void TestUpdateVersioning::parsesValidManifest()
{
    QString error;
    const UpdateInfo info = parseUpdateManifest( toJson( makeManifest() ),
                                                 QLatin1String( kProduct ), &error );
    QVERIFY2( info.isValid(), qPrintable( error ) );
    QVERIFY( error.isEmpty() );
    QCOMPARE( info.schema, 1 );
    QCOMPARE( info.version, QStringLiteral( "0.2.1" ) );
    QCOMPARE( info.tag, QStringLiteral( "v0.2.1" ) );
    QCOMPARE( info.commit, QStringLiteral( "4503ab3" ) );
    QVERIFY( !info.mandatory );
    QVERIFY( info.releasedAt.isValid() );
    QCOMPARE( info.asset.id, QStringLiteral( "app" ) );
    QCOMPARE( info.asset.rootDir, QStringLiteral( "MultiRoot-reST-CPP-0.2.1" ) );
    QCOMPARE( info.asset.size, 157286400LL );
    QCOMPARE( info.asset.entryCount, 82 );
    QCOMPARE( info.asset.sha256, dummySha256() );
    QVERIFY( info.notesUrl.isValid() );
}

void TestUpdateVersioning::rejectsFutureSchema()
{
    QJsonObject root = makeManifest();
    root[ QStringLiteral( "schema" ) ] = kSupportedManifestSchema + 1;

    QString error;
    const UpdateInfo info = parseUpdateManifest( toJson( root ), QLatin1String( kProduct ), &error );
    QVERIFY( !info.isValid() );
    QVERIFY( !error.isEmpty() );
}

void TestUpdateVersioning::rejectsOtherProduct()
{
    QJsonObject root = makeManifest();
    root[ QStringLiteral( "product" ) ] = QStringLiteral( "SomeOtherApp" );
    QVERIFY( !parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) ).isValid() );
}

void TestUpdateVersioning::rejectsNonGithubAssetHost()
{
    // 매니페스트가 위조되어도 임의의 서버에서 실행 파일을 받아오지 않아야 한다.
    const UpdateInfo info = parseWithAssetField(
        QStringLiteral( "url" ), QStringLiteral( "https://evil.example.com/app.zip" ) );
    QVERIFY( !info.isValid() );
}

void TestUpdateVersioning::rejectsPlainHttpAssetHost()
{
    const UpdateInfo info = parseWithAssetField(
        QStringLiteral( "url" ), QStringLiteral( "http://github.com/a/b/releases/download/v1/a.zip" ) );
    QVERIFY( !info.isValid() );
}

void TestUpdateVersioning::rejectsOutOfRangeSize()
{
    QVERIFY( !parseWithAssetField( QStringLiteral( "size" ), 0 ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "size" ), 1024 ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "size" ), -1 ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "size" ),
                                   static_cast< double >( kMaxAssetSize ) + 1 ).isValid() );
}

void TestUpdateVersioning::rejectsMalformedSha256()
{
    QVERIFY( !parseWithAssetField( QStringLiteral( "sha256" ), QString() ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "sha256" ),
                                   QString( 63, QLatin1Char( 'a' ) ) ).isValid() );
    // 16진이 아닌 문자
    QVERIFY( !parseWithAssetField( QStringLiteral( "sha256" ),
                                   QString( 64, QLatin1Char( 'z' ) ) ).isValid() );
    // 비ASCII 가 섞여도 죽지 않고 거부해야 한다.
    QVERIFY( !parseWithAssetField( QStringLiteral( "sha256" ),
                                   QString( 63, QLatin1Char( 'a' ) ) + QStringLiteral( "가" ) ).isValid() );
}

void TestUpdateVersioning::rejectsRootDirWithPathSeparator()
{
    // rootDir 은 파일 시스템 경로를 만드는 데 쓰이므로 이름 한 조각이어야 한다.
    QVERIFY( !parseWithAssetField( QStringLiteral( "rootDir" ), QStringLiteral( "a/b" ) ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "rootDir" ), QStringLiteral( "..\\b" ) ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "rootDir" ), QStringLiteral( ".." ) ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "rootDir" ), QStringLiteral( "C:" ) ).isValid() );
    QVERIFY( !parseWithAssetField( QStringLiteral( "rootDir" ), QString() ).isValid() );
}

void TestUpdateVersioning::rejectsZeroEntryCount()
{
    QVERIFY( !parseWithAssetField( QStringLiteral( "entryCount" ), 0 ).isValid() );
}

void TestUpdateVersioning::rejectsManifestWithoutMatchingAsset()
{
    // 다른 플랫폼용 파일만 들어 있는 매니페스트
    QJsonObject asset = makeAsset();
    asset[ QStringLiteral( "platform" ) ] = QStringLiteral( "linux" );
    QJsonObject root                      = makeManifest();
    root[ QStringLiteral( "assets" ) ]     = QJsonArray{ asset };
    QVERIFY( !parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) ).isValid() );

    // assets 자체가 비어 있는 경우
    QJsonObject empty                   = makeManifest();
    empty[ QStringLiteral( "assets" ) ] = QJsonArray{};
    QVERIFY( !parseUpdateManifest( toJson( empty ), QLatin1String( kProduct ) ).isValid() );

    // JSON 이 아예 아닌 경우 (릴리스가 없을 때 GitHub 이 주는 HTML 등)
    QVERIFY( !parseUpdateManifest( QByteArrayLiteral( "<html>Not Found</html>" ),
                                   QLatin1String( kProduct ) ).isValid() );
}

void TestUpdateVersioning::dropsUnsafeRemovals()
{
    QJsonObject root = makeManifest();
    root[ QStringLiteral( "removals" ) ] = QJsonArray{
        QStringLiteral( "Qt6Lottie.dll" ),
        QStringLiteral( "../../Windows/System32/kernel32.dll" ),
        QStringLiteral( "sub/dir.dll" ),
        QStringLiteral( ".." ),
        QString(),
    };

    const UpdateInfo info = parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) );
    QVERIFY( info.isValid() );
    // 안전한 이름만 남는다. 목록이 부실한 것은 구버전 파일이 남는 정도의
    // 문제이지만, 경로 탈출을 허용하면 앱 디렉터리 밖을 지운다.
    QCOMPARE( info.removals.size(), 1 );
    QCOMPARE( info.removals.at( 0 ), QStringLiteral( "Qt6Lottie.dll" ) );
}

void TestUpdateVersioning::dropsUntrustedNotesUrl()
{
    QJsonObject root = makeManifest();
    root[ QStringLiteral( "notesUrl" ) ] = QStringLiteral( "https://evil.example.com/notes" );

    const UpdateInfo info = parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) );
    // 매니페스트 전체를 버리지는 않지만, 사용자를 그 링크로 보내지도 않는다.
    QVERIFY( info.isValid() );
    QVERIFY( info.notesUrl.isEmpty() );
}

void TestUpdateVersioning::allowsExplicitlyChosenMirrorHost()
{
    QJsonObject asset                  = makeAsset();
    asset[ QStringLiteral( "url" ) ]   = QStringLiteral( "http://mirror.local:8080/app.zip" );
    QJsonObject root                   = makeManifest();
    root[ QStringLiteral( "assets" ) ] = QJsonArray{ asset };
    root[ QStringLiteral( "notesUrl" ) ] = QStringLiteral( "http://mirror.local:8080/notes.html" );

    // 기본 경로에서는 거부한다.
    QVERIFY( !parseUpdateManifest( toJson( root ), QLatin1String( kProduct ) ).isValid() );

    // 사용자가 update/manifestUrl 로 그 호스트를 직접 지정했다면, 같은 곳에서
    // 파일을 받겠다는 뜻이므로 허용한다 (사내 미러 / 개발용 로컬 서버).
    const UpdateInfo info = parseUpdateManifest( toJson( root ), QLatin1String( kProduct ),
                                                 nullptr, QStringLiteral( "mirror.local" ) );
    QVERIFY( info.isValid() );
    QVERIFY( info.notesUrl.isValid() );

    // 지정한 호스트와 다른 곳은 여전히 막힌다.
    QVERIFY( !parseUpdateManifest( toJson( root ), QLatin1String( kProduct ),
                                   nullptr, QStringLiteral( "other.local" ) ).isValid() );
}

// ── 주기 판정 ─────────────────────────────────────────────

void TestUpdateVersioning::neverDueWhenIntervalIsZero()
{
    const QDateTime now = QDateTime( QDate( 2026, 8, 14 ), QTime( 12, 0 ), QTimeZone::UTC );
    QVERIFY( !isCheckDue( now.addDays( -100 ), 0, now ) );
    QVERIFY( !isCheckDue( QDateTime(), 0, now ) );
    // 사람이 INI 에 음수를 적어도 "확인하지 않음" 으로 처리한다.
    QVERIFY( !isCheckDue( now.addDays( -100 ), -7, now ) );
}

void TestUpdateVersioning::dueWhenNeverChecked()
{
    const QDateTime now = QDateTime( QDate( 2026, 8, 14 ), QTime( 12, 0 ), QTimeZone::UTC );
    QVERIFY( isCheckDue( QDateTime(), 7, now ) );
}

void TestUpdateVersioning::dueWhenLastCheckIsInTheFuture()
{
    const QDateTime now = QDateTime( QDate( 2026, 8, 14 ), QTime( 12, 0 ), QTimeZone::UTC );
    // 시계를 되돌렸거나 사람이 INI 를 고친 경우. 뺄셈만 하면 영원히 점검하지
    // 않는 상태에 빠진다.
    QVERIFY( isCheckDue( now.addDays( 30 ), 7, now ) );
}

void TestUpdateVersioning::respectsIntervalBoundary()
{
    const QDateTime now = QDateTime( QDate( 2026, 8, 14 ), QTime( 12, 0 ), QTimeZone::UTC );
    QVERIFY( !isCheckDue( now.addDays( -6 ), 7, now ) );
    QVERIFY( isCheckDue( now.addDays( -7 ), 7, now ) );
    QVERIFY( isCheckDue( now.addDays( -8 ), 7, now ) );
    // 하루 주기의 경계
    QVERIFY( !isCheckDue( now.addSecs( -3600 ), 1, now ) );
    QVERIFY( isCheckDue( now.addDays( -1 ), 1, now ) );
}

void TestUpdateVersioning::clampsInterval()
{
    QCOMPARE( clampCheckIntervalDays( 7 ), 7 );
    QCOMPARE( clampCheckIntervalDays( -1 ), 0 );
    QCOMPARE( clampCheckIntervalDays( 100000 ), kMaxCheckIntervalDays );
}

// ── URL ───────────────────────────────────────────────────

void TestUpdateVersioning::defaultManifestUrlIsTrusted()
{
    const QUrl url = defaultManifestUrl();
    QVERIFY( url.isValid() );
    QVERIFY( isAllowedAssetHost( url ) );
    QVERIFY( url.path().endsWith( QStringLiteral( "/releases/latest/download/update-manifest.json" ) ) );
}

void TestUpdateVersioning::cacheBusterReplacesPreviousValue()
{
    const QUrl once  = withCacheBuster( defaultManifestUrl(), 1000 );
    const QUrl twice = withCacheBuster( once, 2000 );
    QCOMPARE( twice.query(), QStringLiteral( "t=2000" ) );
    QVERIFY( isAllowedAssetHost( twice ) );
}

MRST_REGISTER_TEST( TestUpdateVersioning );

#include "tst_UpdateVersioning.moc"
