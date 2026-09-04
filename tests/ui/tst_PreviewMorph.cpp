#include "TestRunner.hpp"

#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QWebEnginePage>

using namespace mrst;

namespace {

/// 검사에 쓰는 표본 문서.
///
/// 프리뷰가 실제로 다루는 것들을 한 벌씩 담는다 — 줄 번호 속성, 이미지, 코드
/// 블록, 표. 이미지와 표는 "노드가 살아남는가" 를 보는 자리다. 그것이 죽으면
/// 브라우저가 자리를 다시 잡으면서 화면이 출렁인다.
const char* const kSample =
    "<section id=\"s1\" data-mrr-start-line=\"1\">"
    "<h1 data-mrr-start-line=\"1\">Title</h1>"
    "<p data-mrr-start-line=\"3\">alpha beta</p>"
    "<img src=\"a.png\" alt=\"a\">"
    "<pre data-mrr-start-line=\"5\"><code>x = 1</code></pre>"
    "<table><tbody><tr><td>c1</td><td>c2</td></tr></tbody></table>"
    "</section>";

/// 페이지에 심어 두는 도우미. 각 검사가 이것을 통해 morph 를 부른다.
const char* const kPreamble = R"JS(
window.__t = (function () {
    var stage = document.getElementById("stage");
    var hooks = window.__mrrTestHooks;
    function parseBody(html) {
        return new DOMParser().parseFromString("<body>" + html + "</body>", "text/html").body;
    }
    return {
        stage: stage,
        hooks: hooks,
        parseBody: parseBody,
        setBase: function (html) { stage.innerHTML = ""; return hooks.morph(stage, parseBody(html)); },
        morphTo: function (html) { return hooks.morph(stage, parseBody(html)); },
        // 같은 목표 HTML 을 옛 경로로 넣었을 때의 결과.
        viaInnerHtml: function (html) { stage.innerHTML = parseBody(html).innerHTML; return stage.innerHTML; },
        shiftLines: function (html, by) {
            return html.replace(/(data-mrr-(?:start|end)-line)="(\d+)"/g,
                function (m, a, n) { return a + '="' + (Number(n) + by) + '"'; });
        }
    };
})();
true;
)JS";

/// kSample 을 JS 문자열 리터럴로 만든다. 검사 스크립트가 BASE 로 받는다.
QString sampleLiteral()
{
    QString escaped = QString::fromLatin1( kSample );
    escaped.replace( QLatin1Char( '"' ), QLatin1String( "\\\"" ) );
    return QLatin1Char( '"' ) + escaped + QLatin1Char( '"' );
}

}   // namespace

/// morph 가 innerHTML 교체와 **같은 DOM 에 도달하는가**, 그리고 도달하는 동안
/// 노드를 살려 두는가.
///
/// 이 둘이 이 기능의 전부다. 같은 DOM 에 도달하기 때문에 실패했을 때 옛 경로로
/// 되돌아가도 되고, 노드가 살아남기 때문에 편집 중 화면이 출렁이지 않는다.
/// 눈으로는 확인하기 어려운 성질이라 여기서 기계로 잡는다.
class TestPreviewMorph : public QObject
{
    Q_OBJECT

private slots:
    void                    initTestCase();
    void                    equivalence_data();
    void                    equivalence();
    void                    nodesSurvive();
    void                    lineNumbersUpdate();
    void                    scriptsNeverRun();
    void                    lookaheadKeepsNeighbours();
    void                    keepStampPreservesSubtree();
    void                    keepStampReleasesOnChange();
    void                    changeCounts();
    void                    diagramsRedrawOnlyWhatIsNew();
    void                    diagramsSkippedWhenNothingIsNew();
    void                    diagramClassRestoredWhenRendererThrows();

private:
    QVariant                run( const QString& script );

    QWebEnginePage*         page_ = nullptr;
};

QVariant TestPreviewMorph::run( const QString& script )
{
    QVariant   result;
    QEventLoop loop;
    page_->runJavaScript( script, [ &result, &loop ]( const QVariant& value ) {
        result = value;
        loop.quit();
    } );
    loop.exec();
    return result;
}

