# 데모 캡처

README 에 실리는 스크린샷(`docs/images`)과 동작 영상(`docs/media`)을 다시 만드는 스크립트다.
찍는 대상은 이 저장소의 `docs/demo` 워크스페이스다.

```powershell
# 한 장만
tools\demo\Capture-Demo.ps1 -Scene overview -Theme dark -Lang ko

# 스크린샷 전부 (약 7분)
tools\demo\Capture-Demo.ps1 -Shots

# 영상 전부 (약 7분)
tools\demo\Capture-Demo.ps1 -Videos
```

**도는 동안 화면을 그대로 찍는다.** 다른 창을 띄우거나 마우스·키보드를 건드리면 그대로
들어간다. 씬마다 앱을 새로 띄우고 닫으므로 중간에 손대지 말 것.

필요한 것은 빌드된 실행 파일과 `ffmpeg` 뿐이다. 실행 파일은
`cmake-build-relwithdebinfo-msvc2022` → `_build\RelWithDebInfo` → `cmake-build-debug-msvc2022`
순으로 찾는다(`-Exe` 로 지정할 수 있다). **이미 `Environment/` 가 만들어져 있는 디렉터리**를
쓰는 편이 좋다. 깨끗한 폴더에 복사해 두고 찍으면 첫 실행에서 파이썬 런타임 238MB 를 새로 받는다.

## 어떻게 같은 그림이 매번 나오는가

앱은 **인자 없이 실행되면** `<워크스페이스>/.multiroot/workspace.json` 을 읽어 열 문서, 캐럿
위치, 스플리터 배치를 되살린다(`MainWindow::restoreLastSession`). 스크립트는 씬마다 그 파일을
먼저 써 두고 앱을 띄운다. 그래서 탭을 열거나 창을 나누는 마우스 조작이 필요 없다.

설정은 실행 파일 옆 ini 하나이므로, 테마·언어·워크스페이스 경로도 파일을 갈아 끼우는 것으로
정한다. 원래 ini 는 백업했다가 끝나면 되돌린다.

기동이 끝났는지는 `MRST_PHASE_TRACE` 가 남기는 `phase.ready.end` 로 판정하고, 그 뒤
`-SettleSeconds`(기본 25초) 를 더 기다린다. **구문 강조의 3-state 는 언어 서버가 지시어
목록을 준 뒤에야 확정되므로** 이 대기를 줄이면 지시어가 전부 "판단 보류" 색으로 찍힌다.

## 손대야 할 때 알아 둘 것

- **창 크기는 안정화가 끝난 뒤에 고정한다.** 탭이 열리는 동안 레이아웃이 최소 폭을 다시
  요구해 창이 커지므로, 기동 직후에 잡아 두면 덮인다.
- **스크립트를 고칠 때 UTF-8 BOM 을 유지할 것.** Windows PowerShell 5.1 은 BOM 이 없으면
  파일을 CP949 로 읽는다. 한글이 든 경로 리터럴이 깨져 그 파일만 조용히 열리지 않는다.
- **이 프로세스는 DPI 를 인식한다**(`SetProcessDpiAwarenessContext`). 인식하지 않으면
  `SetWindowPos` 에 준 크기가 배율로 가상화되는데 DWM 이 돌려주는 실제 경계는 물리 픽셀이라,
  요청한 크기와 찍히는 크기가 어긋난다.
- **찍히는 그림은 요청보다 16×8 픽셀쯤 작다.** 창 사각형에 보이지 않는 리사이즈 테두리가
  들어 있기 때문이다. 그만큼 키워 재요청하면 앱이 논리 픽셀 경계로 반올림하면서 값이 널을 뛴다.
  몇 픽셀 차이는 그냥 둔다.
- **`Ctrl+Tab` 으로는 탭이 넘어가지 않는다.** 찍어 보면 109 프레임이 전부 같은 그림이다.
  탭은 좌표로 누른다 — 그래서 탭 x 좌표가 파일 이름 길이(=언어)에 따라 다르다.
  하단 패널의 탭도 마찬가지다(`진단` 대 `Diagnostics`).
- **커서는 찍지 않는다.** gdigrab 이 그리는 커서가 흰 사각형으로 깨져 나온다.
- 자동완성 팝업은 별개의 최상위 창이라 창 캡처로는 잡히지 않는다. 그래서 창이 아니라
  **화면 영역**을 찍는다. 캐럿이 창 아래쪽에 있으면 팝업이 화면 밖으로 잘리므로, 타이핑은
  화면 위쪽 1/3 지점에서 시작한다.

## 씬 목록

| 씬 | 산출물 |
|---|---|
| `overview` | `docs/images/overview-<테마>-<언어>.png` |
| `multiroot` | `docs/images/multiroot-<테마>-<언어>.png` |
| `virtual-project` | `docs/images/virtual-project-<테마>-<언어>.png` |
| `completion` | `docs/images/completion-directive-*.png`, `completion-path-*.png` |
| `video-tabs` | `docs/media/multiroot-<언어>.apng` |
| `video-scroll` | `docs/media/scroll-sync-<언어>.apng` |
| `video-completion` | `docs/media/completion-<언어>.apng` |

## 뒷정리

씬이 도는 동안 `docs/demo` 아래에 `.multiroot/` 와 각 프로젝트의 `_build/` 가 생긴다. 둘 다
`.gitignore` 에 있으므로 커밋에는 올라오지 않는다. 다만 **`mrst_package` 는 추적되지 않는
파일이 있으면 릴리스를 멈추므로**, 릴리스 직전에는 `git status` 를 한 번 보는 편이 좋다.
