#pragma once

#include <QFrame>
#include <QIcon>
#include <QList>
#include <QString>

class QKeyEvent;
class QLabel;
class QListWidget;

namespace mrst {

/// 탭 목록 팝업의 한 줄.
struct TabSwitcherEntry
{
    QString                             title;      ///< 탭 제목 (수정 표시 포함)
    QString                             detail;     ///< 파일 경로. 이름 없는 버퍼는 빈 문자열
    QIcon                               icon;
    int                                 tabIndex = -1;   ///< QTabWidget 인덱스
};

/// Ctrl+Tab 으로 뜨는 열린 문서 목록. Visual Studio 의 IDE Navigator 를 따른다.
///
/// **Ctrl 을 떼는 순간 확정된다.** 그래서 이 위젯은 키보드를 잡아야 하고
/// (`Qt::Popup` 이 그 일을 해 준다), 포커스를 절대 가져가지 않는 자동완성
/// 팝업과는 정반대의 선택이다. 목록이 열릴 때 Ctrl 은 이미 눌려 있는데, 그
/// keyRelease 를 우리가 받지 못하면 목록이 영영 닫히지 않는다.
///
/// `Qt::Popup` 창은 활성화되지 않으므로, 키 이벤트는 여전히 메인 창의
/// `QWidgetWindow::handleKeyEvent()` 로 들어온 뒤 활성 팝업으로 돌려진다
/// (`QApplication::activePopupWidget()`). 그 함수는 팝업의 `focusWidget()` 이
/// 있으면 그쪽을 먼저 주므로, 안쪽 목록은 `Qt::NoFocus` 여야 한다 — 아니면
/// 방향키만 목록이 먹고 Tab 과 Ctrl 뗌은 아무도 받지 않는다.
class TabSwitcherPopup final : public QFrame
{
    Q_OBJECT

public:
    explicit TabSwitcherPopup( QWidget* parent = nullptr );

    /// 목록을 채우고 부모 창 가운데에 띄운다.
    ///
    /// startRow 는 처음 강조할 줄이다. Ctrl+Tab 은 1(= 직전 문서),
    /// Ctrl+Shift+Tab 은 마지막 줄을 준다 — Visual Studio 와 같다.
    void                                showEntries( const QList< TabSwitcherEntry >& entries, int startRow );

signals:
    /// 사용자가 항목을 확정했다. 취소(Esc, 바깥 클릭)면 발신하지 않는다.
    void                                tabChosen( int tabIndex );

protected:
    void                                keyPressEvent( QKeyEvent* event ) override;
    void                                keyReleaseEvent( QKeyEvent* event ) override;

private:
    /// 목록을 순환 이동한다. 끝에서 다음은 처음이다 (Ctrl 을 누른 채로 계속
    /// Tab 을 치는 조작이므로 멈추면 막다른 길처럼 느껴진다).
    void                                step( int delta );
    void                                chooseCurrent();
    void                                refreshDetail();

    QListWidget*                        list_ = nullptr;
    QLabel*                             detailLabel_ = nullptr;
};

}  // namespace mrst
