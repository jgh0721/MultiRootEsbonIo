#include "TestRunner.hpp"
#include "core/solPreviewFonts.hpp"
#include "core/solPreviewCss.hpp"
#include "core/solThemeManager.hpp"
#include "editor/DocumentLineEnding.hpp"

#include <QEventLoop>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <memory>

namespace {
QVariant run( QWebEnginePage& page, const QString& script )
{
    QEventLoop loop;
    auto result = std::make_shared<QVariant>();
    QTimer timeout;
    timeout.setSingleShot( true );
    QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
    timeout.start( 5000 );
    page.runJavaScript( script, QWebEngineScript::ApplicationWorld,
                       [result, loopGuard = QPointer<QEventLoop>( &loop )]( const QVariant& value ) {
        *result = value;
        if( loopGuard ) loopGuard->quit();
    } );
    loop.exec();
    return *result;
}

const auto html = R"HTML(<!doctype html><html><head><style>
body { font-family: 'Times New Roman'; font-size: 16px; line-height: 1.25; }
@layer project { #p { font-family: 'Georgia' !important; font-size: 19px !important; } }
pre, code, code span {font-family: monospace !important; font-size: 12px;}
.math {font-family: 'Math Font'; font-size: 21px;}
.icon {font-family: 'Icon Font';}
</style></head><body><h1 id="heading">Heading</h1><p id="p">Text <strong>bold</strong></p>
<p id="inline" style="font-family: Verdana !important; font-size: 17px !important">Inline</p>
<pre><code id="code"><span id="token">code</span></code></pre>
<div class="math"><span id="math">math</span></div><span id="icon" class="icon">icon</span>
<div id="dynamic"></div></body></html>)HTML";

const auto snapshot = R"JS(JSON.stringify(['p','inline','heading','code','token','math','icon'].map(id=>{
    const s=getComputedStyle(document.getElementById(id));return [s.fontFamily,s.fontSize,s.lineHeight];
})))JS";

QJsonObject customOptions()
{
    return {{"enabled", true}, {"mode", "user"}, {"bodyFamily", "Arial"},
            {"codeFamily", "Courier New"}, {"bodySize", 20}, {"codeSize", 18},
            {"lineHeight", 1.8}, {"headings", false}};
}
}

class TestPreviewFonts : public QObject
{
    Q_OBJECT
private slots:
    void overridesAndRestores();
    void navigationAndIndependentSettings();
    void newDocumentLineEndings();
    void contentWidthOverride();
};

void TestPreviewFonts::overridesAndRestores()
{
    QWebEnginePage page;
    QSignalSpy loaded( &page, &QWebEnginePage::loadFinished );
    page.setHtml( QString::fromUtf8( html ) );
    QVERIFY( loaded.wait( 10000 ) );
    const QString before = run( page, snapshot ).toString();
    auto options = customOptions();
    run( page, mrst::previewFontScript( options ) );
    QVERIFY( run( page, "getComputedStyle(p).fontFamily.includes('Arial')" ).toBool() );
    QCOMPARE( run( page, "getComputedStyle(p).fontSize" ).toString(), QString( "20px" ) );
    QCOMPARE( run( page, "getComputedStyle(p).lineHeight" ).toString(), QString( "36px" ) );
    QVERIFY( run( page, "getComputedStyle(document.getElementById('inline')).fontFamily.includes('Arial')" ).toBool() );
    QVERIFY( run( page, "getComputedStyle(token).fontFamily.includes('Courier New')" ).toBool() );
    QCOMPARE( run( page, "getComputedStyle(token).fontSize" ).toString(), QString( "18px" ) );
    QVERIFY( run( page, "getComputedStyle(heading).fontFamily.includes('Times New Roman')" ).toBool() );
    QVERIFY( run( page, "getComputedStyle(document.getElementById('math')).fontFamily.includes('Math Font')" ).toBool() );
    QVERIFY( run( page, "getComputedStyle(icon).fontFamily.includes('Icon Font')" ).toBool() );
    run( page, "dynamic.innerHTML='<p id=\"later\" style=\"font-family: Georgia !important\">new</p>'" );
    QTRY_VERIFY( run( page, "getComputedStyle(later).fontFamily.includes('Arial')" ).toBool() );
    options["headings"] = true;
    run( page, mrst::previewFontScript( options ) );
    QVERIFY( run( page, "getComputedStyle(heading).fontFamily.includes('Arial')" ).toBool() );
    options["mode"] = "document";
    run( page, mrst::previewFontScript( options ) );
    QCOMPARE( run( page, snapshot ).toString(), before );
    QVERIFY( run( page, "getComputedStyle(later).fontFamily.includes('Georgia')" ).toBool() );
    options["mode"] = "user";
    run( page, mrst::previewFontScript( options ) );
    options["enabled"] = false;
    run( page, mrst::previewFontScript( options ) );
    QCOMPARE( run( page, snapshot ).toString(), before );
}

