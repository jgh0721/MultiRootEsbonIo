"""MultiRoot-reST 프리뷰 빌더.

Sphinx 를 Python API 로 직접 구동해 HTML 을 만들고, docutils 노드가 알고 있는
원본 줄 번호를 그대로 HTML 속성으로 심는다. 결과 요약은 사이드카 JSON 으로
내보내며 C++ 쪽은 그 JSON 만 신뢰한다 (stdout 은 로그 패널 표시용).

에디터 <-> 프리뷰 양방향 스크롤 동기화가 정확하려면 "이 DOM 요소가 원본 몇 번째
줄에서 몇 번째 줄까지인가" 를 알아야 한다. 생성된 HTML 에서 원본 텍스트를
substring 검색하는 방식으로는 인라인 마크업(**굵게**, :ref:`x`) 한 개만 있어도
깨지므로, 빌드 안에서 doctree 를 통해 얻는다.

종료 코드:
  0 성공 / 2 Sphinx 예외 / 3 인자 오류 / 4 sphinx·docutils 임포트 불가
"""

from __future__ import annotations

import argparse
import io
import json
import re
import sys
import traceback
from pathlib import Path

try:
    import docutils.writers._html_base as docutils_html_base
    from docutils import nodes
    from sphinx.application import Sphinx
    from sphinx.errors import ExtensionError, ThemeError
except Exception:  # noqa: BLE001 - 런타임이 덜 갖춰진 경우를 명시적으로 구분한다.
    traceback.print_exc(file=sys.stderr)
    sys.exit(4)


REPORT_SCHEMA = 1

# Sphinx 경고의 표준 출력 형식: "<path>:<line>: WARNING: <message>"
WARNING_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):\s*(?P<level>WARNING|ERROR|CRITICAL|SEVERE):\s*(?P<message>.*)$"
)
# 줄 번호 없이 파일만 나오는 형태도 있다.
WARNING_NO_LINE_RE = re.compile(
    r"^(?P<path>.+?):\s*(?P<level>WARNING|ERROR|CRITICAL|SEVERE):\s*(?P<message>.*)$"
)

MISSING_EXTENSION_RE = re.compile(r"Could not import extension ([\w.]+)")
MISSING_THEME_RE = re.compile(r"no theme named ['\"]([^'\"]+)['\"]")
MISSING_MODULE_RE = re.compile(r"No module named ['\"]([^'\"]+)['\"]")


# ── docutils 패치 ────────────────────────────────────────────────────────────
#
# HTMLTranslator.starttag() 는 **attributes 로 넘어온 것과 node 의
# 'classes'/'ids' 만 출력한다. node.attributes 를 순회하지 않으므로 임의
# 속성(data-mrr-*)은 그냥 사라진다. 파이썬 원본은 이 사실을 놓쳐서 범위 기반
# 동기화가 한 번도 동작한 적이 없다(항상 앵커 보간으로 degrade).
#
# docutils 베이스 클래스를 패치하면 Sphinx 의 HTML5Translator 와 테마가 제공하는
# 서브클래스까지 모두 커버된다. 값은 docutils 자체 attval() 이스케이프를 탄다.
def _install_starttag_passthrough() -> None:
    base = docutils_html_base.HTMLTranslator
    if getattr(base, "_mrr_patched", False):
        return

    original_starttag = base.starttag

    def starttag(self, node, tagname, suffix="\n", empty=False, **attributes):
        node_attributes = getattr(node, "attributes", None)
        if isinstance(node_attributes, dict):
            for key, value in node_attributes.items():
                if isinstance(key, str) and key.startswith("data-mrr-"):
                    attributes[key] = value
        return original_starttag(self, node, tagname, suffix, empty, **attributes)

    base.starttag = starttag
    base._mrr_patched = True


# ── 줄 번호 스탬핑 ───────────────────────────────────────────────────────────

