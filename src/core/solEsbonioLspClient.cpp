#include "stdafx.h"
#include "solEsbonioLspClient.hpp"

#include "utils/ProcessReaper.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QUrl>

namespace mrst {
namespace {

/// MRST_LSP_TRACE 가 지정되면 오가는 프레임을 그대로 파일에 남긴다.
/// LSP 문제는 "아무 일도 안 일어난다" 로만 관측되기 때문에, 실제 트래픽을
/// 볼 수단이 없으면 원인 추적이 사실상 불가능하다.
void traceLsp(const char* direction, const QByteArray& payload) {
    static const QString tracePath =
        QString::fromLocal8Bit(qgetenv("MRST_LSP_TRACE")).trimmed();
    if (tracePath.isEmpty()) {
        return;
    }

    QFile file(tracePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(direction);
    file.write(" ");
    file.write(payload.left(4000));
    file.write("\n");
}

// 경로 변환은 solSphinxScanner.hpp 의 mrst::toCanonicalQString() 을 쓴다.
QString projectBuildChild(const SphinxProject& project, const QString& child) {
    return QDir(toCanonicalQString(project.buildPath)).filePath(child);
}

int oneBasedRangeValue(const QJsonObject& range, const QString& side, const QString& field, int fallback = 0) {
    return range.value(side).toObject().value(field).toInt(fallback) + 1;
}

LspDocumentSymbol parseDocumentSymbolObject(const QJsonObject& object) {
    const QJsonObject range = object.value(QStringLiteral("selectionRange")).toObject(object.value(QStringLiteral("range")).toObject());
    const QJsonObject fullRange = object.value(QStringLiteral("range")).toObject(range);
    LspDocumentSymbol symbol;
    symbol.name = object.value(QStringLiteral("name")).toString();
    symbol.detail = object.value(QStringLiteral("detail")).toString();
    symbol.kind = object.value(QStringLiteral("kind")).toInt();
    symbol.line = oneBasedRangeValue(range, QStringLiteral("start"), QStringLiteral("line"));
    symbol.column = oneBasedRangeValue(range, QStringLiteral("start"), QStringLiteral("character"));
    symbol.endLine = oneBasedRangeValue(fullRange, QStringLiteral("end"), QStringLiteral("line"), symbol.line - 1);
    symbol.endColumn = oneBasedRangeValue(fullRange, QStringLiteral("end"), QStringLiteral("character"), symbol.column - 1);
    const QJsonArray children = object.value(QStringLiteral("children")).toArray();
    for (const QJsonValue& child : children) {
        if (child.isObject()) {
            symbol.children.push_back(parseDocumentSymbolObject(child.toObject()));
        }
    }
    return symbol;
}

LspDocumentSymbol parseSymbolInformationObject(const QJsonObject& object) {
    const QJsonObject location = object.value(QStringLiteral("location")).toObject();
    const QJsonObject range = location.value(QStringLiteral("range")).toObject();
    LspDocumentSymbol symbol;
    symbol.name = object.value(QStringLiteral("name")).toString();
    symbol.kind = object.value(QStringLiteral("kind")).toInt();
    symbol.line = oneBasedRangeValue(range, QStringLiteral("start"), QStringLiteral("line"));
    symbol.column = oneBasedRangeValue(range, QStringLiteral("start"), QStringLiteral("character"));
    symbol.endLine = oneBasedRangeValue(range, QStringLiteral("end"), QStringLiteral("line"), symbol.line - 1);
    symbol.endColumn = oneBasedRangeValue(range, QStringLiteral("end"), QStringLiteral("character"), symbol.column - 1);
    return symbol;
}

}  // namespace

LspMessageKind classifyLspMessage(const QJsonObject& message) {
    const bool hasId = message.contains(QStringLiteral("id"))
                       && !message.value(QStringLiteral("id")).isNull();
    const bool hasMethod = message.contains(QStringLiteral("method"))
                           && !message.value(QStringLiteral("method")).toString().isEmpty();

    if (hasId && hasMethod) {
        return LspMessageKind::Request;
    }
    if (hasId) {
        return LspMessageKind::Response;
    }
    if (hasMethod) {
        return LspMessageKind::Notification;
    }
    return LspMessageKind::Invalid;
}

QList<LspDocumentSymbol> parseDocumentSymbols(const QJsonValue& result) {
    QList<LspDocumentSymbol> symbols;
    const QJsonArray rawSymbols = result.toArray();
    symbols.reserve(rawSymbols.size());
    for (const QJsonValue& value : rawSymbols) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        LspDocumentSymbol symbol = object.contains(QStringLiteral("location")) ? parseSymbolInformationObject(object) : parseDocumentSymbolObject(object);
        if (!symbol.name.isEmpty()) {
            symbols.push_back(symbol);
        }
    }
    return symbols;
}


int JsonRpcWriter::nextId() const
{
    return nextId_;
}

QByteArray JsonRpcWriter::request( const QString& method, const QJsonObject& params, int* idOut )
{
    const int id = nextId_++;
    if( idOut != nullptr )
    {
        *idOut = id;
    }
    return frame( {
        {QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" )},
        {QStringLiteral( "id" ), id},
        {QStringLiteral( "method" ), method},
        {QStringLiteral( "params" ), params},
    } );
}

QByteArray JsonRpcWriter::notify( const QString& method, const QJsonObject& params )
{
    return frame( {
        {QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" )},
        {QStringLiteral( "method" ), method},
        {QStringLiteral( "params" ), params},
    } );
}

QByteArray JsonRpcWriter::response( const QJsonValue& id, const QJsonValue& result )
{
    return frame( {
        {QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" )},
        {QStringLiteral( "id" ), id},
        {QStringLiteral( "result" ), result},
    } );
}

QByteArray JsonRpcWriter::frame( const QJsonObject& payload )
{
    const QByteArray body = QJsonDocument( payload ).toJson( QJsonDocument::Compact );
    return QByteArray( "Content-Length: " ) + QByteArray::number( body.size() ) + QByteArray( "\r\n\r\n" ) + body;
}

void JsonRpcParser::append( const QByteArray& bytes )
{
    buffer_.append( bytes );
}

QList<QJsonObject> JsonRpcParser::takeMessages()
{
    QList<QJsonObject> messages;
    while( true )
    {
        const qsizetype headerEnd = buffer_.indexOf( "\r\n\r\n" );
        if( headerEnd < 0 )
        {
            break;
        }
        const QByteArray header = buffer_.left( headerEnd );
        qsizetype contentLength = -1;
        for( const QByteArray& line : header.split( '\n' ) )
        {
            const QByteArray trimmed = line.trimmed();
            const qsizetype colon = trimmed.indexOf( ':' );
            if( colon > 0 && trimmed.left( colon ).toLower() == "content-length" )
            {
                bool ok = false;
                contentLength = trimmed.mid( colon + 1 ).trimmed().toLongLong( &ok );
                if( !ok )
                {
                    contentLength = -1;
                }
                break;
            }
        }
        if( contentLength < 0 )
        {
            buffer_.remove( 0, headerEnd + 4 );
            continue;
        }
        if( buffer_.size() < headerEnd + 4 + contentLength )
        {
            break;
        }
        const QByteArray body = buffer_.mid( headerEnd + 4, contentLength );
        buffer_.remove( 0, headerEnd + 4 + contentLength );
        const QJsonDocument doc = QJsonDocument::fromJson( body );
        if( doc.isObject() )
        {
            messages.push_back( doc.object() );
        }
    }
    return messages;
}

LspClient::LspClient(QObject* parent) : QObject(parent) {}

LspClient::~LspClient() {
    stop();
}

bool LspClient::isRunning() const {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

QString LspClient::activeProjectId() const {
    return activeProjectId_;
}

void LspClient::start(const SphinxProject& project, const QString& pythonExe, const QString& sphinxBuildExe) {
    if (isRunning() && activeProjectId_ == QString::fromStdWString(project.projectId)) {
        return;
    }
    stop();
    project_ = project;
    sphinxBuildExe_ = sphinxBuildExe;
    activeProjectId_ = QString::fromStdWString(project.projectId);
    documentVersions_.clear();
    pendingRequests_.clear();
    documentSymbolRequestPaths_.clear();

    process_ = std::make_unique<QProcess>();
    process_->setProgram(pythonExe);
    process_->setArguments({QStringLiteral("-m"), QStringLiteral("esbonio.server")});
    process_->setWorkingDirectory(toCanonicalQString(project.rootPath));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process_->setProcessEnvironment(env);
    connect(process_.get(), &QProcess::readyReadStandardOutput, this, &LspClient::readStdout);
    connect(process_.get(), &QProcess::readyReadStandardError, this, &LspClient::readStderr);
    connect(process_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit logMessage(QStringLiteral("Esbonio process error: %1").arg(error));
    });
    process_->start();
    if (!process_->waitForStarted(5000)) {
        emit logMessage(QStringLiteral("Esbonio 시작 실패: %1").arg(pythonExe));
        return;
    }

    // Esbonio 는 sphinx_agent 를 손자 프로세스로 띄운다. 우리가 자식만
    // 종료해서는 손자가 남으므로 Job Object 로 묶어 앱과 함께 정리되게 한다.
    assignToKillOnExitJob(process_->processId());

    emit logMessage(QStringLiteral("Esbonio 시작: %1").arg(activeProjectId_));
    initialize();
}

void LspClient::stop() {
    if (process_ == nullptr) {
        return;
    }
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(1500)) {
            process_->kill();
            process_->waitForFinished(1500);
        }
    }
    process_.reset();
    activeProjectId_.clear();
    documentVersions_.clear();
    pendingRequests_.clear();
    documentSymbolRequestPaths_.clear();
}

void LspClient::didOpen(const QString& path, const QString& text, const QString& languageId) {
    if (!isRunning()) {
        return;
    }
    const QString uri = pathToUri(path);
    documentVersions_[uri] = 1;
    write(writer_.notify(QStringLiteral("textDocument/didOpen"), {
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), uri},
            {QStringLiteral("languageId"), languageId},
            {QStringLiteral("version"), 1},
            {QStringLiteral("text"), text},
        }},
    }));
}

