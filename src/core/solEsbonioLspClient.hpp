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

/// JSON-RPC 메시지 종류.
///
/// 이걸 잘못 나누면 서버가 보낸 **요청** 을 우리 요청의 **응답** 으로 오인해
/// 답을 하지 않게 되고, 서버는 그 자리에서 멈춘다. Esbonio 는 초기화 중
/// workspace/configuration 을 요청하므로 곧바로 치명적이다.
enum class LspMessageKind
{
    Invalid,
    Request,        ///< id 와 method 가 둘 다 있다 -> 반드시 응답해야 한다
    Response,       ///< id 만 있다 -> 우리가 보낸 요청의 결과
    Notification,   ///< method 만 있다 -> 응답 불필요
};

/// 서버가 보낸 id 와 우리가 발급한 id 는 서로 다른 번호 공간이다.
/// 따라서 id 를 우리 요청 목록에서 찾아보는 것으로 종류를 판단하면 안 된다.
[[nodiscard]] LspMessageKind classifyLspMessage(const QJsonObject& message);

class JsonRpcWriter final
{
public:
    [[nodiscard]] int nextId() const;
    [[nodiscard]] QByteArray request( const QString& method, const QJsonObject& params, int* idOut = nullptr );
    [[nodiscard]] QByteArray notify( const QString& method, const QJsonObject& params );
    /// 응답의 id 는 요청에서 받은 값을 **그대로** 돌려줘야 한다.
    /// LSP/JSON-RPC 의 id 는 숫자일 수도 문자열일 수도 있는데, Esbonio 는
    /// 문자열 UUID 를 쓴다. 숫자로 변환해 보내면 서버가 자기 요청과 짝지을 수
    /// 없어 영원히 기다린다 (증상은 "아무 진단도 안 뜸").
    [[nodiscard]] QByteArray response( const QJsonValue& id, const QJsonValue& result );

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

    /// conf.py 에 html_style = '' 이 있으면 Sphinx 8 에서 _static checksum
    /// 오류가 난다. configOverrides 로 None 을 강제하기 위해 미리 확인한다.
    void setHtmlStyleOverride(bool overrideToNull);

    /// conf.py / 확장 / 테마를 해석할 인터프리터. 비우면 esbonio 가 자기 것을 쓴다.
    ///
    /// esbonio 서버 자체는 항상 번들에서 돌리고, sphinx_agent 만 이 인터프리터로
    /// 띄운다. 그래서 사용자 venv 에 esbonio 를 설치할 필요가 없다.
    void setSphinxPythonCommand(const QString& pythonExe);

    void start(const SphinxProject& project, const QString& pythonExe, const QString& sphinxBuildExe);
    void stop();
    void didOpen(const QString& path, const QString& text, const QString& languageId = QStringLiteral("rst"));
    void didChange(const QString& path, const QString& text);
    void didSave(const QString& path, const QString& text);
    void didClose(const QString& path);
    /// triggerCharacter 를 주면 triggerKind=2 로 보낸다. Esbonio 가 등록한
    /// 문자(: / < > 공백 백틱)일 때만 의미가 있다.
    int completion(const QString& path, int line, int column, const QString& triggerCharacter = {});
    int documentSymbols(const QString& path);

signals:
    void logMessage(const QString& text);
    void jsonMessage(const QJsonObject& message);
    /// sphinx/clientCreated, sphinx/appCreated, sphinx/clientErrored, $/progress 등.
    /// 상태 표시와 "빌드 끝난 뒤 자동완성 재시도" 에 쓴다.
    void serverNotification(const QString& method, const QJsonObject& params);
    void diagnosticsReady(const QString& source, const QVector<DiagnosticEntry>& entries);
    void completionsReady(int requestId, const QList<LspCompletionItem>& items);
    void documentSymbolsReady(const QString& path, const QList<LspDocumentSymbol>& symbols);

private slots:
    void readStdout();
    void readStderr();

private:
    void initialize();
    void handleMessage(const QJsonObject& message);
    void handleServerRequest(const QJsonObject& message);
    void handleResponse(const QJsonObject& message);
    void handleNotification(const QString& method, const QJsonObject& params);
    void write(const QByteArray& frame);
    [[nodiscard]] QString pathToUri(const QString& path) const;
    [[nodiscard]] QJsonObject textDocumentIdentifier(const QString& path) const;
    [[nodiscard]] QJsonObject sphinxConfiguration() const;
    [[nodiscard]] QJsonValue configurationForSection(const QString& section) const;

    std::unique_ptr<QProcess> process_;
    JsonRpcWriter writer_;
    JsonRpcParser parser_;
    SphinxProject project_;
    QString sphinxBuildExe_;
    QString activeProjectId_;
    bool htmlStyleOverride_ = false;
    QString sphinxPythonCommand_;
    QHash<QString, int> documentVersions_;
    /// 우리가 보낸 요청: id -> method. 예전에는 completion/documentSymbol 을
    /// 따로 들고 있었는데, 그러면 "이 id 가 내 것인가" 를 추측하게 된다.
    QHash<int, QString> pendingRequests_;
    QHash<int, QString> documentSymbolRequestPaths_;
};

}  // namespace mrst


