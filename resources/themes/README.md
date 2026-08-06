# Qlementine 테마

`dark.json` / `light.json` 은 [oclero/qlementine](https://github.com/oclero/qlementine)
`v1.4.2` 의 `showcase/resources/themes` 에 있는 공식 테마를 그대로 가져온 것이다
(MIT License, © Olivier Cléro).

우리가 바꾼 부분은 `"useSystemFonts": true` 하나다. 이 값이 `false`(qlementine 기본)면
번들된 Inter / Inter Display 를 쓰는데, 두 글꼴에 한글 글리프가 없어서 UI 문자열이
글리프 단위로 대체 글꼴로 떨어지고 자간·높이가 어긋난다. 시스템 글꼴(Segoe UI /
맑은 고딕)을 쓰면 기존 앱과 같은 글자 모양을 유지한다.

qlementine 태그를 올릴 때 `Theme` 의 JSON 스키마가 바뀌었는지 확인해야 한다.
읽지 못한 키는 조용히 무시되고 기본값(라이트 팔레트)이 남는다.
`QlementineTheme::install()` 은 `meta.name` 을 검사해 로드 실패를 잡아낸다.
