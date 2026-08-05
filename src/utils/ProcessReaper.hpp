#pragma once

#include <QtGlobal>

namespace mrst {

/// 자식 프로세스를 "앱이 죽으면 같이 죽는" 그룹에 넣는다.
///
/// 왜 필요한가: Esbonio 서버는 다시 sphinx_agent 를 자식으로 띄운다. Windows
/// 에는 프로세스 트리 개념이 없어서 우리가 직접 자식만 terminate/kill 해도
/// 손자는 그대로 살아남는다. 실제로 앱을 한 번 띄웠다 닫을 때마다 python
/// 프로세스 4개가 고아로 남았다.
///
/// Job Object 에 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 를 걸어 두면, 우리 프로세스가
/// 정상 종료하든 크래시하든 핸들이 닫히면서 그룹 전체가 정리된다.
///
/// Windows 외 플랫폼에서는 아무 것도 하지 않는다.
void assignToKillOnExitJob( qint64 processId );

}  // namespace mrst
