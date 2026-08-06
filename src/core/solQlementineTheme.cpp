#include "stdafx.h"
#include "core/solQlementineTheme.hpp"
#include "core/solThemeManager.hpp"

#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>

#include <QApplication>
#include <QPointer>

namespace
{
    // 팔레트는 qlementine 공식 테마(showcase, MIT)를 resources/themes 에 그대로 두고
    // qrc 로 묶었다. 자세한 배경은 resources/themes/README.md 참고.
    constexpr auto kDarkThemePath  = ":/themes/dark.json";
    constexpr auto kLightThemePath = ":/themes/light.json";

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

bool QlementineTheme::install()
{
    if( !qApp || g_style )
        return isActive();

    auto* style = new oclero::qlementine::QlementineStyle( qApp );
    style->setAnimationsEnabled( true );

    // 위젯이 만들어지기 전에 팔레트까지 확정해야 첫 프레임이 라이트로 깜빡이지 않는다.
    if( !applyThemeToStyle( style, ThemeManager::instance().currentTheme() ) )
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
    return !g_style.isNull() && QApplication::style() == g_style;
}