void TestPreviewMorph::initTestCase()
{
    page_ = new QWebEnginePage( this );

    QSignalSpy loaded( page_, &QWebEnginePage::loadFinished );
    page_->setHtml( QStringLiteral( "<html><body><div id=\"stage\"></div></body></html>" ) );
    // setHtml() 이 아주 빨리 끝나면 wait() 호출 전에 loadFinished 가 올 수 있다.
    // 이미 받은 신호를 놓치고 다음 신호를 기다리는 경합을 피하면서, 단순히
    // 신호가 왔는지만 보던 기존 검사보다 실제 로드 성공 값까지 확인한다.
    QVERIFY2( !loaded.isEmpty() || loaded.wait( 15000 ), "페이지가 로드되지 않았다" );
    QVERIFY2( loaded.constLast().constFirst().toBool(), "페이지 로드가 실패했다" );

    // 앱이 하는 것과 같다 — 파일을 읽어 스크립트로 주입한다
    // (PreviewBridge::attachTo). QWebChannel 이 없으므로 boot() 는 조용히 물러나고
    // morph 만 window 에 남는다.
    QFile source( QStringLiteral( MRST_PREVIEW_JS ) );
    QVERIFY2( source.open( QIODevice::ReadOnly ), MRST_PREVIEW_JS );
    run( QString::fromUtf8( source.readAll() ) );

    QVERIFY2( run( QStringLiteral( "typeof window.__mrrTestHooks.morph" ) ).toString()
                  == QLatin1String( "function" ),
              "mrr_preview.js 가 morph 를 열지 않았다" );

    run( QString::fromLatin1( kPreamble ) );
}

void TestPreviewMorph::equivalence_data()
{
    QTest::addColumn< QString >( "targetExpression" );

    // 표본에서 목표 HTML 을 만드는 JS 식. BASE 는 아래 equivalence() 가 넣는다.
    // 행 이름은 ASCII 로 둔다 — QTest 로거가 로컬 코드페이지로 찍어 한글이 깨진다.
    QTest::newRow( "one-char" ) << QStringLiteral( "BASE.replace('alpha', 'alpba')" );
    QTest::newRow( "line-shift" ) << QStringLiteral( "window.__t.shiftLines(BASE, 7)" );
    QTest::newRow( "block-insert" )
        << QStringLiteral( "BASE.replace('<img', '<p data-mrr-start-line=\"4\">new</p><img')" );
    QTest::newRow( "block-delete" ) << QStringLiteral( "BASE.replace('<img src=\"a.png\" alt=\"a\">', '')" );
    QTest::newRow( "attr-removed" ) << QStringLiteral( "BASE.replace(' alt=\"a\"', '')" );
    QTest::newRow( "append-tail" ) << QStringLiteral( "BASE + '<p data-mrr-start-line=\"9\">tail</p>'" );
    QTest::newRow( "full-replace" ) << QStringLiteral( "'<div data-mrr-start-line=\"1\">other</div>'" );
}

void TestPreviewMorph::equivalence()
{
    QFETCH( QString, targetExpression );

    const QString body = QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            var target = %2;
            window.__t.setBase(BASE);
            window.__t.morphTo(target);
            var viaMorph = window.__t.stage.innerHTML;
            return viaMorph === window.__t.viaInnerHtml(target) ? "" : viaMorph;
        })()
    )JS" )
                             .arg( sampleLiteral(), targetExpression );

    const QString mismatch = run( body ).toString();
    QVERIFY2( mismatch.isEmpty(), qPrintable( QStringLiteral( "morph 결과가 다르다:\n" ) + mismatch ) );
}

void TestPreviewMorph::nodesSurvive()
{
    const bool ok = run( QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            window.__t.setBase(BASE);
            var img = window.__t.stage.querySelector("img");
            var pre = window.__t.stage.querySelector("pre");
            var table = window.__t.stage.querySelector("table");
            window.__t.morphTo(BASE.replace('alpha', 'alpba'));
            return window.__t.stage.querySelector("img") === img
                && window.__t.stage.querySelector("pre") === pre
                && window.__t.stage.querySelector("table") === table;
        })()
    )JS" )
                             .arg( sampleLiteral() ) )
                        .toBool();
    QVERIFY2( ok, "한 글자를 고쳤는데 이미지/코드블록/표 노드가 새로 만들어졌다" );
}

void TestPreviewMorph::lineNumbersUpdate()
{
    // 줄 하나를 넣으면 아래 블록의 줄 번호가 전부 밀린다. 그때도 노드는 살고
    // 속성만 바뀌어야 한다 — 이것이 편집 중 가장 흔한 경우다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            window.__t.setBase(BASE);
            var img = window.__t.stage.querySelector("img");
            window.__t.morphTo(window.__t.shiftLines(BASE, 7));
            return [window.__t.stage.querySelector("img") === img,
                    window.__t.stage.querySelector("p").getAttribute("data-mrr-start-line")];
        })()
    )JS" )
                                      .arg( sampleLiteral() ) )
                                 .toList();
    QCOMPARE( out.value( 0 ).toBool(), true );
    QCOMPARE( out.value( 1 ).toString(), QStringLiteral( "10" ) );
}

