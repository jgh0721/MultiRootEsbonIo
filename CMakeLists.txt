cmake_minimum_required(VERSION 3.21)

project("MultiRoot-reST-CPP" VERSION 0.3.0 LANGUAGES CXX)

# 실행 파일 아이콘(resources/app.rc)을 컴파일하려면 RC 언어가 필요하다.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}/src/uis")

set(MRST_QT_ROOT "D:/Qt/6.11.1/msvc2022_64" CACHE PATH "Qt 6.11 MSVC 2022 root")
set(MRST_SCINTILLA_ROOT "${CMAKE_SOURCE_DIR}/third_party/scintilla/scintilla" CACHE PATH "missdeer/scintilla source root")
set(MRST_LEXILLA_ROOT "${CMAKE_SOURCE_DIR}/third_party/scintilla/lexilla" CACHE PATH "ScintillaOrg/lexilla source root")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── 버전 헤더 ─────────────────────────────────────────────
# C++ 과 리소스 컴파일러(app.rc 의 VERSIONINFO)가 같은 값을 쓰도록
# project(VERSION ...) 을 헤더 하나로 흘려보낸다.
configure_file("${CMAKE_SOURCE_DIR}/cmake/mrst_version.h.in"
        "${CMAKE_BINARY_DIR}/mrst_version.h" @ONLY)

# ── Qt 경로 설정 ──────────────────────────────────────────
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(PREPEND CMAKE_PREFIX_PATH "${MRST_QT_ROOT}")
endif()

# ── Qt 패키지 ─────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
        Core Core5Compat Gui Widgets Svg PrintSupport UiTools Network
        WebChannel WebEngineCore WebEngineWidgets
        # lupdate / lrelease / lconvert. 빌드 도구라 링크 대상은 아니다.
        LinguistTools
)

# ── 외부 라이브러리 (FetchContent) ────────────────────────
include(cmake/BuildScintillaLexilla.cmake)
include(cmake/BuildQlementine.cmake)

# windeployqt 인자를 개발 빌드와 릴리스 패키징이 함께 쓴다.
include(cmake/MrstDeployFlags.cmake)

