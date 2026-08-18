# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Qt .ts 파일을 LLM 번역 왕복에 쓰기 위한 최소 도구.

설계 메모
---------
* `<translation>` 의 **텍스트 노드만** 건드린다. `type` 속성, `<extracomment>`,
  `<extra-*>`, `<comment>` 는 손대지 않는다. LLM 에게 XML 을 다시 쓰게 하면
  반드시 무언가를 흘린다.
* 메시지 식별자는 ``sha1(context \0 source \0 comment)`` 다. 원문 문자열만으로
  찾으면 `comment` 로만 구분되는 동음이의 쌍에서 틀린 자리에 넣게 된다.
* 쓰고 나면 반드시 `lconvert` 로 정규화한다(ts_merge.py 가 한다).
  ElementTree 는 DOCTYPE·들여쓰기·속성 순서를 Qt 와 다르게 써서, 사람이
  Linguist 로 한 번 저장하는 순간 파일 전체가 diff 로 뜬다.
"""

from __future__ import annotations

import hashlib
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

# ─────────────────────────────────────────────────────────────
# 모델
# ─────────────────────────────────────────────────────────────


@dataclass
class Message:
    context: str
    source: str
    comment: str = ""          # tr( s, comment ) — 동음이의 구분자
    extracomment: str = ""     # //: 주석
    translations: list[str] = field(default_factory=list)   # 복수형이면 2개 이상
    is_plural: bool = False
    unfinished: bool = True

    @property
    def id(self) -> str:
        raw = "\0".join((self.context, self.source, self.comment))
        return hashlib.sha1(raw.encode("utf-8")).hexdigest()[:16]

    @property
    def translated(self) -> bool:
        return any(t.strip() for t in self.translations)

    def label(self) -> str:
        return f"{self.context} / {self.source!r}" + (f" [{self.comment}]" if self.comment else "")


def load(path: Path) -> list[Message]:
    tree = ET.parse(path)
    out: list[Message] = []
    for ctx in tree.getroot().findall("context"):
        name = ctx.findtext("name") or ""
        for msg in ctx.findall("message"):
            tr = msg.find("translation")
            forms = tr.findall("numerusform") if tr is not None else []
            if forms:
                texts = [(f.text or "") for f in forms]
                plural = True
            else:
                texts = [(tr.text or "") if tr is not None else ""]
                plural = msg.get("numerus") == "yes"
            out.append(
                Message(
                    context=name,
                    source=msg.findtext("source") or "",
                    comment=msg.findtext("comment") or "",
                    extracomment=msg.findtext("extracomment") or "",
                    translations=texts,
                    is_plural=plural,
                    unfinished=(tr is not None and tr.get("type") == "unfinished"),
                )
            )
    return out


def write_translations(path: Path, by_id: dict[str, list[str]], keep_unfinished: bool = True) -> int:
    """`by_id` 에 있는 항목의 번역문만 갈아 끼우고 같은 경로에 쓴다. 바꾼 개수를 돌려준다.

    `keep_unfinished=True` 는 의도적인 기본값이다. qttools 의 releasehelper.cpp 는
    "번역이 있으면서 unfinished 인" 항목만 검사하므로, finished 로 표시하면
    `lrelease -fail-on-invalid` 가 아무것도 검사하지 않는다. unfinished 라도
    번역 텍스트가 있으면 .qm 에 그대로 들어가므로 사용자에게는 차이가 없고,
    Linguist 의 "미완료" 필터가 그대로 사람 검수 대기 목록이 된다.
    """
    tree = ET.parse(path)
    changed = 0
    for ctx in tree.getroot().findall("context"):
        name = ctx.findtext("name") or ""
        for msg in ctx.findall("message"):
            key = Message(
                context=name,
                source=msg.findtext("source") or "",
                comment=msg.findtext("comment") or "",
            ).id
            if key not in by_id:
                continue
            texts = by_id[key]
            tr = msg.find("translation")
            if tr is None:
                continue
            forms = tr.findall("numerusform")
            if forms:
                for i, form in enumerate(forms):
                    form.text = texts[i] if i < len(texts) else texts[-1]
            else:
                tr.text = texts[0]
            if keep_unfinished:
                tr.set("type", "unfinished")
            else:
                tr.attrib.pop("type", None)
            changed += 1
    tree.write(path, encoding="utf-8", xml_declaration=True)
    return changed


# ─────────────────────────────────────────────────────────────
# 검증 규칙
#
# 린터(ts_lint.py)와 머지 도구가 **같은 함수**를 쓴다. 규칙이 두 벌이 되면
# 머지가 통과시킨 것을 린터가 거부하는 상황이 생긴다.
# ─────────────────────────────────────────────────────────────

_PLACE = re.compile(r"%(\d+)")
_HANGUL = re.compile(r"[가-힣ᄀ-ᇿ㄰-㆏]")
_TAG = re.compile(r"</?([a-zA-Z][a-zA-Z0-9]*)\b[^>]*>")

#: 원문과 글자 하나까지 같아야 하는 것들 — 기호, 단위, 구분자.
IDENTITY_EXPECTED = {"⫶", "☰", "■", "✕", "¶", "● ", ", ", " MB", " ms", " pt", " x"}


def place_markers(text: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for m in _PLACE.finditer(text):
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def mnemonic_count(text: str) -> int:
    n, i = 0, 0
    while i + 1 < len(text):
        if text[i] == "&":
            nxt = text[i + 1]
            if nxt == "&":          # "&&" 는 리터럴 앰퍼샌드
                i += 2
                continue
            if not nxt.isspace() and nxt != "#":
                n += 1
        i += 1
    return n


def _lead_ws(s: str) -> str:
    return s[: len(s) - len(s.lstrip())]


def _trail_ws(s: str) -> str:
    return s[len(s.rstrip()) :]


def check(msg: Message, target_lang: str) -> list[str]:
    """번역문이 원문과 기계적으로 아귀가 맞는지. 어긋난 규칙 이름들을 돌려준다."""
    problems: list[str] = []
    src = msg.source
    for text in msg.translations:
        if not text.strip():
            continue   # 미번역은 여기서 다루지 않는다 (진행률은 따로 센다)

        if place_markers(text) != place_markers(src):
            problems.append("플레이스홀더(%1..%9) 불일치")
        if ("%n" in src) != ("%n" in text):
            problems.append("%n 누락/추가")
        if _lead_ws(text) != _lead_ws(src) or _trail_ws(text) != _trail_ws(src):
            problems.append("앞뒤 공백 불일치")
        if text.count("\n") != src.count("\n"):
            problems.append("개행 개수 불일치")
        if mnemonic_count(text) != mnemonic_count(src):
            problems.append("니모닉(&) 개수 불일치")
        if ";;" in src and text.count(";;") != src.count(";;"):
            problems.append("파일 필터 ';;' 개수 불일치")
        if src.count("*") != text.count("*") and ";;" in src:
            problems.append("파일 필터 글롭 '*' 개수 불일치")
        if sorted(_TAG.findall(src)) != sorted(_TAG.findall(text)):
            problems.append("HTML 태그 불일치")
        if _HANGUL.search(text):
            problems.append("번역문에 한글이 남아 있음")
        if src.strip() in IDENTITY_EXPECTED and text != src:
            problems.append("기호/단위는 원문 그대로여야 함")
        if len(text) > max(40, len(src) * 3):
            problems.append("번역이 원문의 3배를 넘음(레이아웃 위험)")
        if target_lang == "ja" and _HANGUL.search(text):
            problems.append("일본어 번역에 한글")

    return problems
