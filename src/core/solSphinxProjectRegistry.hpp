#pragma once

#include "solSphinxScanner.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <vector>

namespace mrst {

/// 워크스페이스의 Sphinx 프로젝트 목록을 소유한다.
///
/// ProjectScanner 를 감싸서 (1) 비동기 스캔, (2) `<root>/.multiroot/projects.json`
/// 캐시, (3) 파일 -> 프로젝트 해석을 제공한다. 스캔은 디스크 전체를 훑으므로
/// 절대 GUI 스레드에서 돌리지 않는다.
class ProjectRegistry final : public QObject
{
    Q_OBJECT

public:
    explicit ProjectRegistry( QObject* parent = nullptr );
    ~ProjectRegistry() override;

    void                                setWorkspaceRoot( const QString& root );
    [[nodiscard]] QString               workspaceRoot() const;

    void                                setScannerSettings( ScannerSettings settings );
    [[nodiscard]] const ScannerSettings& scannerSettings() const;

    [[nodiscard]] bool                  isScanning() const;
    [[nodiscard]] const std::vector< SphinxProject >& projects() const;

    /// 파일에 가장 가까운 프로젝트. 없으면 nullptr (가상 프로젝트 대상).
    /// 반환 포인터는 다음 스캔/캐시 로드 전까지만 유효하다.
    [[nodiscard]] const SphinxProject*  resolveForFile( const QString& filePath ) const;
    [[nodiscard]] const SphinxProject*  findById( const QString& projectId ) const;

    /// 캐시를 읽어 즉시 사용 가능한 목록을 만든다. conf.py 가 사라진 항목은 버린다.
    /// 유효한 항목을 하나라도 얻으면 true.
    bool                                loadCache();
    void                                saveCache() const;

    /// 백그라운드 스캔을 시작한다. 이미 진행 중이면 무시한다.
    void                                rescanAsync();

signals:
    void                                scanStarted();
    void                                scanFinished( int projectCount );
    void                                logMessage( const QString& text );

private:
    [[nodiscard]] QString               cacheFilePath() const;
    void                                applyScanResult( std::vector< SphinxProject > scanned, quint64 generation );

    QString                             workspaceRoot_;
    ScannerSettings                     settings_;
    std::vector< SphinxProject >        projects_;
    std::atomic_bool                    scanning_{ false };
    quint64                             generation_ = 0;   ///< 늦게 도착한 스캔 결과 폐기용
};

}  // namespace mrst
