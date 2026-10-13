#include "MaskPaint.hpp"
#include <ScintillaEditBase.h>
#include <Scintilla.h>

#include <QApplication>
#include "PlatQtMetrics.hpp"
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRawFont>
#include <QStyleHints>
#include <QTextLayout>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QtGui/private/qrawfont_p.h>
#include <algorithm>
#include <cstdio>
#include <memory>

namespace {
void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
QString imageHash(const QImage& source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    return QString::fromLatin1(QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
        QCryptographicHash::Sha256).toHex());
}
QVariant js(QWebEnginePage& page, const QString& script)
{
    QEventLoop loop;
    auto result = std::make_shared<QVariant>();
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    page.runJavaScript(script, [result, guard = QPointer<QEventLoop>(&loop)](const QVariant& value) {
        *result = value;
        if (guard) guard->quit();
    });
    loop.exec();
    return *result;
}
QJsonArray glyphEngines(const QFont& font)
{
    QTextLayout layout(QString::fromUtf8("English 한글 🙂 ffi العربية"), font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    QJsonArray result;
    for (const QGlyphRun& run : layout.glyphRuns()) {
        const QRawFont raw = run.rawFont();
        const auto* engine = QRawFontPrivate::get(raw)->fontEngine;
        QString type = "other";
        if (engine && engine->type() == QFontEngine::DirectWrite) type = "DirectWrite";
        else if (engine && engine->type() == QFontEngine::Freetype) type = "FreeType";
        else if (engine && engine->type() == QFontEngine::Win) type = "GDI";
        bool missing = false;
        for (quint32 glyph : run.glyphIndexes()) missing |= glyph == 0;
        result.append(QJsonObject{{"family", raw.familyName()}, {"type", type}, {"missingGlyph", missing}});
    }
    return result;
}
QJsonObject comparePngs(const QString& first, const QString& second, const QString& region)
{
    QImage a(first), b(second);
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return {{"error", "missing or differently sized images"}};
    if (!region.isEmpty()) {
        const QStringList values = region.split(QLatin1Char(','));
        if (values.size() != 4) return {{"error", "invalid crop"}};
        const QRect crop(values[0].toInt(), values[1].toInt(), values[2].toInt(), values[3].toInt());
        a = a.copy(crop);
        b = b.copy(crop);
    }
    a = a.convertToFormat(QImage::Format_ARGB32);
    b = b.convertToFormat(QImage::Format_ARGB32);
    quint64 changed = 0;
    quint64 firstColored = 0, secondColored = 0;
    QRect bounds;
    for (int y = 0; y < a.height(); ++y) {
        const auto* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            firstColored += qRed(pa[x]) != qGreen(pa[x]) || qGreen(pa[x]) != qBlue(pa[x]);
            secondColored += qRed(pb[x]) != qGreen(pb[x]) || qGreen(pb[x]) != qBlue(pb[x]);
            if (pa[x] == pb[x]) continue;
            ++changed;
            bounds |= QRect(x, y, 1, 1);
        }
    }
    return {{"coloredPixelsFirst", double(firstColored)}, {"coloredPixelsSecond", double(secondColored)},
            {"changedPixels", double(changed)}, {"totalPixels", double(a.width()) * a.height()},
            {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}}};
}

