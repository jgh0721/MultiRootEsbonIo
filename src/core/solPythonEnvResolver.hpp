#pragma once

#include "solSphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace mrst {

class PythonEnvManager;

// Q_NAMESPACE 는 네임스페이스당 한 번만 선언할 수 있고 solPythonEnvMgr.hpp 가
// 이미 갖고 있다. 이 enum 은 시그널/QVariant 로 넘기지 않으므로 등록이 필요 없다.
enum class EnvKind
{
    Bundled,           ///< 앱이 들고 다니는 런타임
    ProjectVenv,       ///< 프로젝트 근처에서 찾은 .venv / venv / env
    ExplicitOverride,  ///< 사용자가 직접 지정
};

/// 한 Sphinx 프로젝트에 대해 확정된 Python 실행 환경.
struct ResolvedPythonEnv
{
    QString                             projectKey;
    EnvKind                             kind = EnvKind::Bundled;
    QString                             pythonExe;
    QString                             originPath;   ///< 이 환경을 고른 근거 경로
    QString                             reason;       ///< 사람이 읽을 설명 (한국어)

    [[nodiscard]] bool                  isBundled() const { return kind == EnvKind::Bundled; }
    [[nodiscard]] QString               displayName() const;
};

/// 프로젝트마다 어떤 Python 으로 Sphinx 를 돌릴지 정한다.
///
/// 왜 필요한가: 실제 문서 저장소는 자기 venv 에 테마와 확장을 설치해 둔다.
/// 번들 런타임에는 그것들이 없으므로, 번들만 고집하면 남의 프로젝트는 대부분
/// 빌드에 실패한다.
///
/// 반대로 Esbonio **서버 자체**는 항상 번들에서 돌린다. esbonio 2.x 는
/// sphinx_agent 를 별도 인터프리터로 띄우는 구조라(esbonio.sphinx.pythonCommand),
/// 사용자 venv 에 esbonio 를 설치하지 않아도 된다. 우리는 검증된 esbonio 하나만
/// 유지하고, conf.py/확장/테마 해석만 프로젝트 인터프리터에 맡긴다.
class PythonEnvResolver final : public QObject
{
    Q_OBJECT

public:
    explicit PythonEnvResolver( PythonEnvManager* manager, QObject* parent = nullptr );

    [[nodiscard]] static QString        projectKeyFor( const std::filesystem::path& rootPath );

    /// 워크스페이스 루트를 알려주면 상향 탐색을 그 위로 올라가지 않는다.
    void                                setWorkspaceRoot( const QString& root );

    [[nodiscard]] ResolvedPythonEnv     resolve( const SphinxProject& project );
    void                                invalidate( const QString& projectKey );
    void                                invalidateAll();

    /// 사용자가 특정 프로젝트에 인터프리터를 직접 지정한다. 빈 문자열이면 해제.
    void                                setOverride( const SphinxProject& project, const QString& pythonExe );

    /// 파일 검사로 잡지 못한 손상을 실제 Python 실행 결과로 확인했을 때 호출한다.
    void                                reportRuntimeFailure( const SphinxProject& project,
                                                              const QString& pythonExe,
                                                              const QString& reason );
    /// 복구가 끝난 환경을 다시 탐색할 수 있게 손상 표시와 캐시를 지운다.
    void                                clearDamage( const QString& projectKey );

signals:
    void                                logMessage( const QString& text );
    void                                environmentDamaged( const QString& projectKey,
                                                            const QString& projectId,
                                                            const QString& environmentPath,
                                                            const QString& pythonExe,
                                                            const QString& reason );
    void                                environmentDamageCleared( const QString& projectKey );

private:
    [[nodiscard]] QString               findProjectVenv( const std::filesystem::path& rootPath,
                                                         QString* originPath ) const;

    PythonEnvManager*                   manager_ = nullptr;
    QString                             workspaceRoot_;
    QHash< QString, ResolvedPythonEnv > cache_;
    QSet< QString >                     damagedKeys_;
};

}  // namespace mrst
