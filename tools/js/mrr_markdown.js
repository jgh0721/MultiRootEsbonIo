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

    /// 렌더러가 준비되기 전에 들어온 마지막 요청. 하나만 들고 있는다 —
    /// PreviewBridge::requestScrollToLine 의 hasPendingScroll_ 과 같은 관용구다.
    ///
    /// 이 큐가 없으면 경합이 생긴다. mrr_preview.js 는 DocumentReady 에 주입되어
    /// 곧바로 bridge.ready() 를 보내는데, 그 시점에 코어 로드가 끝나 있다고
    /// 보장할 수 없다(원격 우선일 때는 특히).
    var queued = null;
    var parser = null;

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

    function render(text, baseUrl) {
        var md = ensureParser();
        if (!md) {
            return Promise.reject(new Error("renderer not ready"));
        }
        setDocumentBase(baseUrl);

        var source = String(text);
        // BOM 을 남기면 첫 줄의 해시가 0열이 아니게 되어 첫 제목만 조용히 사라진다.
        if (source.charCodeAt(0) === 0xFEFF) {
            source = source.slice(1);
        }

        var html = md.render(source, { mrrLineCount: source.split("\n").length });
        rootElement().innerHTML = html;
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

    // 렌더 결과를 눈으로 보기 어려운 것들을 자동으로 확인할 수 있게 한다
    // (mrr_preview.js 의 __mrrTestHooks 와 같은 목적).
    window.__mrrMarkdownTestHooks = {
        renderToHtml: function (text) {
            var md = ensureParser();
            return md ? md.render(String(text), { mrrLineCount: String(text).split("\n").length }) : "";
        }
    };
}());