void LspClient::didChange(const QString& path, const QString& text) {
    if (!isRunning()) {
        return;
    }
    const QString uri = pathToUri(path);
    const int version = documentVersions_.value(uri, 1) + 1;
    documentVersions_[uri] = version;
    write(writer_.notify(QStringLiteral("textDocument/didChange"), {
        {QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), uri}, {QStringLiteral("version"), version}}},
        {QStringLiteral("contentChanges"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}},
    }));
}

void LspClient::didSave(const QString& path, const QString& text) {
    if (!isRunning()) {
        return;
    }
    write(writer_.notify(QStringLiteral("textDocument/didSave"), {
        {QStringLiteral("textDocument"), textDocumentIdentifier(path)},
        {QStringLiteral("text"), text},
    }));
}

void LspClient::didClose(const QString& path) {
    if (!isRunning()) {
        return;
    }
    documentVersions_.remove(pathToUri(path));
    write(writer_.notify(QStringLiteral("textDocument/didClose"), {
        {QStringLiteral("textDocument"), textDocumentIdentifier(path)},
    }));
}

int LspClient::completion(const QString& path, int line, int column) {
    if (!isRunning()) {
        return 0;
    }
    int id = 0;
    write(writer_.request(QStringLiteral("textDocument/completion"), {
        {QStringLiteral("textDocument"), textDocumentIdentifier(path)},
        {QStringLiteral("position"), QJsonObject{{QStringLiteral("line"), qMax(0, line - 1)}, {QStringLiteral("character"), qMax(0, column - 1)}}},
    }, &id));
    pendingRequests_.insert(id, QStringLiteral("textDocument/completion"));
    return id;
}

