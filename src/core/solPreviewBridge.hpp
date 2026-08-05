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
    /// 브리지 핸드셰이크 전이면 마지막 요청 하나를 큐잉했다가 ready 때 보낸다.
    void                                requestScrollToLine( int sourceIndex, int line, double ratio );
    void                                requestRebind();
    /// milliseconds 동안 프리뷰의 스크롤 보고를 무시한다 (피드백 루프 차단).
    void                                suppressScrollFeedback( int milliseconds );

public slots:   // JS 에서 호출한다
    void                                ready( int protocolVersion );
    void                                sourceLocationClicked( int sourceIndex, int line, double viewportRatio );
    void                                previewScrolled( int sourceIndex, int line, double viewportRatio );

signals:
    // → JS
    void                                scrollToLineRequested( int sourceIndex, int line, double ratio );
    void                                rebindRequested();
    void                                scrollFeedbackSuppressed( int milliseconds );

    // → C++ 내부
    void                                bridgeReady();
    void                                editorNavigationRequested( int sourceIndex, int line, double viewportRatio );
    void                                previewScrollChanged( int sourceIndex, int line, double viewportRatio );

private:
    bool                                ready_ = false;
    bool                                hasPendingScroll_ = false;
    int                                 pendingSourceIndex_ = 0;
    int                                 pendingLine_ = 1;
    double                              pendingRatio_ = 0.5;
};

}  // namespace mrst
