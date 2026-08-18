#pragma once

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
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

    static QString legacyIniFilePath()
    {
        return QApplication::applicationDirPath() + "/" + LEGACY_SETTINGS_FILENAME;
    }

    /// 0.4.0 에서 실행 파일 이름이 바뀌면서 설정 파일 이름도 함께 바뀌었다.
    /// 구 파일이 있고 새 파일이 없으면 한 번 복사해 온다. 옮겼으면 true.
    ///
    /// **QSettings 로 키를 옮기지 않고 파일을 그대로 복사한다.** 사람이 손으로 넣은
    /// 주석과 키 순서가 남고, 우리가 모르는 키(다른 버전이 쓴 것)도 함께 살아난다.
    /// QSettings 왕복은 그 둘을 조용히 버린다.
    ///
    /// **구 파일은 지우지 않는다.** 구버전으로 되돌아가는 경우가 이 릴리스에서는
    /// 정상 경로다(0.3.x 는 자동 업데이트로 올라올 수 없어 수동 재설치를 안내한다).
    /// 그때 지난 설정이 남아 있어야 한다.
    ///
    /// 실패해도 기동을 막지 않는다 — 쓰기 권한이 없는 폴더(Program Files 등)에
    /// 설치했으면 애초에 설정 저장 자체가 안 되는 환경이고, 그 판단은 여기가 아니다.
    /// **반드시 설정을 처음 읽기 전에 부를 것** (main() 의 번역기 설치보다 앞).
    static bool migrateLegacyFile()
    {
        if( QFileInfo::exists( iniFilePath() ) )
            return false;                       // 새 이름이 이미 있다. 손대지 않는다.

        const QString legacy = legacyIniFilePath();
        if( !QFileInfo::exists( legacy ) )
            return false;

        // QFile::copy 를 쓰지 않는다. 중간에 죽으면 잘린 ini 가 남고, 그다음 기동은
        // "새 파일이 이미 있다" 로 판정해 **다시 옮기지 않는다** — 설정이 조용히
        // 반쪽이 된다. QSaveFile 은 임시 파일에 다 쓴 뒤 rename 한다.
        QFile source( legacy );
        if( !source.open( QIODevice::ReadOnly ) )
            return false;

        QSaveFile target( iniFilePath() );
        if( !target.open( QIODevice::WriteOnly ) )
            return false;
        if( target.write( source.readAll() ) < 0 )
            return false;                       // ~QSaveFile 이 임시 파일을 치운다
        return target.commit();
    }
};