void TestPreviewMorph::scriptsNeverRun()
{
    // innerHTML 대입은 스크립트를 실행하지 않는다(HTML 명세). morph 가
    // importNode 로 넣으면 실행된다 — 그러면 본문 스크립트가 편집할 때마다 다시
    // 돈다. 순회에서 제외하는 이유가 이것이다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            window.__t.setBase(BASE);
            window.__ran = undefined;
            window.__t.morphTo(BASE + '<scr' + 'ipt>window.__ran = 1;</scr' + 'ipt>');
            return [window.__ran === undefined,
                    window.__t.stage.querySelector("script") === null];
        })()
    )JS" )
                                      .arg( sampleLiteral() ) )
                                 .toList();
    QVERIFY2( out.value( 0 ).toBool(), "morph 가 넣은 스크립트가 실행됐다" );
    QVERIFY2( out.value( 1 ).toBool(), "morph 가 스크립트를 DOM 에 넣었다" );
}

void TestPreviewMorph::lookaheadKeepsNeighbours()
{
    // 문단 하나가 새로 생기면 그 아래 형제가 한 칸씩 밀린다. 앞보기가 없으면
    // 밀린 자리에서 <p> 와 <img> 가 짝지어져 이미지가 통째로 교체된다.
    const bool ok = run( QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            window.__t.setBase(BASE);
            var img = window.__t.stage.querySelector("img");
            window.__t.morphTo(BASE.replace('<img', '<p data-mrr-start-line="4">new</p><img'));
            return window.__t.stage.querySelector("img") === img;
        })()
    )JS" )
                             .arg( sampleLiteral() ) )
                        .toBool();
    QVERIFY2( ok, "문단을 삽입했더니 아래 이미지가 새로 만들어졌다" );
}

void TestPreviewMorph::keepStampPreservesSubtree()
{
    // 렌더러가 서브트리를 변형한 뒤(mermaid 가 SVG 로 바꾸는 것) 남긴 지문이
    // 맞으면 morph 가 그 안을 건드리지 않아야 한다. 그래야 타이핑 중에 도형이
    // 원문으로 되돌아갔다가 다시 나타나지 않는다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            window.__t.setBase('<p data-mrr-start-line="1">before</p>'
                + '<pre class="mermaid" data-mrr-start-line="3">graph TD; A--&gt;B;</pre>');
            var diagram = window.__t.stage.querySelector("pre.mermaid");
            var source = diagram.innerHTML;
            diagram.innerHTML = "<svg id='drawn'></svg>";
            diagram.setAttribute("data-processed", "true");
            window.__t.hooks.stampKeep(diagram, source);

            window.__t.morphTo('<p data-mrr-start-line="2">before</p>'
                + '<pre class="mermaid" data-mrr-start-line="4">graph TD; A--&gt;B;</pre>');
            return [window.__t.stage.querySelector("pre.mermaid") === diagram,
                    window.__t.stage.querySelector("#drawn") !== null,
                    diagram.getAttribute("data-processed") === "true",
                    diagram.getAttribute("data-mrr-start-line")];
        })()
    )JS" ) )
                                 .toList();
    QVERIFY2( out.value( 0 ).toBool(), "보존 대상 노드가 교체됐다" );
    QVERIFY2( out.value( 1 ).toBool(), "그려 둔 SVG 가 사라졌다" );
    QVERIFY2( out.value( 2 ).toBool(), "렌더러가 붙인 표식이 지워졌다" );
    // 보존해도 줄 번호는 따라가야 한다. 스크롤 동기화가 이 값을 읽는다.
    QCOMPARE( out.value( 3 ).toString(), QStringLiteral( "4" ) );
}

