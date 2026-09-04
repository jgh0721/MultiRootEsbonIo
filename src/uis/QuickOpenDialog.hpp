#pragma once

#include "core/solQuickOpenSearch.hpp"

#include <QDialog>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <stop_token>

class QEvent;
class QLabel;
class QLineEdit;
class QListView;
class QShowEvent;
class QThreadPool;
class QTimer;
class QToolButton;
class QWidget;

namespace mrst {

class QuickOpenListModel;

/// 워크스페이스의 파일을 이름 또는 상대 경로로 빠르게 찾아 여는 대화상자.
///
/// 인덱스 소유권은 MainWindow/인덱서에 있다. 이 위젯은 전달받은 스냅샷을
/// 백그라운드에서 점수화하고, 결과를 150개씩 점진적으로 노출하는 일만 맡는다.
/// paths 는 '/'와 '\\' 어느 쪽 구분자든 받을 수 있으며 절대 경로도 현재
/// workspaceRoot 안에 있으면 상대 경로로 바꿔 받아들인다.
class QuickOpenDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit QuickOpenDialog( QWidget* parent = nullptr );
    ~QuickOpenDialog() override;

    /// 새 검색 세션을 시작하고 대화상자를 부모 창 가운데에 표시한다.
    void                                showForWorkspace(
                                            const QString& workspaceRoot,
                                            const QStringList& paths,
                                            bool indexing,
                                            const QStringList& recentRelativePaths = {},
                                            const QString& shortcutNativeText = {} );

    /// PathIndex가 보장한 root-relative '/' 경로를 복사 없이 받아 표시한다.
    /// 일반 호출자는 showForWorkspace()를 써야 한다. 이 fast path에는 경로
    /// 정리·범위 검사·중복 제거 비용이 없는 대신 입력 계약을 호출자가 지킨다.
    void                                showForPathIndex(
                                            const QString& workspaceRoot,
                                            const QStringList& normalizedRelativePaths,
                                            bool indexing,
                                            const QStringList& recentRelativePaths = {},
                                            const QString& shortcutNativeText = {} );

    /// PathIndex의 진행 중 불변 chunk들을 평탄화하지 않고 받아 표시한다.
    /// scannedPathCount에는 아직 GUI로 전달되지 않은 발견 항목도 포함될 수 있다.
    void                                showForPathIndexChunks(
                                            const QString& workspaceRoot,
                                            const QuickOpenPathChunks& normalizedRelativePathChunks,
                                            bool indexing, qsizetype scannedPathCount,
                                            const QStringList& recentRelativePaths = {},
                                            const QString& shortcutNativeText = {} );

    /// 현재 인덱스 스냅샷을 통째로 교체한다.
    void                                replaceIndexedPaths( const QStringList& paths );

    /// 백그라운드 인덱서가 보내 온 새 경로 묶음을 더한다.
    /// indexedTotal 이 음수이면 중복 제거 뒤의 현재 경로 수를 상태에 표시한다.
    void                                appendIndexedPaths( const QStringList& batch,
                                                           qsizetype indexedTotal = -1 );

    /// PathIndex 전용 fast path. 한 스캔 안에서 중복 없는 정규화 경로라는
    /// PathIndex 계약을 신뢰한다.
    void                                replacePathIndexSnapshot(
                                            const QStringList& normalizedRelativePaths );
    void                                replacePathIndexChunks(
                                            const QuickOpenPathChunks& normalizedRelativePathChunks,
                                            qsizetype scannedPathCount );
    void                                appendPathIndexBatch( const QStringList& normalizedBatch,
                                                             qsizetype indexedTotal );

    /// 스캔 수만 먼저 바뀌었을 때 하단 진행 상태를 갱신한다.
    void                                setIndexingProgress( qsizetype indexedTotal );

    /// 현재 경로 목록을 그대로 두고 인덱싱 완료 상태로 전환한다.
    void                                finishIndexing();

