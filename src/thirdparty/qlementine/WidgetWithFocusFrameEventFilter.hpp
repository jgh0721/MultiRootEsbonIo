// SPDX-FileCopyrightText: Olivier Cléro <oclero@hotmail.com>
// SPDX-License-Identifier: MIT
//
// ── 이 저장소의 사본 ───────────────────────────────────────────────────────
//
// 상류(oclero/qlementine)의 같은 파일을 대체한다. 고친 것은 `_focusFrame` 을
// 생 포인터에서 `QPointer` 로 바꾼 것과, 프레임이 사라졌으면 다시 만드는 것뿐이다.
//
// **왜.** `QFocusFrame::setWidget(w)` 은 프레임을 `w` 의 **부모**로 재부모화한다
// (Qt 의 `setParent(widget->parentWidget())`). 그런데 이 필터는 프레임을 `_widget`
// 의 자식으로 만들어 두고 그 뒤로는 생 포인터로만 들고 있다. 그래서 프레임의
// 새 부모가 파괴되면 프레임은 함께 죽는데 필터는(그것은 `_widget` 의 자식이라)
// 살아남아 **죽은 포인터**를 쥔다. 다음 `QEvent::Show` 에서 그 포인터로
// `setWidget()` 을 부르면 프로세스가 죽는다.
//
// 이 앱에서는 Qt-Advanced-Docking-System 이 그 조건을 만든다. 도크를 닫고 열면
// (F11 프리뷰 전체 화면이 `hideAllDockPanels()` 로 그렇게 한다) ADS 가 탭 바와
// 컨테이너를 다시 만들면서 도크 탭의 버튼들을 옮긴다. 그 버튼은
// `shouldHaveExternalFocusFrame()` 대상이라 이 필터가 붙어 있다.
//
// 실측(RelWithDebInfo, Qt 6.11.1): .md 문서를 열고 F11 을 200~250 ms 간격으로
// 연타하면 13~24회에서 0xC0000005 로 종료한다. 디버거로 잡은 스택은
//
//     MainWindow::setPreviewFullScreen → hideAllDockPanels
//       → ads::CDockWidget::toggleView → … → ads::CDockWidgetTab::setActiveTab
//         → QWidget::setVisible → (Show 이벤트)
//           → WidgetWithFocusFrameEventFilter::eventFilter
//             → QFocusFrame::setWidget → QWidget::style()   ← this 가 해제된 메모리
//
// 이고, 덤프에서 `_focusFrame` 이 가리키는 16바이트가 전부 `feeefeee` 였다
// (`_widget` 은 살아 있는 QPushButton 이었다). 크래시 직전에
// `QWidget::mapTo(): parent must be in parent hierarchy` 경고가 수백 줄 나오는데,
// 그것도 같은 원인이다 — `QFocusFramePrivate::updateSize()` 가 이미 부모 관계가
// 끊어진 프레임의 좌표를 옮기려 하기 때문이다.
//
// 상류에 올려야 하는 고침이다. 올라가면 이 사본을 지우고
// cmake/BuildQlementine.cmake 의 교체 단계도 함께 지운다.
//
// 이 파일은 상류와 나란히 놓고 보는 것이 목적이라 BOM 을 붙이지 않는다
// (src/thirdparty/scintilla-qt/PlatQt.cpp 와 같은 판단). 줄끝은 CRLF 다.

#pragma once

#include <QAbstractScrollArea>
#include <QFocusFrame>
#include <QPointer>
#include <QTimer>
#include <QEvent>

namespace oclero::qlementine {
class WidgetWithFocusFrameEventFilter : public QObject {
  Q_OBJECT
public:
  explicit WidgetWithFocusFrameEventFilter(QWidget* widget)
    : QObject(widget)
    , _widget(widget) {
    _focusFrame = new QFocusFrame(_widget);
  }

  bool eventFilter(QObject* watchedObject, QEvent* evt) override {
    if (watchedObject == _widget) {
      const auto type = evt->type();
      // Create the focus frame as late as possible to give
      // more chances to any parent (e.g. scrollarea) to already exist.
      // QEvent::Show isn't sufficient. We need to delay even more, so
      // waiting for the first QEvent::Paint is our only solution.
      if (type == QEvent::Paint && !_added) {
        QTimer::singleShot(0, this, [this]() {
          if (!_added) {
            _added = true;
            ensureFocusFrame();
            if (_focusFrame) {
              _focusFrame->setWidget(_widget);
            }
          }
        });
      } else if (type == QEvent::Show && _added) {
        // setWidget() 이 프레임을 위젯의 부모로 옮겨 놓기 때문에, 그 부모가
        // 파괴되면 프레임도 함께 사라진다. QPointer 가 그것을 알려 주므로
        // 여기서 다시 만든다.
        ensureFocusFrame();
        if (_focusFrame) {
          _focusFrame->setWidget(nullptr);
          _focusFrame->setWidget(_widget);
        }
      }
    }

    return QObject::eventFilter(watchedObject, evt);
  }

private:
  void ensureFocusFrame() {
    if (!_focusFrame && _widget) {
      _focusFrame = new QFocusFrame(_widget);
    }
  }

  QWidget* _widget{ nullptr };
  QPointer<QFocusFrame> _focusFrame{ nullptr };
  bool _added{ false };
};
} // namespace oclero::qlementine
