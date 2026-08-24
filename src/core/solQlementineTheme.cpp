#include "stdafx.h"
#include "core/solQlementineTheme.hpp"
#include "core/solThemeManager.hpp"

#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QPointer>

namespace
{
    // 팔레트는 qlementine 공식 테마(showcase, MIT)를 resources/themes 에 그대로 두고
    // qrc 로 묶었다. 자세한 배경은 resources/themes/README.md 참고.
    constexpr auto kDarkThemePath  = ":/themes/dark.json";
    constexpr auto kLightThemePath = ":/themes/light.json";

    /// 편집기 탭의 닫기(X) 를 되살리는 것 하나만 하는 얇은 껍데기.
    ///
    /// `QStyleSheetStyle` 은 `PE_IndicatorTabClose` 를 기반 스타일로 넘기기 **전에**
    /// 위젯을 닫기 단추가 아니라 그 부모인 탭 바로 바꿔치기한다
    /// (qstylesheetstyle.cpp: `w = w->parentWidget()`). QSS 규칙을 탭 바에 대고
    /// 맞춰 보려는 것인데, `QTabBar::close-button` 규칙이 아무 데도 없으면 그렇게
    /// 바뀐 위젯이 그대로 기반 스타일로 내려간다. qlementine 은 그 자리에서 단추를
    /// 기대하고 `qobject_cast< QAbstractButton* >` 을 하므로 캐스트가 실패해
    /// **한 픽셀도 그리지 않는다.** Qt 의 진짜 닫기 단추 위젯은 살아 있으니 클릭은
    /// 계속 먹어서, X 가 안 보이는데 그 자리를 누르면 탭이 닫히는 상태가 된다.
    ///
    /// 이 경로는 피할 수 없다 — 편집기 탭 바는 ADS 도크 안에 있고 ADS 는 자기
    /// 스타일시트를 늘 도크 매니저에 걸며, 스타일시트가 걸린 조상이 있으면 자손의
    /// 스타일은 `QStyleSheetStyle` 로 갈린다(QWidgetPrivate::inheritStyle).
    ///
    /// 그런데 Qt 는 바꿔치기하면서 **진짜 단추를 기반 스타일의 속성에 남겨 준다**
    /// (QMacStyle 이 쓰라고 둔 것이다). 그것을 되찾아 다시 넘기면 qlementine 이
    /// 본래 의도대로 그린다 — 색·크기·호버 배경·애니메이션이 모두 테마를 따르고,
    /// 스타일이 직접 그리므로 배율 150% 에서도 흐려지지 않는다.
    class TabCloseButtonStyle : public oclero::qlementine::QlementineStyle
    {
    public:
        using QlementineStyle::QlementineStyle;

        void drawPrimitive( const PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                            const QWidget* widget = nullptr ) const override
        {
            if( element == PE_IndicatorTabClose && qobject_cast< const QAbstractButton* >( widget ) == nullptr )
            {
                // 이름은 Qt 내부의 것이다. 사라지면 X 가 다시 안 보이게 될 뿐이라
                // (지금과 같은 상태) 없을 때를 대비한 분기는 두지 않는다.
                if( const auto* button = static_cast< const QWidget* >(
                            property( "_q_styleSheetRealCloseButton" ).value< void* >() ) )
                {
                    QlementineStyle::drawPrimitive( element, option, painter, button );
                    return;
                }
            }

            QlementineStyle::drawPrimitive( element, option, painter, widget );
        }
    };

    QPointer< oclero::qlementine::QlementineStyle > g_style;

    /// setThemeJsonPath 는 파싱에 실패해도 조용히 넘어가고 기본(라이트) 팔레트를
    /// 남긴다. meta.name 을 확인해야 실제로 반영됐는지 알 수 있다.
    bool applyThemeToStyle( oclero::qlementine::QlementineStyle* style, ThemeManager::Theme theme )
    {
        if( !style )
            return false;

        const bool    dark     = ( theme == ThemeManager::Dark );
        const QString path     = QString::fromLatin1( dark ? kDarkThemePath : kLightThemePath );
        const QString expected = dark ? QStringLiteral( "Dark" ) : QStringLiteral( "Light" );

        style->setThemeJsonPath( path );
        return style->theme().meta.name == expected;
    }
}

bool QlementineTheme::install( ThemeManager::Theme theme )
{
    if( !qApp || g_style )
        return isActive();

    auto* style = new TabCloseButtonStyle( qApp );
    style->setAnimationsEnabled( true );

    // 위젯이 만들어지기 전에 팔레트까지 확정해야 첫 프레임이 라이트로 깜빡이지 않는다.
    if( !applyThemeToStyle( style, theme ) )
    {
        delete style;
        return false;
    }

    QApplication::setStyle( style );
    g_style = style;

    QObject::connect( &ThemeManager::instance(), &ThemeManager::themeChanged, qApp,
                      []( ThemeManager::Theme theme ) { applyThemeToStyle( g_style, theme ); } );

    return true;
}

bool QlementineTheme::isActive()
{
    if( g_style.isNull() )
        return false;
    if( QApplication::style() == g_style )
        return true;

    // qApp 에 스타일시트가 걸려 있으면 Qt 는 QApplication::setStyle 에서 우리 스타일을
    // QStyleSheetStyle 로 감싸고 그 프록시를 부모로 삼는다(QWidget/QStyle 소유권).
    // 이때 포인터 비교만 하면 isActive() 가 영원히 false 가 되고,
    // applyToApplication 이 스타일시트를 계속 다시 씌워 스스로를 고착시킨다.
    return g_style->parent() == QApplication::style();
}
