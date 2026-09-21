#include "solPreviewFonts.hpp"
#include "solAppSettings.hpp"
#include "solThemeManager.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <cmath>

namespace mrst {

QString previewFontSettingsPrefix( const bool markdown )
{
    return markdown ? QStringLiteral( "preview/fonts/md/" )
                    : QStringLiteral( "preview/fonts/rst/" );
}

QJsonObject previewFontOptions( const bool markdown, const QWebEngineSettings* webSettings )
{
    const AppSettings settings;
    const QString prefix = previewFontSettingsPrefix( markdown );
    QString mode = settings.value( prefix + "mode", "document" ).toString();
    if( mode != "user" && mode != "web" )
        mode = QStringLiteral( "document" );
    const QFont body = ThemeManager::configuredFont( markdown
        ? ThemeManager::FontRole::PreviewMarkdownBody : ThemeManager::FontRole::PreviewRstBody );
    const QFont code = ThemeManager::configuredFont( markdown
        ? ThemeManager::FontRole::PreviewMarkdownCode : ThemeManager::FontRole::PreviewRstCode );
    double lineHeight = settings.value( prefix + "lineHeight", 1.7 ).toDouble();
    if( !std::isfinite( lineHeight ) || lineHeight < 1.0 || lineHeight > 3.0 )
        lineHeight = 1.7;
    const bool web = mode == "web" && webSettings != nullptr;
    return {
        { "enabled", settings.value( prefix + "enabled", false ).toBool() },
        { "mode", mode },
        { "bodyFamily", web ? webSettings->fontFamily( QWebEngineSettings::StandardFont ) : body.family() },
        { "codeFamily", web ? webSettings->fontFamily( QWebEngineSettings::FixedFont ) : code.family() },
        { "bodySize", web ? webSettings->fontSize( QWebEngineSettings::DefaultFontSize ) : body.pointSizeF() * 96.0 / 72.0 },
        { "codeSize", web ? webSettings->fontSize( QWebEngineSettings::DefaultFixedFontSize ) : code.pointSizeF() * 96.0 / 72.0 },
        { "lineHeight", lineHeight },
        { "headings", settings.value( prefix + "headings", true ).toBool() },
    };
}

QString previewFontScript( const QJsonObject& options )
{
    static const QString source = [] {
        QFile file( QStringLiteral( ":/preview/mrr_fonts.js" ) );
        return file.open( QIODevice::ReadOnly ) ? QString::fromUtf8( file.readAll() ) : QString{};
    }();
    return source + QStringLiteral( "\nwindow.__mrrPreviewFonts.apply(%1);" ).arg(
        QString::fromUtf8( QJsonDocument( options ).toJson( QJsonDocument::Compact ) ) );
}

void applyPreviewFonts( QWebEnginePage* page, const bool markdown )
{
    if( page == nullptr )
        return;
    const QString source = previewFontScript( previewFontOptions( markdown, page->settings() ) );
    auto& scripts = page->scripts();
    const QString name = QStringLiteral( "mrr_preview_fonts" );
    for( const auto& previous : scripts.find( name ) )
        scripts.remove( previous );
    QWebEngineScript script;
    script.setName( name );
    script.setInjectionPoint( QWebEngineScript::DocumentReady );
    script.setWorldId( QWebEngineScript::ApplicationWorld );
    script.setRunsOnSubFrames( false );
    script.setSourceCode( source );
    scripts.insert( script );
    page->runJavaScript( source, QWebEngineScript::ApplicationWorld );
}

} // namespace mrst
