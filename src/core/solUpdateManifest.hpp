#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace mrst {

/// 이 클라이언트가 이해하는 매니페스트 스키마 번호.
/// 매니페스트가 이보다 큰 값을 들고 오면 형식을 모른다는 뜻이므로
/// 업데이트를 시도하지 않는다 (구 클라이언트가 새 형식을 오해하지 않게).
constexpr int                           kSupportedManifestSchema = 1;

/// 배포 파일 크기의 상식 범위. 설정 파일과 달리 매니페스트는 **남이** 고칠 수
/// 있으므로, 값이 이 범위를 벗어나면 파싱 자체를 거부한다.
constexpr qint64                        kMinAssetSize = 1LL * 1024 * 1024;
constexpr qint64                        kMaxAssetSize = 2LL * 1024 * 1024 * 1024;

/// 점검 주기(일)의 허용 범위. 0 은 "확인하지 않음" 센티널이다
/// (preview/unsavedEditMaxReadMs 의 "0 = 제한 없음" 과 같은 관례).
constexpr int                           kMaxCheckIntervalDays = 365;
constexpr int                           kDefaultCheckIntervalDays = 7;

/// 릴리스에 첨부된 배포 파일 하나.
struct UpdateAsset
{
    QString                             id;                ///< "app" 등. 나중에 델타 패치를 구분할 자리
    QString                             kind;              ///< 지금은 "zip" 만
    QString                             platform;          ///< "windows"
    QString                             arch;              ///< "x64"
    QUrl                                url;
    QString                             rootDir;           ///< ZIP 최상위 폴더 이름. 해제할 때 한 겹 벗긴다
    qint64                              size = 0;
    QString                             sha256;            ///< 소문자 64자 16진
    int                                 entryCount = 0;    ///< 해제 진행률 계산용 (bsdtar -v 출력 줄 수)

    [[nodiscard]] bool                  isValid() const;
};

/// update-manifest.json 을 읽어 담은 결과.
struct UpdateInfo
{
    int                                 schema = 0;
    QString                             product;
    QString                             version;
    QString                             tag;
    QDateTime                           releasedAt;
    QString                             commit;
    bool                                mandatory = false;
    QString                             minimumFromVersion;
    QUrl                                notesUrl;
    UpdateAsset                         asset;             ///< assets[] 에서 고른 이 플랫폼용 파일
    QStringList                         removals;          ///< 이번 버전에서 사라진 최상위 항목 이름

    [[nodiscard]] bool                  isValid() const;
    /// currentVersion 보다 새 버전인가.
    [[nodiscard]] bool                  isNewerThan( const QString& currentVersion ) const;
    /// 사용자가 건너뛰기로 지정한 버전에 포함되는가.
    [[nodiscard]] bool                  isSkipped( const QString& skippedVersion ) const;
    /// currentVersion 에서 제자리 교체로 올라갈 수 있는가.
    /// minimumFromVersion 보다 낮은 설치본은 수동 재설치가 필요하다.
    [[nodiscard]] bool                  isUpgradableFrom( const QString& currentVersion ) const;
};

/// 두 버전 문자열을 비교한다. lhs 가 크면 양수, 같으면 0, 작으면 음수.
///
/// 문자열을 직접 쪼개지 않고 QVersionNumber 를 쓰는 이유: 구버전이 남긴
/// "0.2.0.0" 과 새 매니페스트의 "0.2.1" 처럼 자리 수가 다른 값을 비교해야
/// 하고, normalized() 가 뒤쪽 0 세그먼트를 떼어 둘을 같은 축에 올려 준다.
/// 어느 쪽이든 파싱에 실패하면 0 (같음) 을 돌려준다 — 판단할 근거가 없을 때
/// "새 버전이다" 로 기울면 엉뚱한 업데이트가 시작된다.
[[nodiscard]] int                       compareVersions( const QString& lhs, const QString& rhs );

/// 버전 문자열이 비교에 쓸 수 있는 형태인가.
[[nodiscard]] bool                      isVersionParsable( const QString& text );

/// 에셋 URL 로 허용할 호스트인가. 매니페스트가 위조되어도 임의의 서버에서
/// 실행 파일을 내려받지 않도록, 우리 배포 채널로 한정한다.
///
/// extraAllowedHost 는 사용자가 update/manifestUrl 로 매니페스트 위치를 직접
/// 지정한 경우 그 호스트다. 매니페스트를 어디서 받을지 고른 사람이라면 그
/// 매니페스트가 가리키는 파일도 같은 곳에서 받겠다는 뜻이므로 함께 허용한다
/// (사내 미러와 개발용 로컬 서버가 이 경로로 동작한다).
[[nodiscard]] bool                      isAllowedAssetHost( const QUrl& url,
                                                            const QString& extraAllowedHost = {} );

/// update-manifest.json 을 파싱한다.
///
/// expectedProduct 는 호출자가 MRST_UPDATE_PRODUCT_ID 를 넘긴다(solUpdateService.cpp).
/// **배포 파일 이름(MRST_INTERNAL_NAME)을 여기에 쓰지 않는다** — 파일 이름을 바꾸는
/// 순간 자기 매니페스트를 "다른 제품" 으로 거부하게 된다. 이 파일이
/// 생성 헤더(mrst_version.h)를 직접 include 하지 않는 이유는 테스트 타깃의
/// include 경로에 빌드 디렉터리가 없기 때문이다.
///
/// 실패하면 isValid() == false 인 값을 돌려주고 errorMessage 를 채운다.
[[nodiscard]] UpdateInfo                parseUpdateManifest( const QByteArray& json,
                                                             const QString& expectedProduct,
                                                             QString* errorMessage = nullptr,
                                                             const QString& extraAllowedHost = {} );

/// 점검 주기가 되었는가.
///
/// lastCheckedAt 이 now 보다 미래면 즉시 점검한다. 시계를 되돌렸거나 사람이
/// INI 를 고쳤을 때, 그냥 뺄셈만 하면 영원히 점검하지 않는 상태에 빠진다.
[[nodiscard]] bool                      isCheckDue( const QDateTime& lastCheckedAt, int intervalDays,
                                                    const QDateTime& now );

/// 설정 파일에서 읽은 주기 값을 상식 범위로 자른다.
[[nodiscard]] int                       clampCheckIntervalDays( int days );

/// 기본 매니페스트 URL. GitHub 릴리스의 "latest" 고정 다운로드 경로다.
///
/// API(api.github.com) 를 쓰지 않는 이유는 비인증 60회/시간 제한이 IP 단위라
/// 사내 NAT 뒤에서 공유되기 때문이다. 이 경로는 릴리스 CDN 으로 302 되며
/// 그 제한을 받지 않는다.
[[nodiscard]] QUrl                      defaultManifestUrl();

/// 저장소 홈. 정보 대화상자가 보여 준다.
///
/// 매니페스트 URL 과 **같은 상수**에서 만든다. 슬러그를 한 벌 더 적어 두면
/// 저장소를 옮겼을 때 한쪽만 고쳐진 채로 남는다.
[[nodiscard]] QUrl                      repositoryUrl();

/// CDN 캐시를 우회할 쿼리를 붙인다. 캐시 제어 헤더보다 확실하다.
[[nodiscard]] QUrl                      withCacheBuster( const QUrl& url, qint64 epochSeconds );

}  // namespace mrst
