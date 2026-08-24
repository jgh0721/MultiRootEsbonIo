include_guard(GLOBAL)
include(FetchContent)

# oclero/qlementine — 모던 데스크톱 Qt 앱용 QStyle.
# Qt 6.8 이상 / CMake 3.21 이상 / C++17 을 요구한다 (이 프로젝트는 Qt 6.11 + C++20).
#
# 릴리스 태그를 쓰지 않는다. 상류의 개발은 `dev` 브랜치에서 이뤄지고 `master`
# (=최신 태그 v1.4.2) 는 한참 뒤처져 있어서, 태그에 머무르면 이미 고쳐진
# 스타일 버그를 우리가 계속 우회해야 한다.
#
# 그렇다고 브랜치 이름으로 두면 안 된다. 어느 날 상류가 깨진 채로 내려와
# 빌드가 멈추고, 같은 소스가 시점에 따라 다르게 빌드되어 재현이 불가능해진다.
# **빌드와 UI 동작을 확인한 커밋 하나**에 고정하고, 올릴 때마다 사람이 확인한다.
#
# 커밋을 올리는 방법:
#   1) 시험 빌드 — 기존 캐시를 건드리지 않도록 별도 디렉터리를 쓴다.
#        cmake -S . -B _build/qlementine-probe -DMRST_QLEMENTINE_GIT_REF=<새 커밋> ...
#   2) 빌드가 되고 앱의 콤보박스·툴바·입력칸이 멀쩡하면
#      아래 기본값을 그 커밋으로 바꾸고 확인 날짜와 근거를 함께 적는다.
#
# 고정 커밋: dev @ 2026-04-14
#   "Merge pull request #172 from mario-liu/fix/abstract-item-list-widget-refresh"
#   v1.4.2 보다 36 커밋 앞선다. 2026-08-11 확인 —
#   MSVC 2022 + Qt 6.11.1 정적 링크로 빌드/실행 정상.
set(MRST_QLEMENTINE_GIT_REF "aea6ea220177bc1cc34101d64bf8d81ca0045439"
    CACHE STRING "oclero/qlementine 에서 받을 git 커밋(권장) 또는 태그/브랜치")

# 얕은 클론(GIT_SHALLOW)은 쓰지 않는다. CMake 는 그 경우 `--branch <ref>` 로
# 클론하는데 커밋 해시는 브랜치 이름 자리에 올 수 없어서 실패한다. ref 가
# 해시인지 태그인지 골라내는 조건을 두면 그 판별 자체가 새 버그가 된다
# (해시가 숫자로 시작하면 태그처럼 보인다). 저장소가 작아 전체를 받아도
# 몇 초면 끝나므로 항상 온전히 받는다.
#
# 참고: 예전에 태그로 받아 둔 얕은 클론이 남아 있으면 커밋으로 바꿀 때
# 갱신에 실패한다. 그때는 <빌드 디렉터리>/_deps/qlementine-* 을 지우고 다시 구성한다.

# 예제 앱은 빌드하지 않는다. 특히 showcase 는 qlementine-icons 를 별도로
# FetchContent 로 또 내려받으므로 켜 두면 불필요한 네트워크 의존이 하나 늘어난다.
set(QLEMENTINE_SANDBOX  OFF CACHE BOOL "Build the qlementine sandbox app"  FORCE)
set(QLEMENTINE_SHOWCASE OFF CACHE BOOL "Build the qlementine showcase app" FORCE)

# 정적 라이브러리로만 링크한다. 배포할 DLL 이 늘어나지 않고, qlementine 의 qrc
# (아이콘/글꼴) 도 실행 파일 안에 그대로 들어간다.
set(BUILD_SHARED_LIBS OFF)

FetchContent_Declare(
    qlementine
    GIT_REPOSITORY https://github.com/oclero/qlementine.git
    GIT_TAG        ${MRST_QLEMENTINE_GIT_REF}
)

FetchContent_MakeAvailable(qlementine)

if(NOT TARGET qlementine)
    message(FATAL_ERROR "qlementine 타깃이 생성되지 않았다. 상류 CMakeLists 구조가 바뀐 것으로 보인다.")
endif()

# ── WidgetWithFocusFrameEventFilter.hpp 교체 ─────────────────────────────
#
# 상류가 포커스 프레임을 생 포인터로 들고 있어, 프레임이 먼저 파괴되면 다음
# QEvent::Show 에서 죽은 포인터로 setWidget() 을 부른다. 이 앱에서는 F11
# (프리뷰 전체 화면)이 도크를 닫고 여는 동안 ADS 가 탭 바를 다시 만들면서 그
# 조건을 만든다 — 200 ms 간격 연타에서 13~24회에 0xC0000005 로 죽는다.
# 자세한 근거와 스택은 우리 사본의 머리 주석에 있다.
#
# 파일이 하나이고 헤더 전용이라 통째로 갈아 끼운다(PlatQt.cpp 와 같은 방식).
set(MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM
    "${qlementine_SOURCE_DIR}/lib/src/style/eventFilters/WidgetWithFocusFrameEventFilter.hpp")