# 소스 문자열의 원어가 한국어라는 사실을 여기 한 곳에서만 선언한다.
# 아래 qt_add_translations() 가 이 값으로 .ts 파일 이름과 <TS sourcelanguage="ko">,
# 그리고 복수형 전용 mrst_ko.ts 의 생성 여부를 정한다.
qt_standard_project_setup(
        I18N_SOURCE_LANGUAGE      ko
        I18N_TRANSLATED_LANGUAGES en ja
)

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
        src/core/solEsbonioLspPool.cpp
        src/core/solEsbonioLspPool.hpp
        src/core/solPythonEnvMgr.cpp
        src/core/solPythonEnvMgr.hpp
        src/core/solPythonEnvResolver.cpp
        src/core/solPythonEnvResolver.hpp
        src/core/solUvTaskRunner.cpp
        src/core/solUvTaskRunner.hpp
        src/core/solQlementineTheme.cpp
        src/core/solQlementineTheme.hpp
        src/core/solThemeManager.cpp
        src/core/solThemeManager.hpp
        src/core/solGlossaryIndex.cpp
        src/core/solGlossaryIndex.hpp
        src/core/solLanguageManager.cpp
        src/core/solLanguageManager.hpp
        src/core/solShadowBackupStore.cpp
        src/core/solShadowBackupStore.hpp
        src/core/solSphinxScanner.cpp
        src/core/solSphinxScanner.hpp
        src/core/solSphinxDiagnostics.cpp
        src/core/solSphinxDiagnostics.hpp
        src/core/solSphinxDiagnosticsStore.cpp
        src/core/solSphinxDiagnosticsStore.hpp
        src/core/solSphinxPreviewController.cpp
        src/core/solSphinxPreviewController.hpp
        src/core/solPreviewBridge.cpp
        src/core/solPreviewBridge.hpp
        src/core/solRestCompletionCoordinator.cpp
        src/core/solRestCompletionCoordinator.hpp
        src/core/solRestOutlineService.cpp
        src/core/solRestOutlineService.hpp
        src/core/solRestWorkspaceController.cpp
        src/core/solRestWorkspaceController.hpp
        src/core/solRstOfflineCompletions.cpp
        src/core/solRstOfflineCompletions.hpp
        src/core/solSphinxProjectRegistry.cpp
        src/core/solSphinxProjectRegistry.hpp
        src/core/solVirtualProjectMgr.cpp
        src/core/solVirtualProjectMgr.hpp
        src/core/solWorkspaceSearch.cpp
        src/core/solWorkspaceSearch.hpp
        src/core/solWorkspaceSession.cpp
        src/core/solWorkspaceSession.hpp
        src/core/solUpdateManifest.cpp
        src/core/solUpdateManifest.hpp
        src/core/solUpdateInstaller.cpp
        src/core/solUpdateInstaller.hpp
        src/core/solUpdateService.cpp
        src/core/solUpdateService.hpp
        src/uis/dlgAbout.cpp
        src/uis/dlgAbout.hpp
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
        src/editor/CompletionPopupWidget.cpp
        src/editor/CompletionPopupWidget.hpp
        src/editor/CompletionDetailPopup.cpp
        src/editor/CompletionDetailPopup.hpp
        src/editor/FindReplaceWidget.cpp
        src/editor/FindReplaceWidget.hpp
        src/editor/ColumnRulerWidget.hpp
        src/editor/TextFileSession.cpp
        src/editor/TextFileSession.hpp
        src/editor/TextLexerRegistry.cpp
        src/editor/TextLexerRegistry.hpp
        src/editor/TextSaveDialog.cpp
        src/editor/TextSaveDialog.hpp
        src/utils/AuthenticodeVerifier.cpp
        src/utils/AuthenticodeVerifier.hpp
        src/utils/DwmTitleBar.cpp
        src/utils/DwmTitleBar.hpp
        src/utils/FileLoadHelper.cpp
        src/utils/FileLoadHelper.hpp
        src/utils/ProcessReaper.cpp
        src/utils/ProcessReaper.hpp
        src/utils/solPhaseTrace.cpp
        src/utils/solPhaseTrace.hpp
        src/utils/solBackgroundWork.cpp
        src/utils/solBackgroundWork.hpp
        uniqueLibs/solDocument_Defs.hpp
        uniqueLibs/solEncodingDetector.cpp
        uniqueLibs/solEncodingDetector.hpp
)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE resources/app.rc)
endif()

# ── MSVC stdafx.h 미리 컴파일된 헤더 지정 ────────────────
target_precompile_headers(${PROJECT_NAME} PRIVATE src/stdafx.h)

target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}")
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_SOURCE_DIR}/uniqueLibs")
# 생성된 mrst_version.h 를 C++ 과 RC 양쪽에서 찾을 수 있게 한다.
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_BINARY_DIR}")

# wintrust/crypt32 = 내려받은 배포물의 Authenticode 서명 확인,
# version = 그 실행 파일의 FileVersion 확인 (자동 업데이트 검증 단계).
target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi user32 windowsapp d3d11 dxgi runtimeobject
        wintrust crypt32 version)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Core5Compat
    # 업데이트 매니페스트 조회와 배포 파일 다운로드. Qt6Network.dll 과 tls/
    # 플러그인은 WebEngine 이 이미 끌어오므로 배포 크기는 늘지 않는다.
    Qt6::Network
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
    qlementine
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

