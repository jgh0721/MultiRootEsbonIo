#pragma once

#include <QFrame>
#include <QString>

class QLabel;

namespace mrst {

/// 자동완성 목록 옆에 붙는 상세 패널.
///
/// 목록 행 안에 오른쪽 정렬로 그리던 `detail` 은 행 폭의 2/5 로 잘려서
/// (`CompletionItemDelegate::paint`) 용어집 정의처럼 긴 글을 보여 줄 수 없다.
/// 이 패널은 그 내용을 별도 창에 줄바꿈해서 담는다.
///
/// **포커스를 가져가지 않는다.** 캐럿은 편집기에 남아 있어야 한다.
class CompletionDetailPopup final : public QFrame
{
    Q_OBJECT

public:
    explicit CompletionDetailPopup( QWidget* parent = nullptr );

    /// 표시할 내용을 채운다. title 과 body 가 모두 비면 false 를 돌려주고
    /// 아무것도 표시하지 않는다 (호출 측은 이때 숨기면 된다).
    bool                                setContent( const QString& title, const QString& body,
                                                    const QString& source = {} );

    /// anchor 오른쪽 바깥에 붙여 띄운다. 화면을 벗어나면 왼쪽으로 뒤집는다.
    /// anchor 는 전역 좌표 (보통 자동완성 팝업의 geometry()).
    void                                showBesideAnchor( const QRect& anchor );
    /// 마우스 커서 근처에 띄운다 (본문 호버용).
    void                                showNearPoint( const QPoint& globalPos );

    [[nodiscard]] bool                  hasContent() const;

private:
    /// 내용에 맞춰 크기를 잡는다. 폭은 고정, 높이는 본문 길이에 따른다.
    void                                resizeToContent();
    /// 화면 경계 안으로 밀어 넣는다.
    [[nodiscard]] QPoint                clampToScreen( QPoint target, const QPoint& reference ) const;

    QLabel*                             titleLabel_ = nullptr;
    QLabel*                             bodyLabel_ = nullptr;
    QLabel*                             sourceLabel_ = nullptr;
};

}  // namespace mrst
