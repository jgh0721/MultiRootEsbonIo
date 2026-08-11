// MultiRoot-reST 프리뷰 페이지 스크립트.
//
// 에디터와 프리뷰의 스크롤을 "뷰포트 비율 보존" 방식으로 맞춘다.
// 즉, 어떤 줄이 에디터 창의 세로 50% 위치에 있으면 프리뷰에서도 그 줄에
// 해당하는 내용이 창의 세로 50% 위치에 오게 한다. 단순히 "그 줄로 스크롤"
// 하는 것과 다르다.
//
// 매핑의 근거는 빌더가 심어 둔 속성이다:
//   data-mrr-start-line / data-mrr-end-line : 이 요소가 덮는 원본 줄 범위
//   data-mrr-src                            : 원본 파일 인덱스 (include 대응)
//
// C++ 과는 QWebChannel 객체 `bridge` 로만 통신한다. runJavaScript 로 문자열을
// 조립해 보내지 않는다 (이스케이프 버그의 온상이고 매번 재파싱된다).

(function () {
    "use strict";

    if (window.__mrrPreviewInstalled) {
        return;
    }
    window.__mrrPreviewInstalled = true;

    var PROTOCOL_VERSION = 1;
    // 스크롤 위치를 대표하는 기준점. 0.5 = 창의 정중앙.
    var ANCHOR_RATIO = 0.5;
    // C++ 이 우리를 스크롤시킨 직후 우리가 다시 C++ 에 보고하면 무한 왕복이 된다.
    var FEEDBACK_GUARD_MS = 250;

    var bridge = null;
    var suppressUntil = 0;
    var scrollTimer = null;
    var cachedRanges = null;
    var cachedAnchors = null;
    var lastViewportWidth = 0;

    function now() {
        return new Date().getTime();
    }

    function invalidateCache() {
        cachedRanges = null;
        cachedAnchors = null;
    }

    /// 창 너비가 바뀌면 자동 줄바꿈 위치가 달라져 모든 기준점 좌표가 무의미해진다.
    function invalidateIfResized() {
        if (window.innerWidth !== lastViewportWidth) {
            lastViewportWidth = window.innerWidth;
            invalidateCache();
        }
    }

    /// 화면에 실제로 자리를 차지하는 범위 요소들을 문서 좌표로 수집한다.
    function ranges() {
        if (cachedRanges) {
            return cachedRanges;
        }
        var out = [];
        var elements = document.querySelectorAll("[data-mrr-start-line]");
        for (var i = 0; i < elements.length; i += 1) {
            var element = elements[i];
            var start = Number(element.getAttribute("data-mrr-start-line"));
            if (!start) {
                continue;
            }
            var end = Number(element.getAttribute("data-mrr-end-line") || start);
            var src = Number(element.getAttribute("data-mrr-src") || 0);
            var rect = element.getBoundingClientRect();
            if (rect.height <= 0) {
                continue;
            }
            out.push({
                src: src,
                start: start,
                end: Math.max(start, end),
                top: rect.top + window.scrollY,
                height: Math.max(1, rect.height)
            });
        }
        cachedRanges = out;
        return out;
    }

    /// 앵커(id="mrr-line-<src>-<line>") 기반 폴백. 범위 요소가 하나도 없을 때 쓴다.
    function anchors() {
        var out = [];
        var elements = document.querySelectorAll('[id^="mrr-line-"]');
        for (var i = 0; i < elements.length; i += 1) {
            var match = String(elements[i].id).match(/^mrr-line-(\d+)-(\d+)$/);
            if (!match) {
                continue;
            }
            out.push({
                src: Number(match[1]),
                line: Number(match[2]),
                y: elements[i].getBoundingClientRect().top + window.scrollY
            });
        }
        out.sort(function (a, b) { return a.line - b.line; });
        return out;
    }

    /// 보간 기준점 목록.
    ///
    /// 여기가 이 파일의 핵심이다. 원본 줄 간격과 렌더된 픽셀 간격은 비례하지
    /// 않는다 — 에디터에서 접히지도 줄바꿈되지도 않은 한 줄이, 좁은 프리뷰
    /// 창에서는 여러 줄로 접혀 몇 배 높이를 차지할 수 있다. 반대로 여러 원본
    /// 줄이 한 문단으로 합쳐져 한 줄 높이만 차지할 수도 있다.
    ///
    /// 그래서 "줄 수에 비례해 픽셀을 나눈다" 는 가정을 쓰지 않고, 실제로 측정된
    /// 기준점 쌍 사이를 구간별 선형 보간한다. 두 기준점 사이의 픽셀 거리는 이미
    /// 줄바꿈이 반영된 실제 렌더 높이이므로, 접힘 정도가 구간마다 달라도 맞는다.
    function anchorTable() {
        if (cachedAnchors) {
            return cachedAnchors;
        }

        var byKey = {};
        var all = ranges();
        for (var i = 0; i < all.length; i += 1) {
            var range = all[i];
            var key = range.src + ":" + range.start;
            var existing = byKey[key];
            // 같은 시작 줄을 가진 요소가 여럿이면 가장 작은 것을 쓴다.
            // (section 같은 큰 요소보다 문단 단위 요소가 훨씬 정확하다.)
            if (!existing || range.height < existing.height) {
                byKey[key] = range;
            }
        }

        var points = [];
        for (var k in byKey) {
            if (Object.prototype.hasOwnProperty.call(byKey, k)) {
                var r = byKey[k];
                points.push({ src: r.src, line: r.start, end: r.end, y: r.top, height: r.height });
            }
        }
        points.sort(function (a, b) { return a.y - b.y || a.line - b.line; });

        // 보간이 흔들리지 않으려면 y 와 line 이 **둘 다** 증가해야 한다.
        //
        // y 가 같은 기준점이 연달아 나오는 경우가 실제로 많다. 예를 들어
        // list-table 한 행의 셀 8개는 소스에서는 8줄이지만 화면에서는 모두 같은
        // 높이에 있다. 이걸 그대로 두면 에디터에서 그 8줄을 지나는 동안 프리뷰가
        // 멈춰 있다가 다음 행에서 갑자기 점프한다.
        //
        // 같은 y 를 가진 묶음에서는 첫 항목만 남긴다. 그러면 그 줄 범위 전체가
        // 다음 기준점까지의 픽셀 구간에 고르게 매핑되어 스크롤이 매끄러워지고,
        // 역변환도 하나의 값으로 확정된다.
        var monotonic = [];
        for (var m = 0; m < points.length; m += 1) {
            var last = monotonic.length ? monotonic[monotonic.length - 1] : null;
            if (last && points[m].src === last.src
                && (points[m].line <= last.line || points[m].y <= last.y)) {
                continue;
            }
            monotonic.push(points[m]);
        }

        // 마지막 요소의 끝을 기준점으로 하나 더 둬서 문서 끝쪽 보간을 가둔다.
        if (monotonic.length) {
            var tail = monotonic[monotonic.length - 1];
            monotonic.push({
                src: tail.src,
                line: Math.max(tail.line + 1, tail.end + 1),
                end: tail.end + 1,
                y: tail.y + tail.height,
                height: 0
            });
        }

        cachedAnchors = monotonic;
        return monotonic;
    }

    /// 원본 줄(소수 가능) -> 문서 Y 좌표.
    function documentYForLine(src, line) {
        var table = anchorTable();
        var mine = [];
        for (var i = 0; i < table.length; i += 1) {
            if (table[i].src === src) {
                mine.push(table[i]);
            }
        }
        if (!mine.length) {
            return null;
        }
        if (line <= mine[0].line) {
            return mine[0].y;
        }
        for (var j = 0; j < mine.length - 1; j += 1) {
            var a = mine[j];
            var b = mine[j + 1];
            if (line >= a.line && line <= b.line) {
                var lineSpan = Math.max(1e-6, b.line - a.line);
                var t = (line - a.line) / lineSpan;
                // a.y ~ b.y 의 픽셀 거리에는 프리뷰 쪽 자동 줄바꿈이 이미 반영돼 있다.
                return a.y + (b.y - a.y) * t;
            }
        }
        return mine[mine.length - 1].y;
    }

    /// 문서 Y 좌표 -> 원본 줄(소수). documentYForLine 의 역함수.
    function lineForDocumentY(y) {
        var table = anchorTable();
        if (!table.length) {
            return null;
        }
        if (y <= table[0].y) {
            return { src: table[0].src, line: table[0].line };
        }
        for (var i = 0; i < table.length - 1; i += 1) {
            var a = table[i];
            var b = table[i + 1];
            if (y >= a.y && y <= b.y) {
                // 두 기준점이 다른 파일에 속하면(include 경계) 보간하지 않고
                // 가까운 쪽으로 붙인다.
                if (a.src !== b.src) {
                    return (y - a.y) <= (b.y - y)
                        ? { src: a.src, line: a.line }
                        : { src: b.src, line: b.line };
                }
                var ySpan = Math.max(1e-6, b.y - a.y);
                var t = (y - a.y) / ySpan;
                return { src: a.src, line: a.line + (b.line - a.line) * t };
            }
        }
        var lastPoint = table[table.length - 1];
        return { src: lastPoint.src, line: lastPoint.line };
    }

    /// 프리뷰를 스크롤해 (src, line) 이 창의 ratio 위치에 오게 한다.
    function scrollToSourceLine(src, line, ratio) {
        invalidateIfResized();

        // 에디터가 문서 맨 위다. 첫 기준점은 테마 헤더나 여백만큼 아래에 있어서
        // 비율 보정을 태우면 0 보다 큰 값이 나오고, 그러면 제목이 화면 위로
        // 잘려 나간다. 문서 시작은 문서 시작으로 맞춘다.
        if (line <= 1) {
            suppressUntil = now() + FEEDBACK_GUARD_MS;
            window.scrollTo(0, 0);
            return;
        }

        var y = documentYForLine(src, line);
        if (y === null) {
            return;
        }
        var target = Math.max(0, y - window.innerHeight * ratio);
        suppressUntil = now() + FEEDBACK_GUARD_MS;
        window.scrollTo(0, target);
    }

    function reportScroll() {
        if (!bridge || now() < suppressUntil) {
            return;
        }
        invalidateIfResized();
        var hit = lineForDocumentY(window.scrollY + window.innerHeight * ANCHOR_RATIO);
        if (hit) {
            bridge.previewScrolled(hit.src, hit.line, ANCHOR_RATIO);
        }
    }

    window.addEventListener("scroll", function () {
        if (now() < suppressUntil) {
            return;
        }
        if (scrollTimer) {
            window.clearTimeout(scrollTimer);
        }
        scrollTimer = window.setTimeout(reportScroll, 60);
    }, { passive: true });

    // 레이아웃이 바뀌면 캐시한 좌표가 무의미해진다.
    window.addEventListener("resize", invalidateCache);
    if (window.ResizeObserver) {
        var observer = new ResizeObserver(invalidateCache);
        observer.observe(document.documentElement);
    }

    document.addEventListener("click", function (event) {
        if (!bridge) {
            return;
        }
        // 링크 클릭은 원래 동작을 살린다.
        var node = event.target;
        while (node && node !== document.body) {
            if (node.tagName === "A" && node.getAttribute("href")) {
                return;
            }
            node = node.parentNode;
        }
        var hit = lineForDocumentY(event.clientY + window.scrollY);
        if (hit) {
            var viewportRatio = Math.min(0.9, Math.max(0,
                event.clientY / Math.max(1, window.innerHeight)));
            bridge.sourceLocationClicked(hit.src, hit.line, viewportRatio);
        }
    }, true);

    function connectBridge(channel) {
        bridge = channel.objects.bridge;
        if (!bridge) {
            return;
        }
        bridge.scrollToLineRequested.connect(function (src, line, ratio) {
            scrollToSourceLine(src, line, ratio);
        });
        bridge.rebindRequested.connect(function () {
            invalidateCache();
        });
        bridge.scrollFeedbackSuppressed.connect(function (milliseconds) {
            suppressUntil = now() + milliseconds;
        });
        bridge.hotSwapRequested.connect(function (documentHtml, baseUrl, token) {
            hotSwap(documentHtml, baseUrl, token);
        });
        bridge.ready(PROTOCOL_VERSION);
    }

    /// 전체 리로드 없이 body 만 교체한다.
    ///
    /// 재빌드마다 페이지를 새로 로드하면 흰 화면이 한 번 깜빡이고 스크롤이
    /// 튄다. <head> 가 그대로일 때만 C++ 이 이 경로를 고르므로, 이미 적용된
    /// 스타일시트는 그대로 두고 본문만 바꾸면 된다.
    function hotSwap(documentHtml, baseUrl, token) {
        try {
            var parsed = new DOMParser().parseFromString(documentHtml, "text/html");
            if (!parsed || !parsed.body) {
                throw new Error("parse failed");
            }

            // 출력 디렉터리가 빌드마다 바뀌므로 상대 경로(_static/, 이미지)가
            // 새 디렉터리를 가리키도록 <base> 를 갱신한다.
            if (baseUrl) {
                var base = document.querySelector("base[data-mrr-preview-base]");
                if (!base) {
                    base = document.createElement("base");
                    base.setAttribute("data-mrr-preview-base", "");
                    document.head.insertBefore(base, document.head.firstChild);
                }
                base.setAttribute("href", baseUrl);
            }

            var keepX = window.scrollX;
            var keepY = window.scrollY;

            // 교체 도중 문서 높이가 줄면 브라우저가 스크롤을 잘라버린다.
            var previousMinHeight = document.body.style.minHeight;
            document.body.style.minHeight = document.body.scrollHeight + "px";

            document.body.innerHTML = parsed.body.innerHTML;
            invalidateCache();

            window.scrollTo(keepX, keepY);
            document.body.style.minHeight = previousMinHeight;

            suppressUntil = now() + FEEDBACK_GUARD_MS;
            bridge.hotSwapResult(token, true, "");
        } catch (error) {
            // 실패하면 C++ 이 전체 리로드로 되돌린다.
            bridge.hotSwapResult(token, false, String(error));
        }
    }

    // 매핑 함수 검증용 훅. 스크롤 동기화 정확도는 눈으로 보기 어려워서
    // 왕복 변환(line -> Y -> line)이 일치하는지 자동으로 확인할 수 있게 한다.
    window.__mrrTestHooks = {
        documentYForLine: documentYForLine,
        lineForDocumentY: lineForDocumentY,
        anchorTable: anchorTable,
        ranges: ranges
    };

    function boot() {
        if (typeof QWebChannel === "undefined" || !window.qt || !window.qt.webChannelTransport) {
            return;
        }
        new QWebChannel(window.qt.webChannelTransport, connectBridge);
    }

    if (document.readyState === "complete" || document.readyState === "interactive") {
        boot();
    } else {
        document.addEventListener("DOMContentLoaded", boot);
    }
}());
