# 릴리스 절차

배포는 ZIP 하나다. 사용자는 그것을 원하는 폴더에 풀어 쓰고(포터블), 앱이 스스로
새 버전을 확인해 같은 자리에 교체한다.

## 최초 1회 준비

```powershell
winget install --id GitHub.cli
gh auth login          # 토큰은 Windows 자격 증명 관리자에 보관된다
```

토큰을 `-D` 인자나 파일로 넘기지 않는다 — `-D` 로 준 값은 `CMakeCache.txt` 에 평문으로
남고 셸 히스토리에도 남는다.

서명에는 `tools/CertWithEV.cmd` 와 `tools/SignotaurTool.exe` 가 필요하다. 두 파일은
자격 정보를 담고 있어 **저장소에 커밋하지 않는다**(`.gitignore` 로 막아 두었다).
없으면 패키징은 경고만 내고 계속하지만, 그렇게 만든 ZIP 은 배포할 수 없다 —
클라이언트가 설치 전에 서명을 확인하고 거부한다.

## 절차

### 1. 버전 올리기

`CMakeLists.txt` 3행 하나만 고친다. 여기서 태그 이름, ZIP 이름, 매니페스트 버전,
실행 파일 리소스의 FileVersion 이 전부 파생된다.

```cmake
project("MultiRoot-reST-CPP" VERSION 0.2.1 LANGUAGES CXX)
```

### 1-1. 번역 원본 갱신

문자열을 추가하거나 고쳤다면 패키징 **전에** 돌리고 **반드시 커밋한다**.

```powershell
cmake --build --preset update-translations
tools\uv.exe run --script tools\i18n\ts_lint.py
git add translations
```

이 타깃은 소스 트리의 `translations/*.ts` 를 고친다. 커밋하지 않고 패키징하면
아래 3단계의 워킹 트리 검사에 걸려 멈춘다. 번역 절차 전체는 `docs/I18N.md` 참고.

`.qm` 은 exe 안에 임베드되므로 배포물에 파일이 늘지 않는다 — 스테이징 목록도
windeployqt 인자도 건드릴 것이 없다.

### 2. 커밋하고 태그 붙이기

```powershell
git commit -am "VER : 0.2.1"
git tag -a v0.2.1 -m "v0.2.1"
git push origin main
git push origin v0.2.1
```

패키징 스크립트는 워킹 트리가 깨끗한지, 현재 커밋에 `v<버전>` 태그가 있는지 확인한다.
어긋나면 멈춘다 — 어떤 소스에서 나왔는지 알 수 없는 배포물을 만들지 않기 위해서다.
시험 삼아 만들어 볼 때만 `-DMRST_ALLOW_DIRTY=ON` 으로 넘긴다.

### 3. 빌드하고 패키징하기

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo
cmake --build --preset package
```

`package` 타깃이 하는 일:

1. 깨끗한 스테이징 폴더에 `MultiRoot-reST-CPP.exe` 와 `mrst_updater.exe` 만 복사
2. 그 exe 를 인자로 `windeployqt` 실행 (Qt DLL, 플러그인, WebEngine 리소스)
3. `tools/CertWithEV.cmd` 로 **우리 exe 2개만** 서명
   (Qt DLL 은 The Qt Company 서명이 이미 유효하고, `dxil.dll` 은 재서명하면 깨진다)
4. 있어야 할 파일 / 있으면 안 되는 파일 점검
5. ZIP 생성 (7z 가 있으면 멀티스레드, 없으면 CMake 내장)
6. sha256 · 크기 · 커밋 해시 · 엔트리 수 계산
7. `update-manifest.json` 생성
8. 심볼 ZIP (PDB + 그 시점 exe) 생성

빌드 디렉터리를 그대로 압축하지 않기 때문에, 런타임이 만드는 `Environment/`(238MB)
와 사용자 설정 `MultiRoot-reST-CPP.ini` 는 **구조적으로** 배포물에 들어올 수 없다.

이번 버전에서 사라진 파일이 있으면 구성 시점에 알려 준다. 업데이터는 이 목록에
적힌 것만 추가로 치우고 나머지는 건드리지 않는다.

```powershell
cmake --preset RelWithDebInfo -DMRST_REMOVALS="Qt6Lottie.dll;예전이름.dll"
```

### 4. 올리기

```powershell
cmake --build --preset release
```

에셋 3개(앱 ZIP, `update-manifest.json`, 심볼 ZIP)가 모두 있어야 진행한다.
**매니페스트 없는 릴리스가 latest 가 되면 그 순간부터 모든 사용자의 업데이트
확인이 404 를 맞는다.**

## 사용자 쪽에서 벌어지는 일

1. 앱 시작 5초 뒤, 마지막 확인으로부터 설정한 날짜가 지났으면
   `releases/latest/download/update-manifest.json` 을 받는다
   (GitHub API 가 아니라 이 고정 URL 을 쓴다 — API 는 비인증 60회/시간 제한이
   IP 단위라 사내 NAT 뒤에서 공유된다)
2. 새 버전이면 창 위쪽에 비모달 바로 알린다. **내려받기는 사용자가 누를 때만** 시작한다
3. 받은 ZIP 의 sha256 을 매니페스트와 대조 → `System32\tar.exe`(bsdtar)로 `.update/staging` 에 해제
4. 해제된 exe 2개의 Authenticode 서명과 FileVersion 확인
5. "지금 재시작" 을 누르면 앱이 저장 확인을 거쳐 닫히면서 `mrst_updater.exe` 를 띄운다
6. 업데이터가 앱 종료를 기다린 뒤 최상위 항목을 rename 으로 교체하고 앱을 다시 실행한다
7. 새 버전이 한 번 정상 기동하면 `.update/backup` 을 지운다 (여기가 롤백을 포기하는 지점)

## 업데이트가 잘못됐을 때

교체 직후 새 버전이 뜨지 않으면 백업이 아직 남아 있다. 되돌리는 방법:

```powershell
cd <설치폴더>
.\mrst_updater.exe --rollback --target "<설치폴더>" --backup "<설치폴더>\.update\backup" --log "<설치폴더>\.update\rollback.log"
```

무슨 일이 있었는지는 `<설치폴더>\.update\updater.log` 와 `.update\result.ini` 에 남는다.

## 첫 릴리스에서 주의할 점

- 정식(non-prerelease) 릴리스가 하나도 없으면 `latest/download` 는 404 다.
  클라이언트는 이를 "업데이트 없음" 으로 조용히 처리하지만, **0.2.0 은 반드시
  정식 릴리스로 올려야** 이후 버전이 보인다.
- 에셋 이름은 릴리스마다 바꾸지 않는다. 고정 URL 이 이름으로 만들어진다.
- pre-release 로 올린 릴리스는 `latest` 에 잡히지 않는다. 별도 인프라 없이
  시험 배포를 하고 싶을 때 쓸 수 있다.
