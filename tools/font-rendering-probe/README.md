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
