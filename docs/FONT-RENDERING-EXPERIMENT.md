# I — 글꼴 렌더링 적용 검증

검증일: 2026-10-13. Qt 6.11.1, Chromium 140.0.7339.225, 현재 Scintilla Qt 포트 빌드를 사용하였다.
29개 조건에서 편집기 16개, 프리뷰 13개 결과를 확보하였다. 화면 비교 16개와 이미지·SVG 영역 비교 4개를 수행하였다.

## 적용 판정

| 대상과 방법 | 상태 | 결과 | 적용 판단 |
|---|---|---|---|
| 편집기: Qt FreeType | 실측 | 영문·한글·컬러 이모지 표시. 150%에서 글자 위치 변경 | 선택적 시험 모드 후보. 기본 적용 보류 |
| 편집기: 마스크 감마·농도 | 실측 | 캐럿 좌표·행 수 유지. 캐시로 반복 그리기 비용 완화 | 회색조 렌더링 수용 시 후보 |
| 프리뷰: Chromium 감마·대비 | 실측 | 화면 변경, DOM 위치·줄바꿈 유지 | 우선 적용 후보 |
| 프리뷰: CSS text-stroke | 실측 | 화면 변경, DOM 위치·줄바꿈 유지 | 문서별 농도 조정 후보 |
| 프리뷰: Qt FreeType 선택 | 실측 | 기본 프리뷰와 픽셀·배치 동일 | 프리뷰 엔진 전환에는 적용되지 않음 |
| 프리뷰: font-smoothing | 실측 | 이 Windows 환경에서 픽셀 변화 없음 | 효과 있는 설정으로 제공하지 않음 |
| MacType DLL 직접 주입 | 미검증 | MacType 미설치 환경. 본 실험에 DLL 주입 미포함 | 별도 프로파일·다중 프로세스 검증 필요 |

[화면 비교](../_build/font-rendering-probe/results/comparison.html)에서 조건 선택, 경계 이동, 확대가 가능하다.
실제 캡처의 픽셀 차이는 품질 향상의 점수가 아니다. 최적값은 글꼴·배율·테마에 따른 시각 비교로 결정한다.

## 편집기 결과

