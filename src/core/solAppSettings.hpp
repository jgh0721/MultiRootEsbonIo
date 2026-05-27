#pragma once

#include <QApplication>
#include <QSettings>

#include "uniqueLibs/solDocument_Defs.hpp"

/// EXE와 같은 디렉토리에 MultiViewer.ini 파일을 사용하는 설정 래퍼
class AppSettings : public QSettings
{
public:
    explicit AppSettings(QObject* parent = nullptr)
        : QSettings(iniFilePath(), QSettings::IniFormat, parent)
    {}

    static QString iniFilePath()
    {
        return QApplication::applicationDirPath() + "/" + DEFAULT_SETTINGS_FILENAME;
    }
};

