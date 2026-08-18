# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""translations/*.ts 를 기계적으로 검사한다. 커밋 전에 돌린다.

같은 규칙이 mrst_tests 의 tst_Translations 에도 있어 ctest 에서 항상 돈다.
이 스크립트는 번역 작업 중에 빠르게 돌려 보기 위한 것이다 — 빌드가 필요 없다.

사용:
    tools\\uv.exe run --script tools\\i18n\\ts_lint.py
    tools\\uv.exe run --script tools\\i18n\\ts_lint.py --strict   # 미번역도 실패로
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _tslib import check, load   # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = ROOT / "translations"
LANGS = ("en", "ja")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true", help="미번역이 하나라도 있으면 실패")
    args = ap.parse_args()

    failed = False
    for lang in LANGS:
        path = TRANSLATIONS / f"mrst_{lang}.ts"
        if not path.exists():
            print(f"{lang}: 파일 없음 — {path}")
            failed = True
            continue

        messages = load(path)
        untranslated = [m for m in messages if not m.translated]
        problems: list[str] = []
        for m in messages:
            found = check(m, lang)
            if found:
                problems.append(f"  {m.label()}\n    → {m.translations}\n    ✗ {', '.join(sorted(set(found)))}")

        done = len(messages) - len(untranslated)
        pct = (done * 100 // len(messages)) if messages else 0
        print(f"{lang}: {done}/{len(messages)} 번역됨 ({pct}%), 규칙 위반 {len(problems)}건")
        if problems:
            print("\n".join(problems))
            failed = True
        if args.strict and untranslated:
            print(f"  미번역 {len(untranslated)}건:")
            for m in untranslated[:20]:
                print(f"    {m.label()}")
            if len(untranslated) > 20:
                print(f"    … 그 외 {len(untranslated) - 20}건")
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
