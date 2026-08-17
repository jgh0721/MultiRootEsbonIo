# cmake -P 전용 스크립트. mrst_package 타깃이 -D 로 값을 넘긴다.
#
# 스테이징 -> 서명 -> 검증 -> ZIP -> 해시 -> 매니페스트 -> 심볼 ZIP 을 한 번에 한다.
# add_custom_command 여러 개로 쪼개지 않는 이유는 단계 사이의 의존이 파일이 아니라
# **순서** 이기 때문이다 — 서명은 exe 바이트를 바꾸므로 sha256 은 서명 뒤에, 매니페스트는
# ZIP 뒤에 계산해야 한다. 스크립트 모드라 실패했을 때 재구성 없이 단독으로 다시 돌릴 수 있다.

cmake_minimum_required(VERSION 3.21)

include("${MRST_SOURCE_DIR}/cmake/MrstDeployFlags.cmake")

# ── 0. 가드 ───────────────────────────────────────────────
# 멀티 구성 제너레이터에서는 구성 시점에 빌드 타입을 알 수 없으므로
# 빌드 시점에 $<CONFIG> 로 받아 여기서 판단한다.
if(MRST_CONFIG STREQUAL "Debug")
    message(FATAL_ERROR "패키징은 RelWithDebInfo/Release 에서만 한다 (현재: ${MRST_CONFIG}).")
endif()

set(MRST_REPO_SLUG "jgh0721/MultiRootEsbonIo")
set(MRST_PRODUCT   "MultiRoot-reST-CPP")

set(MRST_STAGE_NAME   "${MRST_PRODUCT}-${MRST_VERSION}")
set(MRST_STAGE_DIR    "${MRST_PACKAGE_DIR}/${MRST_STAGE_NAME}")
set(MRST_ZIP_NAME     "${MRST_STAGE_NAME}-win64.zip")
set(MRST_SYMBOLS_NAME "${MRST_STAGE_NAME}-win64-symbols.zip")
set(MRST_ZIP          "${MRST_PACKAGE_DIR}/${MRST_ZIP_NAME}")
set(MRST_SYMBOLS_ZIP  "${MRST_PACKAGE_DIR}/${MRST_SYMBOLS_NAME}")

get_filename_component(MRST_EXE_NAME     "${MRST_APP_EXE}" NAME)
get_filename_component(MRST_UPDATER_NAME "${MRST_UPDATER_EXE}" NAME)

find_program(MRST_GIT NAMES git)

# 바이너리와 태그가 어긋난 릴리스를 막는다. 급할 때는 -DMRST_ALLOW_DIRTY=ON 으로 넘긴다.
if(MRST_GIT AND NOT MRST_ALLOW_DIRTY)
    execute_process(COMMAND "${MRST_GIT}" status --porcelain
            WORKING_DIRECTORY "${MRST_SOURCE_DIR}"
            OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _dirty STREQUAL "")
        message(FATAL_ERROR
                "커밋하지 않은 변경이 있다. 이 상태로 만든 배포물은 어떤 소스에서 나왔는지 알 수 없다.\n"
                "그래도 진행하려면 -DMRST_ALLOW_DIRTY=ON 을 준다.\n${_dirty}")
    endif()

    execute_process(COMMAND "${MRST_GIT}" describe --tags --exact-match
            WORKING_DIRECTORY "${MRST_SOURCE_DIR}"
            OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _tagResult)
    if(NOT _tagResult EQUAL 0 OR NOT _tag STREQUAL "v${MRST_VERSION}")
        message(FATAL_ERROR
                "현재 커밋에 v${MRST_VERSION} 태그가 없다 (찾은 값: '${_tag}').\n"
                "  git tag -a v${MRST_VERSION} -m \"v${MRST_VERSION}\"\n"
                "그래도 진행하려면 -DMRST_ALLOW_DIRTY=ON 을 준다.")
    endif()
endif()

# ── 1. 깨끗한 스테이징 ────────────────────────────────────
# 빌드 디렉터리를 그대로 쓰지 않고 새로 깐다. 그래서 Environment/ (런타임이
# 만드는 238MB 파이썬 환경), *.ini (사용자 설정), *.pdb, _deps/, *_autogen/,
# mrst_tests.exe 는 "제외 규칙" 없이도 들어올 수 없다.
message(STATUS "[1/8] 스테이징: ${MRST_STAGE_DIR}")
file(REMOVE_RECURSE "${MRST_STAGE_DIR}")
file(MAKE_DIRECTORY "${MRST_STAGE_DIR}")
file(COPY "${MRST_APP_EXE}" "${MRST_UPDATER_EXE}" DESTINATION "${MRST_STAGE_DIR}")

# ── 2. Qt 런타임 ──────────────────────────────────────────
# --dir 을 주지 않고 "스테이징에 복사한 exe" 를 인자로 준다. windeployqt 는 대상
# exe 의 디렉터리에 배치하므로, 결과 배치가 매일 실행해 검증하고 있는 개발 빌드
# 디렉터리와 정확히 같아진다.
message(STATUS "[2/8] windeployqt")
execute_process(
        COMMAND "${MRST_WINDEPLOYQT}" ${MRST_WINDEPLOYQT_ARGS} --release
                "${MRST_STAGE_DIR}/${MRST_EXE_NAME}"
        RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "windeployqt 실패 (${_rc})")
