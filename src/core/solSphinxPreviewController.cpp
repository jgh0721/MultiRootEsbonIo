#include "stdafx.h"
#include "solSphinxPreviewController.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTextStream>

namespace mrst {

namespace {

bool isRestAdornmentLine(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.size() < 3) {
        return false;
    }
    static const QString chars = QStringLiteral("=-`:'\"~^_*+#<>");
    if (!chars.contains(trimmed.front())) {
        return false;
    }
    for (const QChar ch : trimmed) {
        if (ch != trimmed.front()) {
            return false;
        }
    }
    return true;
}

QString sourceMapSupportMarkup() {
    return QStringLiteral(R"(
<style id="mrst-source-map-style">.mrst-source-anchor{display:inline-block;width:0;height:0;overflow:hidden;scroll-margin-top:32px;}</style>
<script id="mrst-source-map">
window.mrstSourceAnchors = function() {
  return Array.from(document.querySelectorAll('[data-mrst-source-line]')).map(function(e) {
    return { line: parseInt(e.getAttribute('data-mrst-source-line'), 10) || 1, y: e.getBoundingClientRect().top + window.scrollY, element: e };
  }).sort(function(a, b) { return a.line - b.line; });
};
window.mrstScrollToSourceLine = function(line) {
  const anchors = window.mrstSourceAnchors();
  if (!anchors.length) return false;
  let best = anchors[0];
  for (const anchor of anchors) {
    if (anchor.line <= line) best = anchor; else break;
  }
  best.element.scrollIntoView({block: 'center', behavior: 'auto'});
  return true;
};
window.mrstLineFromViewportY = function(y) {
  const anchors = window.mrstSourceAnchors();
  if (!anchors.length) return 0;
  const target = window.scrollY + y;
  let best = anchors[0];
  let distance = Math.abs(best.y - target);
  for (const anchor of anchors) {
    const d = Math.abs(anchor.y - target);
    if (d < distance) { best = anchor; distance = d; }
  }
  return best.line;
};
</script>
)");
}

}  // namespace