class SourceLineStamper:
    """doctree 노드에 원본 (파일, 시작줄, 끝줄) 을 심는다."""

    def __init__(self) -> None:
        self.sources: list[str] = []
        self._source_index: dict[str, int] = {}
        self._line_counts: dict[str, int] = {}

    def source_index(self, path: str | None) -> int:
        if not path:
            return -1
        resolved = str(Path(path).resolve())
        if resolved not in self._source_index:
            self._source_index[resolved] = len(self.sources)
            self.sources.append(resolved)
        return self._source_index[resolved]

    def line_count(self, path: str) -> int:
        if path not in self._line_counts:
            try:
                text = Path(path).read_text(encoding="utf-8", errors="replace")
                self._line_counts[path] = max(1, text.count("\n") + 1)
            except OSError:
                self._line_counts[path] = 1
        return self._line_counts[path]

    def stamp_doctree(self, doctree: nodes.document) -> None:
        entries: list[dict] = []
        self._collect(doctree, entries, getattr(doctree, "source", None))
        if not entries:
            return

        # 1) line 이 없는 노드는 문서 순서상 가장 가까운 앞 노드의 줄을 물려받는다.
        #    (원본은 이런 노드를 통째로 건너뛰어 커버리지가 크게 낮았다.)
        last_known: int | None = None
        for entry in entries:
            if entry["line"] is None:
                entry["line"] = last_known
            else:
                last_known = entry["line"]

        _mark_first_anchor_per_line(entries)
        self._apply(entries)

    def _apply(self, entries: list[dict]) -> None:
        # 끝줄 = 문서 순서상 자기 서브트리 바로 다음 노드의 시작줄 - 1.
        # rawsource 의 줄 수를 세는 방식은 section/container 처럼 rawsource 가
        # 빈 노드에서 end == start 가 되어, 정작 범위 보간이 필요한 큰 요소가
        # 쓸모없어진다.
        total = len(entries)
        for entry in entries:
            line = entry["line"]
            if line is None or line < 1:
                continue

            end_line = None
            following = entry["subtree_last"] + 1
            if following < total:
                candidate = entries[following]
                if candidate["src"] == entry["src"] and candidate["line"]:
                    end_line = candidate["line"] - 1
            if end_line is None and entry["src"]:
                end_line = self.line_count(entry["src"])
            if end_line is None or end_line < line:
                end_line = line

            node = entry["node"]
            node["data-mrr-start-line"] = str(line)
            node["data-mrr-end-line"] = str(end_line)

            src_index = self.source_index(entry["src"])
            if src_index >= 0:
                node["data-mrr-src"] = str(src_index)

            # ids 는 어떤 경우에도 출력되므로, starttag 패치가 깨지더라도
            # 앵커 기반 보간은 계속 동작한다.
            if not entry["anchor_taken"]:
                anchor = f"mrr-line-{max(0, src_index)}-{line}"
                ids = node.setdefault("ids", [])
                if anchor not in ids:
                    ids.append(anchor)

    def _collect(self, node: nodes.Element, entries: list[dict], parent_source: str | None) -> int:
        index = len(entries)
        raw_line = getattr(node, "line", None)
        line = raw_line if isinstance(raw_line, int) and raw_line >= 1 else None
        source = getattr(node, "source", None) or parent_source

        entries.append(
            {
                "node": node,
                "line": line,
                "src": str(Path(source).resolve()) if source else None,
                "subtree_last": index,
                "anchor_taken": False,
            }
        )

        for child in node.children:
            if isinstance(child, nodes.Element):
                self._collect(child, entries, source)

        entries[index]["subtree_last"] = len(entries) - 1
        return index


def _mark_first_anchor_per_line(entries: list[dict]) -> None:
    """같은 (파일, 줄) 조합에는 앵커를 하나만 남긴다."""
    seen: set[tuple[str | None, int]] = set()
    for entry in entries:
        key = (entry["src"], entry["line"])
        if key in seen:
            entry["anchor_taken"] = True
        else:
            seen.add(key)


# ── 진단 수집 ────────────────────────────────────────────────────────────────

