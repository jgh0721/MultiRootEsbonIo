cmake_minimum_required(VERSION 3.21)

project("MultiRoot-reST-CPP" VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}/src/uis")

#set(MRST_QT_ROOT "D:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt 6.11 MSVC 2022 root")
set(MRST_SCINTILLA_ROOT "${CMAKE_SOURCE_DIR}/third_party/scintilla/scintilla" CACHE PATH "missdeer/scintilla source root")
set(MRST_LEXILLA_ROOT "${CMAKE_SOURCE_DIR}/third_party/scintilla/lexilla" CACHE PATH "ScintillaOrg/lexilla source root")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Qt 경로 설정 ──────────────────────────────────────────
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
#    list(PREPEND CMAKE_PREFIX_PATH "${MRST_QT_ROOT}")
endif()

# ── Qt 패키지 ─────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
        Core Core5Compat Gui Widgets Svg PrintSupport UiTools
        WebChannel WebEngineCore WebEngineWidgets
)

# ── 외부 라이브러리 (FetchContent) ────────────────────────
include(cmake/BuildScintillaLexilla.cmake)

qt_standard_project_setup()

add_executable(${PROJECT_NAME}
        src/stdafx.h
        src/stdafx.cpp
        src/main.cpp
        src/MainWindow.cpp
        src/MainWindow.hpp
        src/uis/mainWindow.ui
        src/MultiRoot-reST-CPP.qrc
        src/core/solBaseView.cpp
        src/core/solBaseView.hpp
        src/core/solEsbonioLspClient.cpp
        src/core/solEsbonioLspClient.hpp
        src/core/solPythonEnvMgr.cpp
        src/core/solPythonEnvMgr.hpp
        src/core/solThemeManager.cpp
        src/core/solThemeManager.hpp
        src/core/solShadowBackupStore.cpp
        src/core/solShadowBackupStore.hpp
        src/core/solSphinxScanner.cpp
        src/core/solSphinxScanner.hpp
        src/core/solSphinxDiagnostics.cpp
        src/core/solSphinxDiagnostics.hpp
        src/core/solSphinxPreviewController.cpp
        src/core/solSphinxPreviewController.hpp
        src/core/solRestWorkspaceController.cpp
        src/core/solRestWorkspaceController.hpp
        src/core/solSphinxProjectRegistry.cpp
        src/core/solSphinxProjectRegistry.hpp
        src/uis/dlgSettings.cpp
        src/uis/dlgSettings.hpp
        src/uis/dlgSettings.ui
        src/editor/ScintillaEditorSettings.cpp
        src/editor/ScintillaEditorSettings.hpp
        src/editor/ScintillaQtDirectBackend.cpp
        src/editor/ScintillaQtDirectBackend.hpp
        src/editor/QBaseEditor.cpp
        src/editor/QBaseEditor.hpp
        src/editor/RstContainerLexer.cpp
        src/editor/RstContainerLexer.hpp
        src/editor/FindReplaceWidget.cpp
        src/editor/FindReplaceWidget.hpp
        src/editor/ColumnRulerWidget.hpp
        src/editor/TextFileSession.cpp
        src/editor/TextFileSession.hpp
        src/editor/TextLexerRegistry.cpp
        src/editor/TextLexerRegistry.hpp
        src/editor/TextSaveDialog.cpp
        src/editor/TextSaveDialog.hpp
        src/utils/DwmTitleBar.cpp
        src/utils/DwmTitleBar.hpp
        src/utils/FileLoadHelper.cpp
        src/utils/FileLoadHelper.hpp
        uniqueLibs/solDocument_Defs.hpp
        uniqueLibs/solEncodingDetector.cpp
        uniqueLibs/solEncodingDetector.hpp
)

# ── MSVC stdafx.h 미리 컴파일된 헤더 지정 ────────────────
target_precompile_headers(${PROJECT_NAME} PRIVATE src/stdafx.h)

target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}")
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}/uniqueLibs")

target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi user32 windowsapp d3d11 dxgi runtimeobject)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::PrintSupport
    Qt6::UiTools
    Qt6::WebChannel
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    ScintillaEditBaseQt
    LexillaStatic
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
endif()

# ── Win32 설정 ────────────────────────────────────────────
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            WIN32_EXECUTABLE TRUE
    )
endif()


# ── DLL 배포 (Windows) ────────────────────────────────────
if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if(NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if(NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif()
    endif()

    # windeployqt 로 Qt DLL + 플러그인 + QML 모듈 자동 복사
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_INSTALL_PATH}/bin")
    if(WINDEPLOYQT_EXE)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND "${WINDEPLOYQT_EXE}"
                --no-translations
                --no-compiler-runtime
                --no-system-d3d-compiler
                "$<TARGET_FILE:${PROJECT_NAME}>"
                COMMENT "Running windeployqt..."
        )
    else()
        message(WARNING "windeployqt not found — manual DLL copy fallback")
        # 수동 폴백: 최소 필수 DLL
        set(DEBUG_SUFFIX)
        if(MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
            set(DEBUG_SUFFIX "d")
        endif()

        # platforms 플러그인
        if(EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/"
                    COMMAND ${CMAKE_COMMAND} -E copy
                    "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/"
            )
        endif()

        # Qt DLL (직접 + 간접 의존)
        foreach(QT_LIB Core Core5Compat Gui Widgets Svg PrintSupport UiTools WebEngineCore WebEngineWidgets )
            if(EXISTS "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
                        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                )
            endif()
        endforeach()
    endif()
endif()
