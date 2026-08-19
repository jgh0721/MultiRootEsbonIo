#pragma once

#include <QLatin1String>
#include <QString>

/// Markdown 프리뷰가 쓰는 자산의 **단일 출처**.
///
/// 번들 파일의 버전과 CDN URL 의 버전이 어긋나면 같은 문서가 네트워크 상태에 따라
/// 다르게 렌더된다. 개발자에게는 늘 네트워크가 있으므로 그 버그는 개발 중에 절대
/// 보이지 않는다 — 최악의 종류다. 그래서 버전을 여기 한 곳에만 두고, CMake 가 구성
/// 시점에 `tools/js/vendor/<이름>-<버전>...` 파일이 실제로 있는지 대조한다.
///
/// 디스크의 벤더 파일 이름에는 버전이 박혀 있고 qrc 별칭은 버전을 벗긴다. 셸의
/// `src` 가 안정되면서도 파일은 감사 가능한 채로 남는다.
namespace mrst::mdassets {

/// 번들하는 것.
inline constexpr auto                   kMarkdownItVersion       = "15.0.0";
inline constexpr auto                   kMarkdownItFootnoteVersion = "4.0.0";

/// 번들하지 않고 CDN 에서만 받는 것.
///
/// mermaid 는 sphinxcontrib-mermaid 의 `mermaid_version` 기본값과 같은 값으로 둔다
/// (설치본 sphinxcontrib/mermaid/__init__.py). 그러면 같은 다이어그램이 .rst(Sphinx)
/// 프리뷰와 .md(내장) 프리뷰에서 같은 버전으로 그려진다. 그 값은 사용자가 conf.py 에서
/// 덮을 수 있으므로 이 정렬은 "작성 시점의 기본값 일치" 이고, sphinx 의존을 올릴 때
/// 다시 볼 항목이다.
inline constexpr auto                   kMermaidVersion          = "11.12.1";
inline constexpr auto                   kKatexVersion            = "0.18.4";

/// CDN 뿌리. sphinx.ext.mathjax(sphinx/ext/mathjax.py) 와 sphinxcontrib-mermaid 가
/// 이미 jsdelivr 를 쓴다. 사내망 allowlist 를 두 곳 뚫게 만들 이유가 없다.
inline constexpr auto                   kCdnBase                 = "https://cdn.jsdelivr.net/npm/";

/// 원격에서 받을 때 붙이는 SRI 해시.
///
/// CDN 이 조용히 다른 바이트를 주면 스크립트가 차단되고 onerror 로 떨어져 **번들
/// 폴백이 발동한다** — 즉 SRI 실패가 좋은 실패가 된다. 번들 파일에서 계산했다
/// (`openssl dgst -sha384 -binary <파일> | openssl base64 -A`).
inline constexpr auto                   kMarkdownItSri =
    "sha384-RFgiWKVXntFwXKC7cM/vTNo+YCPWtoe5WjzaQWX8NMxIt3CnW1Sjhgt6YPVyrGT5";

/// qrc 안의 셸 페이지. 문서마다 새로 만들지 않는다 — 본문은 브리지로 밀어 넣는다.
inline constexpr auto                   kShellResourcePath       = "qrc:/preview/md/shell.html";

/// markdown-it 코어를 어디서 가져오는가의 기본값 ("remote" | "bundled").
///
/// 원격 우선이다. 그러면 항상 최신 코어를 쓰지만, 그 경로는 네트워크가 있는 개발
/// 환경에서 늘 성공하므로 **폴백이 도는 것을 일부러 확인해야 한다**(네트워크를 끊고
/// .md 를 열어 로그에 "내장본을 사용합니다" 가 나오는지). 판단이 바뀌면 이 한 줄만
/// "bundled" 로 고친다.
inline constexpr auto                   kDefaultCoreSource       = "remote";

/// 수식 렌더러의 기본값. 설정 preview/mathRenderer 가 덮는다.
inline constexpr auto                   kDefaultMathRenderer     = "katex";

}   // namespace mrst::mdassets
