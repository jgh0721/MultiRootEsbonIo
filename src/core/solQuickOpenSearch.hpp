#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <stop_token>

namespace mrst {

/// UI와 순수 검색 API가 함께 적용하는 질의 길이 상한.
inline constexpr int kQuickOpenMaximumQueryLength = 256;

/// 빠른 파일 열기 한 행에 필요한 순수 검색 결과.
///
/// 모든 경로는 워크스페이스 기준이며 구분자는 `/` 로 정규화된다.
/// matchedPositions 의 인덱스는 QString 의 UTF-16 인덱스다. 파일명 위치는
/// fileName 기준이고, 경로 위치는 relativePath 기준이다. 경로 위치에는 파일명
/// 부분이 들어가지 않으므로 directoryPath 를 별도로 그릴 때도 그대로 쓸 수 있다.
struct QuickOpenMatch
{
    QString                             relativePath;
    QString                             fileName;
    QString                             directoryPath;
    QVector< int >                      fileNameMatchedPositions;
    QVector< int >                      pathMatchedPositions;
};

/// 증분 인덱싱 결과를 다시 점수화하지 않고 병합하기 위한 내부 정렬 정보.
/// QuickOpen UI 모델은 이 값을 보관하지만 화면에는 match만 노출한다.
struct QuickOpenRankedMatch
{
    QuickOpenMatch                      match;
    int                                 tier = 0;
    int                                 score = 0;
    int                                 recentRank = 0;
    qsizetype                           inputOrder = 0;
};

using QuickOpenRankedMatches = QVector< QuickOpenRankedMatch >;

/// GUI 스레드에서 증가하는 전체 QStringList를 복사하지 않도록 유지하는
/// 불변 경로 묶음. 각 chunk는 게시된 뒤 수정하지 않는다.
using QuickOpenPathChunk = std::shared_ptr< const QStringList >;
using QuickOpenPathChunks = QVector< QuickOpenPathChunk >;

/// 빠른 열기에서 쓰는 경로 표기로 바꾼다.
///
/// `/` 와 `\` 를 같은 구분자로 보고, 중복 구분자와 `.`/`..` 구간을 정리한다.
/// 반환값에는 선행·후행 구분자가 없고 루트 자체는 빈 문자열이다.
[[nodiscard]] QString normalizeQuickOpenPath( const QString& path );

/// 워크스페이스 상대 경로 전체를 query 에 맞춰 정렬한다.
///
/// 결과 개수는 제한하지 않는다. 화면에 150개씩 노출하는 일은 UI 모델의
/// 책임이다. query 가 비었으면 recentRelativePaths 순서가 먼저이고 나머지는
/// 안정적인 알파벳 순서다. query 가 있으면 파일명 정확 일치, 파일명
/// 접두/경계 일치, 파일명 퍼지 일치, 경로 일치 순으로 정렬한다.
/// query가 kQuickOpenMaximumQueryLength를 넘으면 빈 결과를 반환한다.
/// stopToken에 취소가 요청되면 부분 결과를 노출하지 않고 빈 벡터를 반환한다.
[[nodiscard]] QVector< QuickOpenMatch > rankQuickOpenFiles(
    const QStringList& relativePaths, const QString& query,
    const QStringList& recentRelativePaths = {}, std::stop_token stopToken = {} );

/// 불변 경로 묶음들을 평탄화하지 않고 순서대로 검색한다.
///
/// GUI는 chunk 포인터만 복사해 안정적인 snapshot을 만들 수 있으므로, 인덱싱 중
/// 다음 batch가 도착해도 누적 QStringList의 copy-on-write detach가 발생하지 않는다.
[[nodiscard]] QVector< QuickOpenMatch > rankQuickOpenFileChunks(
    const QuickOpenPathChunks& relativePathChunks, const QString& query,
    const QStringList& recentRelativePaths = {}, std::stop_token stopToken = {} );

/// chunk 범위의 정렬 정보를 보존한 채 검색한다. inputOrderOffset은 전체
/// 인덱스에서 이 범위보다 앞선 경로 수이며, 증분 결과의 안정적인 순서를
/// 유지하는 데 사용한다.
[[nodiscard]] QuickOpenRankedMatches rankQuickOpenFileChunksDetailed(
    const QuickOpenPathChunks& relativePathChunks, const QString& query,
    const QStringList& recentRelativePaths, qsizetype inputOrderOffset,
    std::stop_token stopToken = {} );

/// 같은 query와 최근 파일 목록으로 각각 정렬된 두 결과를 선형 시간에 합친다.
[[nodiscard]] QuickOpenRankedMatches mergeQuickOpenRankedMatches(
    const QuickOpenRankedMatches& existing, QuickOpenRankedMatches added,
    const QString& query, std::stop_token stopToken = {} );

}   // namespace mrst
