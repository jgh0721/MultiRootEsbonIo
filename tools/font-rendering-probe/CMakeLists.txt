cmake_minimum_required(VERSION 3.24)
project(MrstFontRenderingProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Core5Compat Gui GuiPrivate Widgets WebEngineWidgets)
qt_standard_project_setup(REQUIRES 6.11)

set(MRST_EXISTING_BUILD "" CACHE PATH "Existing application build containing ScintillaEditBaseQt")
if(NOT EXISTS "${MRST_EXISTING_BUILD}/RelWithDebInfo/ScintillaEditBaseQt.lib")
    message(FATAL_ERROR "Build ScintillaEditBaseQt in MRST_EXISTING_BUILD first")
endif()
get_filename_component(MRST_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SCINTILLA_SOURCE "${MRST_EXISTING_BUILD}/_deps/mv_scintilla_src-src")
# The executable supplies the complete PlatQt object; the archive supplies the
# unchanged Scintilla core/widget objects, including the application's patches.
qt_add_executable(mrst_font_probe
    main.cpp
    MaskPaint.hpp
    "${CMAKE_CURRENT_BINARY_DIR}/PlatQt-probe.cpp"
)
target_link_libraries(mrst_font_probe PRIVATE
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
    Qt6::WebEngineWidgets
    "${MRST_EXISTING_BUILD}/RelWithDebInfo/ScintillaEditBaseQt.lib"
)
target_include_directories(mrst_font_probe PRIVATE
    "${SCINTILLA_SOURCE}/include"
    "${SCINTILLA_SOURCE}/qt/ScintillaEditBase"
    "${MRST_EXISTING_BUILD}/scintilla-core"
    "${MRST_SOURCE_ROOT}/src/thirdparty/scintilla-qt"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(mrst_font_probe PRIVATE EXPORT_IMPORT_API= SCINTILLA_QT=1)
if(MSVC)
    target_compile_options(mrst_font_probe PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
endif()

# Patch a build-directory copy only. Ordinary application rendering is untouched.
file(READ "${MRST_SOURCE_ROOT}/src/thirdparty/scintilla-qt/PlatQt.cpp" platqt)
string(REPLACE "#include <QtMath>" "#include <QtMath>\n#include \"MaskPaint.hpp\"" platqt "${platqt}")
string(REPLACE
    "GetPainter()->drawText(QPointF(rc.left, ybase), su);"
    "mrst::probe::drawText(*GetPainter(), QPointF(rc.left, ybase), su);"
    platqt "${platqt}"
)
string(REPLACE
    "mrst::scintilla::drawTextWithMask(*GetPainter(), QPointF(rc.left, ybase), su);"
    "mrst::probe::drawText(*GetPainter(), QPointF(rc.left, ybase), su);"
    platqt "${platqt}"
)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PlatQt-probe.cpp"
    CONTENT "${platqt}" @ONLY NEWLINE_STYLE CRLF)
