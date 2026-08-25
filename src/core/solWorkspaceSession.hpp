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

    /// 창의 크기·위치·최대화 여부. QWidget::saveGeometry() 를 base64 로 담는다.
    ///
    /// dockLayout 과 같은 이유로 스키마를 올리지 않는다. 비어 있으면 기본 크기로
    /// 뜨면 될 뿐이고, 올리면 이 버전으로 올라오는 사용자의 열린 탭이 사라진다.
    ///
    /// **전체 화면 상태는 담지 않는다.** F11(프리뷰 전체 화면)이 켜진 채로 종료하면
    /// saveGeometry() 가 그 플래그까지 담아, 다음 실행이 메뉴도 편집기도 없는
    /// 전체 화면으로 뜬다. 저장하는 쪽이 전체 화면에 들어가기 직전 값을 쓴다.
    QString                             windowGeometry;

    [[nodiscard]] bool isEmpty() const { return workspaceRoot.isEmpty() && documents.isEmpty(); }
};

/// 마지막으로 보고 있던 문서의 경로. 없으면 빈 문자열.
///
/// **activeIndex 는 documents 의 번호이지 탭 위젯의 번호가 아니다.** 복원 쪽에서
/// 그것을 곧바로 setCurrentIndex() 에 넣었다가 물렸다 — 핫 엑시트 스냅샷이 먼저
/// 탭을 열어 두고 사라진 파일은 건너뛰므로 두 번호가 어긋나고, 밀린 자리가 이름
/// 없는 버퍼면 경로가 없어 프리뷰가 아예 만들어지지 않는다. 그래서 "번호" 가
/// 아니라 "경로" 를 돌려주는 이름 있는 함수로 못 박는다.
[[nodiscard]] QString activeDocumentPath( const WorkspaceSession& session );

/// `<root>/.multiroot/workspace.json`
[[nodiscard]] QString sessionFilePath( const QString& workspaceRoot );

[[nodiscard]] QJsonObject sessionToJson( const WorkspaceSession& session );
/// 스키마가 다르거나 형식이 깨졌으면 빈 세션을 돌려준다.
/// 복원에 실패하는 것은 불편할 뿐이지만, 깨진 값을 그대로 쓰면 창이 이상해진다.
[[nodiscard]] WorkspaceSession sessionFromJson( const QJsonObject& object );

[[nodiscard]] WorkspaceSession loadWorkspaceSession( const QString& workspaceRoot );
bool saveWorkspaceSession( const WorkspaceSession& session );

}  // namespace mrst