int LspClient::documentSymbols(const QString& path) {
    if (!isRunning() || path.trimmed().isEmpty()) {
        return 0;
    }
    int id = 0;
    write(writer_.request(QStringLiteral("textDocument/documentSymbol"), {
        {QStringLiteral("textDocument"), textDocumentIdentifier(path)},
    }, &id));
    pendingRequests_.insert(id, QStringLiteral("textDocument/documentSymbol"));
    documentSymbolRequestPaths_.insert(id, QFileInfo(path).absoluteFilePath());
    return id;
}

void LspClient::readStdout() {
    if (process_ == nullptr) {
        return;
    }
    parser_.append(process_->readAllStandardOutput());
    for (const QJsonObject& message : parser_.takeMessages()) {
        traceLsp("<<", QJsonDocument(message).toJson(QJsonDocument::Compact));
        handleMessage(message);
        emit jsonMessage(message);
    }
}

void LspClient::readStderr() {
    if (process_ != nullptr) {
        const QString text = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!text.isEmpty()) {
            emit logMessage(text);
        }
    }
}

void LspClient::initialize() {
    const QString rootPath = toCanonicalQString(project_.rootPath);
    const QString rootUri = pathToUri(rootPath);
    const QString confDir = toCanonicalQString(project_.confPath.parent_path());
    const QString sourceDir = toCanonicalQString(project_.sourcePath);
    const QString buildDir = projectBuildChild(project_, QStringLiteral("lsp/dummy"));
    const QString doctreeDir = projectBuildChild(project_, QStringLiteral("lsp/doctrees"));

    int initializeId = 0;
    write(writer_.request(QStringLiteral("initialize"), {
        {QStringLiteral("processId"), QJsonValue::Null},
        {QStringLiteral("rootUri"), rootUri},
        {QStringLiteral("workspaceFolders"), QJsonArray{QJsonObject{{QStringLiteral("uri"), rootUri}, {QStringLiteral("name"), activeProjectId_}}}},
        // Esbonio 는 선언된 capability 에 따라 동작을 바꾼다. 너무 얇게 선언하면
        // 아예 다른 경로를 타므로 파이썬 원본 수준으로 맞춘다.
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("textDocument"), QJsonObject{
                {QStringLiteral("synchronization"), QJsonObject{{QStringLiteral("didSave"), true}, {QStringLiteral("dynamicRegistration"), false}}},
                {QStringLiteral("completion"), QJsonObject{
                    {QStringLiteral("contextSupport"), true},
                    {QStringLiteral("completionItem"), QJsonObject{
                        {QStringLiteral("snippetSupport"), false},
                        {QStringLiteral("commitCharactersSupport"), true},
                        {QStringLiteral("documentationFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}},
                        {QStringLiteral("deprecatedSupport"), true},
                        {QStringLiteral("preselectSupport"), true},
                        {QStringLiteral("insertReplaceSupport"), true},
                        {QStringLiteral("labelDetailsSupport"), true},
                        {QStringLiteral("resolveSupport"), QJsonObject{
                            {QStringLiteral("properties"), QJsonArray{QStringLiteral("documentation"),
                                                                      QStringLiteral("detail"),
                                                                      QStringLiteral("additionalTextEdits")}}}},
                    }},
                    {QStringLiteral("completionList"), QJsonObject{
                        {QStringLiteral("itemDefaults"), QJsonArray{QStringLiteral("commitCharacters"),
                                                                    QStringLiteral("editRange"),
                                                                    QStringLiteral("insertTextFormat"),
                                                                    QStringLiteral("insertTextMode"),
                                                                    QStringLiteral("data")}}}},
                }},
                {QStringLiteral("documentSymbol"), QJsonObject{
                    {QStringLiteral("hierarchicalDocumentSymbolSupport"), true},
                }},
                {QStringLiteral("publishDiagnostics"), QJsonObject{{QStringLiteral("relatedInformation"), true}}},
            }},
            {QStringLiteral("window"), QJsonObject{{QStringLiteral("workDoneProgress"), true}}},
            {QStringLiteral("workspace"), QJsonObject{{QStringLiteral("workspaceFolders"), true}, {QStringLiteral("configuration"), true}}},
        }},
        {QStringLiteral("initializationOptions"), QJsonObject{
            {QStringLiteral("esbonio"), QJsonObject{{QStringLiteral("sphinx"), sphinxConfiguration()}}},
            // Esbonio 1.x 형식 폴백.
            {QStringLiteral("sphinx"), QJsonObject{{QStringLiteral("confDir"), confDir}, {QStringLiteral("srcDir"), sourceDir}, {QStringLiteral("buildDir"), buildDir}}},
        }},
    }, &initializeId));
    pendingRequests_.insert(initializeId, QStringLiteral("initialize"));
    write(writer_.notify(QStringLiteral("initialized"), {}));
}