void TestPreviewFonts::navigationAndIndependentSettings()
{
    AppSettings settings;
    const QString rst = mrst::previewFontSettingsPrefix( false );
    const QString md = mrst::previewFontSettingsPrefix( true );
    const QStringList keys{rst + "enabled", rst + "mode", md + "enabled", md + "mode"};
    QMap<QString, QVariant> previous;
    for( const auto& key : keys ) previous.insert( key, settings.value( key ) );
    const auto restore = qScopeGuard( [&] {
        for( const auto& key : keys ) {
            if( previous[key].isValid() ) settings.setValue( key, previous[key] );
            else settings.remove( key );
        }
    } );
    settings.setValue( rst + "enabled", true );
    settings.setValue( rst + "mode", "web" );
    settings.setValue( md + "enabled", true );
    settings.setValue( md + "mode", "document" );
    QWebEnginePage page;
    page.settings()->setFontFamily( QWebEngineSettings::StandardFont, "Arial" );
    page.settings()->setFontSize( QWebEngineSettings::DefaultFontSize, 23 );
    const auto rstOptions = mrst::previewFontOptions( false, page.settings() );
    QCOMPARE( rstOptions["bodyFamily"].toString(), QString( "Arial" ) );
    QCOMPARE( rstOptions["bodySize"].toInt(), 23 );
    QCOMPARE( mrst::previewFontOptions( true, page.settings() )["mode"].toString(), QString( "document" ) );
    mrst::applyPreviewFonts( &page, false );
    QSignalSpy loaded( &page, &QWebEnginePage::loadFinished );
    page.setHtml( QString::fromUtf8( html ) );
    QVERIFY( loaded.wait( 10000 ) );
    QCOMPARE( run( page, "getComputedStyle(p).fontSize" ).toString(), QString( "23px" ) );
    mrst::applyPreviewFonts( &page, true );
    QCOMPARE( run( page, "getComputedStyle(p).fontSize" ).toString(), QString( "19px" ) );
    page.setHtml( QString::fromUtf8( html ) );
    QVERIFY( loaded.wait( 10000 ) );
    QCOMPARE( run( page, "getComputedStyle(p).fontSize" ).toString(), QString( "19px" ) );
}