def _parse_warnings(text: str) -> list[dict]:
    diagnostics: list[dict] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue

        match = WARNING_RE.match(line)
        if match:
            diagnostics.append(
                {
                    "path": match.group("path"),
                    "line": int(match.group("line")),
                    "severity": match.group("level").lower(),
                    "message": match.group("message").strip(),
                }
            )
            continue

        match = WARNING_NO_LINE_RE.match(line)
        if match and not match.group("path").upper().startswith("WARNING"):
            diagnostics.append(
                {
                    "path": match.group("path"),
                    "line": 1,
                    "severity": match.group("level").lower(),
                    "message": match.group("message").strip(),
                }
            )
    return diagnostics


def _collect_missing(text: str) -> tuple[list[dict], list[str]]:
    modules = {m for m in MISSING_EXTENSION_RE.findall(text)}
    modules.update(MISSING_MODULE_RE.findall(text))
    themes = sorted(set(MISSING_THEME_RE.findall(text)))
    missing = [{"module": name, "distribution": _distribution_for(name)} for name in sorted(modules)]
    return missing, themes


def _distribution_for(module: str) -> str:
    """모듈명 -> PyPI 배포명 추정. 확실하지 않으면 일반 규칙을 쓴다."""
    known = {
        "myst_parser": "myst-parser",
        "sphinx_design": "sphinx-design",
        "sphinx_copybutton": "sphinx-copybutton",
        "sphinx_tabs": "sphinx-tabs",
        "sphinx_rtd_theme": "sphinx-rtd-theme",
        "sphinx_book_theme": "sphinx-book-theme",
        "pydata_sphinx_theme": "pydata-sphinx-theme",
        "sphinxawesome_theme": "sphinxawesome-theme",
    }
    if module in known:
        return known[module]
    return module.replace("_", "-").replace(".", "-")


# ── conf.py 정적 검사 ────────────────────────────────────────────────────────

def _conf_declares_empty_html_style(conf_path: Path) -> bool:
    """레거시 html_style = '' 감지. Sphinx 8 에서 _static checksum 오류를 낸다."""
    try:
        import ast

        tree = ast.parse(conf_path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, SyntaxError):
        return False

    found = False
    for node in ast.walk(tree):
        targets = []
        if isinstance(node, ast.Assign):
            targets = node.targets
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
            value = node.value
        else:
            continue

        for target in targets:
            if isinstance(target, ast.Name) and target.id == "html_style":
                if isinstance(value, ast.Constant) and value.value == "":
                    found = True
                else:
                    found = False
    return found


# ── 메인 ─────────────────────────────────────────────────────────────────────

def _parse_shadow_sources(values: list[str]) -> dict[str, Path]:
    shadow: dict[str, Path] = {}
    for value in values:
        source_text, separator, temp_text = value.partition("=")
        if not separator or not source_text.strip() or not temp_text.strip():
            continue
        shadow[str(Path(source_text.strip()).resolve())] = Path(temp_text.strip())
    return shadow


def _shadow_source_reader(app: Sphinx, shadow: dict[str, Path]):
    def replace_source(_app: Sphinx, docname: str, source: list[str]) -> None:
        try:
            doc_path = str(Path(_app.env.doc2path(docname)).resolve())
        except Exception:  # noqa: BLE001
            return
        replacement = shadow.get(doc_path)
        if replacement is None:
            return
        try:
            source[0] = replacement.read_text(encoding="utf-8")
        except OSError:
            pass

    return replace_source


