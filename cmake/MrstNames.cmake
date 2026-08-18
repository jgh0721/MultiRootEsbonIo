# 이름의 단일 원천. CMakeLists.txt 와 cmake -P 스크립트(PackageRelease / UploadRelease)가
# 함께 include 한다.
#
# 이름을 두 벌로 적으면 한쪽만 고쳐지고, 그 사고는 **빌드를 통과한 뒤 배포본에서만**
# 드러난다. 실제로 그런 자리가 있었다 — PackageRelease.cmake 가 자기만의 제품 이름
# 리터럴을 들고 있어서, project() 이름을 바꾸면 신 버전이 자기 매니페스트를
# "다른 제품" 으로 거부하게 되어 있었다.

# ── 빌드 타깃 이름 ────────────────────────────────────────
# 공백을 쓸 수 없다. CMake 는 타깃 이름에 공백을 허용하지 않는다
# ("The target name ... is reserved or not valid for certain CMake features,
#  such as generator expressions"). 그래서 타깃 이름은 하이픈으로 두고,
# 배포 파일 이름만 OUTPUT_NAME 으로 따로 준다.
set(MRST_APP_TARGET         "MultiRoot-reST-Editor")     # = PROJECT_NAME
set(MRST_UPDATER_TARGET     "MultiRoot-reST-Updater")

# ── 배포 파일 이름 (공백을 쓴다) ──────────────────────────
# OUTPUT_NAME 은 .pdb 이름까지 따라간다. PackageRelease 의 금지 목록이 pdb 를
# 이름으로 찾으므로, 그 목록은 이 값이 아니라 실제 TARGET_PDB_FILE 에서 유도한다.
set(MRST_APP_BASENAME       "MultiRoot-reST Editor")
set(MRST_UPDATER_BASENAME   "MultiRoot-reST Updater")

# ── 사용자 설정 파일 ──────────────────────────────────────
# 실행 파일 이름과 맞춘다. 구 이름은 기동 시 한 번 복사해 오는 마이그레이션의
# 원본이고(AppSettings::migrateLegacyFile), 패키징 금지 목록에도 함께 남긴다 —
# 개발 폴더에 굴러다니는 구 ini 가 배포본에 섞이는 것을 계속 막아야 한다.
set(MRST_SETTINGS_INI        "MultiRoot-reST Editor.ini")
set(MRST_LEGACY_SETTINGS_INI "MultiRoot-reST-CPP.ini")

# ── 배포 아카이브 이름 (공백을 쓰지 않는다) ───────────────
# 매니페스트의 다운로드 URL 이 이 이름으로 조립되는데(update-manifest.json.in),
# 생성 쪽에 퍼센트 인코딩이 없다. 게다가 GitHub 릴리스는 올린 에셋 이름의 공백을
# 다른 문자로 바꾸므로, 공백을 넣으면 URL 과 실제 에셋 이름이 어긋나 404 가 된다.
# rootDir 은 업데이터가 한 겹 벗기는 ZIP 안의 실제 폴더 이름이라 함께 맞춘다.
set(MRST_ARCHIVE_BASENAME   "MultiRoot-reST-Editor")

# ── 자동 업데이트 제품 식별자 ─────────────────────────────
# **이미 배포된 클라이언트와의 와이어 계약이다.** 매니페스트의 product 필드와
# 클라이언트의 기대값(mrst_version.h 의 MRST_UPDATE_PRODUCT_ID)이 한 글자라도
# 다르면 그 클라이언트는 매니페스트를 "다른 제품" 으로 보고 파싱을 중단한다
# (solUpdateManifest.cpp 의 expectedProduct 검사).
#
# 0.4.0 에서 이 값을 일부러 갈았다. 실행 파일 이름이 바뀌었고, 0.3.x 는 스테이징에서
# **자기 exe 이름**을 찾아 검증하므로(solUpdateService.cpp) 제자리 교체가 원리적으로
# 불가능하다. 구버전이 166MB 를 받아 놓고 검증에서 실패하는 것보다, 애초에 자기
# 제품이 아니라고 보게 하는 편이 낫다 — 그 대신 구버전은 수동 재설치가 필요하다.
# 다음에 이 값을 바꿀 때도 같은 대가를 치른다는 것을 알고 바꿀 것.
set(MRST_UPDATE_PRODUCT_ID  "MultiRoot-reST-Editor")

# 저장소 슬러그. 같은 값이 src/core/solUpdateManifest.cpp 의 kManifestOwner /
# kManifestRepo 에도 있다(그쪽은 클라이언트가 매니페스트를 받는 기본 URL 이라
# 생성 헤더에 의존할 수 없다). 저장소를 옮기면 둘 다 고친다.
set(MRST_REPO_SLUG          "jgh0721/MultiRootEsbonIo")

# 위 두 값에는 공백을 쓸 수 없다. 아카이브 이름은 매니페스트의 다운로드 URL 에
# 그대로 들어가고(퍼센트 인코딩이 없다), product 는 User-Agent 의 product 토큰으로도
# 쓰이는데 그 자리에는 공백을 넣을 수 없다(RFC 9110). 주석이 아니라 검사로 막는다.
foreach(_mrstNoSpace MRST_ARCHIVE_BASENAME MRST_UPDATE_PRODUCT_ID)
    if("${${_mrstNoSpace}}" MATCHES "[ \t]")
        message(FATAL_ERROR
                "${_mrstNoSpace} 에는 공백을 쓸 수 없다 (매니페스트 URL / product 계약): "
                "${${_mrstNoSpace}}")
    endif()
endforeach()
unset(_mrstNoSpace)
