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

    function now() {
        return new Date().getTime();
    }

    function invalidateCache() {
        cachedRanges = null;
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

    /// 원본 줄 -> 문서 Y 좌표.
    /// 줄을 포함하는 가장 좁은 범위를 골라 그 안에서 비율 보간한다.
    /// (가장 좁은 것을 고르는 이유: section 같은 큰 요소보다 문단/줄 단위 요소가
    ///  훨씬 정확하기 때문)
    function documentYForLine(src, line) {
        var all = ranges();
        var best = null;
        for (var i = 0; i < all.length; i += 1) {
            var range = all[i];
            if (range.src !== src || line < range.start || line > range.end) {
                continue;
            }
            if (!best || range.height < best.height) {
                best = range;
            }
        }
        if (best) {
            var span = Math.max(1, best.end - best.start + 1);
            var ratioInRange = Math.min(1, Math.max(0, (line - best.start) / span));
            return best.top + best.height * ratioInRange;
        }

        var list = anchors();
        if (!list.length) {
            return null;
        }
        var previous = list[0];
        var next = list[list.length - 1];
        for (var j = 0; j < list.length; j += 1) {
            if (list[j].line <= line) { previous = list[j]; }
            if (list[j].line >= line) { next = list[j]; break; }
        }
        var lineSpan = Math.max(1, next.line - previous.line);
        var t = Math.min(1, Math.max(0, (line - previous.line) / lineSpan));
        return previous.y + (next.y - previous.y) * t;
    }

    /// 문서 Y 좌표 -> 원본 줄. documentYForLine 의 역함수.
    function lineForDocumentY(y) {
        var all = ranges();
        var best = null;
        for (var i = 0; i < all.length; i += 1) {
            var range = all[i];
            if (y < range.top || y > range.top + range.height) {
                continue;
            }
            if (!best || range.height < best.height) {
                best = range;
            }
        }
        if (best) {
            var ratioInRange = Math.min(1, Math.max(0, (y - best.top) / best.height));
            var span = best.end - best.start;
            return {
                src: best.src,
                line: Math.max(1, Math.round(best.start + span * ratioInRange))
            };
        }

        var list = anchors();
        if (!list.length) {
            return null;
        }
        var previous = list[0];
        var next = list[list.length - 1];
        for (var j = 0; j < list.length; j += 1) {
            if (list[j].y <= y) { previous = list[j]; }
            if (list[j].y >= y) { next = list[j]; break; }
        }
        var ySpan = Math.max(1, next.y - previous.y);
        var t = Math.min(1, Math.max(0, (y - previous.y) / ySpan));
        return {
            src: previous.src,
            line: Math.max(1, Math.round(previous.line + (next.line - previous.line) * t))
        };
    }

    /// 프리뷰를 스크롤해 (src, line) 이 창의 ratio 위치에 오게 한다.
    function scrollToSourceLine(src, line, ratio) {
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
        bridge.ready(PROTOCOL_VERSION);
    }

    // 매핑 함수 검증용 훅. 스크롤 동기화 정확도는 눈으로 보기 어려워서
    // 왕복 변환(line -> Y -> line)이 일치하는지 자동으로 확인할 수 있게 한다.
    window.__mrrTestHooks = {
        documentYForLine: documentYForLine,
        lineForDocumentY: lineForDocumentY,
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
