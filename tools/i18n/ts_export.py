# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""translations/mrst_<lang>.ts → translations/_work/<lang>/chunk_NN.json

LLM 에게 XML 을 통째로 먹이지 않는다. 700개면 언어당 ~120K 토큰이라 한 번에
들어가지 않고, 잘라 넣으면 XML 이 깨진다. 게다가 LLM 이 XML 을 재생성하면
`type` 속성과 `<extracomment>` 를 반드시 흘린다.

청크는 **컨텍스트(클래스) 단위**로 자른다. 컨텍스트가 곧 화면이고 화면이 곧
문맥이라, 한 청크 안의 문자열끼리는 말투와 용어가 저절로 맞는다.

사용:
    tools\\uv.exe run --script tools\\i18n\\ts_export.py --lang en
    tools\\uv.exe run --script tools\\i18n\\ts_export.py --lang ja --only-untranslated
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _tslib import Message, load   # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = ROOT / "translations"

MAX_ITEMS_PER_CHUNK = 40
#: 항목 수가 적어도 원문이 길면(설명 툴팁이 몰린 페이지) 나눈다.
MAX_CHARS_PER_CHUNK = 3000


def load_glossary() -> list[dict[str, str]]:
    path = TRANSLATIONS / "glossary.tsv"
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 3:
            rows.append({"ko": parts[0], "en": parts[1], "ja": parts[2]})
    return rows


def chunk(messages: list[Message]) -> list[list[Message]]:
    """컨텍스트를 가로지르지 않고 자른다."""
    chunks: list[list[Message]] = []
    by_context: dict[str, list[Message]] = {}
    for m in messages:
        by_context.setdefault(m.context, []).append(m)

    for _, group in sorted(by_context.items()):
        current: list[Message] = []
        chars = 0
        for m in group:
            if current and (len(current) >= MAX_ITEMS_PER_CHUNK or chars >= MAX_CHARS_PER_CHUNK):
                chunks.append(current)
                current, chars = [], 0
            current.append(m)
            chars += len(m.source)
        if current:
            chunks.append(current)
    return chunks


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def write_tsv(path: Path, messages: list[Message], english: dict[str, str]) -> None:
    """한 파일로 몰아 쓰는 납작한 형식.

    청크 JSON 은 도구가 읽기 좋지만 사람이(그리고 긴 문맥을 한 번에 보는 번역기가)
    훑기에는 줄 수가 너무 많다. 이쪽은 한 항목이 한 줄이라 700개를 한눈에 본다.
    개행과 탭은 이스케이프한다 — 되돌릴 때 그대로 복원된다.
    """
    lines = ["# id\tcontext\tplural\tnote\tsource\ten(참고)"]
    for m in messages:
        lines.append(
            "\t".join((
                m.id,
                m.context,
                str(len(m.translations)) if m.is_plural else "",
                escape(m.extracomment or m.comment),
                escape(m.source),
                escape(english.get(m.id, "")),
            ))
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", required=True, choices=["en", "ja"])
    ap.add_argument("--only-untranslated", action="store_true")
    ap.add_argument("--tsv", action="store_true",
                    help="청크 JSON 대신 all.tsv 한 파일로 쓴다")
    args = ap.parse_args()

    ts = TRANSLATIONS / f"mrst_{args.lang}.ts"
    messages = load(ts)
    if args.only_untranslated:
        messages = [m for m in messages if not m.translated]
    if not messages:
        print("내보낼 항목이 없습니다.")
        return 0

    # 일본어는 한국어 원문과 영어 번역을 **둘 다** 실어 보낸다. 한국어는 주어와
    # 조사를 자주 생략해서 원문만으로는 문맥을 놓치기 쉽다.
    english: dict[str, str] = {}
    if args.lang == "ja":
        for m in load(TRANSLATIONS / "mrst_en.ts"):
            if m.translated:
                english[m.id] = m.translations[0]

    out_dir = TRANSLATIONS / "_work" / args.lang
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    if args.tsv:
        out = out_dir / "all.tsv"
        write_tsv(out, messages, english)
        print(f"{len(messages)}개 항목 → {out}")
        return 0

    glossary = load_glossary()
    chunks = chunk(messages)
    for i, group in enumerate(chunks, start=1):
        payload = {
            "source_language": "ko",
            "target_language": args.lang,
            "context": group[0].context,
            "chunk": f"{i}/{len(chunks)}",
            "glossary": glossary,
            "items": [],
        }
        for m in group:
            item: dict[str, object] = {"id": m.id, "src": m.source}
            if m.comment:
                item["comment"] = m.comment
            if m.extracomment:
                item["note"] = m.extracomment
            if m.is_plural:
                item["plural_forms"] = len(m.translations)
            if args.lang == "ja" and m.id in english:
                item["en"] = english[m.id]
            payload["items"].append(item)
        (out_dir / f"chunk_{i:02d}.json").write_text(
            json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
        )

    print(f"{len(messages)}개 항목을 {len(chunks)}개 청크로 내보냈습니다 → {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
