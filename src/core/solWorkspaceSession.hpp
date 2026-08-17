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
    QList< int >                        sideSplitterSizes;      ///< 사이드바 | 본문
    QList< int >                        contentSplitterSizes;   ///< 편집기 | 진단/로그
    QList< int >                        previewSplitterSizes;   ///< 편집기 | 프리뷰

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
