include_guard(GLOBAL)
include(FetchContent)

# githubuser0xFFFF/Qt-Advanced-Docking-System — Visual Studio 식 도킹.
#
# KDDockWidgets 를 쓰지 않은 이유는 라이선스다. 그쪽은 GPL-2.0-only /
# GPL-3.0-only 또는 KDAB 상용뿐이어서, MIT 인 이 프로젝트가 서명한 ZIP 을
# 배포하는 구조와 맞지 않는다. ADS 는 LGPL-2.1 이다.
#
# 기능 면에서도 이쪽이 맞다. 우리가 원하는 것은 패널을 창 가장자리 탭으로 핀
# 고정해 두고 필요할 때만 겹쳐 꺼내는 그 동작인데, 그것이 ADS 의 핵심 기능이다
# (자기 qrc 에 vs-pin-button*.svg 를 들고 있다).
#
# 고정 커밋: v5.1.1 @ 2026-08-20 "Bumped version number to 5.1.1"
#   2026-08-21 확인 — MSVC 2022 + Qt 6.11.1 정적 링크로 빌드/실행 정상.
#   태그가 아직 새롭다. 상류에서 회귀가 나오면
#   v5.1.0 (fa104ca2ca99093f4434e8186cd28012b6874d08) 으로 내린다.
#
# 커밋을 올리는 방법은 BuildQlementine.cmake 의 절차와 같다 — 별도 빌드
# 디렉터리에서 시험한 뒤, 도크의 핀/떼내기/배치 복원까지 눌러 보고 바꾼다.
set(MRST_QTADS_GIT_REF "023ce95934fecfd4cf5c672c9c404fabe0f54923"
    CACHE STRING "Qt-Advanced-Docking-System 에서 받을 git 커밋(권장) 또는 태그/브랜치")

# 얕은 클론(GIT_SHALLOW)은 쓰지 않는다 — 이유는 BuildQlementine.cmake 와 같다.
# CMake 가 `--branch <ref>` 로 클론하는데 커밋 해시는 브랜치 이름 자리에 올 수 없다.

# 정적 라이브러리로 링크한다. 배포 ZIP 에 파일이 하나도 늘지 않으므로
# PackageRelease.cmake 의 필수/금지 목록도, windeployqt 인자도, ZIP 통째 교체인
# 자동 업데이트 경로도 건드릴 것이 없다.
#
# ADS 는 BUILD_SHARED_LIBS 를 보지 않고 자기 옵션 BUILD_STATIC 을 쓴다.
set(BUILD_STATIC   ON  CACHE BOOL "Build QtADS as a static library" FORCE)
# 켜 두면 examples/ 와 demo/ 까지 add_subdirectory 한다 (기본값이 ON 이다).
set(BUILD_EXAMPLES OFF CACHE BOOL "Build the QtADS examples"        FORCE)

FetchContent_Declare(
    qtads
    GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
    GIT_TAG        ${MRST_QTADS_GIT_REF}
)

FetchContent_MakeAvailable(qtads)

# 타깃 이름에 Qt 메이저 버전이 들어간다(qtadvanceddocking-qt6). 이름을 직접 쓰는
# 대신 상류가 함께 만들어 주는 버전 무관 별칭으로 링크하되, 그 별칭이 실제로
# 생겼는지는 여기서 확인한다.
if(NOT TARGET qtadvanceddocking-qt6)
    message(FATAL_ERROR "qtadvanceddocking-qt6 타깃이 생성되지 않았다. 상류 CMakeLists 구조가 바뀐 것으로 보인다.")
endif()

# 정적 링크는 요구사항이다. BUILD_STATIC 이 조용히 무시되면 배포 구성이 어긋난
# 채로 빌드가 통과하므로 (Qlementine 과 같은 관용구로) 못 박는다.
get_target_property(MRST_QTADS_TYPE qtadvanceddocking-qt6 TYPE)
if(NOT MRST_QTADS_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Qt ADS 는 정적 라이브러리여야 한다 (현재: ${MRST_QTADS_TYPE}).\n"
            "DLL 이 되면 배포 ZIP 에 파일이 늘어나 PackageRelease.cmake 의 점검 목록도 함께 고쳐야 한다.")
endif()

set_target_properties(qtadvanceddocking-qt6 PROPERTIES FOLDER third_party)
if(MSVC)
    # ADS 는 /WX 를 걸지 않으므로 Qlementine 처럼 옵션을 갈아끼우지는 않는다.
    # 병렬 컴파일만 켠다 (상류는 자기가 최상위일 때만 /MP 를 붙인다).
    target_compile_options(qtadvanceddocking-qt6 PRIVATE /MP)
endif()

message(STATUS "Qt ADS docking enabled: ${MRST_QTADS_GIT_REF} (${MRST_QTADS_TYPE})")
