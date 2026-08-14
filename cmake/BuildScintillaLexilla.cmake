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

add_library(ScintillaEditBaseQt STATIC
    ${MV_SCINTILLA_QT_SOURCES}
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
endif()

set(MV_HAVE_SCINTILLA_DIRECT_BACKEND ON)
set(MV_SCINTILLA_DIRECT_TARGETS
    ScintillaEditBaseQt
    LexillaStatic
)

# 컴파일 정의는 위 두 타깃의 INTERFACE 로 전파된다. 예전에는 여기서 변수로만
# 계산하고 어떤 타깃에도 적용하지 않아 관련 #if 분기가 전부 죽어 있었다.

message(STATUS "MV direct Scintilla backend enabled: Scintilla ${MV_SCINTILLA_GIT_TAG}, Lexilla ${MV_LEXILLA_GIT_TAG}")

