# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""LLM 응답(JSON)을 translations/mrst_<lang>.ts 에 되돌린다.

응답 형식은 `[{"id": "...", "t": "..."}]` 또는 복수형이면
`[{"id": "...", "t": ["단수", "복수"]}]` 이고, 파일 이름은
`translations/_work/<lang>/chunk_NN.out.json` 이다.

검증에 걸린 항목은 **파일에 쓰지 않고** 목록으로 돌려준다. 잘못된 번역이
저장소에 들어간 뒤에 찾는 것보다, 왕복 안에서 다시 시키는 편이 싸다.

쓰고 나면 lconvert 로 정규화한다. ElementTree 의 출력은 Qt 와 형식이 달라서,
정규화하지 않으면 사람이 Linguist 로 한 번 저장하는 순간 파일 전체가 diff 로
뜬다.

사용:
    tools\\uv.exe run --script tools\\i18n\\ts_merge.py --lang en
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _tslib import Message, check, load, write_translations   # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = ROOT / "translations"
DEFAULT_QT_BIN = Path(os.environ.get("MRST_QT_BIN", r"D:\Qt\6.11.1\msvc2022_64\bin"))


def unescape(text: str) -> str:
    """ts_export.py 의 escape() 를 되돌린다."""
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "t":
                out.append("\t")
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
        out.append(text[i])
        i += 1
    return "".join(out)


def normalize(ts_path: Path) -> bool:
    lconvert = DEFAULT_QT_BIN / "lconvert.exe"
    if not lconvert.exists():
        print(f"  (lconvert 없음: {lconvert} — 정규화를 건너뜁니다)")
        return False
    tmp = ts_path.with_suffix(".tmp.ts")
    ts_path.replace(tmp)
    result = subprocess.run(
        [str(lconvert), "-locations", "none", "-sort-contexts", "-sort-messages",
         "-i", str(tmp), "-o", str(ts_path)],
        capture_output=True, text=True,
    )
    if result.returncode != 0 or not ts_path.exists():
        tmp.replace(ts_path)
        print(f"  lconvert 실패: {result.stderr.strip()}")
        return False
    tmp.unlink()
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", required=True, choices=["en", "ja"])
    ap.add_argument("--finished", action="store_true",
                    help="type=\"unfinished\" 를 떼고 검수 완료로 표시한다 (기본은 유지)")
    args = ap.parse_args()

    ts = TRANSLATIONS / f"mrst_{args.lang}.ts"
    by_id: dict[str, Message] = {m.id: m for m in load(ts)}

    work = TRANSLATIONS / "_work" / args.lang
    entries: list[tuple[str, dict[str, object]]] = []

    # 납작한 형식: "id<TAB>번역" 한 줄에 한 항목. 복수형은 탭으로 형을 이어 붙인다.
    tsv = work / "all.out.tsv"
    if tsv.exists():
        for line in tsv.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            forms = [unescape(p) for p in parts[1:] if p != ""]
            entries.append((tsv.name, {"id": parts[0], "t": forms if len(forms) > 1 else (forms[0] if forms else "")}))

    for path in sorted(work.glob("chunk_*.out.json")):
        for entry in json.loads(path.read_text(encoding="utf-8")):
            entries.append((path.name, entry))

    if not entries:
        print(f"응답 파일이 없습니다: {work}\\all.out.tsv 또는 chunk_NN.out.json")
        return 1

    accepted: dict[str, list[str]] = {}
    rejected: list[str] = []
    unknown: list[str] = []

    for origin, entry in entries:
            mid = entry.get("id", "")
            msg = by_id.get(mid)
            if msg is None:
                unknown.append(f"{origin}: 모르는 id {mid}")
                continue
            raw = entry.get("t", "")
            texts = raw if isinstance(raw, list) else [raw]
            if msg.is_plural and len(texts) < len(msg.translations):
                texts = texts + [texts[-1]] * (len(msg.translations) - len(texts))

            candidate = Message(context=msg.context, source=msg.source, comment=msg.comment,
                                translations=texts, is_plural=msg.is_plural)
            problems = check(candidate, args.lang)
            if problems:
                rejected.append(f"  {msg.label()}\n    → {texts}\n    ✗ {', '.join(sorted(set(problems)))}")
                continue
            accepted[mid] = texts

    if unknown:
        print("모르는 id:")
        print("\n".join(unknown))

    changed = write_translations(ts, accepted, keep_unfinished=not args.finished)
    normalize(ts)

    print(f"{changed}개 반영 → {ts.name}")
    if rejected:
        print(f"\n검증 실패 {len(rejected)}개 (반영하지 않음):")
        print("\n".join(rejected))
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
