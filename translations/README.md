# translations/

| 파일 | 무엇 | 커밋 |
|---|---|---|
| `mrst_ko.ts` | 복수형 전용(원어). `qtbase_ko.qm` 의 운반체이기도 하다 | ✅ |
| `mrst_en.ts` | 영어 번역 원본 | ✅ |
| `mrst_ja.ts` | 일본어 번역 원본 | ✅ |
| `glossary.tsv` | 용어집. `ts_export.py` 가 모든 청크에 함께 실어 보낸다 | ✅ |
| `_work/` | LLM 왕복용 중간 파일 | ❌ (`.gitignore`) |
| `*.qm` | `lrelease` 산출물. 빌드 디렉터리에 생기고 exe 에 임베드된다 | ❌ |

절차와 규칙은 **`docs/I18N.md`** 에 있다.

`.ts` 는 `lupdate`/`lrelease`/Linguist 가 LF 로 쓴다. 이 저장소는
`core.autocrlf=true` 라 `.gitattributes` 에서 `eol=lf` 로 못박아 두었다 —
없으면 도구가 저장할 때마다 파일 전체가 "수정됨" 으로 뜬다.