실제 앱의 Scintilla 정적 라이브러리를 사용하였다. 앱의 PlatQt 소스를 수정하지 않고 빌드 디렉터리 사본에 텍스트 그리기 실험 경로를 삽입하였다.
Qt의 글꼴 대체·조합 처리를 유지하고 QRawFont의 엔진 종류로 DirectWrite·FreeType 전환을 확인하였다.
엔진 선택 경로는 [Qt 6.11.1 Windows integration 소스](https://raw.githubusercontent.com/qt/qtbase/v6.11.1/src/plugins/platforms/windows/qwindowsintegration.cpp)에 명시되어 있다.

### 그리기 비용

960×360 logical viewport의 반복 grab 40회 중앙값이다. 실제 앱 전체 프레임 시간과 타이핑 지연은 미측정이다.
캐시 초기 값은 Qt 글꼴 캐시를 유지하고 마스크 캐시만 비운 후의 1회 값이다. 반복 경로의 작은 수치 차이만으로 성능 향상을 주장하지 않는다.

| 배율·조건 | 기본 반복 | 마스크 캐시 초기 | 마스크 캐시 반복 | FreeType 반복 |
|---|---:|---:|---:|---:|
| 100%, 반복 행 | 2.90 ms | 3.58 ms | 2.73 ms | 18.66 ms |
| 100%, 서로 다른 행 | 3.10 ms | 5.06 ms | 2.83 ms | 미측정 |
| 150%, 반복 행 | 4.37 ms | 5.80 ms | 3.85 ms | 19.34 ms |

FreeType 비용은 기본의 약 4.4~6.4배였다. 앱의 등폭 폭 캐시를 비활성화한 대조군에서도 20.87 ms를 측정하였다.
비용 증가를 기존 폭 캐시만의 문제로 분류하지 않는다.
100%에서는 비교한 캐럿 좌표가 동일하고, 150%에서는 FreeType과 DirectWrite의 위치가 일부 달랐다. 각 엔진 내부의 위치→클릭 판정 왕복은 통과하였다.
마스크 경로는 100%·150%와 밝은 테마·어두운 테마에서 비교한 캐럿 좌표와 행 수가 동일하였다.

### 마스크 보정 제약

alpha coverage에 `min(1, strength × alpha^(1/gamma))`를 적용한다. 시험값은 gamma 1.2, strength 1.1, 캐시 한도는 4 MiB이다.
이 gamma 값은 MacType DirectWrite gamma 값과 수치상 동등하지 않다.

투명 이미지 중간 렌더링으로 LCD 서브픽셀 색상이 회색조로 변환되었다.
영문 첫 행의 RGB 성분이 다른 픽셀은 기본 1,423개, identity 마스크 0개였다. gamma 1.0·strength 1.0도 기본과 동일한 이미지가 아니다.
컬러 이모지는 캡처에서 표시를 유지하였다. 회색조 텍스트 보정과 RGB LCD 보정은 구분한다.

RGB LCD 보존에는 별도 글리프 마스크·배경 합성 경로가 필요하다. 현재 alpha 마스크를 기본과 동일한 대체로 제공하지 않는다.
부분 다시 그리기, 선택 배경, IME 조합, 모든 lexer 스타일과 장시간 캐시 사용은 후속 검사 범위이다.
Qt FreeType 선택은 프로세스 전체의 시작 옵션으로, 편집기만의 실시간 전환 기능과 구분한다.

## 프리뷰 결과

실제 QWebEngineView에서 로컬 HTML을 렌더링하였다. Markdown과 Sphinx는 같은 WebEngine을 사용하므로 엔진 적용 가능성은 공통이다.
실제 Sphinx 테마·KaTeX·Mermaid 문서 전체의 품질은 미측정이다.

다음 조건에서 화면 차이를 확인하였다.

- `--text-gamma=1.0 --text-contrast=0.0`
- `--text-gamma=2.2 --text-contrast=0.5`
- CSS `-webkit-text-stroke: 0.15px currentColor`

감마·대비 변경은 100%·150% 및 어두운 테마에서도 DOM 위치와 줄바꿈을 유지하였다.
이미지·SVG 단색 영역 4개 비교의 변경 픽셀은 0개였다. 복잡한 SVG 텍스트와 수식까지 일반화하지 않는다.
Consolas의 font-weight 500은 기본 400과 픽셀이 같았고, 700은 화면을 변경하였다. 굵기 효과는 글꼴의 지원 범위에 따른다.

설치된 Chromium의 `skia/ext/legacy_display_globals.cc`는 명령행 gamma·contrast를 SkSurfaceProps에 우선 반영한다.
내부 시험 옵션이므로 Qt·Chromium 갱신 시 재검증이 필요하다.
소스 위치는 `D:/Qt/6.11.1/Src/qtwebengine/src/3rdparty/chromium/skia/ext/legacy_display_globals.cc`이다.

실제 앱의 `src/main.cpp:38`은 `QTWEBENGINE_CHROMIUM_FLAGS`를 고정 문자열로 덮어쓴다.
적용에는 첫 WebEngine 초기화 전에 기존 플래그를 보존하며 설정값을 병합하는 변경이 필요하다.
감마·대비는 WebEngine 프로세스 전체에 적용된다. Markdown과 reST에 각각 다른 값을 지정하는 방법과 구분한다.
문서별 조정에는 CSS를 사용하고, 수식·아이콘·SVG·이미지를 제외하는 적용 범위를 검사해야 한다.

## MacType와의 관계

[MacType DirectWrite 소스](https://raw.githubusercontent.com/snowie2000/mactype/directwrite/directwrite.cpp)는 gamma, contrast, ClearType level, pixel geometry, grid-fit과 DirectWrite 후킹을 사용한다.
[MacType README](https://github.com/snowie2000/mactype)에는 FreeType와 별도 DirectWrite 매개변수 조절이 명시되어 있다.
본 실험은 해당 목적에 대응하는 Qt·Chromium 경로의 결과이며, MacType 프로파일을 동일하게 재현한 결과가 아니다.

MacType 코드 직접 편입은 [GPL-3.0 라이선스](https://github.com/snowie2000/mactype/blob/directwrite/LICENSE)에 맞는 배포 검토가 필요하다.
현재 실험 코드에는 MacType 코드를 복사하지 않았다.
DLL 주입 검증에는 별도 배포본, 프로파일, QtWebEngineProcess의 다중 프로세스·sandbox 조건을 포함한다. 효과를 현재 결과로 확정하지 않는다.

## 다음 적용 단계

1. 프리뷰 감마·대비 설정: 선택 해제 가능한 옵션과 시작 시 플래그 병합을 구현하고, 실제 Markdown/reST 문서를 검사한다.
2. 편집기 회색조 마스크 설정: 선택 해제 가능한 시험 모드로 구현하고, 선택·IME·부분 다시 그리기·캐시 한도를 검사한다.
3. FreeType 또는 RGB LCD 경로: 별도 선택 옵션으로 유지하며, UI 전체 영향과 성능 개선 후 기본 적용을 판단한다.

[실험 코드·재현 명령](../tools/font-rendering-probe/README.md), [원시 결과](../_build/font-rendering-probe/results/runs.json), [픽셀·배치 검증](../_build/font-rendering-probe/results/summary.json)을 참조한다.

EOD
