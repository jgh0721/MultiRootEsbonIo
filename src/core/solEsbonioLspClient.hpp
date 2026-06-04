#pragma once

#include "solSphinxScanner.hpp"
#include "solSphinxDiagnostics.hpp"

#include <QObject>
#include <QProcess>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QSet>

#include <memory>

namespace mrst {

struct LspCompletionItem {
    QString label;
    QString insertText;
    QString detail;
    int kind = 0;
    QString filterText;
};

struct LspDocumentSymbol {
    QString name;
    QString detail;
    int kind = 0;
    int line = 1;
    int column = 1;
    int endLine = 1;
    int endColumn = 1;
    QList<LspDocumentSymbol> children;
};

[[nodiscard]] QList<LspDocumentSymbol> parseDocumentSymbols(const QJsonValue& result);

class JsonRpcWriter final
{
public:
    [[nodiscard]] int nextId() const;
    [[nodiscard]] QByteArray request( const QString& method, const QJsonObject& params, int* idOut = nullptr );
    [[nodiscard]] QByteArray notify( const QString& method, const QJsonObject& params );
    [[nodiscard]] QByteArray response( int id, const QJsonValue& result );

private:
    int nextId_ = 1;
    [[nodiscard]] static QByteArray frame( const QJsonObject& payload );
};

class JsonRpcParser final
{
public:
    void append( const QByteArray& bytes );
    [[nodiscard]] QList<QJsonObject> takeMessages();

private:
    QByteArray buffer_;
};

class LspClient final : public QObject {
    Q_OBJECT

public:
    explicit LspClient(QObject* parent = nullptr);
    ~LspClient() override;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString activeProjectId() const;

    void start(const SphinxProject& project, const QString& pythonExe, const QString& sphinxBuildExe);
    void stop();
    void didOpen(const QString& path, const QString& text, const QString& languageId = QStringLiteral("rst"));
    void didChange(const QString& path, const QString& text);
    void didSave(const QString& path, const QString& text);
    void didClose(const QString& path);
    int completion(const QString& path, int line, int column);
    int documentSymbols(const QString& path);

signals:
    void logMessage(const QString& text);
    void jsonMessage(const QJsonObject& message);
    void diagnosticsReady(const QString& source, const QVector<DiagnosticEntry>& entries);
    void completionsReady(const QList<LspCompletionItem>& items);
    void documentSymbolsReady(const QString& path, const QList<LspDocumentSymbol>& symbols);

private slots:
    void readStdout();
    void readStderr();

private:
    void initialize();
    void handleMessage(const QJsonObject& message);
    void write(const QByteArray& frame);
    [[nodiscard]] QString pathToUri(const QString& path) const;
    [[nodiscard]] QJsonObject textDocumentIdentifier(const QString& path) const;

    std::unique_ptr<QProcess> process_;
    JsonRpcWriter writer_;
    JsonRpcParser parser_;
    SphinxProject project_;
    QString sphinxBuildExe_;
    QString activeProjectId_;
    QHash<QString, int> documentVersions_;
    QSet<int> completionRequestIds_;
    QHash<int, QString> documentSymbolRequestPaths_;
};

}  // namespace mrst