void LspClient::handleMessage(const QJsonObject& message) {
    switch (classifyLspMessage(message)) {
        case LspMessageKind::Request:
            handleServerRequest(message);
            return;
        case LspMessageKind::Response:
            handleResponse(message);
            return;
        case LspMessageKind::Notification:
            handleNotification(message.value(QStringLiteral("method")).toString(),
                               message.value(QStringLiteral("params")).toObject());
            return;
        case LspMessageKind::Invalid:
            return;
    }
}

void LspClient::handleServerRequest(const QJsonObject& message) {
    // id 를 int 로 바꾸지 않는다. Esbonio 는 문자열 UUID 를 쓴다.
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();

    if (method == QStringLiteral("workspace/configuration")) {
        // Esbonio 2.x 는 설정을 여기로 끌어간다. 빈 객체를 돌려주면 빌드 방법을
        // 영영 알 수 없어 진단이 하나도 나오지 않는다.
        const QJsonArray items = message.value(QStringLiteral("params")).toObject()
                                     .value(QStringLiteral("items")).toArray();
        QJsonArray result;
        for (const QJsonValue& item : items) {
            result.push_back(configurationForSection(
                item.toObject().value(QStringLiteral("section")).toString()));
        }
        write(writer_.response(id, result));
        return;
    }

    // 응답하지 않으면 서버가 그 자리에서 멈춘다. 모르는 요청도 null 로 답한다.
    write(writer_.response(id, QJsonValue()));
}

