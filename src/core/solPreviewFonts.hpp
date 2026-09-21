#pragma once

#include <QJsonObject>
#include <QString>

class QWebEnginePage;
class QWebEngineSettings;

namespace mrst {

QString previewFontSettingsPrefix( bool markdown );
QJsonObject previewFontOptions( bool markdown, const QWebEngineSettings* webSettings );
QString previewFontScript( const QJsonObject& options );
// Applies to the current document and subsequent navigations; never edits build output.
void applyPreviewFonts( QWebEnginePage* page, bool markdown );

} // namespace mrst
