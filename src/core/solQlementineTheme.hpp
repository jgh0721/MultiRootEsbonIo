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
namespace QlementineTheme
{
    /// QlementineStyle 을 설치하고 ThemeManager 의 현재 테마를 반영한다.
    /// 이후 ThemeManager::themeChanged 를 따라 다크/라이트를 자동으로 바꾼다.
    /// 테마 JSON 리소스를 읽지 못하면 아무것도 바꾸지 않고 false 를 돌려준다.
    bool                                install();

    /// QlementineStyle 이 현재 QApplication 의 스타일인지.
    bool                                isActive();
}