# ── 업데이터 (별도 실행 파일) ─────────────────────────────
# 실행 중인 프로세스는 자기 exe 와 로드된 DLL 을 지울 수 없고, 교체가 중간에
# 실패했을 때 되돌릴 주체도 필요하다. 그래서 파일 교체만 하는 작은 프로그램을
# 따로 만들어 %TEMP% 사본으로 띄운다.
#
# Qt 를 링크하지 않는다 — Qt6Core.dll 을 로드하면 자기가 교체해야 하는 파일을
# 스스로 잠근다. 정적 CRT 라 VC 재배포 DLL 도 필요 없다(windeployqt 에
# --no-compiler-runtime 을 주고 있어서 그 DLL 은 배포되지 않는다).
if(WIN32)
    add_executable(mrst_updater WIN32
            updater/main.cpp
            updater/solUpdaterStrings.cpp
            updater/solUpdaterStrings.hpp
            updater/solUpdaterSwap.cpp
            updater/solUpdaterSwap.hpp
            updater/updater.rc
    )
    # 생성된 mrst_version.h 를 RC 컴파일러가 찾을 수 있게 한다.
    target_include_directories(mrst_updater PRIVATE "${CMAKE_BINARY_DIR}")
    target_link_libraries(mrst_updater PRIVATE kernel32 user32 shell32)
    if(MSVC)
        target_compile_options(mrst_updater PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
        set_property(TARGET mrst_updater PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # 앱 옆에 놓아 개발 중에도 설치 흐름을 그대로 시험할 수 있게 한다.
    # (배포 ZIP 에도 이 위치로 들어간다.)
    add_dependencies(${PROJECT_NAME} mrst_updater)
    add_custom_command(TARGET mrst_updater POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:mrst_updater>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "Copying mrst_updater next to the app"
    )
endif()


# ── 유닛 테스트 ───────────────────────────────────────────
# 위젯도 프로세스도 띄우지 않는 순수 로직만 검증한다.
# esbonio/sphinx-build 를 실제로 돌리는 테스트는 두지 않는다 (느리고 환경 의존적).
option(MRST_BUILD_TESTS "Build unit tests" OFF)
if(MRST_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(mrst_tests
            tests/main.cpp
            tests/TestRunner.hpp
            tests/tst_RstContainerLexer.cpp
            tests/tst_RstFolding.cpp
            tests/tst_LspMessageRouting.cpp
            tests/tst_ProjectResolution.cpp
            tests/tst_RstCompletionContext.cpp
            tests/tst_RstOutline.cpp
            tests/tst_WorkspaceSearch.cpp
            tests/tst_GlossaryParsing.cpp
            tests/tst_UpdateVersioning.cpp
            tests/tst_AuthenticodeVerifier.cpp
            tests/tst_Translations.cpp
            # 테스트 대상 프로덕션 소스는 OBJECT 라이브러리로 빼지 않고 명시 나열한다
            # (globbing 금지 + 단일 타깃이라는 이 프로젝트의 규칙을 따른다).
            src/editor/RstContainerLexer.cpp
            src/core/solRstOfflineCompletions.cpp
            src/core/solUpdateManifest.cpp
            src/core/solGlossaryIndex.cpp
            src/core/solRestOutlineService.cpp
            src/core/solWorkspaceSearch.cpp
            src/core/solWorkspaceSession.cpp
            src/core/solEsbonioLspClient.cpp
            src/core/solEsbonioLspClient.hpp
            src/utils/ProcessReaper.cpp
            src/utils/solPhaseTrace.cpp
            src/utils/solBackgroundWork.cpp
            src/utils/AuthenticodeVerifier.cpp
            src/core/solSphinxDiagnostics.cpp
            src/core/solSphinxScanner.cpp
            src/core/solLanguageManager.cpp
            src/core/solLanguageManager.hpp
    )
    # 번역 무결성 테스트가 .ts 원본을 직접 파싱한다. .qm 은 :/i18n 에서 읽는다
    # (아래 qt_add_translations 가 mrst_tests 에도 같은 리소스를 붙인다).
    target_compile_definitions(mrst_tests PRIVATE
            MRST_I18N_TS_DIR="${CMAKE_SOURCE_DIR}/translations")
    target_include_directories(mrst_tests PRIVATE
            "${CMAKE_SOURCE_DIR}"
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests"
            "${CMAKE_SOURCE_DIR}/uniqueLibs"
    )
    # 테스트 대상 소스가 stdafx.h 를 포함하고, 거기서 Qt 전 모듈을 끌어온다.
    # 실제로 쓰는 건 Core 뿐이지만 헤더 경로를 얻으려면 링크가 필요하다.
    target_link_libraries(mrst_tests PRIVATE
            Qt6::Core Qt6::Core5Compat Qt6::Gui Qt6::Widgets
            Qt6::WebEngineCore Qt6::WebEngineWidgets Qt6::Test
    )
    # AuthenticodeVerifier 를 검증 대상에 넣었으므로 같은 Win32 라이브러리가 필요하다.
    if(WIN32)
        target_link_libraries(mrst_tests PRIVATE wintrust crypt32 version)
    endif()
    if(MSVC)
        target_compile_options(mrst_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    endif()

    add_test(NAME mrst_tests COMMAND mrst_tests)
endif()

# ── 번역 ──────────────────────────────────────────────────
# .qm 을 exe 안에 넣는다(:/i18n/mrst_<lang>.qm). 배포 ZIP 에 파일이 한 개도 늘지
# 않으므로 PackageRelease.cmake 의 필수/금지 목록도, windeployqt 인자도, 자동
# 업데이트(ZIP 통째 교체) 경로도 건드릴 것이 없다. 느슨한 .qm 파일로 배포하면
# 개발 빌드에서는 되고 릴리스에서만 번역이 사라진다.
set(MRST_I18N_TARGETS ${PROJECT_NAME})
if(TARGET mrst_tests)
    # 테스트가 :/i18n 을 직접 읽어 .qm 무결성을 검사한다.
    list(APPEND MRST_I18N_TARGETS mrst_tests)
endif()

# lupdate 는 프로젝트 모드에서도 .qrc 를 "안에 든 파일 목록" 으로 펼친 뒤
# (projectprocessor.cpp: expandQrcFiles), 확장자를 모르는 파일을 전부 C++ 파서로
# 넘긴다(trlib/trparser.cpp: processSources). 우리 qrc 에는 67MB 짜리 uv.exe 가
# 들어 있고 번역할 문자열은 하나도 없다. removeExcludedSources() 가 qrc 전개보다
# 먼저 돌기 때문에 이 한 줄로 전개 자체가 막힌다.
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
        QT_EXCLUDE_SOURCES_FROM_TRANSLATION "src/MultiRoot-reST-CPP.qrc")

qt_add_translations(
        TARGETS         ${MRST_I18N_TARGETS}
        # 생략하면 CMake 가 이 호출을 디렉터리 끝으로 미루고 이 스코프의 모든
        # 타깃 — qlementine / ScintillaEditBaseQt / LexillaStatic, 전부 file(GLOB)
        # 로 수천 파일 — 을 lupdate 대상으로 긁어모은다.
        SOURCE_TARGETS  ${PROJECT_NAME}
        TS_FILE_DIR     "${CMAKE_SOURCE_DIR}/translations"
        TS_FILE_BASE    mrst
        RESOURCE_PREFIX "/i18n"          # 기존 /python /preview /icons /themes 와 겹치지 않는다

        # Qt 자신의 문자열(QMessageBox 버튼, QColorDialog, 비네이티브 QFileDialog)을
        # 우리 .qm 안에 함께 굽는다. windeployqt 는 --no-translations 라 Qt 의 .qm 을
        # 배포하지 않는데, 이렇게 하면 배포 파일을 늘리지 않고도 표준 대화상자가
        # 번역되고 런타임은 QTranslator 를 하나만 설치하면 된다.
        #
        # 카탈로그를 명시하지 않으면 WebEngineCore 때문에 qtwebengine 카탈로그가
        # 딸려 오는데, Qt 6.11.1 에는 qtwebengine_ja.qm 이 아예 없어서 구성할 때마다
        # 경고가 뜬다. qtbase 하나로 못 박는다.
        MERGE_QT_TRANSLATIONS
        QT_TRANSLATION_CATALOGS qtbase

        # -locations none : .ts 에 파일 경로/줄 번호를 남기지 않는다. 남기면 코드가
        #                   한 줄만 밀려도 700개 항목이 통째로 diff 에 뜨고 브랜치마다
        #                   충돌한다. 컨텍스트 이름(=클래스명)만으로 위치는 충분하다.
        # -sort-messages  : 컨텍스트 안에서 순서를 고정한다. 문자열이 하나 늘어도
        #                   그 자리만 바뀐다.
        LUPDATE_OPTIONS  -locations none -sort-messages

        # 번역문이 원문과 같은 항목은 굽지 않는다 (어차피 원문으로 폴백된다).
        LRELEASE_OPTIONS -removeidentical
)

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
                ${MRST_WINDEPLOYQT_ARGS}
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

# ── 릴리스 패키징 ─────────────────────────────────────────
# 위의 windeployqt POST_BUILD 는 개발 편의를 위해 빌드 디렉터리를 계속 오염시킨다.
# 배포물은 그 디렉터리를 참조하지 않고 package/ 아래에 새로 깔기만 하므로,
# Environment/ (런타임이 만드는 238MB 파이썬 환경), *.ini (사용자 설정), *.pdb,
# _deps/, *_autogen/, mrst_tests.exe 는 "제외 규칙" 없이도 들어올 수 없다.
if(WIN32 AND WINDEPLOYQT_EXE)
    set(MRST_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")

    set(MRST_REMOVALS "" CACHE STRING
            "이번 릴리스에서 사라진 최상위 항목 (세미콜론 구분). 업데이터가 이것만 추가로 치운다.")
    set(MRST_MIN_FROM_VERSION "" CACHE STRING
            "제자리 교체를 지원하는 최소 설치 버전. 비우면 0.2.0.")
    option(MRST_ALLOW_DIRTY "커밋되지 않은 변경이나 태그 없이도 패키징을 허용한다" OFF)

    add_custom_target(mrst_package
            COMMAND "${CMAKE_COMMAND}"
                    -D "MRST_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                    -D "MRST_PACKAGE_DIR=${MRST_PACKAGE_DIR}"
                    -D "MRST_VERSION=${PROJECT_VERSION}"
                    -D "MRST_CONFIG=$<CONFIG>"
                    -D "MRST_WINDEPLOYQT=${WINDEPLOYQT_EXE}"
                    -D "MRST_APP_EXE=$<TARGET_FILE:${PROJECT_NAME}>"
                    -D "MRST_UPDATER_EXE=$<TARGET_FILE:mrst_updater>"
                    -D "MRST_APP_PDB=$<TARGET_PDB_FILE:${PROJECT_NAME}>"
                    -D "MRST_REMOVALS=${MRST_REMOVALS}"
                    -D "MRST_MIN_FROM_VERSION=${MRST_MIN_FROM_VERSION}"
                    -D "MRST_ALLOW_DIRTY=${MRST_ALLOW_DIRTY}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/PackageRelease.cmake"
            COMMENT "릴리스 패키지 생성 → ${MRST_PACKAGE_DIR}"
            VERBATIM USES_TERMINAL
    )
    add_dependencies(mrst_package ${PROJECT_NAME} mrst_updater)

    # 업로드는 자격 정보를 다루므로 타깃을 나눈다. mrst_package 는 토큰을 모른다.
    add_custom_target(mrst_release
            COMMAND "${CMAKE_COMMAND}"
                    -D "MRST_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                    -D "MRST_PACKAGE_DIR=${MRST_PACKAGE_DIR}"
                    -D "MRST_VERSION=${PROJECT_VERSION}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/UploadRelease.cmake"
            COMMENT "GitHub 릴리스 생성 및 에셋 업로드 (gh CLI 필요)"
            VERBATIM USES_TERMINAL
    )
endif()
