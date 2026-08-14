#pragma once

#include "solSphinxDiagnostics.hpp"
#include "solSphinxScanner.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>

class QTimer;

namespace mrst {

class UvTask;

/// 한 번의 프리뷰 빌드 요청.
struct PreviewBuildRequest
{
    SphinxProject                       project;
    QString                             pythonExe;          ///< 빌더 스크립트를 돌릴 인터프리터
    /// pythonExe 에 sphinx 가 없을 때 대신 쓸 인터프리터(번들). 비우면 폴백 없음.
    QString                             fallbackPythonExe;
    QString                             builderScript;      ///< mrr_sphinx_preview_build.py 절대 경로
    QString                             sourceFile;         ///< 편집 중인 원본 (htmlPath 계산용)
    QString                             shadowFile;         ///< 미저장 버퍼 사본. 없으면 빈 문자열
};

/// 빌더가 남긴 사이드카 JSON 을 그대로 옮긴 결과.
struct PreviewBuildResult
{
    bool                                ok = false;
    bool                                cancelled = false;
    QString                             projectId;
    QString                             htmlPath;
    /// 이 결과를 만든 빌드의 일련번호. 출력 디렉터리가 고정이라 HTML 경로가
    /// 매번 같으므로, 이 값을 URL 쿼리에 실어 Chromium 캐시를 무효화한다.
    int                                 serial = 0;
    /// 이 결과를 만든 요청의 원본 파일. 요청과 결과를 짝지어야 "요청한 문서가
    /// 정말 렌더됐는가" 를 판정할 수 있다 (빌더는 못 찾으면 root 문서로 물러선다).
    QString                             sourceFile;
    QString                             primaryDocname;
    /// data-mrr-src 인덱스와 순서가 같다. 증분 빌드에서도 인덱스가 흔들리지 않도록
    /// 빌더가 출력 디렉터리에 누적해 두는 대응표라, 이번 빌드가 건드리지 않은
    /// 파일도 들어 있다.
    QStringList                         sources;
    /// 이번 빌드가 실제로 다시 처리한 원본. 진단 교체 범위는 이쪽을 써야 한다.
    QStringList                         processedSources;
    QString                             sphinxVersion;
    QString                             htmlTheme;
    QVector< DiagnosticEntry >          diagnostics;
    QStringList                         missingExtensions;  ///< 배포명 (설치 제안용)
    QStringList                         missingModules;     ///< 모듈명 (표시용)
    QStringList                         missingThemes;
    QString                             traceback;
};

/// Sphinx HTML 프리뷰 빌드를 관리한다.
///
/// sphinx-build 를 직접 부르지 않고 번들 인터프리터로 mrr_sphinx_preview_build.py
/// 를 돌린다. 그래야 doctree 에서 정확한 원본 줄 범위를 얻을 수 있다.
class SphinxPreviewController final : public QObject
{
    Q_OBJECT

public:
    explicit SphinxPreviewController( QObject* parent = nullptr );
    ~SphinxPreviewController() override;

    [[nodiscard]] bool                  isBuilding() const;
    [[nodiscard]] QString               lastHtmlPath() const;

    void                                setShadowDir( const QString& path );
    void                                setDebounceInterval( int milliseconds );

    /// 디바운스 후 빌드한다. 편집 중에는 이쪽을 쓴다.
    void                                requestBuild( const PreviewBuildRequest& request );
    /// 즉시 빌드한다. 저장/탭 전환처럼 사용자가 결과를 기다리는 경우.
    void                                buildNow( const PreviewBuildRequest& request );
    void                                cancel();

signals:
    void                                logMessage( const QString& text );
    /// 이번 빌드에서 실제로 다시 읽힌 원본 파일들. 진단을 이 범위로만
    /// 교체해야 증분 빌드에서 다른 파일의 진단이 사라지지 않는다.
    /// diagnosticsReady 보다 먼저 발신된다.
    void                                processedSourcesKnown( const QStringList& sources );
    void                                buildStarted( const QString& projectId );
    void                                buildFinished( const mrst::PreviewBuildResult& result );
    void                                diagnosticsReady( const QString& source,
                                                          const QVector< DiagnosticEntry >& entries );
    /// Phase 8 (누락 패키지 자동 설치) 로 이어지는 진입점.
    void                                missingDependenciesDetected( const QString& projectId,
                                                                     const QStringList& distributions,
                                                                     const QStringList& themes );

private:
    void                                startBuild();
    void                                finishBuild( int exitCode, bool crashed, bool cancelled );
    [[nodiscard]] PreviewBuildResult    readReport( const QString& reportPath ) const;
    /// 프로젝트당 고정된 출력 디렉터리. 회전시키면 Sphinx 의 증분 판정이 죽는다.
    [[nodiscard]] QString               outputDir() const;
    void                                cleanupStaleOutputDirs( const QString& keepDir ) const;
    [[nodiscard]] QString               writeShadowCopy() const;

    PreviewBuildRequest                 pending_;
    PreviewBuildRequest                 active_;
    bool                                hasPending_ = false;
    QTimer*                             debounceTimer_ = nullptr;
    QPointer< UvTask >                  task_;
    QString                             shadowDir_;
    QString                             activeOutDir_;
    QString                             activeReportPath_;
    QString                             lastHtmlPath_;
    int                                 buildSerial_ = 0;
    /// 이번 요청에서 이미 번들로 한 번 물러섰는가 (무한 재시도 방지).
    bool                                usedFallbackPython_ = false;
};

}  // namespace mrst

Q_DECLARE_METATYPE( mrst::PreviewBuildResult )