bool annotateHtmlWithSourceLines(const QString& htmlPath, const QString& sourceFile) {
    if (htmlPath.trimmed().isEmpty() || sourceFile.trimmed().isEmpty()) {
        return false;
    }
    QFile htmlFile(htmlPath);
    if (!htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QString html = QString::fromUtf8(htmlFile.readAll());
    htmlFile.close();
    if (html.contains(QStringLiteral("id=\"mrst-source-map\""))) {
        return true;
    }

    QFile source(sourceFile);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QStringList lines = QString::fromUtf8(source.readAll()).split(QLatin1Char('\n'));
    int inserted = 0;
    for (int i = 0; i < lines.size() && inserted < 800; ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.size() < 3 || isRestAdornmentLine(trimmed) || trimmed.startsWith(QStringLiteral(".. ")) || trimmed.startsWith(QLatin1Char(':'))) {
            continue;
        }
        const QString escaped = trimmed.toHtmlEscaped();
        const int pos = html.indexOf(escaped, 0, Qt::CaseSensitive);
        if (pos < 0) {
            continue;
        }
        const QString anchor = QStringLiteral("<span id=\"mrst-source-line-%1\" data-mrst-source-line=\"%1\" class=\"mrst-source-anchor\"></span>").arg(i + 1);
        if (html.mid(qMax(0, pos - 160), 320).contains(anchor)) {
            continue;
        }
        html.insert(pos, anchor);
        ++inserted;
    }
    const QString support = sourceMapSupportMarkup();
    const int body = html.lastIndexOf(QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
    if (body >= 0) {
        html.insert(body, support);
    } else {
        html.append(support);
    }
    if (!htmlFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    htmlFile.write(html.toUtf8());
    return inserted > 0;
}

SphinxPreviewController::SphinxPreviewController(QObject* parent) : QObject(parent) {}

SphinxPreviewController::~SphinxPreviewController() {
    cancel();
}

bool SphinxPreviewController::isBuilding() const {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

QString SphinxPreviewController::lastHtmlPath() const {
    return lastHtmlPath_;
}

void SphinxPreviewController::build(const SphinxProject& project, const QString& sphinxBuildExe, const QString& sourceFile) {
    cancel();
    activeProject_ = project;
    activeSourceFile_ = sourceFile;
    activeOutput_.clear();
    const QString buildRoot = projectPath(project.buildPath);
    activeOutDir_ = QDir(buildRoot).filePath(QStringLiteral("preview/html"));
    const QString doctreeDir = QDir(buildRoot).filePath(QStringLiteral("preview/doctrees"));
    QDir().mkpath(activeOutDir_);
    QDir().mkpath(doctreeDir);

    process_ = std::make_unique<QProcess>();
    process_->setProgram(sphinxBuildExe);
    process_->setArguments({
        QStringLiteral("-b"), QStringLiteral("html"),
        QStringLiteral("-c"), projectPath(project.confPath.parent_path()),
        QStringLiteral("-d"), doctreeDir,
        projectPath(project.sourcePath),
        activeOutDir_,
    });
    process_->setWorkingDirectory(projectPath(project.rootPath));
    process_->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process_->setProcessEnvironment(env);
    connect(process_.get(), &QProcess::readyReadStandardOutput, this, &SphinxPreviewController::readOutput);
    connect(process_.get(), &QProcess::finished, this, &SphinxPreviewController::processFinished);
    emit buildStarted();
    process_->start();
    if (!process_->waitForStarted(5000)) {
        emit logMessage(QStringLiteral("sphinx-build 시작 실패: %1").arg(sphinxBuildExe));
        emit buildFinished(false, {});
        process_.reset();
    }
}

void SphinxPreviewController::cancel() {
    if (process_ == nullptr) {
        return;
    }
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    process_.reset();
}

void SphinxPreviewController::readOutput() {
    if (process_ == nullptr) {
        return;
    }
    const QString output = QString::fromUtf8(process_->readAllStandardOutput()).trimmed();
    if (!output.isEmpty()) {
        activeOutput_ += output;
        activeOutput_ += QLatin1Char('\n');
        emit logMessage(output);
    }
}

void SphinxPreviewController::processFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    readOutput();
    const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;
    lastHtmlPath_ = success ? sourceFileHtmlPath(activeProject_, activeOutDir_, activeSourceFile_) : QString{};
    if (success && !QFileInfo(lastHtmlPath_).isFile()) {
        lastHtmlPath_ = rootDocHtmlPath(activeProject_, activeOutDir_);
    }
    if (success && QFileInfo(lastHtmlPath_).isFile() && !activeSourceFile_.isEmpty()) {
        annotateHtmlWithSourceLines(lastHtmlPath_, activeSourceFile_);
    }
    emit diagnosticsReady(QStringLiteral("sphinx-build"), parseSphinxBuildDiagnostics(activeOutput_, projectPath(activeProject_.rootPath)));
    emit logMessage(success ? QStringLiteral("Sphinx preview build 완료") : QStringLiteral("Sphinx preview build 실패(exit=%1)").arg(exitCode));
    emit buildFinished(success, lastHtmlPath_);
    process_.reset();
}

QString SphinxPreviewController::projectPath(const std::filesystem::path& path) const {
    return QString::fromStdWString(std::filesystem::weakly_canonical(path).wstring());
}

QString SphinxPreviewController::rootDocHtmlPath(const SphinxProject& project, const QString& outDir) const {
    QString docName = QString::fromStdString(project.rootDoc);
    if (docName.isEmpty()) {
        docName = QStringLiteral("index");
    }
    return QDir(outDir).filePath(docName + QStringLiteral(".html"));
}

QString SphinxPreviewController::sourceFileHtmlPath(const SphinxProject& project, const QString& outDir, const QString& sourceFile) const {
    if (sourceFile.trimmed().isEmpty()) {
        return rootDocHtmlPath(project, outDir);
    }
    std::error_code ec;
    const std::filesystem::path sourcePath = std::filesystem::weakly_canonical(std::filesystem::path(sourceFile.toStdWString()), ec);
    const std::filesystem::path sourceRoot = std::filesystem::weakly_canonical(project.sourcePath, ec);
    std::filesystem::path relative = std::filesystem::relative(sourcePath, sourceRoot, ec);
    if (ec || relative.empty() || relative.native().starts_with(L"..")) {
        relative = sourcePath.filename();
    }
    QString relativeDoc = QString::fromStdWString(relative.wstring()).replace(QLatin1Char('\\'), QLatin1Char('/'));
    relativeDoc.replace(QRegularExpression(QStringLiteral(R"(\.(rst|txt|md)$)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(".html"));
    if (!relativeDoc.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive)) {
        relativeDoc += QStringLiteral(".html");
    }
    return QDir(outDir).filePath(relativeDoc);
}

}  // namespace mrst

