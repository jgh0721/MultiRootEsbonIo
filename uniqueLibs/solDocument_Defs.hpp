#pragma once

#include <QtCore>

const auto DEFAULT_SETTINGS_FILENAME = QStringLiteral( "MultiRoot-reST-CPP.ini" );

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