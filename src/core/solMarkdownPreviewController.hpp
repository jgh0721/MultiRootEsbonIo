#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

class QTimer;

namespace mrst {

/// 내장 Markdown 렌더러의 오케스트레이터.
///
/// SphinxPreviewController 와 나란히 두는 형제다. WorkspaceController 안의 분기로
/// 하지 않는 이유: 그 클래스는 이미 1500줄에 멤버 40여 개인데, md 경로는 자기만의
/// 디바운스 타이머·렌더 토큰·마지막 push 해시·자산 출처 상태를 갖는다. 다 얹으면
/// 어느 멤버가 어느 경로 것인지 읽어서 알 수 없게 된다.
///
/// **단, 페이지 상태는 소유하지 않는다.** QWebEngineView 는 한 개뿐이라 두
/// 컨트롤러가 각자 previewUrl_/previewLoadInFlight_ 를 들면 반드시 어긋난다.
/// 그것은 WorkspaceController 가 계속 단독으로 갖고, 이 클래스는 "셸을 띄워 달라 /
/// 이 원문을 밀어 달라" 를 시그널로 요청한다.
class MarkdownPreviewController final : public QObject
{
    Q_OBJECT

public:
    explicit MarkdownPreviewController( QObject* parent = nullptr );

    void                                reloadSettings();

    /// 셸 페이지의 브리지 핸드셰이크가 끝났다. 큐에 걸린 요청을 흘려보낸다.
    void                                notifyBridgeReady();
    /// 셸을 다시 로드했다(또는 다른 페이지로 갔다). 핸드셰이크 상태를 되돌린다.
    void                                notifyShellReloaded();

    /// 편집 중이면 immediate=false 로 부른다. 저장·탭 전환·F5 는 true.
    ///
    /// force 는 마지막으로 보낸 원문과 같아도 다시 보내게 한다(F5 전용).
    void                                requestRender( const QString& path, const QString& text,
                                                       bool immediate, bool force );
    void                                cancel();

    /// qrc 안의 셸 URL. serial 을 쿼리에 실어 Chromium 캐시를 무효화한다.
    [[nodiscard]] QUrl                  shellUrl();

signals:
    void                                logMessage( const QString& text );
    /// 브리지로 원문을 밀어 달라. WorkspaceController 만 브리지를 만진다.
    void                                pushRequested( const QString& text, const QString& baseUrl,
                                                       const QString& optionsJson, int token );
    /// 렌더가 끝났다(또는 실패했다). 상태 칩과 스크롤 정렬의 계기다.
    void                                renderFinished( const QString& path, bool ok, const QString& message );

public slots:
    /// 브리지의 markdownRenderCompleted 를 받는다.
    void                                onRenderCompleted( int token, bool ok, const QString& message );
    /// 브리지의 markdownRendererOrigin 을 받는다. 사실만 오므로 문장은 여기서 만든다.
    void                                onRendererOrigin( const QString& origin, const QString& version );
    void                                onAssetFailed( const QString& assetId, const QString& reason );

private:
    void                                push();
    [[nodiscard]] QString               buildOptionsJson() const;
    /// 프리뷰 CSS 변수로 꽂을 색 팔레트. markdown.* 테마 키를 그대로 쓴다.
    [[nodiscard]] QJsonObject           buildThemeJson() const;

    QTimer*                             debounce_ = nullptr;
    /// 브리지 핸드셰이크 전에 들어온 요청. 마지막 하나만 들고 있는다 —
    /// PreviewBridge::requestScrollToLine 의 hasPendingScroll_ 과 같은 관용구다.
    bool                                bridgeReady_ = false;
    bool                                hasPending_ = false;
    QString                             pendingPath_;
    QString                             pendingText_;
    /// 마지막으로 실제로 밀어 넣은 원문의 해시.
    ///
    /// 세션 복원은 같은 문서에 대해 setActiveDocument 와 sigFileOpened 로 두 번
    /// 요청한다. 100KB 문서를 8Hz 로 QWebChannel JSON 에 실으면 800KB/s 이므로
    /// 값싼 가드가 값을 한다.
    QByteArray                          lastPushedHash_;
    QString                             lastPushedPath_;
    int                                 token_ = 0;
    int                                 shellSerial_ = 0;
    /// 자산 실패는 문서마다 한 번만 로그에 남긴다. 렌더가 8Hz 로 도는데 그때마다
    /// 같은 줄을 쌓으면 로그가 못 쓰게 된다.
    QString                             reportedAssetFailure_;
};

}  // namespace mrst
