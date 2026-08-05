#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mrst {

struct SearchMatch
{
    QString                             path;
    int                                 line = 1;     ///< 1-based
    int                                 column = 1;   ///< 1-based
    QString                             text;         ///< 매치가 있는 줄 전체
};

struct ReplacePreview
{
    QString                             path;
    int                                 replacements = 0;
    QString                             diff;         ///< unified diff
};

struct SearchOptions
{
    bool                                caseSensitive = false;
    bool                                wholeWords = false;
    bool                                regex = false;
    /// 소문자 확장자 목록 (점 없이). 비면 기본 텍스트 확장자.
    QStringList                         suffixes;
    int                                 maxMatches = 5000;
};

/// 검색 대상 파일을 훑는다. 빌드/캐시/VCS 디렉터리는 건너뛴다.
[[nodiscard]] QStringList collectSearchableFiles( const QString& root, const QStringList& suffixes = {} );

/// 디스크 I/O 가 있으므로 작업 스레드에서 부른다.
[[nodiscard]] QVector< SearchMatch > findInFiles( const QString& root, const QString& query,
                                                  const SearchOptions& options = {} );

/// 바꾸기 미리보기. 실제로 쓰지는 않는다.
[[nodiscard]] QVector< ReplacePreview > previewReplaceInFiles( const QString& root, const QString& query,
                                                               const QString& replacement,
                                                               const SearchOptions& options = {},
                                                               int contextLines = 3 );

/// 미리보기에서 확인한 파일들에 실제로 적용한다. 바뀐 파일 경로를 돌려준다.
[[nodiscard]] QStringList applyReplaceInFiles( const QStringList& paths, const QString& query,
                                               const QString& replacement,
                                               const SearchOptions& options = {} );

/// 진짜 unified diff.
///
/// src_cpp 의 것은 같은 인덱스끼리 비교하는 방식이라 한 줄만 삽입돼도 그 뒤
/// 전부가 변경으로 보고됐다. LCS 로 공통 부분을 먼저 찾고 hunk 를 만든다.
[[nodiscard]] QStringList unifiedDiffLines( const QStringList& before, const QStringList& after,
                                            int contextLines = 3 );
[[nodiscard]] QString unifiedDiff( const QString& before, const QString& after,
                                   const QString& label, int contextLines = 3 );

}  // namespace mrst