def _write_report(path: Path, payload: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError:
        pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build a Sphinx HTML preview for MultiRoot reST.")
    parser.add_argument("--conf-dir", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--doctree-dir", required=True, type=Path)
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--primary", type=Path, default=None,
                        help="편집 중인 원본 파일. htmlPath 계산에 쓴다.")
    parser.add_argument("--shadow", action="append", default=[], metavar="SRC=TMP",
                        help="원본 파일의 내용을 임시 파일 내용으로 대체 (미저장 버퍼 반영).")
    parser.add_argument("--auto-fix-legacy-conf", action="store_true",
                        help="html_style='' 을 감지해 None 으로 override.")
    parser.add_argument("--define", action="append", default=[], metavar="KEY=VALUE")
    args = parser.parse_args(argv)

    report_path = args.report or (args.out_dir / ".mrr-build.json")
    report: dict = {
        "schema": REPORT_SCHEMA,
        "ok": False,
        "outDir": str(args.out_dir),
        "htmlPath": "",
        "primaryDocname": "",
        "sources": [],
        "diagnostics": [],
        "missingExtensions": [],
        "missingThemes": [],
        "traceback": None,
    }

    confoverrides: dict[str, object] = {}
    conf_file = args.conf_dir / "conf.py"
    if args.auto_fix_legacy_conf and _conf_declares_empty_html_style(conf_file):
        confoverrides["html_style"] = None
        report["confOverridesApplied"] = {"html_style": None}

    for define in args.define:
        key, separator, value = define.partition("=")
        if separator and key.strip():
            confoverrides[key.strip()] = value

    warning_stream = io.StringIO()
    status_stream = io.StringIO()

    try:
        _install_starttag_passthrough()

        app = Sphinx(
            srcdir=str(args.source_dir),
            confdir=str(args.conf_dir),
            outdir=str(args.out_dir),
            doctreedir=str(args.doctree_dir),
            buildername="html",
            confoverrides=confoverrides,
            status=status_stream,
            warning=warning_stream,
        )

        shadow = _parse_shadow_sources(args.shadow)
        if shadow:
            app.connect("source-read", _shadow_source_reader(app, shadow))

        stamper = SourceLineStamper()
        app.connect("doctree-resolved",
                    lambda _app, doctree, _docname: stamper.stamp_doctree(doctree))
        app.build()

        # app.config.version 은 "프로젝트" 버전이다. Sphinx 자체 버전이 필요하다.
        import sphinx

        report["sphinxVersion"] = sphinx.__version__
        report["htmlTheme"] = getattr(app.config, "html_theme", "")
        report["sources"] = stamper.sources

        if args.primary is not None:
            try:
                docname = app.env.path2doc(str(args.primary.resolve()))
            except Exception:  # noqa: BLE001
                docname = None
            if docname:
                report["primaryDocname"] = docname
                candidate = args.out_dir / (docname + ".html")
                if candidate.exists():
                    report["htmlPath"] = str(candidate)

        if not report["htmlPath"]:
            root_doc = getattr(app.config, "root_doc", None) or getattr(app.config, "master_doc", "index")
            candidate = args.out_dir / (str(root_doc) + ".html")
            if candidate.exists():
                report["htmlPath"] = str(candidate)

        report["ok"] = True

    except (ExtensionError, ThemeError, ModuleNotFoundError) as error:
        report["traceback"] = traceback.format_exc()
        warning_stream.write("\n" + str(error) + "\n")
        traceback.print_exc(file=sys.stderr)
        _finish(report, warning_stream, status_stream, report_path)
        return 2
    except Exception:  # noqa: BLE001
        report["traceback"] = traceback.format_exc()
        traceback.print_exc(file=sys.stderr)
        _finish(report, warning_stream, status_stream, report_path)
        return 2

    _finish(report, warning_stream, status_stream, report_path)
    return 0


def _finish(report: dict, warning_stream: io.StringIO, status_stream: io.StringIO,
            report_path: Path) -> None:
    warnings_text = warning_stream.getvalue()
    report["diagnostics"] = _parse_warnings(warnings_text)
    missing_extensions, missing_themes = _collect_missing(
        warnings_text + "\n" + (report.get("traceback") or "")
    )
    report["missingExtensions"] = missing_extensions
    report["missingThemes"] = missing_themes

    # stdout/stderr 은 로그 패널 표시용으로만 흘려보낸다.
    sys.stdout.write(status_stream.getvalue())
    sys.stderr.write(warnings_text)

    _write_report(report_path, report)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
