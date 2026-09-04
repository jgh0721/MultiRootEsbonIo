#include "TestRunner.hpp"

#include "core/solThemeManager.hpp"

#include <QApplication>
#include <QScopeGuard>
#include <QTest>

#include <array>

/// 테마 글꼴의 범위별 저장과 QApplication 적용을 검증한다.
class ThemeFontSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void                                rolesRoundTripIndependently();
    void                                pointSizeIsClamped();
    void                                applicationUsesUiFont();
};

void ThemeFontSettingsTest::rolesRoundTripIndependently()
{
    constexpr std::array roles{
        ThemeManager::FontRole::UserInterface,
        ThemeManager::FontRole::Explorer,
        ThemeManager::FontRole::Outline,
        ThemeManager::FontRole::DiagnosticsAndLog,
    };

    std::array< QFont, roles.size() > previous;
    for( size_t index = 0; index < roles.size(); ++index )
        previous[ index ] = ThemeManager::configuredFont( roles[ index ] );
    const auto restore = qScopeGuard( [&previous, &roles] {
        for( size_t index = 0; index < roles.size(); ++index )
            ThemeManager::setConfiguredFont( roles[ index ], previous[ index ] );
    } );

    const QString family = QApplication::font().family();
    for( size_t index = 0; index < roles.size(); ++index )
        ThemeManager::setConfiguredFont( roles[ index ],
                                         QFont( family, 11 + static_cast< int >( index ) ) );

    for( size_t index = 0; index < roles.size(); ++index )
    {
        const QFont actual = ThemeManager::configuredFont( roles[ index ] );
        QCOMPARE( actual.family(), family );
        QCOMPARE( actual.pointSize(), 11 + static_cast< int >( index ) );
    }
}

void ThemeFontSettingsTest::pointSizeIsClamped()
{
    const auto role = ThemeManager::FontRole::Explorer;
    const QFont previous = ThemeManager::configuredFont( role );
    const auto restore = qScopeGuard(
        [role, previous] { ThemeManager::setConfiguredFont( role, previous ); } );

    QFont tooSmall( previous );
    tooSmall.setPointSize( 1 );
    ThemeManager::setConfiguredFont( role, tooSmall );
    QCOMPARE( ThemeManager::configuredFont( role ).pointSize(),
              ThemeManager::kMinimumFontPointSize );

    QFont tooLarge( previous );
    tooLarge.setPointSize( 200 );
    ThemeManager::setConfiguredFont( role, tooLarge );
    QCOMPARE( ThemeManager::configuredFont( role ).pointSize(),
              ThemeManager::kMaximumFontPointSize );
}

void ThemeFontSettingsTest::applicationUsesUiFont()
{
    const auto role = ThemeManager::FontRole::UserInterface;
    const QFont previousConfigured = ThemeManager::configuredFont( role );
    const QFont previousApplication = QApplication::font();
    const QString previousStyleSheet = qApp->styleSheet();
    const auto restore = qScopeGuard( [previousConfigured, previousApplication,
                                       previousStyleSheet] {
        ThemeManager::setConfiguredFont( ThemeManager::FontRole::UserInterface,
                                         previousConfigured );
        qApp->setFont( previousApplication );
        qApp->setStyleSheet( previousStyleSheet );
    } );

    QFont chosen( previousApplication );
    chosen.setPointSize( 17 );
    ThemeManager::setConfiguredFont( role, chosen );
    ThemeManager::instance().applyToApplication();

    QCOMPARE( QApplication::font().family(), chosen.family() );
    QCOMPARE( QApplication::font().pointSize(), 17 );
}

MRST_REGISTER_TEST( ThemeFontSettingsTest );

#include "tst_ThemeFontSettings.moc"