void LspClient::handleResponse(const QJsonObject& message) {
    const int id = message.value(QStringLiteral("id")).toInt();
    const QString method = pendingRequests_.take(id);
    if (method.isEmpty()) {
        return;
    }

    if (method == QStringLiteral("textDocument/documentSymbol")) {
        const QString path = documentSymbolRequestPaths_.take(id);
        emit documentSymbolsReady(path, parseDocumentSymbols(message.value(QStringLiteral("result"))));
        return;
    }

    if (method == QStringLiteral("textDocument/completion")) {
        const QJsonValue result = message.value(QStringLiteral("result"));
        QJsonArray rawItems;
        if (result.isArray()) {
            rawItems = result.toArray();
        } else if (result.isObject()) {
            rawItems = result.toObject().value(QStringLiteral("items")).toArray();
        }
        QList<LspCompletionItem> items;
        items.reserve(rawItems.size());
        for (const QJsonValue& rawItem : rawItems) {
            if (!rawItem.isObject()) {
                continue;
            }
            const QJsonObject object = rawItem.toObject();
            LspCompletionItem item;
            item.label = object.value(QStringLiteral("label")).toString();
            item.insertText = object.value(QStringLiteral("insertText")).toString(item.label);
            item.detail = object.value(QStringLiteral("detail")).toString();
            item.kind = object.value(QStringLiteral("kind")).toInt();
            item.filterText = object.value(QStringLiteral("filterText")).toString(item.label);
            if (!item.label.isEmpty()) {
                items.push_back(item);
            }
        }
        emit completionsReady(items);
    }
}