endif()

# ── 3. 디지털 서명 ────────────────────────────────────────
# 우리가 빌드한 exe 2개만 서명한다.
#  - Qt DLL 은 The Qt Company 서명이 이미 유효하다. 덮어쓰면 보안 이득 없이
#    게시자만 바뀌고, 원격 서명 서버 왕복이라 40개면 분 단위가 걸린다.
#  - dxil.dll 은 Microsoft 서명 자체가 DXIL 검증에 쓰이므로 재서명하면 안 된다.
# 반드시 ZIP/해시 전에 한다 (서명은 파일 바이트를 바꾼다).
set(MRST_SIGN_CMD "${MRST_SOURCE_DIR}/tools/CertWithEV.cmd")
if(EXISTS "${MRST_SIGN_CMD}")
    foreach(_name "${MRST_EXE_NAME}" "${MRST_UPDATER_NAME}")
        message(STATUS "[3/8] 서명: ${_name}")
        execute_process(COMMAND cmd /c "${MRST_SIGN_CMD}" "${MRST_STAGE_DIR}/${_name}"
                RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "코드 서명 실패 (${_rc}): ${_name}\n"
                    "서명 없는 배포본은 만들지 않는다 — 클라이언트가 설치 전에 서명을 확인한다.")
        endif()
    endforeach()
else()
    message(WARNING "[3/8] tools/CertWithEV.cmd 가 없어 서명을 건너뛴다.\n"
            "이 패키지는 배포용으로 쓸 수 없다 (클라이언트가 서명 확인에서 거부한다).")
endif()

# ── 4. 배포물 점검 ────────────────────────────────────────
# windeployqt 가 조용히 빠뜨리면 사용자 PC 에서만 죽는다. 반대로 들어가면 안 되는
# 것이 들어가면 사용자 설정을 덮어쓴다. 양쪽 다 여기서 잡는다.
message(STATUS "[4/8] 배포물 점검")
foreach(_required
        "${MRST_EXE_NAME}" "${MRST_UPDATER_NAME}"
        QtWebEngineProcess.exe Qt6WebEngineCore.dll Qt6Network.dll opengl32sw.dll
        platforms/qwindows.dll tls/qschannelbackend.dll resources/qtwebengine_resources.pak)
    if(NOT EXISTS "${MRST_STAGE_DIR}/${_required}")
        message(FATAL_ERROR "배포물 누락: ${_required}")
    endif()
endforeach()
foreach(_forbidden Environment "${MRST_PRODUCT}.ini" mrst_tests.exe "${MRST_PRODUCT}.pdb" .update)
    if(EXISTS "${MRST_STAGE_DIR}/${_forbidden}")
        message(FATAL_ERROR "패키지에 들어가면 안 되는 항목: ${_forbidden}")
    endif()
endforeach()

# ── 5. ZIP ────────────────────────────────────────────────
# 7z 는 멀티스레드라 400MB 페이로드에서 차이가 크다. 없으면 CMake 내장으로
# 폴백한다(다른 개발자 환경). 둘 다 표준 deflate ZIP 을 만든다 — LZMA/.7z 는
# 쓰지 않는다. 클라이언트가 System32 bsdtar 로 푸는데 그것은 deflate 만 안전하다.
message(STATUS "[5/8] ZIP: ${MRST_ZIP_NAME}")
file(REMOVE "${MRST_ZIP}")           # 7z a 는 append 라 반드시 먼저 지운다
find_program(MRST_7Z NAMES 7z HINTS "D:/Utils")
if(MRST_7Z)
    execute_process(COMMAND "${MRST_7Z}" a -tzip -mmt=on -bso0 "${MRST_ZIP_NAME}" "${MRST_STAGE_NAME}"
            WORKING_DIRECTORY "${MRST_PACKAGE_DIR}" RESULT_VARIABLE _rc)
else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${MRST_ZIP_NAME}" --format=zip "${MRST_STAGE_NAME}"
            WORKING_DIRECTORY "${MRST_PACKAGE_DIR}" RESULT_VARIABLE _rc)
endif()
if(NOT _rc EQUAL 0 OR NOT EXISTS "${MRST_ZIP}")
    message(FATAL_ERROR "ZIP 생성 실패 (${_rc})")
endif()

# ── 6. 무결성 값 ──────────────────────────────────────────
message(STATUS "[6/8] 해시와 메타데이터")
file(SHA256 "${MRST_ZIP}" MRST_ZIP_SHA256)
file(SIZE   "${MRST_ZIP}" MRST_ZIP_SIZE)
string(TIMESTAMP MRST_RELEASED_AT "%Y-%m-%dT%H:%M:%SZ" UTC)

