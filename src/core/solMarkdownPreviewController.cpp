#include "stdafx.h"
#include "solMarkdownPreviewController.hpp"

#include "solAppSettings.hpp"
#include "solMarkdownAssets.hpp"
#include "solThemeManager.hpp"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <iterator>

namespace mrst {
namespace {

/// 편집 중 렌더 디바운스.
///
/// Sphinx 쪽 350ms 와 별개 타이머다 — 자릿수가 다르다(Sphinx 빌드 1.7~2.6초 대
/// markdown-it 파싱 수 ms). 100ms 미만이 "즉시" 로 지각되는 구간이라 그 경계에
/// 두고, 그러면 빠른 타이핑의 절반 정도가 접힌다.
///
/// 한국어 IME 조합 중에는 한 글자에 대해 Scintilla 수정 통지가 여러 번 온다.
/// 그 묶음이 이 창 안에 접히기를 기대하는 값이다.
constexpr int kRenderDebounceMs = 120;

/// 이 크기를 넘는 문서는 타이핑 중 렌더를 끊고 저장할 때만 갱신한다.
/// QWebChannel JSON 직렬화가 그만큼 커진다. Sphinx 핫스왑의 상한과 같은 값이다.
constexpr int kMaxPushBytes = 4 * 1024 * 1024;

}  // namespace

MarkdownPreviewController::MarkdownPreviewController( QObject* parent )
    : QObject( parent )
    , debounce_( new QTimer( this ) )
{
    debounce_->setSingleShot( true );
    debounce_->setInterval( kRenderDebounceMs );
    connect( debounce_, &QTimer::timeout, this, &MarkdownPreviewController::push );
}

void MarkdownPreviewController::reloadSettings()
{
    // 지금은 읽을 것이 없다. 수식 렌더러와 원격 허용 여부는 옵션 페이로드에
    // 실어 보내므로 push() 가 그때그때 설정을 읽는다.
}

void MarkdownPreviewController::notifyBridgeReady()
{
    bridgeReady_ = true;
    if( hasPending_ )
        push();
}

void MarkdownPreviewController::notifyShellReloaded()
{
    bridgeReady_ = false;
    // 새 페이지는 아무것도 렌더하지 않은 상태다. 해시를 남겨 두면 셸을 다시 읽은
    // 뒤 같은 원문이 "이미 보냈다" 로 걸러져 프리뷰가 빈 채로 남는다.
    lastPushedHash_.clear();
    lastPushedPath_.clear();
}

void MarkdownPreviewController::requestRender( const QString& path, const QString& text,
                                               const bool immediate, const bool force )
{
    if( path.isEmpty() )
        return;

    const QByteArray utf8 = text.toUtf8();
    if( utf8.size() > kMaxPushBytes && !immediate )
    {
        // 타이핑 중에는 보내지 않는다. 저장하면 immediate 로 들어와 갱신된다.
        return;
    }

    const QByteArray hash = QCryptographicHash::hash( utf8, QCryptographicHash::Sha1 );
    if( !force && path == lastPushedPath_ && hash == lastPushedHash_ )
        return;   // 내용이 그대로다

    pendingPath_ = path;
    pendingText_ = text;
    hasPending_ = true;

    if( immediate )
    {
        debounce_->stop();
        push();
        return;
    }
    debounce_->start();
}

void MarkdownPreviewController::cancel()
{
    debounce_->stop();
    hasPending_ = false;
    pendingText_.clear();
    pendingPath_.clear();
}

QUrl MarkdownPreviewController::shellUrl()
{
    AppSettings settings;

    // 코어 로드에 필요한 것은 쿼리로 넘긴다. 렌더 요청의 optionsJson 을 기다릴 수
    // 없다 — 코어 로드가 그보다 먼저 시작하고, 첫 렌더가 그것을 기다린다. 설정이
    // 바뀌면 C++ 이 셸을 다시 읽으므로 이 값들도 함께 갱신된다.
    QUrlQuery query;
    // 출력이 고정 URL 이라 매번 같으므로, Sphinx 쪽과 같은 방식으로 일련번호를
    // 실어 Chromium 캐시를 무효화한다.
    query.addQueryItem( QStringLiteral( "v" ), QString::number( ++shellSerial_ ) );
    query.addQueryItem( QStringLiteral( "remote" ),
                        settings.value( QStringLiteral( "preview/allowRemoteContent" ), true ).toBool()
                            ? QStringLiteral( "1" )
                            : QStringLiteral( "0" ) );
    query.addQueryItem( QStringLiteral( "core" ),
                        settings.value( QStringLiteral( "preview/markdownCoreSource" ),
                                       QString::fromLatin1( mdassets::kDefaultCoreSource ) ).toString() );
    query.addQueryItem( QStringLiteral( "mi" ), QString::fromLatin1( mdassets::kMarkdownItVersion ) );
    query.addQueryItem( QStringLiteral( "cdn" ), QString::fromLatin1( mdassets::kCdnBase ) );
    query.addQueryItem( QStringLiteral( "sri" ), QString::fromLatin1( mdassets::kMarkdownItSri ) );

    QUrl url( QString::fromLatin1( mdassets::kShellResourcePath ) );
    url.setQuery( query );
    return url;
}

void MarkdownPreviewController::push()
{
    debounce_->stop();
    if( !hasPending_ || pendingPath_.isEmpty() )
        return;
    if( !bridgeReady_ )
        return;   // 핸드셰이크가 끝나면 notifyBridgeReady() 가 흘려보낸다

    const QByteArray utf8 = pendingText_.toUtf8();
    lastPushedHash_ = QCryptographicHash::hash( utf8, QCryptographicHash::Sha1 );
    lastPushedPath_ = pendingPath_;
    hasPending_ = false;

    // 상대 경로 이미지가 살아나는 근거. 문서가 있는 폴더를 <base> 로 세운다.
    const QString baseUrl =
        QUrl::fromLocalFile( QFileInfo( pendingPath_ ).absolutePath() + QLatin1Char( '/' ) ).toString();

    emit pushRequested( pendingText_, baseUrl, buildOptionsJson(), ++token_ );
}

QString MarkdownPreviewController::buildOptionsJson() const
{
    AppSettings settings;
    QJsonObject options{
        // .rst 프리뷰와 같은 설정을 그대로 쓴다. 꺼져 있으면 JS 가 CDN 요청을
        // 아예 시도하지 않는다 — 그러지 않으면 콘솔에 차단 오류만 쌓인다.
        { QStringLiteral( "allowRemote" ),
          settings.value( QStringLiteral( "preview/allowRemoteContent" ), true ).toBool() },
        { QStringLiteral( "cdnBase" ), QString::fromLatin1( mdassets::kCdnBase ) },
        // 본문을 갈아 끼울 것인가, 제자리에서 고칠 것인가. .rst 핫스왑과 같은
        // 설정을 쓴다 — 두 프리뷰가 같은 morph 구현(mrr_preview.js)을 쓰므로
        // 스위치도 하나여야 한다.
        { QStringLiteral( "domMorph" ),
          settings.value( QStringLiteral( "preview/domMorph" ), true ).toBool() },
        { QStringLiteral( "markdownItVersion" ), QString::fromLatin1( mdassets::kMarkdownItVersion ) },
        // 지연 로드 대상. 문서에 해당 문법이 있을 때만 실제로 받는다.
        { QStringLiteral( "katexVersion" ), QString::fromLatin1( mdassets::kKatexVersion ) },
        { QStringLiteral( "mermaidVersion" ), QString::fromLatin1( mdassets::kMermaidVersion ) },
        { QStringLiteral( "mathRenderer" ),
          settings.value( QStringLiteral( "preview/mathRenderer" ),
                         QString::fromLatin1( mdassets::kDefaultMathRenderer ) ).toString() },
        // mermaid 는 초기화할 때 테마 이름을 받는다. 나중에 바꿀 수 없어서
        // 로드 시점에 알려 줘야 한다.
        { QStringLiteral( "dark" ), ThemeManager::instance().currentTheme() == ThemeManager::Dark },
        // 색은 CSS 변수로 꽂는다. 그래서 테마를 바꿀 때 페이지를 다시 읽지 않아도
        // 되고, 세션에서 받아 둔 mermaid/KaTeX 를 버리지 않는다.
        { QStringLiteral( "theme" ), buildThemeJson() },
    };
    return QString::fromUtf8( QJsonDocument( options ).toJson( QJsonDocument::Compact ) );
}

QJsonObject MarkdownPreviewController::buildThemeJson() const
{
    // markdown.* 테마 키 아홉 개를 그대로 쓴다. **새 키를 만들지 않는다** — 그 키는
    // 이미 editableColorEntries() 에 있어 설정 대화상자의 "Markdown" 범위로 노출되어
    // 있었고, 지금까지는 아무 데도 쓰이지 않는 죽은 색이었다. 되살리면 그 UI 가
    // 그날부터 의미를 갖는다.
    //
    // 편집기 구문 색(text.lexer.markdown.*)과는 완전히 별개다. markdown.codeBackground
    // 와 text.lexer.markdown.codeBackground 는 다른 키다.
    auto&      theme = ThemeManager::instance();
    const bool dark = theme.currentTheme() == ThemeManager::Dark;

    const auto hex = [ &theme ]( const char* key ) {
        return theme.color( QString::fromLatin1( key ) ).name( QColor::HexRgb );
    };

    const QString codeBackground = hex( "markdown.codeBackground" );

    QJsonObject out{
        { QStringLiteral( "bg" ), hex( "markdown.background" ) },
        { QStringLiteral( "fg" ), hex( "markdown.foreground" ) },
        { QStringLiteral( "link" ), hex( "markdown.link" ) },
        { QStringLiteral( "heading" ), hex( "markdown.heading" ) },
        { QStringLiteral( "border" ), hex( "markdown.border" ) },
        { QStringLiteral( "code-bg" ), codeBackground },
        { QStringLiteral( "inline-code" ), hex( "markdown.inlineCode" ) },
        { QStringLiteral( "quote" ), hex( "markdown.blockquote" ) },
        { QStringLiteral( "task-done" ), hex( "markdown.taskChecked" ) },
        // 표 머리 배경은 코드 배경을 그대로 쓴다. 키를 하나 더 만들 값이 없다.
        { QStringLiteral( "table-head-bg" ), codeBackground },
    };

    // 경고 상자 다섯 색은 테마 키가 없다. markdown.* 를 늘리는 대신 테마별 고정값을
    // 쓴다 — 요점은 다섯이 서로 구별되는 것이고, 사용자가 그것을 하나씩 고르고 싶어할
    // 이유가 없다. 값은 GitHub 의 두 테마에서 가져왔다.
    static const char* kAlertKeys[] = { "alert-note", "alert-tip", "alert-important",
                                       "alert-warning", "alert-caution" };
    static const char* kAlertLight[] = { "#0969da", "#1a7f37", "#8250df", "#9a6700", "#cf222e" };
    static const char* kAlertDark[] = { "#4493f8", "#3fb950", "#ab7df8", "#d29922", "#f85149" };
    for( std::size_t i = 0; i < std::size( kAlertKeys ); ++i )
    {
        out.insert( QString::fromLatin1( kAlertKeys[ i ] ),
                    QString::fromLatin1( dark ? kAlertDark[ i ] : kAlertLight[ i ] ) );
    }

    return out;
}

void MarkdownPreviewController::onRenderCompleted( const int token, const bool ok, const QString& message )
{
    // 늦게 도착한 회신은 버린다. 타이핑 중에는 앞선 렌더가 아직 끝나지 않은 채
    // 다음 것이 나갈 수 있다.
    if( token != token_ )
        return;

    if( !ok )
        emit logMessage( tr( "프리뷰 렌더에 실패했습니다: %1" ).arg( message ) );
    emit renderFinished( lastPushedPath_, ok, message );
}

void MarkdownPreviewController::onRendererOrigin( const QString& origin, const QString& version )
{
    // JS 는 사실만 올린다. 사용자에게 보이는 문장은 여기서 만든다 — 그쪽에는
    // tr() 이 없다.
    if( origin == QLatin1String( "bundled-fallback" ) )
    {
        emit logMessage( tr( "프리뷰: %1 을 인터넷에서 받지 못해 내장본을 사용합니다." )
                            .arg( version.isEmpty() ? QStringLiteral( "markdown-it" ) : version ) );
        return;
    }
    if( origin == QLatin1String( "cdn" ) )
    {
        emit logMessage( tr( "프리뷰: %1 을 인터넷에서 받았습니다." ).arg( version ) );
    }
}

void MarkdownPreviewController::onAssetFailed( const QString& assetId, const QString& reason )
{
    // 문서마다 한 번만 남긴다. 렌더가 8Hz 로 도는데 그때마다 같은 줄을 쌓으면
    // 로그가 못 쓰게 된다.
    const QString key = assetId + QLatin1Char( '\x1f' ) + lastPushedPath_;
    if( reportedAssetFailure_ == key )
        return;
    reportedAssetFailure_ = key;

    emit logMessage( tr( "프리뷰: %1 을 가져오지 못했습니다(%2). 그 부분은 원본 텍스트로 남습니다." )
                        .arg( assetId, reason ) );
}

}  // namespace mrst
