#pragma once

/// oclero/qlementine QStyle 통합 지점.
///
/// QlementineStyle 은 앱 전체의 QStyle 을 갈아끼우므로 QApplication 을 만든 직후,
/// 위젯이 하나도 생성되기 전에 install() 을 불러야 한다.
///
/// 주의: Qt 스타일시트는 QStyle 이 그린 결과를 덮어쓴다. 그래서 예전처럼
/// `QWidget { background-color: ... }` 같은 전역 스타일시트를 씌우면 이 스타일을
/// 적용한 의미가 사라진다. ThemeManager::applyToApplication 과
/// QBaseView::applyThemeStyleSheet 가 isActive() 로 그 경로를 우회한다.
#include "core/solThemeManager.hpp"

namespace QlementineTheme
{
    /// QlementineStyle 을 설치하고 theme 팔레트를 반영한다.
    /// 이후 ThemeManager::themeChanged 를 따라 다크/라이트를 자동으로 바꾼다.
    /// 테마 JSON 리소스를 읽지 못하면 아무것도 바꾸지 않고 false 를 돌려준다.
    ///
    /// **ThemeManager::setTheme 보다 먼저** 불러야 한다. 순서가 뒤집히면
    /// applyToApplication 이 아직 isActive() 가 아닌 상태에서 레거시 전역
    /// 스타일시트를 씌우고, 그 뒤 setStyle 이 QlementineStyle 을
    /// QStyleSheetStyle 로 감싸 버려 스타일시트를 영영 벗지 못한다.
    bool                                install( ThemeManager::Theme theme );

    /// QlementineStyle 이 현재 QApplication 의 스타일인지.
    /// 스타일시트가 걸려 프록시로 감싸인 경우도 "활성"으로 본다 — 그래야
    /// applyToApplication 이 스타일시트를 다시 씌우는 악순환에 빠지지 않는다.
    bool                                isActive();
}
