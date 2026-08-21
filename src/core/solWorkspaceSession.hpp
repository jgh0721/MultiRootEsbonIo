#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>

namespace mrst {

/// 열려 있던 문서 하나의 복원 정보.
struct OpenDocumentState
{
    QString                             path;
    int                                 caretLine = 1;
    int                                 caretColumn = 1;
    int                                 firstVisibleLine = 1;
};

/// 워크스페이스 하나에 딸린 세션.
///
/// 전역 설정(AppSettings)과 분리한다. 사용자는 워크스페이스를 여러 개 오가고,
/// 각각의 열린 탭과 스플리터 배치는 그 워크스페이스에 속한 상태이지
/// 애플리케이션 전체의 상태가 아니다.
struct WorkspaceSession
{
    int                                 schema = 1;
    QString                             workspaceRoot;
    QVector< OpenDocumentState >        documents;
    int                                 activeIndex = -1;
    QList< int >                        previewSplitterSizes;   ///< 편집기 | 프리뷰
    /// 좌측·하단 도크의 배치. ads::CDockManager::saveState() 를 base64 로 담는다
    /// (기본 설정이 XML 압축이라 사람이 읽을 수 있는 형태가 아니다).
    ///
    /// 스키마를 올리지 않고 키만 더한다. sessionFromJson() 은 스키마가 다르면
    /// 세션을 통째로 버리므로, 올리면 이 버전으로 올라오는 사용자가 열어 둔
    /// 탭을 전부 잃는다. 비어 있으면 기본 배치를 쓰면 되니 그럴 이유가 없다.
    QString                             dockLayout;

    [[nodiscard]] bool isEmpty() const { return workspaceRoot.isEmpty() && documents.isEmpty(); }
};

/// `<root>/.multiroot/workspace.json`
[[nodiscard]] QString sessionFilePath( const QString& workspaceRoot );

[[nodiscard]] QJsonObject sessionToJson( const WorkspaceSession& session );
/// 스키마가 다르거나 형식이 깨졌으면 빈 세션을 돌려준다.
/// 복원에 실패하는 것은 불편할 뿐이지만, 깨진 값을 그대로 쓰면 창이 이상해진다.
[[nodiscard]] WorkspaceSession sessionFromJson( const QJsonObject& object );

[[nodiscard]] WorkspaceSession loadWorkspaceSession( const QString& workspaceRoot );
bool saveWorkspaceSession( const WorkspaceSession& session );

}  // namespace mrst
