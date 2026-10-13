#include "TestRunner.hpp"

#include "core/solThemeManager.hpp"
#include "core/solTextRenderingSettings.hpp"

#include <QApplication>
#include <QScopeGuard>
#include <QTest>
#include <QTemporaryDir>

#include <array>

/// 테마 글꼴의 범위별 저장과 QApplication 적용을 검증한다.
class ThemeFontSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void                                rolesRoundTripIndependently();
    void                                pointSizeIsClamped();
    void                                applicationUsesUiFont();
    void                                renderingSettingsAndStartupFlags();
};

void ThemeFontSettingsTest::rolesRoundTripIndependently()
{
    constexpr std::array roles{
        ThemeManager::FontRole::UserInterface,
        ThemeManager::FontRole::Explorer,
        ThemeManager::FontRole::Outline,
        ThemeManager::FontRole::DiagnosticsAndLog,
        ThemeManager::FontRole::PreviewRstBody,
        ThemeManager::FontRole::PreviewRstCode,
        ThemeManager::FontRole::PreviewMarkdownBody,
        ThemeManager::FontRole::PreviewMarkdownCode,
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

void ThemeFontSettingsTest::renderingSettingsAndStartupFlags()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("rendering.ini");
    QSettings settings(path, QSettings::IniFormat);
    auto rendering = mrst::TextRenderingSettings::load(settings);
    QVERIFY(!rendering.editorEnabled);
    QVERIFY(!rendering.previewEnabled);
    QVERIFY(!rendering.strokeEnabled);
    QCOMPARE(rendering.chromiumFlags("--lang=ko --text-gamma=1.7"),
             QByteArray("--lang=ko --text-gamma=1.7 --no-sandbox"));
    settings.setValue("textView/mask/enabled", true);
    settings.setValue("textView/mask/gamma", 1.35);
    settings.setValue("textView/mask/strength", 1.25);
    settings.setValue("preview/rendering/enabled", true);
    settings.setValue("preview/rendering/gamma", 2.1);
    settings.setValue("preview/rendering/contrast", 0.4);
    settings.setValue("preview/rendering/strokeEnabled", true);
    settings.setValue("preview/rendering/strokeWidth", 0.2);
    settings.sync();
    rendering = mrst::TextRenderingSettings::load(QSettings(path, QSettings::IniFormat));
    QVERIFY(rendering.editorEnabled && rendering.previewEnabled && rendering.strokeEnabled);
    QCOMPARE(rendering.editorGamma, 1.35);
    QCOMPARE(rendering.editorStrength, 1.25);
    QCOMPARE(rendering.strokeWidth, 0.2);
    const auto flags = rendering.chromiumFlags(
        "--lang=ko --text-gamma=1.0 --text-contrast 0.1 --no-sandbox --disk-cache-dir=\"C:/cache dir\"");
    QVERIFY(flags.contains("--disk-cache-dir=\"C:/cache dir\""));
    const auto tokens = QProcess::splitCommand(QString::fromUtf8(flags));
    QCOMPARE(tokens.count("--no-sandbox"), 1);
    QVERIFY(tokens.contains("--text-gamma=2.10"));
    QVERIFY(tokens.contains("--text-contrast=0.40"));
    QVERIFY(!tokens.contains("--text-gamma=1.0"));
    QVERIFY(!tokens.contains("0.1"));
    settings.setValue("textView/mask/gamma", "nan");
    settings.setValue("textView/mask/strength", -4);
    settings.setValue("preview/rendering/gamma", "invalid");
    settings.setValue("preview/rendering/contrast", 30);
    settings.setValue("preview/rendering/strokeWidth", "inf");
    rendering = mrst::TextRenderingSettings::load(settings);
    QCOMPARE(rendering.editorGamma, 1.2);
    QCOMPARE(rendering.editorStrength, 1.1);
    QCOMPARE(rendering.previewGamma, 2.2);
    QCOMPARE(rendering.previewContrast, 0.5);
    QCOMPARE(rendering.strokeWidth, 0.15);
}

#include "tst_ThemeFontSettings.moc"