void TestPreviewMorph::keepStampReleasesOnChange()
{
    // 원문이 바뀌면 보존을 풀고 원문으로 되돌려야 한다. 표식이 남으면 바뀐
    // 도형이 영영 다시 그려지지 않는다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            window.__t.setBase('<pre class="mermaid" data-mrr-start-line="3">graph TD; A--&gt;B;</pre>');
            var diagram = window.__t.stage.querySelector("pre.mermaid");
            window.__t.hooks.stampKeep(diagram, diagram.innerHTML);
            diagram.innerHTML = "<svg id='drawn'></svg>";
            diagram.setAttribute("data-processed", "true");

            window.__t.morphTo('<pre class="mermaid" data-mrr-start-line="3">graph TD; A--&gt;C;</pre>');
            var now = window.__t.stage.querySelector("pre.mermaid");
            return [window.__t.stage.querySelector("#drawn") === null,
                    now.getAttribute("data-mrr-keep") === null,
                    now.getAttribute("data-processed") === null,
                    now.textContent.indexOf("C") >= 0];
        })()
    )JS" ) )
                                 .toList();
    QVERIFY2( out.value( 0 ).toBool(), "원문이 바뀌었는데 옛 그림이 남았다" );
    QVERIFY2( out.value( 1 ).toBool(), "낡은 지문이 남았다" );
    QVERIFY2( out.value( 2 ).toBool(), "렌더러 표식이 남아 다시 그리지 않게 된다" );
    QVERIFY2( out.value( 3 ).toBool(), "새 원문이 들어오지 않았다" );
}

void TestPreviewMorph::changeCounts()
{
    // changed 가 0 이면 화면이 움직이지 않았다는 뜻이고, 호출자가 좌표 캐시를
    // 버리거나 다이어그램을 다시 그릴 이유도 없다. touched 는 후처리 범위다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            var BASE = %1;
            window.__t.setBase(BASE);
            var same = window.__t.morphTo(BASE);
            window.__t.setBase(BASE);
            var one = window.__t.morphTo(BASE.replace('alpha', 'alpba'));
            return [same.changed, same.touched.length,
                    one.changed, one.touched.length,
                    one.touched.length ? one.touched[0].tagName : ""];
        })()
    )JS" )
                                      .arg( sampleLiteral() ) )
                                 .toList();
    QCOMPARE( out.value( 0 ).toInt(), 0 );
    QCOMPARE( out.value( 1 ).toInt(), 0 );
    QCOMPARE( out.value( 2 ).toInt(), 1 );
    QCOMPARE( out.value( 3 ).toInt(), 1 );
    QCOMPARE( out.value( 4 ).toString(), QStringLiteral( "P" ) );
}

namespace {

/// sphinxcontrib-mermaid 가 window 에 남기는 재실행 훅의 가짜.
///
/// 그쪽 계약을 그대로 흉내낸다 — `.mermaid` 를 **전부** 훑어 원문을
/// `data-original-code` 로 떠 두고 이미 그린 것은 되돌린 뒤 다시 그린다. 그리고
/// **목록을 확정하는 데까지가 동기 구간**이다. 우리 쪽이 기대는 성질이 바로
/// 그것이므로(mrr_preview.js 의 `rerenderDiagrams` 주석) 여기서 붙들어 둔다.
const char* const kFakeRunMermaid = R"JS(
window.__seen = [];
window.__calls = 0;
window.__throwNext = false;
window.runMermaid = function (rerun) {
    window.__calls += 1;
    if (window.__throwNext) { throw new Error("renderer blew up"); }
    var all = document.querySelectorAll(".mermaid");
    var batch = [];
    for (var i = 0; i < all.length; i += 1) {
        var el = all[i];
        batch.push(el.getAttribute("data-name"));
        if (el.getAttribute("data-processed") === "true") {
            el.removeAttribute("data-processed");
            el.innerHTML = el.getAttribute("data-original-code");
        } else {
            el.setAttribute("data-original-code", el.innerHTML);
        }
    }
    window.__seen = batch;
    return Promise.resolve().then(function () {
        for (var j = 0; j < batch.length; j += 1) {
            var node = document.querySelector('.mermaid[data-name="' + batch[j] + '"]');
            node.setAttribute("data-processed", "true");
            node.innerHTML = "<svg class='drawn'></svg>";
        }
    });
};
true;
)JS";

/// 그려 둔 도형 하나(A)와 새로 들어온 도형 하나(B).
const char* const kTwoDiagrams = R"JS(
(function () {
    var stage = window.__t.stage;
    window.__t.setBase(
        '<p data-mrr-start-line="1">prose</p>'
        + '<pre class="mermaid" data-name="A" data-mrr-start-line="3">graph TD; A--&gt;B;</pre>'
        + '<pre class="mermaid" data-name="B" data-mrr-start-line="7">graph TD; C--&gt;D;</pre>');
    // A 는 이미 그려졌다고 둔다: 렌더러가 남기는 것 + 우리가 남기는 지문.
    var a = stage.querySelector('[data-name="A"]');
    var source = a.innerHTML;
    a.setAttribute("data-original-code", source);
    a.setAttribute("data-processed", "true");
    a.innerHTML = "<svg class='drawn' id='oldA'></svg>";
    window.__t.hooks.stampKeep(a, source);
    return true;
})()
)JS";

}   // namespace