    /// 최종 스냅샷으로 교체한 뒤 인덱싱 완료 상태로 전환한다.
    void                                finishIndexing( const QStringList& finalPaths );
    void                                finishPathIndexing(
                                            const QStringList& normalizedFinalPaths );

signals:
    /// 선택한 파일의 정리된 절대 경로.
    void                                fileChosen( const QString& absolutePath );

protected:
    void                                changeEvent( QEvent* event ) override;
    bool                                eventFilter( QObject* watched, QEvent* event ) override;
    void                                showEvent( QShowEvent* event ) override;

private:
    void                                buildUi();
    void                                retranslateUi();
    void                                normalizeAndReplacePaths( const QStringList& paths );
    void                                assignPathIndexSnapshot( const QStringList& paths );
    void                                assignPathIndexChunks( const QuickOpenPathChunks& paths );
    void                                ensureIndexedPathKeys();
    [[nodiscard]] QString               normalizeIncomingPath( const QString& path ) const;
    void                                showPreparedWorkspace(
                                            const QStringList& recentRelativePaths,
                                            const QString& shortcutNativeText );
    void                                scheduleRanking();
    void                                scheduleUserRanking();
    [[nodiscard]] QString               currentQuery() const;
    void                                scheduleIncrementalRanking();
    void                                recordQueryInputActivity();
    void                                armPendingRankingTimer();
    void                                beginRanking();
    void                                appendPathChunk( QStringList paths );
    void                                resetPathChunks( QStringList paths );
    void                                scheduleFooterUpdate();
    void                                retireMatchesInBackground(
                                            std::shared_ptr< const QuickOpenRankedMatches > matches );
    void                                recoverCanceledRankingForShow();
    void                                chooseCurrent();
    void                                moveCurrent( int rows );
    void                                prefetchIfNeeded();
    void                                updateFooter();
    void                                updateShortcutLabel();
    void                                positionOverParent();

    QString                             workspaceRoot_;
    QString                             shortcutNativeText_;
    QuickOpenPathChunks                 indexedPathChunks_;
    QStringList                         recentRelativePaths_;
    QSet< QString >                     indexedPathKeys_;
    bool                                indexedPathKeysValid_ = true;
    qsizetype                           indexedPathCount_ = 0;
    qsizetype                           indexedTotal_ = 0;
    quint64                             pathSnapshotGeneration_ = 0;
    quint64                             appliedPathSnapshotGeneration_ = 0;
    qsizetype                           appliedChunkCount_ = 0;
    qsizetype                           appliedPathCount_ = 0;
    QString                             appliedQuery_;
    bool                                appliedRankingValid_ = false;
    quint64                             rankingGeneration_ = 0;
    quint64                             activeRankingGeneration_ = 0;
    std::stop_source                    rankingStopSource_;
    bool                                indexing_ = false;
    bool                                openErrorVisible_ = false;
    bool                                rankingInFlight_ = false;
    bool                                rankingDirty_ = false;
    bool                                rankingRequestIsIncremental_ = false;
    QElapsedTimer                       rankingThrottleClock_;
    QElapsedTimer                       queryInputClock_;
    std::shared_ptr< QObject >          guiDispatcher_;

    QWidget*                            panel_ = nullptr;
    QLabel*                             titleLabel_ = nullptr;
    QLabel*                             shortcutLabel_ = nullptr;
    QToolButton*                        closeButton_ = nullptr;
    QLineEdit*                          searchEdit_ = nullptr;
    QListView*                          resultView_ = nullptr;
    QuickOpenListModel*                 resultModel_ = nullptr;
    QLabel*                             emptyLabel_ = nullptr;
    QLabel*                             statusLabel_ = nullptr;
    QLabel*                             helpLabel_ = nullptr;
    QTimer*                             rankingTimer_ = nullptr;
    QTimer*                             footerTimer_ = nullptr;
    QThreadPool*                        rankingPool_ = nullptr;
};

}  // namespace mrst