QJsonObject editorProbe(const QString& output, int quality, bool dark, bool uniqueLines)
{
    ScintillaEditBase editor;
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(960, 360);
    editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    editor.send(SCI_SETBUFFEREDDRAW, 0);
    editor.send(SCI_SETPHASESDRAW, SC_PHASES_MULTIPLE);
    editor.send(SCI_SETWRAPMODE, SC_WRAP_CHAR);
    for (int margin = 0; margin < 5; ++margin) editor.send(SCI_SETMARGINWIDTHN, margin, 0);
    editor.sends(SCI_STYLESETFONT, STYLE_DEFAULT, "Consolas");
    editor.send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, 1200);
    editor.send(SCI_STYLESETFORE, STYLE_DEFAULT, dark ? 0xe0e0e0 : 0x202020);
    editor.send(SCI_STYLESETBACK, STYLE_DEFAULT, dark ? 0x202020 : 0xffffff);
    editor.send(SCI_STYLECLEARALL);
    editor.send(SCI_SETFONTQUALITY, quality);
    editor.send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    const QString paragraph = QString::fromUtf8(
        "Markdown / reST: Hello World 0123456789 ffi != ->\n"
        "한글 가독성 검사: 가나다라 마바사 아자차카 타파하\n"
        "Color emoji: 🙂 🎉 👩‍💻   Combining: é   Arabic: العربية\n"
        "**강조** :ref:\x60reference\x60   \"quoted\"   code() { return 42; }\n");
    QString text;
    for (int i = 0; i < 24; ++i) {
        if (uniqueLines) {
            for (const QString& line : paragraph.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                text += QString::number(i) + QLatin1Char(' ') + line + QLatin1Char('\n');
        } else text += paragraph;
    }
    const QByteArray utf8 = text.toUtf8();
    editor.sends(SCI_SETTEXT, 0, utf8.constData());
    editor.show();
    settle(150);
    mrst::probe::maskCache().clear();
    QElapsedTimer coldPaint;
    coldPaint.start();
    const QImage image = editor.grab().toImage();
    const double coldPaintMs = coldPaint.nsecsElapsed() / 1.0e6;
    image.save(QDir(output).filePath("editor.png"));
    QJsonArray caret;
    int hitFailures = 0;
    QJsonArray hitDetails;
    for (int line = 0; line < 2; ++line) {
        const sptr_t begin = editor.send(SCI_POSITIONFROMLINE, line);
        const sptr_t end = editor.send(SCI_GETLINEENDPOSITION, line);
        for (sptr_t pos = begin; pos <= end; pos = editor.send(SCI_POSITIONAFTER, pos)) {
            const int x = int(editor.send(SCI_POINTXFROMPOSITION, 0, pos));
            const int y = int(editor.send(SCI_POINTYFROMPOSITION, 0, pos));
            caret.append(QJsonArray{double(pos), x, y});
            // Close hit testing excludes the zero-width end-of-line caret.
            const sptr_t hit = editor.send(pos == end ? SCI_POSITIONFROMPOINT : SCI_POSITIONFROMPOINTCLOSE, x, y + 2);
            if (hit != pos) {
                ++hitFailures;
                hitDetails.append(QJsonObject{{"expected", double(pos)}, {"actual", double(hit)},
                    {"x", x}, {"rightPixel", double(editor.send(SCI_POSITIONFROMPOINTCLOSE, x + 1, y + 2))}});
            }
            if (pos == end) break;
        }
    }
    QList<double> durations;
    for (int i = 0; i < 40; ++i) {
        QElapsedTimer timer;
        timer.start();
        editor.grab();
        durations.append(timer.nsecsElapsed() / 1.0e6);
    }
    std::sort(durations.begin(), durations.end());
    QFont font("Consolas");
    font.setPointSizeF(12);
    return {{"imageHash", imageHash(image)}, {"dpr", editor.devicePixelRatioF()},
        {"caret", caret}, {"hitFailures", hitFailures}, {"hitDetails", hitDetails},
        {"displayLines", double(editor.send(SCI_VISIBLEFROMDOCLINE, 80))},
        {"coldMaskCachePaintMs", coldPaintMs}, {"uniqueLines", uniqueLines},
        {"paintMedianMs", durations.at(20)}, {"paintP95Ms", durations.at(38)},
        {"engines", glyphEngines(font)}, {"maskCalls", double(mrst::probe::maskCalls)},
        {"cacheHits", double(mrst::probe::cacheHits)},
        {"pixelsProcessed", double(mrst::probe::pixelsProcessed)}};
}
QJsonObject previewProbe(const QString& output, const QString& mode, bool dark, const QString& artifact)
{
    QWebEngineView view;
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(artifact.isEmpty() ? 960 : 1280, artifact.isEmpty() ? 360 : 900);
    QString css;
    if (mode == "stroke") css = ".sample { -webkit-text-stroke: .15px currentColor; }";
    else if (mode == "weight") css = ".sample { font-weight: 500; }";
    else if (mode == "weight-bold") css = ".sample { font-weight: 700; }";
    else if (mode == "smoothing") css = ".sample { -webkit-font-smoothing: antialiased; }";
    QString html = QString::fromUtf8(R"HTML(<!doctype html><meta charset="utf-8">
<style>body {margin:20px;background:%1;color:%2;} .sample {font:16px/1.8 Consolas, 'Malgun Gothic', monospace;}
code {font:16px Consolas,monospace;} %3</style>
<div id="content"><div class="sample" id="latin">Markdown / reST: Hello World 0123456789 ffi != -&gt;</div>
<div class="sample" id="korean">한글 가독성 검사: 가나다라 마바사 아자차카 타파하</div>
<div class="sample" id="emoji">Color emoji: 🙂 🎉 👩‍💻 Combining: é Arabic: العربية</div>
<div class="sample" id="code">**강조** :ref: reference &quot;quoted&quot; code() { return 42; }</div>
<div class="sample" id="wrapped" style="width:350px">한글과 English가 섞인 문서에서 줄바꿈 위치와 선택 범위를 검사한다. Selection and layout must remain stable.</div>
<img id="image" width="48" height="24" src="__IMAGE__">
<svg width="80" height="24"><rect width="80" height="24" fill="#0088cc"/></svg></div>
)HTML").arg(dark ? "#202020" : "#ffffff", dark ? "#e0e0e0" : "#202020", css);
    const QByteArray svg("<svg xmlns='http://www.w3.org/2000/svg' width='48' height='24'><rect width='48' height='24' fill='#ff6600'/></svg>");
    html.replace("__IMAGE__", "data:image/svg+xml;base64," + QString::fromLatin1(svg.toBase64()));
    QEventLoop loop;
    bool loaded = false;
    QObject::connect(&view, &QWebEngineView::loadFinished, &loop, [&](bool ok) { loaded = ok; loop.quit(); });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    view.show();
    if (artifact.isEmpty()) view.setHtml(html);
    else view.load(QUrl::fromLocalFile(QFileInfo(artifact).absoluteFilePath()));
    loop.exec();
    if (!loaded) return {{"error", "load failed / timed out"}};
    settle(500);
    if (!artifact.isEmpty()) {
        const QVariant verified = js(*view.page(), "document.getElementById('choice').options.length > 0 && document.getElementById('base').complete && document.getElementById('candidate').complete");
        view.grab().save(QDir(output).filePath("artifact.png"));
        return {{"loaded", loaded}, {"uiVerified", verified.toBool()}};
    }
    const QString measurements = js(*view.page(), QString::fromUtf8(R"JS(JSON.stringify((() => {
        const ids=['latin','korean','emoji','code','wrapped','image'];
        const boxes=ids.map(id=>{ const e=document.getElementById(id), r=e.getBoundingClientRect();
            const t=document.createRange(); t.selectNodeContents(e);
            return {id,x:r.x,y:r.y,width:r.width,height:r.height,rects:[...t.getClientRects()].map(v=>[v.x,v.y,v.width,v.height])}; });
        return {dpr:devicePixelRatio,boxes,ua:navigator.userAgent,
            imageLoaded:document.getElementById('image').complete && document.getElementById('image').naturalWidth > 0,
            weight:getComputedStyle(document.getElementById('latin')).fontWeight,
            smoothing:getComputedStyle(document.getElementById('latin')).webkitFontSmoothing,
            stroke:getComputedStyle(document.getElementById('latin')).webkitTextStrokeWidth};
    })()))JS")).toString();
    const QImage image = view.grab().toImage();
    image.save(QDir(output).filePath("preview.png"));
    QJsonObject result = QJsonDocument::fromJson(measurements.toUtf8()).object();
    result.insert("imageHash", imageHash(image));
    result.insert("loaded", loaded);
    result.insert("chromiumVersion", qWebEngineChromiumVersion());
    return result;
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCommandLineParser args;
    args.addOption({"artifact", "Local HTML comparison artifact", "file"});
    args.addOption({"first", "First PNG to compare", "file"});
    args.addOption({"second", "Second PNG to compare", "file"});
    args.addOption({"region", "Optional pixel crop x,y,width,height", "rect"});
    args.addOption({"output", "Output directory", "directory"});
    args.addOption({"surface", "editor or preview", "name", "editor"});
    args.addOption({"quality", "Scintilla font quality", "number", "2"});
    args.addOption({"css", "Preview CSS variant", "name", "baseline"});
    args.addHelpOption();
    args.addOption({"dark", "Dark background"});
    args.addOption({"unique-lines", "Use distinct source lines instead of repeated paragraphs"});
    args.addOption({"legacy-measure", "Disable application monospace width cache"});
    args.process(app);
    if (args.isSet("legacy-measure")) mrst::scintilla::setMonospaceFastPathEnabled(false);
    const QString output = args.value("output");
    if (output.isEmpty() || !QDir().mkpath(output)) return 2;
    const QJsonObject result = args.value("surface") == "compare"
        ? comparePngs(args.value("first"), args.value("second"), args.value("region"))
        : args.value("surface") == "preview"
        ? previewProbe(output, args.value("css"), args.isSet("dark"), args.value("artifact"))
        : editorProbe(output, args.value("quality").toInt(), args.isSet("dark"), args.isSet("unique-lines"));
    QJsonObject record = result;
    record.insert("qtVersion", qVersion());
    record.insert("platform", QGuiApplication::platformName());
    record.insert("qtFontGamma", app.styleHints()->fontSmoothingGamma());
    record.insert("chromiumFlags", qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS"));
    record.insert("maskGamma", qEnvironmentVariable("MRST_PROBE_GAMMA"));
    record.insert("maskStrength", qEnvironmentVariable("MRST_PROBE_STRENGTH"));
    QFile file(QDir(output).filePath("metrics.json"));
    if (!file.open(QIODevice::WriteOnly)) return 3;
    file.write(QJsonDocument(record).toJson());
    return record.contains("error") ? 4 : 0;
}
