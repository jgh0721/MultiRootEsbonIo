#pragma once

#include <QtCore>

/// 실행 파일 이름과 맞춘다. 여기 리터럴로 두는 이유는 생성 헤더(mrst_version.h)를
/// 쓸 수 없기 때문이다 — 이 헤더는 mrst_tests 가 컴파일하는 번역 단위에 딸려 들어오고,
/// 그 타깃의 include 경로에는 빌드 디렉터리가 없다(solUpdateManifest.hpp 와 같은 사유).
/// 대신 CMakeLists.txt 가 구성 시점에 cmake/MrstNames.cmake 의 값과 대조한다.
const auto DEFAULT_SETTINGS_FILENAME = QStringLiteral( "MultiRoot-reST Editor.ini" );

/// 0.4.0 이전 이름. AppSettings::migrateLegacyFile() 이 한 번 복사해 온다.
/// 지우지는 않는다 — 구버전으로 되돌아가도 그쪽 설정이 살아 있어야 한다.
const auto LEGACY_SETTINGS_FILENAME = QStringLiteral( "MultiRoot-reST-CPP.ini" );

/// Tab 별로 열린 문서에 대한 정보
struct DocumentTab
{
    QString                             FilePath;
    QString                             FileName;
    QString                             TextEncoding;
    bool                                IsIncludeBOM = false;
    QString                             LineEnding;

    int                                 Line = 0;       // 현재 캐럿이 위치한 행 번호
    int                                 Column = 0;     // 현재 캐럿이 위치한 행의 컬럼
};

// 단축키 정보
struct ShortcutItem
{
    QString                             Category;
    QString                             Id;
    QString                             Description;
    QKeySequence                        Shortcut;
    QKeySequence                        DefaultShortcut;
};