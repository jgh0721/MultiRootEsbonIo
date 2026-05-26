include_guard(GLOBAL)
include(FetchContent)

set(MV_ENABLE_SCINTILLA_DIRECT_BACKEND ON CACHE BOOL "Build the direct Scintilla/Lexilla backend" FORCE)
set(MV_DIRECT_SCINTILLA_LINK_LEXILLA_LEXERS ON CACHE BOOL "Link Lexilla lexer implementations into the app" FORCE)

set(MV_SCINTILLA_GIT_TAG "rel-5-6-2" CACHE STRING "Scintilla git tag for the direct backend")
set(MV_LEXILLA_GIT_TAG "rel-5-4-9" CACHE STRING "Lexilla git tag for the direct backend")

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
    "${CMAKE_CURRENT_LIST_DIR}/../src/compat"
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

target_compile_definitions(ScintillaEditBaseQt PRIVATE
    SCINTILLA_QT=1
    MAKING_LIBRARY=1
    _CRT_SECURE_NO_WARNINGS
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

target_compile_definitions(LexillaStatic PRIVATE
    _CRT_SECURE_NO_WARNINGS
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

set(MV_SCINTILLA_DIRECT_COMPILE_DEFINITIONS
    MV_HAVE_SCINTILLA_DIRECT_BACKEND=1
    MV_DIRECT_SCINTILLA_HAS_LEXILLA_LEXERS=1
)

message(STATUS "MV direct Scintilla backend enabled: Scintilla ${MV_SCINTILLA_GIT_TAG}, Lexilla ${MV_LEXILLA_GIT_TAG}")