set(MRST_COMMIT "unknown")
if(MRST_GIT)
    execute_process(COMMAND "${MRST_GIT}" rev-parse --short HEAD
            WORKING_DIRECTORY "${MRST_SOURCE_DIR}"
            OUTPUT_VARIABLE MRST_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

# entryCount 는 클라이언트가 압축 해제 진행률을 계산하는 데 쓴다. 파일 수를 세는
# 대신 실제로 풀 프로그램(System32 bsdtar)에게 물어본다 — 디렉터리 엔트리까지
# 정확히 같은 수가 나온다.
set(MRST_ENTRY_COUNT 0)
find_program(MRST_BSDTAR NAMES tar.exe PATHS "$ENV{SystemRoot}/System32" NO_DEFAULT_PATH)
if(MRST_BSDTAR)
    execute_process(COMMAND "${MRST_BSDTAR}" -tf "${MRST_ZIP}"
            OUTPUT_VARIABLE _entries OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND NOT _entries STREQUAL "")
        string(REPLACE "\n" ";" _entryList "${_entries}")
        list(LENGTH _entryList MRST_ENTRY_COUNT)
    endif()
endif()
if(MRST_ENTRY_COUNT LESS 1)
    message(WARNING "엔트리 수를 셀 수 없어 1 로 둔다. 진행률 표시만 거칠어진다.")
    set(MRST_ENTRY_COUNT 1)
endif()

# 이번 버전에서 사라진 파일. 세미콜론으로 구분해 넘긴다:
#   cmake --build . --target mrst_package -- -D MRST_REMOVALS="Qt6Lottie.dll;old.dll"
# 업데이터는 이 목록에 있는 것만 추가로 치우고 나머지는 건드리지 않는다.
set(MRST_REMOVALS_JSON "")
if(MRST_REMOVALS)
    foreach(_name ${MRST_REMOVALS})
        if(MRST_REMOVALS_JSON STREQUAL "")
            set(MRST_REMOVALS_JSON "\"${_name}\"")
        else()
            set(MRST_REMOVALS_JSON "${MRST_REMOVALS_JSON}, \"${_name}\"")
        endif()
    endforeach()
endif()

# 이 버전보다 낮은 설치본은 제자리 교체를 지원하지 않는다는 뜻이다. 배치나 설정
# 형식이 깨지는 릴리스에서만 올린다.
if(NOT MRST_MIN_FROM_VERSION)
    set(MRST_MIN_FROM_VERSION "0.2.0")
endif()

# ── 7. 매니페스트 ─────────────────────────────────────────
# 앱은 releases/latest/download/update-manifest.json 이라는 고정 URL 로 이 파일을 받는다.
message(STATUS "[7/8] update-manifest.json")
configure_file("${MRST_SOURCE_DIR}/cmake/update-manifest.json.in"
        "${MRST_PACKAGE_DIR}/update-manifest.json" @ONLY)

# ── 8. 심볼 ZIP ───────────────────────────────────────────
# PDB 는 바로 그 바이너리와 1:1 이라(재빌드하면 GUID 가 달라진다) 지금 챙기지
# 않으면 영구히 잃는다. 반면 99MB 라 앱 ZIP 에 넣으면 모든 사용자가 매 업데이트마다
# 받게 되므로 별도 에셋으로 올리고 매니페스트에는 싣지 않는다.
message(STATUS "[8/8] 심볼 ZIP: ${MRST_SYMBOLS_NAME}")
set(_symbolDir "${MRST_PACKAGE_DIR}/symbols")
file(REMOVE_RECURSE "${_symbolDir}")
file(MAKE_DIRECTORY "${_symbolDir}")
if(EXISTS "${MRST_APP_PDB}")
    file(COPY "${MRST_APP_PDB}" DESTINATION "${_symbolDir}")
endif()
# 대조용으로 그 시점의 exe 도 함께 넣는다.
file(COPY "${MRST_STAGE_DIR}/${MRST_EXE_NAME}" DESTINATION "${_symbolDir}")
file(REMOVE "${MRST_SYMBOLS_ZIP}")
if(MRST_7Z)
    execute_process(COMMAND "${MRST_7Z}" a -tzip -mmt=on -bso0 "${MRST_SYMBOLS_NAME}" symbols
            WORKING_DIRECTORY "${MRST_PACKAGE_DIR}" RESULT_VARIABLE _rc)
else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${MRST_SYMBOLS_NAME}" --format=zip symbols
            WORKING_DIRECTORY "${MRST_PACKAGE_DIR}" RESULT_VARIABLE _rc)
endif()
file(REMOVE_RECURSE "${_symbolDir}")

math(EXPR _zipMB "${MRST_ZIP_SIZE} / 1048576")
message(STATUS "")
message(STATUS "패키지 완료: ${MRST_ZIP_NAME} (${_zipMB} MB, 엔트리 ${MRST_ENTRY_COUNT} 개, ${MRST_COMMIT})")
message(STATUS "  ${MRST_PACKAGE_DIR}")
message(STATUS "다음: cmake --build <빌드디렉터리> --target mrst_release")
