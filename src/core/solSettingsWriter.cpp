#include "stdafx.h"
#include "solSettingsWriter.hpp"

#include "solAppSettings.hpp"

#include <QTimer>

namespace mrst {
namespace {

/// 마지막 쓰기 뒤 이만큼 조용하면 파일로 내보낸다.
///
/// 연타(Alt+Z, 확대/축소, 스핀박스)는 간격이 이보다 짧아 한 번으로 접힌다.
/// 반대로 이 값이 크면 강제 종료 시 잃는 창이 넓어진다. 사람이 설정을 바꾸고
/// 앱을 곧바로 죽이는 경우를 생각하면 초 단위가 상한이다.
constexpr int kQuietMs = 800;

}   // namespace

struct SettingsWriter::Impl
{
    /// 오래 사는 QSettings 하나. 지역 변수 관용구와 달리 소멸이 곧 파일 쓰기가
    /// 아니므로, 쓰기 시점을 우리가 정할 수 있다.
    AppSettings                         settings;
    QTimer                              timer;
    bool                                dirty = false;
};

SettingsWriter::SettingsWriter()
    : impl_( new Impl )
{
    impl_->timer.setSingleShot( true );
    impl_->timer.setInterval( kQuietMs );
    QObject::connect( &impl_->timer, &QTimer::timeout, &impl_->timer, [ this ] { flush(); } );
}

SettingsWriter::~SettingsWriter()
{
    // 도달하지 않는 것이 정상이다(instance() 의 주석 참고). 그래도 정의는 둔다 —
    // Impl 이 불완전 타입이라 헤더에서는 delete 할 수 없다.
    flush();
    delete impl_;
}

SettingsWriter& SettingsWriter::instance()
{
    // **일부러 지우지 않는다.** 함수 지역 static 으로 두면 소멸이
    // ~QCoreApplication 뒤로 밀려, 그 시점의 QSettings::sync() 가 이미 정리된
    // Qt 전역 상태(파일 시스템 엔진, 잠금)를 건드린다. 종료 경로가 flush() 를
    // 명시적으로 부르므로 소멸자에 의지할 이유가 없다.
    static SettingsWriter* const writer = new SettingsWriter;
    return *writer;
}

void SettingsWriter::setValue( const QString& key, const QVariant& value )
{
    impl_->settings.setValue( key, value );
    impl_->dirty = true;
    // 마지막 쓰기 기준으로 다시 센다 — 연타 중에는 계속 미뤄지고, 손을 떼면 나간다.
    impl_->timer.start();
}

void SettingsWriter::flush()
{
    impl_->timer.stop();
    if( !impl_->dirty )
        return;

    impl_->dirty = false;
    impl_->settings.sync();
}

}   // namespace mrst
