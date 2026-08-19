include_guard(GLOBAL)
include(FetchContent)

set(MV_ENABLE_SCINTILLA_DIRECT_BACKEND ON CACHE BOOL "Build the direct Scintilla/Lexilla backend" FORCE)
set(MV_DIRECT_SCINTILLA_LINK_LEXILLA_LEXERS ON CACHE BOOL "Link Lexilla lexer implementations into the app" FORCE)

set(MV_SCINTILLA_GIT_TAG "rel-5-6-6" CACHE STRING "Scintilla git tag for the direct backend")
set(MV_LEXILLA_GIT_TAG "rel-5-5-3" CACHE STRING "Lexilla git tag for the direct backend")

FetchContent_Declare(
    mv_scintilla_src
    GIT_REPOSITORY https://github.com/missdeer/scintilla.git
    GIT_TAG        ${MV_SCINTILLA_GIT_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    mv_lexilla_src
    GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
    GIT_TAG        ${MV_LEXILLA_GIT_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(mv_scintilla_src mv_lexilla_src)

set(MV_SCINTILLA_ROOT "${mv_scintilla_src_SOURCE_DIR}")
set(MV_LEXILLA_ROOT   "${mv_lexilla_src_SOURCE_DIR}")

file(GLOB MV_SCINTILLA_QT_SOURCES CONFIGURE_DEPENDS
    "${MV_SCINTILLA_ROOT}/qt/ScintillaEditBase/*.cpp"
)
file(GLOB MV_SCINTILLA_CORE_SOURCES CONFIGURE_DEPENDS
    "${MV_SCINTILLA_ROOT}/src/*.cxx"
)
file(GLOB MV_LEXILLA_LEXLIB_SOURCES CONFIGURE_DEPENDS
    "${MV_LEXILLA_ROOT}/lexlib/*.cxx"
)
file(GLOB MV_LEXILLA_LEXER_SOURCES CONFIGURE_DEPENDS
    "${MV_LEXILLA_ROOT}/lexers/*.cxx"
)

# ── PlatQt.cpp 교체 ───────────────────────────────────────────────────────
# Qt 플랫폼 계층의 폭 측정(SurfaceImpl::MeasureWidths)을 우리가 손봐야 해서 이
# 파일만 사본으로 컴파일한다. 무엇을 왜 고쳤는지는 사본 머리말에 적어 둔다.
# 상류에 낼 수 없는 변경은 아니지만 반영을 기다릴 수 없다.
#
# FetchContent 의 PATCH_COMMAND 를 쓰지 않는 이유: 패치된 파일이 빌드 트리에만
# 존재해서 저장소를 읽는 사람에게도 grep 에도 보이지 않고, 이 저장소는 빌드
# 디렉터리를 여러 벌 쓰기 때문에 부분 적용 상태를 잡아낼 신호가 없다.
set(MV_PLATQT_UPSTREAM "${MV_SCINTILLA_ROOT}/qt/ScintillaEditBase/PlatQt.cpp")
set(MV_PLATQT_OURS     "${CMAKE_SOURCE_DIR}/src/thirdparty/scintilla-qt/PlatQt.cpp")

# 가드 1 — list(REMOVE_ITEM) 은 없는 항목에 대해 조용한 no-op 이다. 상류가 파일을
# 옮기거나 이름을 바꾸면 아무 말 없이 두 벌이 컴파일되어 심볼이 중복된다.
list(FIND MV_SCINTILLA_QT_SOURCES "${MV_PLATQT_UPSTREAM}" MV_PLATQT_INDEX)
if(MV_PLATQT_INDEX EQUAL -1)
    message(FATAL_ERROR
            "상류 PlatQt.cpp 를 GLOB 결과에서 찾지 못했다. 파일이 옮겨졌거나 이름이 바뀌었다.\n"
            "  기대한 경로: ${MV_PLATQT_UPSTREAM}")
endif()
list(REMOVE_ITEM MV_SCINTILLA_QT_SOURCES "${MV_PLATQT_UPSTREAM}")

# 가드 2 — 우리가 갈라져 나온 시점과 상류 내용이 같아야 한다. 다르면 상류가 그
# 파일을 고쳤다는 뜻이고, 그 변경을 우리 사본에 옮기지 않으면 조용히 잃는다.
# 줄끝을 정규화해서 해시한다 (core.autocrlf 로 체크아웃 바이트가 달라진다).
file(READ "${MV_PLATQT_UPSTREAM}" MV_PLATQT_TEXT)
string(REPLACE "\r\n" "\n" MV_PLATQT_TEXT "${MV_PLATQT_TEXT}")
string(SHA256 MV_PLATQT_HASH "${MV_PLATQT_TEXT}")
unset(MV_PLATQT_TEXT)
set(MV_PLATQT_BASE_SHA256 "51392de4d3df3c6611c4074d9aed02652e2ece46bf703cd6efc6e9458b355f6f")
if(NOT MV_PLATQT_HASH STREQUAL MV_PLATQT_BASE_SHA256)
    message(FATAL_ERROR
            "상류 PlatQt.cpp 가 우리 사본의 기준점(rel-5-6-6, 82f7656)과 다르다.\n"
            "  상류: ${MV_PLATQT_UPSTREAM}\n"
            "  우리: ${MV_PLATQT_OURS}\n"
            "  두 파일을 diff 해서 상류 변경을 우리 사본에 옮기고 아래 해시를 갱신할 것.\n"
            "  기대: ${MV_PLATQT_BASE_SHA256}\n"
            "  실제: ${MV_PLATQT_HASH}")
endif()
# file(READ) 는 재구성 의존을 만들지 않는다. 상류가 바뀌었을 때 가드가 실제로
# 돌게 하려면 손으로 걸어야 한다 (CMakeLists.txt 의 다른 가드들과 같은 관용구).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MV_PLATQT_UPSTREAM}")

add_library(ScintillaEditBaseQt STATIC
    ${MV_SCINTILLA_QT_SOURCES}
    ${MV_PLATQT_OURS}
    ${MV_SCINTILLA_CORE_SOURCES}
)

set_target_properties(ScintillaEditBaseQt PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(ScintillaEditBaseQt BEFORE PUBLIC
    "${MV_SCINTILLA_ROOT}/include"
    "${MV_SCINTILLA_ROOT}/src"
    "${MV_SCINTILLA_ROOT}/qt/ScintillaEditBase"
)

target_link_libraries(ScintillaEditBaseQt PUBLIC
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::Widgets
)

target_compile_definitions(ScintillaEditBaseQt
    PRIVATE
        SCINTILLA_QT=1
        MAKING_LIBRARY=1
        _CRT_SECURE_NO_WARNINGS
    INTERFACE
        # 링크하는 쪽이 직접 백엔드 사용 여부를 컴파일 타임에 알 수 있게 전파한다.
        MV_HAVE_SCINTILLA_DIRECT_BACKEND=1
        # ScintillaEditBase.h 는 WIN32 가 정의돼 있고 MAKING_LIBRARY 가 없으면
        # 클래스를 __declspec(dllimport) 로 선언한다. 우리는 정적으로 링크하므로
        # 그러면 링커가 __imp_ 심볼을 찾다가 LNK2019 로 죽는다 (정적 라이브러리에는
        # 그런 심볼이 없다).
        #
        # 이게 지금까지 안 터진 이유는 제너레이터 차이다. CMake 의 Visual Studio
        # 제너레이터는 PreprocessorDefinitions 에 WIN32 를 자동으로 넣고 Ninja 는
        # 넣지 않는다. 그래서 같은 소스가 CLion(Ninja) 에서는 링크되고
        # `-G "Visual Studio 17 2022"` 로 구성한 트리에서는 앱까지 링크되지 않는다.
        #
        # 헤더가 #ifndef EXPORT_IMPORT_API 로 가드하므로 여기서 빈 값으로 못박는다.
        # MAKING_LIBRARY 를 대신 전파하면 dllexport 가 되어 exe 가 심볼을 export 하게
        # 되므로 그쪽은 쓰지 않는다.
        EXPORT_IMPORT_API=
)

add_library(LexillaStatic STATIC
    "${MV_LEXILLA_ROOT}/src/Lexilla.cxx"
    ${MV_LEXILLA_LEXLIB_SOURCES}
    ${MV_LEXILLA_LEXER_SOURCES}
)

set_target_properties(LexillaStatic PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(LexillaStatic BEFORE PUBLIC
    "${MV_LEXILLA_ROOT}/include"
    "${MV_LEXILLA_ROOT}/lexlib"
    "${MV_SCINTILLA_ROOT}/include"
)

target_compile_definitions(LexillaStatic
    PRIVATE
        _CRT_SECURE_NO_WARNINGS
    INTERFACE
        # CreateLexer()/GetLexerCount() 를 정적으로 링크했음을 소비자에게 전파한다.
        # 이 정의가 없으면 ScintillaQtDirectBackend::applyLanguage 와
        # dlgSettings 의 lexer 열거가 조용히 죽은 분기로 빠진다.
        MV_DIRECT_SCINTILLA_HAS_LEXILLA_LEXERS=1
)

if(MSVC)
    target_compile_options(ScintillaEditBaseQt PRIVATE /W0)
    target_compile_options(LexillaStatic PRIVATE /W0)
    # PlatQt.cpp 는 이제 우리가 유지보수하므로 상류용 /W0 에서 빼낸다. 소스 단위
    # COMPILE_OPTIONS 가 타깃 옵션 뒤에 붙고 MSVC 는 마지막 /W 를 쓴다.
    set_source_files_properties("${MV_PLATQT_OURS}" PROPERTIES COMPILE_OPTIONS "/W3")
endif()

set(MV_HAVE_SCINTILLA_DIRECT_BACKEND ON)
set(MV_SCINTILLA_DIRECT_TARGETS
    ScintillaEditBaseQt
    LexillaStatic
)

# 컴파일 정의는 위 두 타깃의 INTERFACE 로 전파된다. 예전에는 여기서 변수로만
# 계산하고 어떤 타깃에도 적용하지 않아 관련 #if 분기가 전부 죽어 있었다.

message(STATUS "MV direct Scintilla backend enabled: Scintilla ${MV_SCINTILLA_GIT_TAG}, Lexilla ${MV_LEXILLA_GIT_TAG}")

