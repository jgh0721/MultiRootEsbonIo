#pragma once

#include "solSphinxDiagnostics.hpp"
#include "solSphinxScanner.hpp"

#include <QObject>
#include <QProcess>
#include <QString>

#include <memory>

namespace mrst {

bool annotateHtmlWithSourceLines(const QString& htmlPath, const QString& sourceFile);

class SphinxPreviewController final : public QObject {
    Q_OBJECT

public:
    explicit SphinxPreviewController(QObject* parent = nullptr);
    ~SphinxPreviewController() override;

    [[nodiscard]] bool isBuilding() const;
    [[nodiscard]] QString lastHtmlPath() const;

    void build(const SphinxProject& project, const QString& sphinxBuildExe, const QString& sourceFile = {});
    void cancel();

signals:
    void logMessage(const QString& text);
    void buildStarted();
    void buildFinished(bool success, const QString& htmlPath);
    void diagnosticsReady(const QString& source, const QVector<DiagnosticEntry>& entries);

private slots:
    void readOutput();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    [[nodiscard]] QString projectPath(const std::filesystem::path& path) const;
    [[nodiscard]] QString rootDocHtmlPath(const SphinxProject& project, const QString& outDir) const;
    [[nodiscard]] QString sourceFileHtmlPath(const SphinxProject& project, const QString& outDir, const QString& sourceFile) const;

    std::unique_ptr<QProcess> process_;
    SphinxProject activeProject_;
    QString activeOutDir_;
    QString activeSourceFile_;
    QString activeOutput_;
    QString lastHtmlPath_;
};

}  // namespace mrst