void TestPreviewMorph::diagramsRedrawOnlyWhatIsNew()
{
    // 산문만 고쳤을 때 문서의 모든 도형이 원문으로 되돌아갔다가 다시 그려지던
    // 것을 막는다. 렌더러는 전부 아니면 전무이므로, 이미 그린 것은 클래스를 떼어
    // 그쪽 시야에서 잠깐 숨긴다.
    run( QString::fromLatin1( kFakeRunMermaid ) );
    run( QString::fromLatin1( kTwoDiagrams ) );
    run( QStringLiteral( "window.__mrrTestHooks.rerenderDiagrams(); true;" ) );

    // 두 번째 왕복이면 마이크로태스크가 이미 비었다.
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            var stage = window.__t.stage;
            var a = stage.querySelector('[data-name="A"]');
            var b = stage.querySelector('[data-name="B"]');
            return [window.__seen.join(","),
                    a.classList.contains("mermaid"),
                    a.querySelector("#oldA") !== null,
                    a.getAttribute("data-mrr-keep") !== null,
                    b.getAttribute("data-processed") === "true",
                    b.getAttribute("data-mrr-keep") !== null];
        })()
    )JS" ) )
                                 .toList();

    QCOMPARE( out.value( 0 ).toString(), QStringLiteral( "B" ) );
    QVERIFY2( out.value( 1 ).toBool(), "숨겼던 도형에 mermaid 클래스가 돌아오지 않았다" );
    QVERIFY2( out.value( 2 ).toBool(), "그려 둔 SVG 가 버려졌다" );
    QVERIFY2( out.value( 3 ).toBool(), "지문이 사라졌다" );
    QVERIFY2( out.value( 4 ).toBool(), "새 도형이 그려지지 않았다" );
    QVERIFY2( out.value( 5 ).toBool(), "새 도형에 지문을 남기지 않았다 — 다음 갱신에서 또 그린다" );
}

void TestPreviewMorph::diagramsSkippedWhenNothingIsNew()
{
    // 그릴 것이 없으면 렌더러를 아예 부르지 않는다. 도형이 있는 문서에서 산문을
    // 타이핑하는 동안 도형이 한 번도 흔들리지 않는 것이 이 한 줄에 달려 있다.
    run( QString::fromLatin1( kFakeRunMermaid ) );
    run( QString::fromLatin1( kTwoDiagrams ) );
    // B 도 그려진 것으로 만든다
    run( QStringLiteral( R"JS(
        (function () {
            var b = window.__t.stage.querySelector('[data-name="B"]');
            var source = b.innerHTML;
            b.setAttribute("data-original-code", source);
            b.setAttribute("data-processed", "true");
            b.innerHTML = "<svg class='drawn'></svg>";
            window.__t.hooks.stampKeep(b, source);
            window.__calls = 0;
            return true;
        })()
    )JS" ) );

    run( QStringLiteral( "window.__mrrTestHooks.rerenderDiagrams(); true;" ) );
    QCOMPARE( run( QStringLiteral( "window.__calls" ) ).toInt(), 0 );
}

void TestPreviewMorph::diagramClassRestoredWhenRendererThrows()
{
    // 클래스를 뗀 채로 예외가 빠져나가면 도형이 테마 CSS 까지 잃는다.
    run( QString::fromLatin1( kFakeRunMermaid ) );
    run( QString::fromLatin1( kTwoDiagrams ) );
    const QVariantList out = run( QStringLiteral( R"JS(
        (function () {
            window.__throwNext = true;
            window.__mrrTestHooks.rerenderDiagrams();
            window.__throwNext = false;
            var stage = window.__t.stage;
            return [stage.querySelector('[data-name="A"]').classList.contains("mermaid"),
                    stage.querySelector('[data-name="B"]').classList.contains("mermaid")];
        })()
    )JS" ) )
                                 .toList();
    QVERIFY2( out.value( 0 ).toBool(), "숨겼던 도형이 클래스를 잃었다" );
    QVERIFY2( out.value( 1 ).toBool(), "숨기지 않은 도형이 클래스를 잃었다" );
}

MRST_REGISTER_TEST( TestPreviewMorph );

#include "tst_PreviewMorph.moc"