set(MRST_QLEMENTINE_FOCUSFRAME_OURS
    "${CMAKE_SOURCE_DIR}/src/thirdparty/qlementine/WidgetWithFocusFrameEventFilter.hpp")

# 고정 커밋(MRST_QLEMENTINE_GIT_REF)에서 잰 상류 파일의 해시. 커밋을 올릴 때
# 이 값이 어긋나면 상류가 같은 파일을 손댔다는 뜻이므로, 우리 사본을 상류
# 기준으로 다시 만든 뒤 두 해시를 함께 갱신한다.
set(MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM_SHA256
    "c873afc24fb6735a72014ef62f0e66ea6bc943f61f637228f75ce0363b40bdf4")
set(MRST_QLEMENTINE_FOCUSFRAME_OURS_SHA256
    "d8138309d8bd0ade21c82a8ca25b09cfa20dc379274165e4b142916cb47c8066")

if(NOT EXISTS "${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM}")
    message(FATAL_ERROR
            "qlementine 의 WidgetWithFocusFrameEventFilter.hpp 를 찾지 못했다:\n"
            "  ${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM}\n"
            "상류가 파일을 옮겼거나 이름을 바꿨다. 우리 사본이 여전히 필요한지 확인할 것.")
endif()
if(NOT EXISTS "${MRST_QLEMENTINE_FOCUSFRAME_OURS}")
    message(FATAL_ERROR "우리 사본이 없다: ${MRST_QLEMENTINE_FOCUSFRAME_OURS}")
endif()

file(SHA256 "${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM}" MRST_QLEMENTINE_FOCUSFRAME_SEEN)
file(SHA256 "${MRST_QLEMENTINE_FOCUSFRAME_OURS}"     MRST_QLEMENTINE_FOCUSFRAME_MINE)

if(NOT MRST_QLEMENTINE_FOCUSFRAME_MINE STREQUAL MRST_QLEMENTINE_FOCUSFRAME_OURS_SHA256)
    message(FATAL_ERROR
            "우리 사본의 해시가 기록과 다르다.\n"
            "  파일: ${MRST_QLEMENTINE_FOCUSFRAME_OURS}\n"
            "  기록: ${MRST_QLEMENTINE_FOCUSFRAME_OURS_SHA256}\n"
            "  실제: ${MRST_QLEMENTINE_FOCUSFRAME_MINE}\n"
            "사본을 고쳤으면 MRST_QLEMENTINE_FOCUSFRAME_OURS_SHA256 도 함께 고칠 것.")
endif()

if(MRST_QLEMENTINE_FOCUSFRAME_SEEN STREQUAL MRST_QLEMENTINE_FOCUSFRAME_MINE)
    # 이미 갈아 끼운 트리다(재구성). 다시 복사할 것이 없다.
    message(STATUS "Qlementine focus-frame patch: 이미 적용됨")
elseif(MRST_QLEMENTINE_FOCUSFRAME_SEEN STREQUAL MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM_SHA256)
    file(COPY_FILE "${MRST_QLEMENTINE_FOCUSFRAME_OURS}" "${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM}")
    message(STATUS "Qlementine focus-frame patch: 적용")
else()
    message(FATAL_ERROR
            "상류 WidgetWithFocusFrameEventFilter.hpp 가 기준점과 다르다.\n"
            "  파일: ${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM}\n"
            "  기준: ${MRST_QLEMENTINE_FOCUSFRAME_UPSTREAM_SHA256}\n"
            "  실제: ${MRST_QLEMENTINE_FOCUSFRAME_SEEN}\n"
            "상류가 이 파일을 고쳤다. 포커스 프레임 수명 문제가 이미 해결되었다면\n"
            "우리 사본과 이 교체 단계를 지운다. 아니라면 새 상류 내용에 우리 고침을\n"
            "다시 얹고 두 해시를 갱신한다.")
endif()

# 정적 링크는 요구사항이다. qlementine 은 qt_add_library(... STATIC ...) 로
# 만들지만, 태그를 올렸을 때 조용히 SHARED 로 바뀌면 배포 구성이 어긋난다.
get_target_property(MRST_QLEMENTINE_TYPE qlementine TYPE)
if(NOT MRST_QLEMENTINE_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "qlementine 은 정적 라이브러리여야 한다 (현재: ${MRST_QLEMENTINE_TYPE}).")
endif()

# qlementine 은 자기 타깃에 MSVC /WX /W4 를 걸어 둔다. 우리가 손댈 수 없는
# 서드파티 코드가 Qt/MSVC 버전이 올라갈 때마다 경고 하나로 빌드를 깨뜨리는 것을
# 막기 위해 컴파일 옵션을 우리 기준으로 갈아끼운다.
set_target_properties(qlementine PROPERTIES
    COMPILE_OPTIONS ""
    FOLDER third_party
)
if(MSVC)
    target_compile_options(qlementine PRIVATE /MP /W3 /utf-8 /wd4996)
endif()

message(STATUS "Qlementine QStyle enabled: ${MRST_QLEMENTINE_GIT_REF} (${MRST_QLEMENTINE_TYPE})")
