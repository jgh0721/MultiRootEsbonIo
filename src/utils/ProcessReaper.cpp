#include "stdafx.h"
#include "ProcessReaper.hpp"

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

#else

void assignToKillOnExitJob( qint64 )
{
}

#endif

}  // namespace mrst
