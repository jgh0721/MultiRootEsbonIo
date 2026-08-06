include_guard(GLOBAL)
include(FetchContent)

# oclero/qlementine — 모던 데스크톱 Qt 앱용 QStyle.
# Qt 6.8 이상 / CMake 3.21 이상 / C++17 을 요구한다 (이 프로젝트는 Qt 6.11 + C++20).
set(MRST_QLEMENTINE_GIT_TAG "v1.4.2" CACHE STRING "oclero/qlementine git tag")

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
    GIT_TAG        ${MRST_QLEMENTINE_GIT_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(qlementine)

if(NOT TARGET qlementine)
    message(FATAL_ERROR "qlementine 타깃이 생성되지 않았다. 상류 CMakeLists 구조가 바뀐 것으로 보인다.")
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

message(STATUS "Qlementine QStyle enabled: ${MRST_QLEMENTINE_GIT_TAG} (${MRST_QLEMENTINE_TYPE})")