void TestPreviewFonts::contentWidthOverride()
{
    AppSettings settings;
    const QString key = QLatin1String(mrst::kPreviewRstUnlimitedWidth);
    const QVariant previous = settings.value(key);
    const auto restore = qScopeGuard([&] {
        if (previous.isValid()) settings.setValue(key, previous);
        else settings.remove(key);
    });
    const QString content = QStringLiteral(R"HTML(<!doctype html><html><head><style>
        body {width:1400px;margin:0;}
        .wy-nav-side {width:300px;position:fixed;}
        .wy-nav-content-wrap {margin-left:300px;}
        .wy-nav-content {max-width:800px;width:800px;padding:20px;box-sizing:border-box;}
        @layer theme {div.document {max-width:900px !important;width:900px !important;}}
        div.body {max-width:700px;}
        table {width:220px;} img {width:120px;max-width:100%;}
    </style></head><body><aside class="wy-nav-side" id="sidebar">Navigation</aside>
    <div class="wy-nav-content-wrap"><main class="wy-nav-content" id="content">
    <div class="document" id="doc"><div class="body" id="body">
    <p>Text</p><table id="table"><tr><td>Cell</td></tr></table><img id="image">
    </div></div></main></div></body></html>)HTML");
    QWebEnginePage page;
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    settings.remove(key);
    mrst::applyPreviewCss(&page, true);
    page.setHtml(content);
    QVERIFY(loaded.wait(10000));
    const QString dimensions = QStringLiteral(
        "JSON.stringify(['content','doc','body','sidebar','table','image'].map(id=>"
        "document.getElementById(id).getBoundingClientRect().width))");
    const QString original = run(page, dimensions).toString();
    QCOMPARE(run(page, "getComputedStyle(document.getElementById('content')).maxWidth").toString(),
             QString("800px"));

    settings.setValue(key, true);
    mrst::applyPreviewCss(&page, true);
    QTRY_COMPARE(run(page, "getComputedStyle(document.getElementById('content')).maxWidth").toString(),
                 QString("none"));
    QCOMPARE(run(page, "document.getElementById('content').getBoundingClientRect().width").toInt(), 1100);
    QCOMPARE(run(page, "document.getElementById('doc').getBoundingClientRect().width").toInt(), 1060);
    QCOMPARE(run(page, "document.getElementById('body').getBoundingClientRect().width").toInt(), 1060);
    QCOMPARE(run(page, "document.getElementById('sidebar').getBoundingClientRect().width").toInt(), 300);
    QCOMPARE(run(page, "document.getElementById('table').getBoundingClientRect().width").toInt(), 220);
    QCOMPARE(run(page, "document.getElementById('image').getBoundingClientRect().width").toInt(), 120);

    // Body replacement (preview hot swap) keeps the head stylesheet.
    run(page, "document.body.innerHTML=document.body.innerHTML");
    QCOMPARE(run(page, "getComputedStyle(document.getElementById('content')).maxWidth").toString(), QString("none"));
    mrst::applyPreviewCss(&page, true);
    QCOMPARE(run(page, "document.querySelectorAll('#mrr-preview-width').length").toInt(), 1);
    page.setHtml(content);
    QVERIFY(loaded.wait(10000));
    QCOMPARE(run(page, "getComputedStyle(document.getElementById('doc')).maxWidth").toString(), QString("none"));

    // Disabling restores the theme exactly; Markdown stays untouched even when enabled.
    settings.setValue(key, false);
    mrst::applyPreviewCss(&page, true);
    QCOMPARE(run(page, dimensions).toString(), original);
    settings.setValue(key, true);
    mrst::applyPreviewCss(&page, false);
    QCOMPARE(run(page, dimensions).toString(), original);
    page.setHtml(content);
    QVERIFY(loaded.wait(10000));
    QCOMPARE(run(page, dimensions).toString(), original);
}

void TestPreviewFonts::newDocumentLineEndings()
{
    AppSettings settings;
    const QString key = QStringLiteral( "textView/defaultLineEnding" );
    const QVariant previous = settings.value( key );
    const auto restore = qScopeGuard( [&] {
        if( previous.isValid() ) settings.setValue( key, previous );
        else settings.remove( key );
    } );
    settings.remove( key );
    QCOMPARE( mrst::detectDocumentLineEnding( {} ), ScintillaDocument::CRLF );
    QCOMPARE( mrst::detectDocumentLineEnding( "one line" ), ScintillaDocument::CRLF );
    settings.setValue( key, "LF" );
    QCOMPARE( mrst::defaultDocumentLineEnding(), ScintillaDocument::LF );
    QCOMPARE( mrst::detectDocumentLineEnding( "a\r\nb" ), ScintillaDocument::CRLF );
    settings.setValue( key, "CR" );
    QCOMPARE( mrst::defaultDocumentLineEnding(), ScintillaDocument::CR );
    QCOMPARE( mrst::detectDocumentLineEnding( "a\nb" ), ScintillaDocument::LF );
    settings.setValue( key, "invalid" );
    QCOMPARE( mrst::defaultDocumentLineEnding(), ScintillaDocument::CRLF );
}

MRST_REGISTER_TEST( TestPreviewFonts );
#include "tst_PreviewFonts.moc"
