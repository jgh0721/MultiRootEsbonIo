#include "stdafx.h"
#include "solSphinxDiagnostics.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace mrst {
namespace {

int intOrDefault(const QJsonValue& value, int fallback) {
    if (value.isDouble()) {
        return value.toInt(fallback);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

QString locationKey(const DiagnosticEntry& entry) {
    QString base = !entry.path.isEmpty() ? QFileInfo(entry.path).absoluteFilePath() : entry.uri;
#ifdef Q_OS_WIN
    base = base.toCaseFolded();
#endif
    return QStringLiteral("%1:%2:%3").arg(base).arg(qMax(1, entry.line)).arg(qMax(1, entry.character));
}

bool isEsbonio(const DiagnosticEntry& entry) {
    return entry.source.compare(QStringLiteral("esbonio"), Qt::CaseInsensitive) == 0;
}

bool isSphinxBuild(const DiagnosticEntry& entry) {
    return entry.source.compare(QStringLiteral("sphinx-build"), Qt::CaseInsensitive) == 0;
}

}  // namespace

QString pathToFileUri(const QString& path) {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QString fileUriToPath(const QString& uri) {
    const QUrl url(uri);
    if (!url.isLocalFile()) {
        return {};
    }
    return QFileInfo(url.toLocalFile()).absoluteFilePath();
}

QString severityLabel(int severity) {
    switch (severity) {
    case 1:
        return QStringLiteral("Error");
    case 2:
        return QStringLiteral("Warning");
    case 3:
        return QStringLiteral("Information");
    case 4:
        return QStringLiteral("Hint");
    default:
        return QStringLiteral("Unknown");
    }
}

QString diagnosticTooltipText(const QVector<DiagnosticEntry>& entries, const QString& path, int line) {
    if (path.trimmed().isEmpty() || line <= 0) {
        return {};
    }
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    QStringList messages;
    for (const DiagnosticEntry& entry : entries) {
        if (entry.path.isEmpty()) {
            continue;
        }
        if (QFileInfo(entry.path).absoluteFilePath().compare(normalizedPath, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const int startLine = qMax(1, entry.line);
        const int endLine = qMax(startLine, entry.endLine);
        if (line < startLine || line > endLine) {
            continue;
        }
        messages.push_back(QStringLiteral("%1 · %2: %3")
            .arg(severityLabel(entry.severity), entry.source.isEmpty() ? QStringLiteral("diagnostic") : entry.source, entry.message));
    }
    return messages.join(QStringLiteral("\n"));
}

QVector<DiagnosticEntry> parseLspDiagnostics(const QString& uri, const QJsonArray& diagnostics) {
    QVector<DiagnosticEntry> entries;
    const QString path = fileUriToPath(uri);
    entries.reserve(diagnostics.size());
    for (const QJsonValue& value : diagnostics) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject diagnostic = value.toObject();
        const QJsonObject range = diagnostic.value(QStringLiteral("range")).toObject();
        const QJsonObject start = range.value(QStringLiteral("start")).toObject();
        const QJsonObject end = range.value(QStringLiteral("end")).toObject();
        DiagnosticEntry entry;
        entry.uri = uri;
        entry.path = path;
        entry.line = intOrDefault(start.value(QStringLiteral("line")), 0) + 1;
        entry.character = intOrDefault(start.value(QStringLiteral("character")), 0) + 1;
        entry.endLine = intOrDefault(end.value(QStringLiteral("line")), entry.line - 1) + 1;
        entry.endCharacter = intOrDefault(end.value(QStringLiteral("character")), entry.character) + 1;
        if (std::pair(entry.endLine, entry.endCharacter) <= std::pair(entry.line, entry.character)) {
            entry.endLine = entry.line;
            entry.endCharacter = entry.character + 1;
        }
        entry.severity = diagnostic.value(QStringLiteral("severity")).isDouble() ? diagnostic.value(QStringLiteral("severity")).toInt() : 0;
        entry.message = diagnostic.value(QStringLiteral("message")).toString();
        entry.source = diagnostic.value(QStringLiteral("source")).toString(QStringLiteral("esbonio"));
        entries.push_back(entry);
    }
    return entries;
}

QVector<DiagnosticEntry> parseSphinxBuildDiagnostics(const QString& output, const QString& workingDirectory) {
    static const QRegularExpression re(QStringLiteral(R"(^(.+?):(\d+):\s*(WARNING|ERROR|CRITICAL):\s*(.*)$)"));
    QVector<DiagnosticEntry> entries;
    const QStringList lines = output.split(QRegularExpression(QStringLiteral("\r?\n")), Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        const QRegularExpressionMatch match = re.match(rawLine.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        QString path = match.captured(1);
        if (QDir::isRelativePath(path)) {
            path = QDir(workingDirectory).absoluteFilePath(path);
        }
        path = QFileInfo(path).absoluteFilePath();
        const QString level = match.captured(3);
        DiagnosticEntry entry;
        entry.path = path;
        entry.uri = pathToFileUri(path);
        entry.line = qMax(1, match.captured(2).toInt());
        entry.character = 1;
        entry.endLine = entry.line;
        entry.endCharacter = 2;
        entry.severity = (level == QStringLiteral("ERROR") || level == QStringLiteral("CRITICAL")) ? 1 : 2;
        entry.message = match.captured(4);
        entry.source = QStringLiteral("sphinx-build");
        entries.push_back(entry);
    }
    return entries;
}

QVector<DiagnosticEntry> deduplicateDiagnostics(const QVector<DiagnosticEntry>& entries) {
    QVector<DiagnosticEntry> result;
    QHash<QString, qsizetype> indexByLocation;
    for (const DiagnosticEntry& entry : entries) {
        const QString key = locationKey(entry);
        const auto it = indexByLocation.constFind(key);
        if (it == indexByLocation.constEnd()) {
            indexByLocation.insert(key, result.size());
            result.push_back(entry);
            continue;
        }
        DiagnosticEntry& existing = result[*it];
        if (isEsbonio(entry) && isSphinxBuild(existing)) {
            existing = entry;
        }
    }
    return result;
}

}  // namespace mrst


