# 글꼴 렌더링 시험 도구

앱의 실제 Scintilla Qt 라이브러리와 Qt WebEngine을 사용하는 독립 시험 타깃이다.
빌드 디렉터리의 PlatQt 사본에만 마스크 보정을 삽입한다. 일반 앱 소스와 기본 렌더링 설정은 변경하지 않는다.

## 준비 조건

- Windows x64, MSVC, Qt 6.11.1 개발 환경
- 현재 앱 빌드의 RelWithDebInfo/ScintillaEditBaseQt.lib
- 현재 앱 빌드의 _deps/mv_scintilla_src-src 및 scintilla-core
- Python 3.12 이상. 별도 Python 패키지는 필요하지 않음

Qt GuiPrivate은 엔진 종류를 확인하는 시험 계측에만 사용한다. 설치 Qt와 기존 Scintilla 빌드는 동일한 버전·구성이어야 한다.
이 CMake 타깃은 현재 Windows/MSVC 작업 환경을 위한 도구이며, 다른 생성기의 라이브러리 위치는 미지원이다.

## 빌드·실행

저장소 루트에서 PowerShell로 실행한다. Qt와 Python 경로는 현재 작업 환경 기준이다.

```powershell
& 'D:/Qt/6.11.1/msvc2022_64/bin/qt-cmake.bat' -S tools/font-rendering-probe -B _build/font-rendering-probe/build -G 'Visual Studio 17 2022' -A x64 -DMRST_EXISTING_BUILD=D:/Dev/MultiRoot-reST-CPP/_build/RelWithDebInfo
cmake --build _build/font-rendering-probe/build --config RelWithDebInfo --parallel 4

& 'cmake-build-relwithdebinfo-msvc2022/Environment/.venv/Scripts/python.exe' -X utf8 tools/font-rendering-probe/run_probe.py --probe _build/font-rendering-probe/build/RelWithDebInfo/mrst_font_probe.exe --qt D:/Qt/6.11.1/msvc2022_64 --output _build/font-rendering-probe/results

& 'cmake-build-relwithdebinfo-msvc2022/Environment/.venv/Scripts/python.exe' -X utf8 tools/font-rendering-probe/analyze.py --results _build/font-rendering-probe/results --probe _build/font-rendering-probe/build/RelWithDebInfo/mrst_font_probe.exe --qt D:/Qt/6.11.1/msvc2022_64
```

run_probe.py는 모니터 DPR을 측정한 뒤 각 조건의 실제 100%·150% 배율을 산정한다.
각 프로세스에만 Qt 엔진, 마스크 보정, Chromium 옵션을 설정한다. 창은 화면에 표시하지 않고 캡처한다.
외부 페이지와 원격 글꼴을 사용하지 않는다. 레지스트리·Windows 글꼴 설정 변경과 MacType 설치·주입은 수행하지 않는다.

## 결과

- cases별 editor.png 또는 preview.png: 실제 화면
- cases별 metrics.json: 엔진, DPR, 캐럿 또는 DOM 위치, 그리기 비용
- runs.json: 29개 조건의 실행 결과
- summary.json: 픽셀·배치 비교와 이미지·SVG 영역 검사
- comparison.html: 이미지를 포함하는 단일 HTML 비교 화면

run_probe.py의 --cases 옵션으로 일부 조건만 실행할 수 있다. 이 경우 runs.json은 선택 조건으로 교체되므로 전체 analyze.py 실행에는 전체 조건 결과가 필요하다.

마스크 캐시 한도는 4 MiB이다. 캐시 초기 그리기와 반복 그리기를 구분한다.
캐럿 왕복 검사는 영문·한글 첫 두 행을 대상으로 하며, 행끝은 비 Close hit-test를 사용한다.
프리뷰에서 Range rect는 배치 검사에 사용한다. 실제 마우스 드래그 선택·IME 상호작용은 이 도구의 검사 범위가 아니다.

[실측 결과와 적용 판단](../../docs/FONT-RENDERING-EXPERIMENT.md)을 참조한다.

## 실제 문서 통합 시험

앱의 UI 시험에 실제 Sphinx 테마와 수식·다이어그램을 추가하려면 다음과 같이 실행한다.
Python 환경에 Sphinx와 sphinx-rtd-theme가 필요하다. 최초 준비 시 인터넷 연결이 필요하며,
KaTeX 0.18.4, MathJax 3.2.2, Mermaid 11.12.1과 폰트를 지정한 출력 폴더에 내려받는다.
다운로드 파일의 URL과 SHA-256은 출력 폴더의 assets.json에 기록한다.
시험 실행은 생성된 로컬 파일을 사용한다.

```powershell
& './cmake-build-relwithdebinfo-msvc2022/Environment/.venv/Scripts/python.exe' tools/font-rendering-probe/prepare_documents.py --output _build/rendering-followup/documents
$env:MRST_RENDERING_DOCUMENTS=(Resolve-Path _build/rendering-followup/documents).Path
$env:MRST_TEST_CLASS='TestPreviewFonts'
$env:QTWEBENGINE_CHROMIUM_FLAGS='--no-sandbox'
& './_build/RelWithDebInfo/RelWithDebInfo/mrst_ui_tests.exe' -o '_build/rendering-followup/preview.txt,txt'
```

Qt DLL 및 플랫폼 플러그인 경로는 기존 UI 시험 환경과 동일하게 설정한다.
MRST_RENDERING_DOCUMENTS가 없으면 외부 자료가 필요한 세 행만 건너뛴다.
Alabaster, Read the Docs 테마, Markdown에서 실제 렌더러의 생성 결과를 확인한 뒤
본문 획 보정, 글꼴 변경 병행, 특수 콘텐츠 및 제목 링크 아이콘 보존, 설정 해제 시 복원을 검사한다.
Chromium 감마·대비의 시각적 비교는 위의 별도 렌더링 프로브를 사용한다.

편집기의 textMaskCompositionAndScroll 시험은 밝은/어두운 배경과 버퍼 사용 여부를
조합해 한글 조합 이벤트, 취소·확정·실행 취소, 선택 범위, 캐럿 위치, 반복 스크롤을 확인한다.
OS의 실제 IME 후보 창 동작은 자동 조합 이벤트 시험의 검증 범위에 포함되지 않는다.
textMaskCacheEviction 시험은 캐시 용량과 교체 후 픽셀 재현성을 검사한다.
