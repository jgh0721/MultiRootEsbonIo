#include "stdafx.h"
#include "ProcessReaper.hpp"

#include <QProcess>
#include <QTimer>

#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mrst {

#ifdef Q_OS_WIN

namespace {

/// 프로세스 전체에서 하나만 쓰는 Job. 앱이 끝나면 핸들이 닫히고, 그 순간
/// 이 Job 에 속한 모든 프로세스(손자 포함)가 커널에 의해 종료된다.
HANDLE killOnExitJob()
{
    static HANDLE job = [] () -> HANDLE {
        HANDLE created = CreateJobObjectW( nullptr, nullptr );
        if( created == nullptr )
            return nullptr;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if( !SetInformationJobObject( created, JobObjectExtendedLimitInformation,
                                     &limits, sizeof( limits ) ) )
        {
            CloseHandle( created );
            return nullptr;
        }
        return created;
    }();

    return job;
}

}  // namespace

void assignToKillOnExitJob( const qint64 processId )
{
    if( processId <= 0 )
        return;

    HANDLE job = killOnExitJob();
    if( job == nullptr )
        return;

    HANDLE process = OpenProcess( PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                 static_cast< DWORD >( processId ) );
    if( process == nullptr )
        return;

    // 이미 다른 Job 에 속해 있으면 실패할 수 있다. 그 경우는 그냥 넘어간다.
    AssignProcessToJobObject( job, process );
    CloseHandle( process );
}

bool isProcessRunning( const qint64 processId )
{
    if( processId <= 0 )
        return false;

    HANDLE process = OpenProcess( SYNCHRONIZE, FALSE, static_cast< DWORD >( processId ) );
    if( process == nullptr )
        return false;

    // 핸들이 열려도 이미 끝난 프로세스일 수 있다 (좀비). 대기 상태로 구분한다.
    const DWORD wait = WaitForSingleObject( process, 0 );
    CloseHandle( process );
    return wait == WAIT_TIMEOUT;
}

#else

void assignToKillOnExitJob( qint64 )
{
}

bool isProcessRunning( qint64 )
{
    return false;
}

#endif

void abandonProcess( std::unique_ptr< QProcess > process )
{
    if( process == nullptr )
        return;

    // 소유자가 곧 delete 되므로 시그널을 먼저 끊는다. 그러지 않으면 죽은
    // 수신자에게 finished/readyRead 가 전달될 수 있다.
    process->disconnect();

    if( process->state() != QProcess::NotRunning )
        process->kill();

    // 함수 지역 static 이라 이 목록의 소멸자는 정적 소멸까지 돌지 않는다.
    // 그때는 이미 커널이 프로세스를 정리했으므로 ~QProcess 도 기다리지 않는다.
    // (기다린다면 30초다 — 헤더 주석 참고.)
    static std::vector< QProcess* > abandoned;
    abandoned.push_back( process.release() );
}

void reapProcessLater( std::unique_ptr< QProcess > process, const int graceMs )
{
    if( process == nullptr )
        return;

    QProcess* raw = process.release();
    // 옛 소유자에게 가던 신호를 끊는다. 아래에서 우리 것만 다시 건다.
    raw->disconnect();

    // 프로세스가 끝난 **뒤에만** 객체를 지운다. 그래야 ~QProcess 가
    // waitForFinished() 로 넘어가지 않는다.
    QObject::connect( raw, &QProcess::finished, raw, [raw] { raw->deleteLater(); } );

    if( raw->state() == QProcess::NotRunning )
    {
        raw->deleteLater();
        return;
    }

    // 유예 시간이 지나도 살아 있으면 죽인다. 컨텍스트 객체가 raw 자신이라
    // 그 전에 지워지면 타이머도 함께 사라진다.
    QTimer::singleShot( graceMs, raw, [raw] {
        if( raw->state() != QProcess::NotRunning )
            raw->kill();   // 뒤이어 오는 finished 가 위 연결로 정리를 마무리한다
    } );
}

}  // namespace mrst
