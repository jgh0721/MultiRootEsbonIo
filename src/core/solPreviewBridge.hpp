#pragma once

#include <QObject>
#include <QString>

class QWebEngineView;

namespace mrst {

/// QWebChannel 로 프리뷰 페이지에 "bridge" 이름으로 등록되는 객체.
///
/// JS -> C++ 는 slot, C++ -> JS 는 signal 로 흐른다. runJavaScript 로 문자열을
/// 조립해 보내지 않는다 — 이스케이프 버그가 나기 쉽고 커서가 움직일 때마다
/// 스크립트를 다시 파싱하게 된다.
class PreviewBridge final : public QObject
{
    Q_OBJECT

public:
    explicit PreviewBridge( QObject* parent = nullptr );

    /// 뷰에 QWebChannel 과 부트스트랩 스크립트를 설치한다. 한 번만 호출한다.
    void                                attachTo( QWebEngineView* view );

    [[nodiscard]] bool                  isReady() const;
    /// 새 페이지를 로드하기 직전에 호출해 핸드셰이크 상태를 초기화한다.
    void                                resetReady();

    /// (src, line) 이 프리뷰 창의 ratio 위치에 오도록 스크롤을 요청한다.
    /// line 은 소수다 — 에디터 쪽 줄바꿈 안에서의 위치까지 담는다.
    /// 브리지 핸드셰이크 전이면 마지막 요청 하나를 큐잉했다가 ready 때 보낸다.
    void                                requestScrollToLine( int sourceIndex, double line, double ratio );
    void                                requestRebind();
    /// milliseconds 동안 프리뷰의 스크롤 보고를 무시한다 (피드백 루프 차단).
    void                                suppressScrollFeedback( int milliseconds );
    /// 전체 리로드 없이 본문만 갱신한다 (재빌드 깜빡임 제거).
    ///
    /// allowMorph 는 `preview/domMorph` 설정이다. 참이면 페이지가 본문을 갈아
    /// 끼우지 않고 **제자리에서 고친다** — 레이아웃 재계산이 사라져 편집 중
    /// 화면이 출렁이지 않는다. 거짓이면 예전처럼 통째로 교체한다. 결과 DOM 은
    /// 어느 쪽이든 같으므로, 새 경로가 말썽을 부리는 사용자가 되돌릴 수 있게
    /// 남겨 둔 스위치다.
    void                                requestHotSwap( const QString& documentHtml, const QString& baseUrl,
                                                        bool allowMorph, int token );
    /// Markdown **원문**을 밀어 페이지가 렌더하게 한다.
    ///
    /// requestHotSwap 을 재사용하지 않는다. 그 계약은 "이 HTML 로 body 를 갈아라" 인데
    /// 여기서 보내는 것은 HTML 이 아니다 — C++ 에는 markdown 렌더러가 없고 있어서도
    /// 안 된다(token.map 을 얻을 방법이 없고, 두면 같은 문법을 두 벌 유지하게 된다).
    /// 이름이 거짓인 시그널은 다음 사람이 반드시 잘못 읽는다. 계약의 *모양*(토큰 +
    /// 성공/실패 회신 + 실패 시 전체 리로드)은 그대로 베낀다.
    ///
    /// optionsJson 에 테마 색·원격 허용 여부·수식 렌더러·고정 버전이 들어간다.
    void                                requestMarkdownRender( const QString& text, const QString& baseUrl,
                                                               const QString& optionsJson, int token );

public slots:   // JS 에서 호출한다
    void                                ready( int protocolVersion );
    void                                sourceLocationClicked( int sourceIndex, double line, double viewportRatio );
    /// 프리뷰의 스크롤 위치가 바뀌었다.
    ///
    /// `userDriven` 은 **그 스크롤을 사용자가 만들었는가** 다. 페이지는 내용이
    /// 바뀐 뒤에도 한참 스스로 움직이므로(이미지·폰트·수식·다이어그램이 자리를
    /// 잡는다) 그 둘을 가르지 않으면 편집기가 따라 밀린다. 판정은 입력 이벤트를
    /// 받는 페이지 쪽에서만 할 수 있어 JS 가 실어 보낸다 — mrr_preview.js 의
    /// `scrollIsUserDriven()` 에 그 근거가 적혀 있다.
    void                                previewScrolled( int sourceIndex, double line, double viewportRatio,
                                                          bool userDriven );
    void                                hotSwapResult( int token, bool ok, const QString& message );
    /// 렌더 결과. 실패하면 C++ 이 전체 리로드로 되돌린다(핫스왑과 같은 규칙).
    void                                markdownRendered( int token, bool ok, const QString& message );
    /// 렌더러가 준비됐다. origin 은 "bundled" | "cdn" | "bundled-fallback".
    /// **사실만 올린다.** JS 에는 tr() 이 없으므로 사용자에게 보이는 문장은 C++ 이 만든다.
    void                                markdownRendererReady( const QString& origin, const QString& version );
    /// 지연 로드 자산(mermaid, KaTeX)을 가져오지 못했다.
    void                                markdownAssetFailed( const QString& assetId, const QString& reason );

signals:
    // → JS
    void                                scrollToLineRequested( int sourceIndex, double line, double ratio );
    void                                rebindRequested();
    void                                scrollFeedbackSuppressed( int milliseconds );
    void                                hotSwapRequested( const QString& documentHtml, const QString& baseUrl,
                                                          bool allowMorph, int token );
    void                                markdownSourceChanged( const QString& text, const QString& baseUrl,
                                                               const QString& optionsJson, int token );

    // → C++ 내부
    void                                bridgeReady();
    void                                hotSwapCompleted( int token, bool ok, const QString& message );
    void                                editorNavigationRequested( int sourceIndex, double line, double viewportRatio );
    /// `userDriven` 의 뜻은 위 슬롯과 같다.
    void                                previewScrollChanged( int sourceIndex, double line, double viewportRatio,
                                                              bool userDriven );
    void                                markdownRenderCompleted( int token, bool ok, const QString& message );
    void                                markdownRendererOrigin( const QString& origin, const QString& version );
    void                                markdownAssetLoadFailed( const QString& assetId, const QString& reason );

private:
    bool                                ready_ = false;
    bool                                hasPendingScroll_ = false;
    int                                 pendingSourceIndex_ = 0;
    double                              pendingLine_ = 1.0;
    double                              pendingRatio_ = 0.5;
};

}  // namespace mrst
