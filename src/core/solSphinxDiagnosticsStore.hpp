#pragma once

#include "solSphinxDiagnostics.hpp"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace mrst {

/// 여러 출처의 진단을 모아 둔다.
///
/// 출처를 분리해 두는 게 핵심이다. Esbonio 는 파일 단위로
/// publishDiagnostics 를 보내고, sphinx-build 는 빌드 한 번에 전체를 보낸다.
/// 한 덩어리로 관리하면 프리뷰를 재빌드할 때마다 LSP 진단이 통째로 날아간다.
class DiagnosticsStore final : public QObject
{
    Q_OBJECT

public:
    explicit DiagnosticsStore( QObject* parent = nullptr );

    /// 한 출처의 진단을 통째로 교체한다 (sphinx-build 처럼 빌드 단위인 경우).
    void                                replaceSource( const QString& source,
                                                       const QVector< DiagnosticEntry >& entries );
    /// 한 출처 + 한 파일의 진단만 교체한다 (LSP publishDiagnostics 처럼 파일 단위).
    void                                replaceSourceForPath( const QString& source, const QString& path,
                                                              const QVector< DiagnosticEntry >& entries );
    void                                clearSource( const QString& source );
    void                                clear();

    /// 중복 제거된 전체 목록. 같은 위치면 Esbonio 가 sphinx-build 를 이긴다.
    [[nodiscard]] QVector< DiagnosticEntry > all() const;
    [[nodiscard]] QVector< DiagnosticEntry > forPath( const QString& path ) const;
    [[nodiscard]] int                   count() const;

signals:
    void                                changed();
    /// 특정 파일의 진단이 바뀌었다. 그 파일 편집기의 스퀴글만 갱신하면 된다.
    void                                pathChanged( const QString& path );

private:
    [[nodiscard]] static QString        normalizeKey( const QString& path );

    /// source -> (정규화된 경로 -> 진단들)
    QHash< QString, QHash< QString, QVector< DiagnosticEntry > > > bySource_;
};

}  // namespace mrst