void LspClient::handleNotification(const QString& method, const QJsonObject& params) {
    if (method == QStringLiteral("textDocument/publishDiagnostics")) {
        emit diagnosticsReady(QStringLiteral("esbonio"), parseLspDiagnostics(
            params.value(QStringLiteral("uri")).toString(),
            params.value(QStringLiteral("diagnostics")).toArray()));
        return;
    }

    if (method == QStringLiteral("window/logMessage") || method == QStringLiteral("window/showMessage")) {
        const QString text = params.value(QStringLiteral("message")).toString().trimmed();
        if (!text.isEmpty()) {
            emit logMessage(text);
        }
        return;
    }

    emit serverNotification(method, params);
}

QJsonObject LspClient::sphinxConfiguration() const {
    const QString rootPath = toCanonicalQString(project_.rootPath);
    const QString confDir = toCanonicalQString(project_.confPath.parent_path());
    const QString sourceDir = toCanonicalQString(project_.sourcePath);
    const QString buildDir = projectBuildChild(project_, QStringLiteral("lsp/dummy"));
    const QString doctreeDir = projectBuildChild(project_, QStringLiteral("lsp/doctrees"));

    QJsonObject configOverrides;
    if (htmlStyleOverride_) {
        configOverrides.insert(QStringLiteral("html_style"), QJsonValue::Null);
    }

    // pythonCommand 는 일부러 넣지 않는다. 넣으면 esbonio 가 그 인터프리터로
    // 하위 프로세스를 다시 띄우는데, 비ASCII 경로에서 깨진다.
    // (프로젝트별 인터프리터 지정은 Phase 8 에서 다룬다.)
    // 첫 항목은 반드시 리터럴 "sphinx-build" 여야 한다.
    // esbonio 의 sphinx_agent/config.py fromcli() 는
    //     if args[0] == "sphinx-build": args = args[1:]
    // 로 딱 이 문자열일 때만 프로그램 이름을 떼어낸다. 절대 경로를 넣으면
    // 그게 그대로 남아 sphinx-build 의 첫 위치 인자, 즉 **source 디렉터리** 로
    // 해석되어 "Cannot find source directory (...sphinx-build.exe)" 로 죽는다.
    // 어차피 에이전트는 Sphinx 를 in-process 로 만들기 때문에 실행 파일 경로는
    // 필요 없다.
    return {
        {QStringLiteral("buildCommand"), QJsonArray{
            QStringLiteral("sphinx-build"), QStringLiteral("-b"), QStringLiteral("dummy"),
            QStringLiteral("-c"), confDir,
            QStringLiteral("-d"), doctreeDir,
            sourceDir, buildDir}},
        {QStringLiteral("cwd"), rootPath},
        {QStringLiteral("configOverrides"), configOverrides},
        {QStringLiteral("enableDevTools"), false},
    };
}

QJsonValue LspClient::configurationForSection(const QString& section) const {
    if (section.isEmpty() || section == QStringLiteral("esbonio")) {
        return QJsonObject{
            {QStringLiteral("sphinx"), sphinxConfiguration()},
            {QStringLiteral("server"), QJsonObject{{QStringLiteral("logLevel"), QStringLiteral("info")}}},
        };
    }
    if (section == QStringLiteral("esbonio.sphinx")) {
        return sphinxConfiguration();
    }
    if (section == QStringLiteral("esbonio.server")) {
        return QJsonObject{{QStringLiteral("logLevel"), QStringLiteral("info")}};
    }
    return QJsonObject{};
}

void LspClient::setHtmlStyleOverride(const bool overrideToNull) {
    htmlStyleOverride_ = overrideToNull;
}

void LspClient::write(const QByteArray& frame) {
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        traceLsp(">>", frame);
        // waitForBytesWritten 을 걸면 메시지마다 GUI 가 멈춘다.
        // QProcess 가 알아서 버퍼링하므로 필요 없다.
        process_->write(frame);
    }
}

QString LspClient::pathToUri(const QString& path) const {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QJsonObject LspClient::textDocumentIdentifier(const QString& path) const {
    return {{QStringLiteral("uri"), pathToUri(path)}};
}

}  // namespace mrst



