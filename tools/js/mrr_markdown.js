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

    /// GitHub 경고 상자의 제목. **영문 고정이다.**
    ///
    /// JS 에는 tr() 이 없으므로 여기서 만든 문자열은 번역할 수 없다. GitHub 도 항상
    /// 영문이고 문법 자체가 영문 키워드이므로, 번역 의존을 만들지 않는 이 선택이 맞다.
    var ALERT_TITLES = {
        NOTE: "Note",
        TIP: "Tip",
        IMPORTANT: "Important",
        WARNING: "Warning",
        CAUTION: "Caution"
    };

    /// 수식 구분자. MyST 의 dollarmath 와 맞춘다 — 같은 문서를 나중에 myst 프로젝트에
    /// 넣어도 문법이 통해야 한다. `$$` 가 `$` 보다 **앞**이어야 한다(auto-render 는
    /// 배열 순서로 매칭한다).
    var MATH_DELIMITERS = [
        { left: "$$", right: "$$", display: true },
        { left: "\\[", right: "\\]", display: true },
        { left: "$", right: "$", display: false },
        { left: "\\(", right: "\\)", display: false }
    ];

    var VENDOR_BASE = "qrc:/preview/md/vendor/";

    /// 원격 로드 타임아웃. 사내망의 멈춘 TCP 연결에서는 error 도 load 도 영원히
    /// 오지 않는다. 그 경우를 잡는 유일한 수단이다.
    var CORE_TIMEOUT_MS = 2500;
    /// mermaid 는 3.4MB 라 더 넉넉히 준다.
    var HEAVY_TIMEOUT_MS = 8000;
    /// 다이어그램은 타이핑이 멎은 뒤에 그린다. 도형 하나가 수십~수백 ms 라
    /// 8Hz 로 다시 그리면 프리뷰가 멎는다.
    var DIAGRAM_IDLE_MS = 400;

    var queued = null;
    var parser = null;
    /// 렌더 요청과 함께 오는 옵션(테마·버전·원격 허용). 지연 로드가 이것을 본다.
    var renderOptions = {};
    var shellOptions = parseShellOptions();
    var coreOrigin = "";
    var coreFallbackReason = "";
    var coreReady = false;
    var reportedOrigin = "";
    var mathState = "idle";        // idle | loading | ready | failed
    var mathPromise = null;
    var mermaidState = "idle";
    var mermaidPromise = null;
    var diagramTimer = 0;

    /// 셸 URL 쿼리에서 코어 로드에 필요한 것만 읽는다.
    ///
    /// 렌더 요청의 optionsJson 을 기다릴 수 없다 — 코어 로드는 그보다 먼저 시작해야
    /// 하고, 첫 렌더가 그것을 기다린다. 설정이 바뀌면 C++ 이 셸을 다시 읽으므로
    /// 이 값들도 함께 갱신된다.
    function parseShellOptions() {
        var out = {
            allowRemote: false,
            coreSource: "bundled",
            markdownItVersion: "",
            cdnBase: "",
            coreSri: ""
        };
        var query = String(window.location.search || "").replace(/^\?/, "");
        var parts = query.split("&");
        for (var i = 0; i < parts.length; i += 1) {
            var pair = parts[i].split("=");
            var key = decodeURIComponent(pair[0] || "");
            var value = decodeURIComponent((pair[1] || "").replace(/\+/g, " "));
            if (key === "remote") { out.allowRemote = value === "1"; }
            else if (key === "core") { out.coreSource = value; }
            else if (key === "mi") { out.markdownItVersion = value; }
            else if (key === "cdn") { out.cdnBase = value; }
            else if (key === "sri") { out.coreSri = value; }
        }
        return out;
    }

    function cdnUrl(pkg, version, path) {
        var base = renderOptions.cdnBase || shellOptions.cdnBase;
        if (!base || !version) {
            return "";
        }
        return base + pkg + "@" + version + "/" + path;
    }

    // ── 자산 로더 ─────────────────────────────────────────

    /// 스크립트를 확보한다.
    ///
    /// 실패 판정을 셋으로 나눈 이유는 각각 다른 고장을 잡기 때문이다. 하나라도
    /// 빼면 그 고장에서 영원히 매달린다.
    ///   * onerror       — DNS 실패, 연결 거부, 4xx/5xx, SRI 불일치, 정책 차단
    ///   * 전역 심볼 확인 — 캡티브 포털이 200 으로 로그인 HTML 을 돌려준 경우.
    ///                     onload 는 정상 발화하고 전역만 없다
    ///   * 타임아웃      — TCP connect 가 매달린 사내망. 위 둘이 영원히 안 온다
    function loadScript(url, isLoaded, timeoutMs, sri) {
        return new Promise(function (resolve, reject) {
            if (!url) {
                reject(new Error("no-url"));
                return;
            }
            var settled = false;
            var element = document.createElement("script");
            var timer = timeoutMs > 0 ? window.setTimeout(function () {
                if (settled) { return; }
                settled = true;
                // 늦게 오는 onload 를 버린다.
                element.onload = null;
                element.onerror = null;
                reject(new Error("timeout"));
            }, timeoutMs) : 0;

            element.onerror = function () {
                if (settled) { return; }
                settled = true;
                if (timer) { window.clearTimeout(timer); }
                reject(new Error("error"));
            };
            element.onload = function () {
                if (settled) { return; }
                settled = true;
                if (timer) { window.clearTimeout(timer); }
                if (isLoaded()) {
                    resolve();
                } else {
                    reject(new Error("no-symbol"));
                }
            };
            if (sri) {
                // SRI 실패는 **좋은 실패**다. CDN 이 조용히 다른 바이트를 주면
                // 스크립트가 차단되고 onerror 로 떨어져 번들 폴백이 발동한다.
                element.integrity = sri;
                element.crossOrigin = "anonymous";
            }
            element.src = url;
            document.head.appendChild(element);
        });
    }

    /// 스타일시트는 실패를 치명으로 보지 않는다. 없어도 수식 자체는 그려진다.
    function loadStyle(url) {
        return new Promise(function (resolve) {
            if (!url) {
                resolve();
                return;
            }
            var element = document.createElement("link");
            element.rel = "stylesheet";
            element.href = url;
            element.onload = function () { resolve(); };
            element.onerror = function () { resolve(); };
            document.head.appendChild(element);
        });
    }

    function reportAssetFailure(assetId, reason) {
        if (!window.__mrrBridge || typeof window.__mrrBridge.markdownAssetFailed !== "function") {
            return;
        }
        window.__mrrBridge.markdownAssetFailed(assetId, reason);
    }

    function invalidatePreviewCache() {
        if (typeof window.__mrrInvalidatePreviewCache === "function") {
            window.__mrrInvalidatePreviewCache();
        }
    }

    // ── 줄 매핑 ───────────────────────────────────────────

    /// 이 토큰의 data-mrr-* 만 뽑아 태그에 넣을 문자열로 만든다.
    function mrrAttrString(token) {
        if (!token.attrs) {
            return "";
        }
        var out = "";
        for (var i = 0; i < token.attrs.length; i += 1) {
            var name = token.attrs[i][0];
            if (name.indexOf("data-mrr-") !== 0) {
                continue;
            }
            out += " " + name + "=\"" + String(token.attrs[i][1]) + "\"";
        }
        return out;
    }

    /// 블록 토큰에 원본 줄 범위를 심는다. **여기가 스탬핑의 유일한 자리다.**
    ///
    /// renderer.renderToken() 이 token.attrs 를 그대로 태그에 뿌리므로 대부분의
    /// 블록 요소는 렌더 규칙을 건드리지 않아도 된다.
    ///
    /// token.map 은 [시작, 끝) 0-기반 끝 배타다. 그래서 1-기반 시작줄은 map[0]+1,
    /// 포함 끝줄은 map[1] 이다. 문단의 map[1] 은 뒤따르는 빈 줄을 포함해 한 줄
    /// 넘칠 수 있는데, data-mrr-end-line 은 mrr_preview.js 에서 **문서 끝 sentinel
    /// 계산에만** 쓰이므로 마지막 요소만 문제가 된다. 그래서 문서 줄 수로 자른다.
    function installLineStamps(md) {
        md.core.ruler.push("mrr_line_stamp", function (state) {
            var lineCount = Number(state.env && state.env.mrrLineCount) || 0;
            for (var i = 0; i < state.tokens.length; i += 1) {
                var token = state.tokens[i];
                // nesting < 0 은 닫는 태그다. 여는 태그와 자기완결 블록에만 심는다.
                if (!token.map || token.nesting < 0) {
                    continue;
                }
                var end = token.map[1];
                if (lineCount > 0 && end > lineCount) {
                    end = lineCount;
                }
                token.attrSet("data-mrr-start-line", String(token.map[0] + 1));
                token.attrSet("data-mrr-end-line", String(end));
                // 단일 파일이므로 include 개념이 없다. 항상 0 이다.
                token.attrSet("data-mrr-src", "0");
            }
        });
    }

    // ── front matter ──────────────────────────────────────

    /// YAML front matter 를 **소비**한다. 잘라내지 않는다.
    ///
    /// 파싱 전에 문자열에서 잘라내면 이후 모든 token.map 이 그 줄 수만큼 밀려
    /// 스크롤 동기화가 문서 전체에서 어긋난다. 블록 규칙으로 state.line 을
    /// 전진시키면 map 이 절대 줄 번호를 유지한다.
    ///
    /// 닫는 구분자를 **먼저** 확인한다. 닫히지 않은 세 붙임표는 front matter 가
    /// 아니라 수평선이므로, 확인 없이 소비하면 문서 전체가 사라진다.
    function installFrontMatter(md) {
        md.block.ruler.before("table", "mrr_front_matter", function (state, startLine, endLine, silent) {
            if (startLine !== 0) {
                return false;
            }
            var lineText = function (line) {
                return state.src.slice(state.bMarks[line] + state.tShift[line], state.eMarks[line]).trim();
            };
            if (lineText(startLine) !== "---") {
                return false;
            }
            var closing = -1;
            for (var line = startLine + 1; line < endLine; line += 1) {
                var text = lineText(line);
                if (text === "---" || text === "...") {
                    closing = line;
                    break;
                }
            }
            if (closing < 0) {
                return false;
            }
            if (silent) {
                return true;
            }
            // 토큰을 만들지 않으므로 렌더에서 자동으로 숨는다.
            state.line = closing + 1;
            return true;
        }, { alt: [] });
    }

    // ── GFM 작업 목록 ─────────────────────────────────────

    /// `- [x] 항목` 을 체크박스로 만든다.
    ///
    /// core.ruler.after("block") 에 두는 것이 핵심이다. 그 시점에는 inline 토큰의
    /// children 이 아직 채워지지 않았으므로(inline 규칙이 그 뒤에 돈다) content 만
    /// 고치면 재파싱이 알아서 일어난다. token.map 은 건드리지 않는다.
    ///
    /// 체크박스는 CSS 로 그린다. DOM 요소를 새로 끼우면 앵커 y 순서에 영향이 갈 수
    /// 있고, <input> 은 프리뷰에서 누를 수 있는 것처럼 보여 오해를 준다.
    function installTaskLists(md) {
        md.core.ruler.after("block", "mrr_task_lists", function (state) {
            var tokens = state.tokens;
            for (var i = 0; i + 2 < tokens.length; i += 1) {
                if (tokens[i].type !== "list_item_open") {
                    continue;
                }
                if (tokens[i + 1].type !== "paragraph_open" || tokens[i + 2].type !== "inline") {
                    continue;
                }
                var match = /^\[([ xX])\][ \t]+/.exec(tokens[i + 2].content);
                if (!match) {
                    continue;
                }
                var done = match[1] !== " ";
                tokens[i + 2].content = tokens[i + 2].content.slice(match[0].length);
                tokens[i].attrJoin("class", done ? "mrr-task mrr-task-done" : "mrr-task");
            }
        });
    }

    // ── GitHub 경고 상자 ──────────────────────────────────

    /// `> [!NOTE]` 를 경고 상자로 만든다.
    ///
    /// **속성만 더하고 텍스트만 잘라낸다.** blockquote_open 을 새로 만들거나 마커
    /// 문단을 삭제하면 그 토큰의 map 이 사라져 앵커에 구멍이 생긴다.
    ///
    /// 제목은 CSS ::before 가 data-mrr-alert-title 을 읽어 그린다. 같은 이유로
    /// DOM 요소를 끼우지 않는다.
    function installAlerts(md) {
        md.core.ruler.after("block", "mrr_alerts", function (state) {
            var tokens = state.tokens;
            for (var i = 0; i + 2 < tokens.length; i += 1) {
                if (tokens[i].type !== "blockquote_open") {
                    continue;
                }
                if (tokens[i + 1].type !== "paragraph_open" || tokens[i + 2].type !== "inline") {
                    continue;
                }
                var match = /^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\][ \t]*(?:\n|$)/
                    .exec(tokens[i + 2].content);
                if (!match) {
                    continue;
                }
                var kind = match[1];
                tokens[i].attrJoin("class", "mrr-alert mrr-alert-" + kind.toLowerCase());
                tokens[i].attrSet("data-mrr-alert-title", ALERT_TITLES[kind]);
                tokens[i + 2].content = tokens[i + 2].content.slice(match[0].length);
            }
        });
    }

    // ── 코드 블록 ─────────────────────────────────────────

    /// 코드블록 속성을 <code> 가 아니라 <pre> 에 붙인다.
    ///
    /// 기본 규칙은 <pre><code{attrs}> 로 조립한다. 스크롤하는 <pre> 안의 <code> 는
    /// 높이는 있으나 기준점 위치가 어긋나므로 바깥으로 옮긴다.
    ///
    /// mermaid 펜스는 여기서 <pre class="mermaid"> 로 갈라낸다.
    function installCodeRenderers(md) {
        var utils = md.utils;

        md.renderer.rules.fence = function (tokens, idx, options) {
            var token = tokens[idx];
            var info = token.info ? utils.unescapeAll(token.info).trim() : "";
            var lang = info.split(/\s+/g)[0];
            var attrs = mrrAttrString(token);

            if (lang === "mermaid") {
                return "<pre class=\"mermaid\"" + attrs + ">"
                    + utils.escapeHtml(token.content) + "</pre>\n";
            }

            var codeClass = lang
                ? " class=\"" + options.langPrefix + utils.escapeHtml(lang) + "\""
                : "";
            return "<pre" + attrs + "><code" + codeClass + ">"
                + utils.escapeHtml(token.content) + "</code></pre>\n";
        };

        md.renderer.rules.code_block = function (tokens, idx) {
            var token = tokens[idx];
            return "<pre" + mrrAttrString(token) + "><code>"
                + utils.escapeHtml(token.content) + "</code></pre>\n";
        };
    }

    // ── 수식 ──────────────────────────────────────────────

    /// 수식 렌더러 계약. 새 렌더러를 넣을 때 이 다섯 개만 채운다.
    ///
    /// typeset() 은 thenable 을 돌려준다. **동기 렌더러는 이미 완료된 것을 돌려주면
    /// 된다** — 호출자가 그것이 끝난 뒤에 좌표 캐시를 버리므로, 비동기 완료 통지가
    /// 이 인터페이스로 표현된다(MathJax 라면 typesetPromise() 를 그대로 돌려준다).
    var MATH_RENDERERS = {
        katex: {
            id: "katex",
            scripts: function () {
                return [
                    cdnUrl("katex", renderOptions.katexVersion, "dist/katex.min.js"),
                    cdnUrl("katex", renderOptions.katexVersion, "dist/contrib/auto-render.min.js")
                ];
            },
            styles: function () {
                return [cdnUrl("katex", renderOptions.katexVersion, "dist/katex.min.css")];
            },
            isLoaded: function () {
                return typeof window.renderMathInElement === "function";
            },
            typeset: function (root) {
                window.renderMathInElement(root, {
                    delimiters: MATH_DELIMITERS,
                    // 문법이 틀리면 예외 대신 빨간 원문으로 남는다. 산문의 달러
                    // 기호가 수식으로 오해되는 경우가 있어(MyST 도 같은 문제다)
                    // 그때 렌더 전체가 실패하면 안 된다.
                    throwOnError: false
                });
                // 폰트가 늦게 오면 수식 폭이 변해 좌표 캐시가 낡는다. 폰트 로드가
                // 정리될 때까지 기다린 뒤 완료를 보고하면 호출자가 그 뒤에 캐시를
                // 버린다. 이 위험은 auto-render 고유가 아니라 KaTeX 고유다.
                return (document.fonts && document.fonts.ready)
                    ? document.fonts.ready
                    : Promise.resolve();
            }
        }
    };

    function currentMathRenderer() {
        return MATH_RENDERERS[renderOptions.mathRenderer] || MATH_RENDERERS.katex;
    }

    function ensureMath() {
        if (mathState === "ready" || mathState === "failed") {
            return Promise.resolve();
        }
        if (mathPromise) {
            return mathPromise;
        }
        if (!renderOptions.allowRemote) {
            // .rst 프리뷰와 같은 등급이다. 꺼져 있으면 수식이 원본 텍스트로 남는다.
            mathState = "failed";
            return Promise.resolve();
        }

        var renderer = currentMathRenderer();
        var scripts = renderer.scripts();
        mathState = "loading";
        mathPromise = Promise.all(renderer.styles().map(loadStyle))
            .then(function () {
                // 순서가 있다. auto-render 는 katex 전역을 필요로 한다.
                return scripts.reduce(function (chain, url) {
                    return chain.then(function () {
                        return loadScript(url, function () { return true; }, HEAVY_TIMEOUT_MS);
                    });
                }, Promise.resolve());
            })
            .then(function () {
                if (!renderer.isLoaded()) {
                    throw new Error("no-symbol");
                }
                mathState = "ready";
            })
            .catch(function (error) {
                mathState = "failed";
                reportAssetFailure(renderer.id, String((error && error.message) || error));
            });
        return mathPromise;
    }

    function hasMath(text) {
        return /\$|\\\(|\\\[/.test(text);
    }

    // ── 다이어그램 ────────────────────────────────────────

    function ensureMermaid() {
        if (mermaidState === "ready" || mermaidState === "failed") {
            return Promise.resolve();
        }
        if (mermaidPromise) {
            return mermaidPromise;
        }
        if (!renderOptions.allowRemote) {
            mermaidState = "failed";
            return Promise.resolve();
        }

        mermaidState = "loading";
        mermaidPromise = loadScript(
            cdnUrl("mermaid", renderOptions.mermaidVersion, "dist/mermaid.min.js"),
            function () { return typeof window.mermaid !== "undefined"; },
            HEAVY_TIMEOUT_MS
        ).then(function () {
            window.mermaid.initialize({
                startOnLoad: false,
                theme: renderOptions.dark ? "dark" : "default"
            });
            mermaidState = "ready";
        }).catch(function (error) {
            mermaidState = "failed";
            reportAssetFailure("mermaid", String((error && error.message) || error));
        });
        return mermaidPromise;
    }

    /// 다이어그램은 타이핑이 멎은 뒤에 그린다.
    ///
    /// 도형 하나가 수십~수백 ms 라 렌더마다(8Hz) 다시 그리면 프리뷰가 멎는다.
    /// 그리고 나서 문서 높이가 달라지므로 좌표 캐시를 버려야 한다.
    function scheduleDiagrams() {
        if (diagramTimer) {
            window.clearTimeout(diagramTimer);
        }
        diagramTimer = window.setTimeout(function () {
            diagramTimer = 0;
            var blocks = document.querySelectorAll("pre.mermaid");
            if (!blocks.length) {
                return;
            }
            ensureMermaid().then(function () {
                if (mermaidState !== "ready") {
                    return null;
                }
                return window.mermaid.run({ nodes: blocks });
            }).then(invalidatePreviewCache, invalidatePreviewCache);
        }, DIAGRAM_IDLE_MS);
    }

    // ── 파서 ──────────────────────────────────────────────

    function ensureParser() {
        if (parser) {
            return parser;
        }
        if (typeof window.markdownit !== "function") {
            return null;
        }
        var md = window.markdownit({
            html: true,
            // GFM 의 자동 링크. linkify-it 은 markdown-it 코어에 이미 들어 있다.
            linkify: true,
            typographer: false
        });
        if (typeof window.markdownitFootnote === "function") {
            md.use(window.markdownitFootnote);
        }
        installFrontMatter(md);
        installTaskLists(md);
        installAlerts(md);
        installCodeRenderers(md);
        // 스탬핑은 마지막이다. 위 규칙들이 토큰을 손본 뒤의 상태에 심는다.
        installLineStamps(md);
        parser = md;
        return parser;
    }

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

    function rootElement() {
        var root = document.getElementById("mrr-md-root");
        if (!root) {
            root = document.createElement("div");
            root.id = "mrr-md-root";
            document.body.appendChild(root);
        }
        return root;
    }

    function applyThemeVariables(theme) {
        if (!theme) {
            return;
        }
        var style = document.documentElement.style;
        for (var name in theme) {
            if (Object.prototype.hasOwnProperty.call(theme, name)) {
                style.setProperty("--mrr-md-" + name, String(theme[name]));
            }
        }
    }

    function render(text, baseUrl, optionsJson) {
        var md = ensureParser();
        if (!md) {
            return Promise.reject(new Error("renderer not ready"));
        }
        if (optionsJson) {
            try {
                renderOptions = JSON.parse(optionsJson) || {};
            } catch (error) {
                renderOptions = {};
            }
        }
        applyThemeVariables(renderOptions.theme);
        setDocumentBase(baseUrl);

        var source = String(text);
        // BOM 을 남기면 첫 줄의 해시가 0열이 아니게 되어 첫 제목만 조용히 사라진다.
        if (source.charCodeAt(0) === 0xFEFF) {
            source = source.slice(1);
        }

        var root = rootElement();
        root.innerHTML = md.render(source, { mrrLineCount: source.split("\n").length });

        // 다이어그램은 유휴 시간에 따로 그린다.
        scheduleDiagrams();

        // 수식이 없으면 렌더러를 아예 받지 않는다. 서브트리 순회 비용도 0 이 된다.
        if (!hasMath(source)) {
            return Promise.resolve();
        }
        return ensureMath().then(function () {
            if (mathState !== "ready") {
                return null;
            }
            return currentMathRenderer().typeset(root);
        });
    }

    // 이 함수는 **동기적으로** 정의되어야 한다. mrr_preview.js 는 DocumentReady 에
    // 주입되어 곧바로 bridge.ready() 를 보내므로, 그 시점에 훅이 없으면 첫 요청이
    // 사라진다. 코어가 아직 없으면 마지막 요청 하나를 큐잉한다
    // (PreviewBridge::requestScrollToLine 의 hasPendingScroll_ 과 같은 관용구다).
    //
    // 큐잉했으면 **null** 을 돌려준다. 호출자가 그것을 보고 회신을 미루고, 실제로
    // 그린 뒤 이쪽에서 회신한다 — 큐잉을 실패로 보고하면 로그에 잡음만 남는다.
    window.__mrrMarkdownRender = function (text, baseUrl, optionsJson, token) {
        if (!ensureParser()) {
            queued = { text: text, baseUrl: baseUrl, optionsJson: optionsJson, token: token };
            return null;
        }
        return render(text, baseUrl, optionsJson);
    };

    /// C++ 에 렌더러 상태를 알린다. 사실만 올리고 문장은 C++ 이 만든다 —
    /// JS 에는 tr() 이 없어서 여기서 만든 문자열은 번역할 수 없다.
    ///
    /// 코어가 브리지보다 먼저 준비되는 것이 보통이므로(번들이면 거의 항상) 이
    /// 함수는 두 번 불릴 수 있다. 브리지가 없으면 조용히 지나가고,
    /// mrr_preview.js 가 브리지를 받은 뒤 __mrrMarkdownBridgeReady 로 깨운다.
    function reportReady() {
        if (!coreReady) {
            return;
        }
        if (!window.__mrrBridge || typeof window.__mrrBridge.markdownRendererReady !== "function") {
            return;
        }
        if (reportedOrigin === coreOrigin) {
            return;   // 이미 같은 사실을 올렸다
        }
        reportedOrigin = coreOrigin;
        var version = coreFallbackReason
            ? "markdown-it (" + coreFallbackReason + ")"
            : "markdown-it " + shellOptions.markdownItVersion;
        window.__mrrBridge.markdownRendererReady(coreOrigin, version);
    }

    // mrr_preview.js 가 브리지를 받은 직후 부른다.
    window.__mrrMarkdownBridgeReady = function () {
        reportReady();
        flushQueued();
    };

    /// 큐에 걸린 요청을 그린다. 그 요청의 회신도 여기서 한다 — 호출자는 null 을
    /// 받고 회신을 미뤄 두었다.
    function flushQueued() {
        if (!queued || !ensureParser()) {
            return;
        }
        var request = queued;
        queued = null;
        render(request.text, request.baseUrl, request.optionsJson).then(function () {
            invalidatePreviewCache();
            if (window.__mrrBridge && typeof window.__mrrBridge.markdownRendered === "function") {
                window.__mrrBridge.markdownRendered(request.token, true, "");
            }
        }, function (error) {
            if (window.__mrrBridge && typeof window.__mrrBridge.markdownRendered === "function") {
                window.__mrrBridge.markdownRendered(request.token, false, String(error));
            }
        });
    }

    /// 코어를 확보한다. 원격 우선일 때 실패하면 번들로 내려간다.
    ///
    /// 플러그인은 **어떤 경우에도 번들이다.** 개별로 받으면 실패 지점이 그만큼 늘고,
    /// 부분 실패가 총 실패보다 나쁘다 — 문서는 그려지는데 각주만 조용히 사라지고
    /// 사용자는 자기가 문법을 틀린 줄 안다. 원격화로 아끼는 것도 압축 후 4KB 다.
    function bootCore() {
        var haveCore = function () { return typeof window.markdownit === "function"; };
        var localCore = VENDOR_BASE + "markdown-it.min.js";
        var remoteCore = cdnUrl("markdown-it", shellOptions.markdownItVersion,
                                "dist/browser/markdown-it.umd.min.js");

        var chain;
        if (shellOptions.coreSource === "remote" && shellOptions.allowRemote && remoteCore) {
            chain = loadScript(remoteCore, haveCore, CORE_TIMEOUT_MS, shellOptions.coreSri)
                .then(function () {
                    coreOrigin = "cdn";
                }, function (error) {
                    // 오프라인·사내망·CDN 장애·SRI 불일치. 번들이 exe 안에 있는데
                    // 쓰지 않으면 프리뷰가 백지가 된다.
                    coreFallbackReason = String((error && error.message) || error);
                    return loadScript(localCore, haveCore, 0).then(function () {
                        coreOrigin = "bundled-fallback";
                    });
                });
        } else {
            chain = loadScript(localCore, haveCore, 0).then(function () {
                coreOrigin = "bundled";
            });
        }

        return chain.then(function () {
            return loadScript(VENDOR_BASE + "markdown-it-footnote.min.js",
                              function () { return typeof window.markdownitFootnote === "function"; },
                              0);
        });
    }

    function boot() {
        bootCore().then(function () {
            coreReady = true;
            ensureParser();
            reportReady();
            flushQueued();
        }, function (error) {
            // 번들까지 실패하는 것은 있을 수 없다(exe 안의 리소스다). 그래도
            // 조용히 죽지 않게 사실을 올린다.
            reportAssetFailure("markdown-it", String((error && error.message) || error));
        });
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", boot);
    } else {
        boot();
    }

    // 렌더 결과를 눈으로 보기 어려운 것들을 자동으로 확인할 수 있게 한다
    // (mrr_preview.js 의 __mrrTestHooks 와 같은 목적).
    window.__mrrMarkdownTestHooks = {
        renderToHtml: function (text) {
            var md = ensureParser();
            return md ? md.render(String(text), { mrrLineCount: String(text).split("\n").length }) : "";
        },
        coreOrigin: function () { return coreOrigin; },
        assetState: function () { return { math: mathState, mermaid: mermaidState }; }
    };
}());
