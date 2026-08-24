#pragma once

#include <QString>
#include <QVariant>

namespace mrst {

/// INI 쓰기를 모아서 한 번에 내보내는 기록기.
///
/// **왜 필요한가.** `AppSettings` 는 `QSettings` 파생이고, 이 저장소의 관용구는
/// 쓸 때마다 스택 지역 변수를 만드는 것이다.
///
/// ```cpp
/// AppSettings settings;                                  // ← 여기서 파일을 읽고
/// settings.setValue( "textView/wordWrapMode", mode );
/// ```                                                    // ← 여기서 파일을 통째로 쓴다
///
/// 파일 쓰기는 `setValue()` 가 아니라 `~QSettings` 의 `sync()` 가 한다. 즉 값 하나를
/// 바꾸는 함수가 **잠금 획득 → 임시 파일 쓰기 → rename** 을 한 번씩 치른다. 파일은
/// EXE 와 같은 디렉터리에 있어 실시간 검사에 걸리기 쉽다.
///
/// 실측(1095줄 문서, Alt+Z): 토글 한 번이 2.9 ms 이고 그중 **1.4 ms 가 이 쓰기**였다.
/// Scintilla 재배치(0.25 ms)의 다섯 배가 넘고, GUI 스레드에서 벌어지는 유일한 동기
/// 디스크 I/O 다. 연타하면 키 입력마다 한 번씩 쌓인다.
///
/// **왜 "QSettings 를 하나 오래 살리기" 로 끝내지 않는가.** 그러면 쓰기가 프로세스
/// 종료까지 미뤄져, 강제 종료 시 그 세션의 설정이 통째로 사라진다. 지연 상한을 두어
/// 연타는 한 번으로 접으면서 그 위험은 유지한다.
///
/// **읽기는 여기를 거치지 않아도 된다.** 같은 파일을 가리키는 `QSettings` 들은 파싱
/// 결과를 공유하므로(`QConfFile`), 아직 `sync()` 하지 않은 값도 다른 `AppSettings`
/// 인스턴스가 그대로 읽는다. 그래서 기존 읽기 코드를 고치지 않아도 값이 어긋나지 않는다.
class SettingsWriter final
{
public:
    /// 프로세스에 하나. GUI 스레드에서만 부른다.
    static SettingsWriter& instance();

    /// 값을 넣고 기록을 예약한다. 같은 키를 여러 번 넣으면 마지막 것만 남는다.
    void setValue( const QString& key, const QVariant& value );

    /// 예약된 기록을 지금 내보낸다. 종료 경로(`MainWindow::closeEvent`)에서 부른다.
    /// 예약이 없으면 아무 일도 하지 않는다.
    void flush();

    SettingsWriter( const SettingsWriter& ) = delete;
    SettingsWriter& operator=( const SettingsWriter& ) = delete;

private:
    SettingsWriter();
    ~SettingsWriter();

    struct Impl;
    Impl* impl_ = nullptr;
};

}   // namespace mrst
