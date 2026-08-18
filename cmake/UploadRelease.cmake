# cmake -P 전용 스크립트. mrst_release 타깃이 -D 로 값을 넘긴다.
#
# 이 파일만 자격 정보를 다룬다. mrst_package 는 토큰을 전혀 알지 못한다.

cmake_minimum_required(VERSION 3.21)

include("${MRST_SOURCE_DIR}/cmake/MrstNames.cmake")

# 에셋 이름은 PackageRelease 가 만든 것과 한 글자도 달라서는 안 된다 —
# 같은 MrstNames 값에서 같은 방식으로 조립한다.
set(MRST_STAGE_NAME   "${MRST_ARCHIVE_BASENAME}-${MRST_VERSION}")
set(MRST_ZIP_NAME     "${MRST_STAGE_NAME}-win64.zip")
set(MRST_SYMBOLS_NAME "${MRST_STAGE_NAME}-win64-symbols.zip")

find_program(MRST_GH NAMES gh)
if(NOT MRST_GH)
    message(FATAL_ERROR
            "gh CLI 를 찾을 수 없다.\n"
            "  winget install --id GitHub.cli\n"
            "  gh auth login          # 토큰은 Windows 자격 증명 관리자에 보관된다\n"
            "\n"
            "토큰을 -D 인자나 환경변수로 넘기는 방법은 쓰지 않는다. -D 로 준 값은\n"
            "CMakeCache.txt 에 평문으로 남고 셸 히스토리에도 남는다.")
endif()

# 에셋이 하나라도 빠지면 올리지 않는다.
# 특히 매니페스트가 없는 릴리스가 latest 가 되면, 그 순간부터 전체 사용자의
# 업데이트 점검이 404 를 맞는다.
foreach(_asset "${MRST_ZIP_NAME}" "update-manifest.json" "${MRST_SYMBOLS_NAME}")
    if(NOT EXISTS "${MRST_PACKAGE_DIR}/${_asset}")
        message(FATAL_ERROR "에셋 누락: ${_asset}\n먼저 mrst_package 를 빌드한다.")
    endif()
endforeach()

# 태그는 로컬에서 annotated 로 만들어 push 해 둔다. gh 가 그 태그를 집어 쓰고
# --generate-notes 로 커밋에서 릴리스 노트를 만든다.
find_program(MRST_GIT NAMES git)
if(MRST_GIT)
    execute_process(COMMAND "${MRST_GIT}" rev-parse "v${MRST_VERSION}"
            WORKING_DIRECTORY "${MRST_SOURCE_DIR}"
            OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE _tagResult)
    if(NOT _tagResult EQUAL 0)
        message(FATAL_ERROR
                "v${MRST_VERSION} 태그가 없다.\n"
                "  git tag -a v${MRST_VERSION} -m \"v${MRST_VERSION}\"\n"
                "  git push origin v${MRST_VERSION}")
    endif()
endif()

message(STATUS "GitHub 릴리스 v${MRST_VERSION} 을 만들고 에셋 3개를 올린다.")
execute_process(
        COMMAND "${MRST_GH}" release create "v${MRST_VERSION}"
                --title "v${MRST_VERSION}" --generate-notes
                "${MRST_PACKAGE_DIR}/${MRST_ZIP_NAME}"
                "${MRST_PACKAGE_DIR}/update-manifest.json"
                "${MRST_PACKAGE_DIR}/${MRST_SYMBOLS_NAME}"
        WORKING_DIRECTORY "${MRST_SOURCE_DIR}"   # gh 가 origin 에서 저장소를 알아낸다
        RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gh release create 실패 (${_rc}).\n"
            "이미 같은 태그의 릴리스가 있다면 gh release upload --clobber 를 쓴다.")
endif()

message(STATUS "완료: https://github.com/${MRST_REPO_SLUG}/releases/tag/v${MRST_VERSION}")
