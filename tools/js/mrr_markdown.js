// Markdown 프리뷰 셸의 렌더러.
//
// C++ 은 QWebChannel 로 **원문**을 밀어 넣고(HTML 이 아니다), 이 파일이 그것을
// 그린다. C++ 쪽에 markdown 렌더러를 두지 않는 이유는 token.map 을 얻을 방법이
// 없고, 두면 같은 문법을 두 벌 유지하게 되기 때문이다.
//
// mrr_preview.js 와의 관계: 그 파일은 markdown 을 모른 채 window.__mrrMarkdownRender
// 에 위임만 한다. sphinxcontrib-mermaid 가 남겨 둔 window.runMermaid 를 그 파일이
// 부르는 것과 같은 모양이다. 그래야 그 파일이 Sphinx 페이지에도 주입된다는 사실과
// 양립한다(셸이 아니면 이 전역이 없어 무해히 지나간다).

(function () {
    "use strict";

    if (window.__mrrMarkdownInstalled) {
        return;
    }
    window.__mrrMarkdownInstalled = true;

    /// 렌더러가 준비되기 전에 들어온 마지막 요청. 하나만 들고 있는다 —
    /// PreviewBridge::requestScrollToLine 의 hasPendingScroll_ 과 같은 관용구다.
    ///
    /// 이 큐가 없으면 경합이 생긴다. mrr_preview.js 는 DocumentReady 에 주입되어
    /// 곧바로 bridge.ready() 를 보내는데, 그 시점에 코어 로드가 끝나 있다고
    /// 보장할 수 없다(원격 우선일 때는 특히).
    var queued = null;
    var parser = null;

    /// `<base>` 를 문서 폴더로 세운다. 상대 경로 이미지가 살아나는 근거이고,
    /// mrr_preview.js 의 핫스왑이 쓰던 것과 같은 요소를 재사용한다.
    function setDocumentBase(baseUrl) {
        if (!baseUrl) {
            return;
        }
        var base = document.querySelector("base[data-mrr-preview-base]");
        if (!base) {
            base = document.createElement("base");
            base.setAttribute("data-mrr-preview-base", "");
            document.head.insertBefore(base, document.head.firstChild);
        }
        base.setAttribute("href", baseUrl);
    }

    function ensureParser() {
        if (parser) {
            return parser;
        }
        if (typeof window.markdownit !== "function") {
            return null;
        }
        parser = window.markdownit({
            html: true,
            // GFM 의 자동 링크. linkify-it 은 markdown-it 코어에 이미 들어 있다.
            linkify: true,
            typographer: false
        });
        if (typeof window.markdownitFootnote === "function") {
            parser.use(window.markdownitFootnote);
        }
        return parser;
    }

    function rootElement() {
        var root = document.getElementById("mrr-md-root");
        if (!root) {
            root = document.createElement("div");
            root.id = "mrr-md-root";
            document.body.appendChild(root);
        }
        return root;
    }

    /// 원문을 그린다.
    ///
    /// 지금은 자리만 잡는다 — 줄마다 data-mrr-* 를 붙인 <pre> 를 만들어 스크롤
    /// 동기화 배관이 실제로 도는지 확인할 수 있게 한다. markdown-it 으로 그리는
    /// 것과 줄 매핑은 다음 커밋에서 붙인다.
    function renderStub(root, text) {
        var lines = String(text).split("\n");
        var pre = document.createElement("pre");
        for (var i = 0; i < lines.length; i += 1) {
            var span = document.createElement("div");
            span.setAttribute("data-mrr-start-line", String(i + 1));
            span.setAttribute("data-mrr-end-line", String(i + 1));
            span.setAttribute("data-mrr-src", "0");
            span.textContent = lines[i] === "" ? " " : lines[i];
            pre.appendChild(span);
        }
        root.textContent = "";
        root.appendChild(pre);
    }

    function render(text, baseUrl) {
        setDocumentBase(baseUrl);
        renderStub(rootElement(), text);
        return Promise.resolve();
    }

    // 이 함수는 **동기적으로** 정의되어야 한다. 위 queued 주석 참고.
    window.__mrrMarkdownRender = function (text, baseUrl, optionsJson) {
        if (!ensureParser()) {
            // 코어가 아직 없다. 마지막 요청만 남기고 준비되면 흘려보낸다.
            queued = { text: text, baseUrl: baseUrl, optionsJson: optionsJson };
            return Promise.reject(new Error("renderer not ready"));
        }
        return render(text, baseUrl, optionsJson);
    };

    /// C++ 에 렌더러 상태를 알린다. 사실만 올리고 문장은 C++ 이 만든다 —
    /// JS 에는 tr() 이 없어서 여기서 만든 문자열은 번역할 수 없다.
    function reportReady(origin) {
        if (!window.__mrrBridge || typeof window.__mrrBridge.markdownRendererReady !== "function") {
            return;
        }
        window.__mrrBridge.markdownRendererReady(origin, ensureParser() ? "markdown-it" : "");
    }

    function flushQueued() {
        if (!queued || !ensureParser()) {
            return;
        }
        var request = queued;
        queued = null;
        render(request.text, request.baseUrl, request.optionsJson);
    }

    function boot() {
        ensureParser();
        reportReady("bundled");
        flushQueued();
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", boot);
    } else {
        boot();
    }
}());
