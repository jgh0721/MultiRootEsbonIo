#pragma once

#include "solAppSettings.hpp"

#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>

namespace mrst {

inline constexpr auto kPreviewRstUnlimitedWidth = "preview/css/rst/unlimitedWidth";

// Only document containers are expanded; sidebar, image and table widths stay intact.
inline QString previewWidthScript( const bool enabled )
{
    return QString::fromLatin1( R"JS((function () {
    const id = 'mrr-preview-width';
    let style = document.getElementById(id);
    if (!%1) {
        if (style) style.remove();
        return;
    }
    if (!document.documentElement) return;
    if (!style) {
        style = document.createElement('style');
        style.id = id;
        // An early layer also overrides !important rules in layered themes.
        const host = document.head || document.documentElement;
        host.insertBefore(style, host.firstChild);
    }
    const css = '@layer mrr-preview-width {' +
        '.wy-nav-content,div.document,div.body,main,.main .content,' +
        '.bd-page-width,.bd-article-container,.bd-main .bd-content,[role="main"] {' +
        'max-width:none !important;max-inline-size:none !important;' +
        'width:auto !important;inline-size:auto !important;}}';
    if (style.textContent !== css) style.textContent = css;
})();)JS" ).arg( enabled ? QStringLiteral( "true" ) : QStringLiteral( "false" ) );
}

// Update the visible page and future navigations without changing Sphinx output.
inline void applyPreviewCss( QWebEnginePage* page, const bool restructuredText )
{
    if( page == nullptr )
        return;
    const bool enabled = restructuredText &&
        AppSettings().value( QLatin1String( kPreviewRstUnlimitedWidth ), false ).toBool();
    const QString source = previewWidthScript( enabled );
    auto& scripts = page->scripts();
    const QString name = QStringLiteral( "mrr_preview_css" );
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
