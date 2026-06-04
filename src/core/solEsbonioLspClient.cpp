#include "stdafx.h"
#include "solEsbonioLspClient.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QUrl>

namespace mrst {
namespace {

QString pathToQString(const std::filesystem::path& path) {
    return QString::fromStdWString(std::filesystem::weakly_canonical(path).wstring());
}

QString projectBuildChild(const SphinxProject& project, const QString& child) {
    return QDir(pathToQString(project.buildPath)).filePath(child);
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

QByteArray JsonRpcWriter::response( int id, const QJsonValue& result )
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
    completionRequestIds_.clear();
    documentSymbolRequestPaths_.clear();

    process_ = std::make_unique<QProcess>();
    process_->setProgram(pythonExe);
    process_->setArguments({QStringLiteral("-m"), QStringLiteral("esbonio.server")});
    process_->setWorkingDirectory(pathToQString(project.rootPath));
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
    completionRequestIds_.clear();
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
    completionRequestIds_.insert(id);
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
    documentSymbolRequestPaths_.insert(id, QFileInfo(path).absoluteFilePath());
    return id;
}

void LspClient::readStdout() {
    if (process_ == nullptr) {
        return;
    }
    parser_.append(process_->readAllStandardOutput());
    for (const QJsonObject& message : parser_.takeMessages()) {
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
    const QString rootPath = pathToQString(project_.rootPath);
    const QString rootUri = pathToUri(rootPath);
    const QString confDir = pathToQString(project_.confPath.parent_path());
    const QString sourceDir = pathToQString(project_.sourcePath);
    const QString buildDir = projectBuildChild(project_, QStringLiteral("lsp/dummy"));
    const QString doctreeDir = projectBuildChild(project_, QStringLiteral("lsp/doctrees"));

    QJsonArray buildCommand{sphinxBuildExe_, QStringLiteral("-b"), QStringLiteral("dummy"), QStringLiteral("-c"), confDir, QStringLiteral("-d"), doctreeDir, sourceDir, buildDir};
    write(writer_.request(QStringLiteral("initialize"), {
        {QStringLiteral("processId"), QJsonValue::Null},
        {QStringLiteral("rootUri"), rootUri},
        {QStringLiteral("workspaceFolders"), QJsonArray{QJsonObject{{QStringLiteral("uri"), rootUri}, {QStringLiteral("name"), activeProjectId_}}}},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("textDocument"), QJsonObject{
                {QStringLiteral("synchronization"), QJsonObject{{QStringLiteral("didSave"), true}, {QStringLiteral("dynamicRegistration"), false}}},
                {QStringLiteral("completion"), QJsonObject{{QStringLiteral("contextSupport"), true}}},
                {QStringLiteral("publishDiagnostics"), QJsonObject{{QStringLiteral("relatedInformation"), true}}},
            }},
            {QStringLiteral("workspace"), QJsonObject{{QStringLiteral("workspaceFolders"), true}, {QStringLiteral("configuration"), true}}},
        }},
        {QStringLiteral("initializationOptions"), QJsonObject{
            {QStringLiteral("esbonio"), QJsonObject{{QStringLiteral("sphinx"), QJsonObject{{QStringLiteral("buildCommand"), buildCommand}, {QStringLiteral("cwd"), rootPath}}}}},
            {QStringLiteral("sphinx"), QJsonObject{{QStringLiteral("confDir"), confDir}, {QStringLiteral("srcDir"), sourceDir}, {QStringLiteral("buildDir"), buildDir}}},
        }},
    }));
    write(writer_.notify(QStringLiteral("initialized"), {}));
}

void LspClient::handleMessage(const QJsonObject& message) {
    if (message.contains(QStringLiteral("id")) && documentSymbolRequestPaths_.contains(message.value(QStringLiteral("id")).toInt())) {
        const int id = message.value(QStringLiteral("id")).toInt();
        const QString path = documentSymbolRequestPaths_.take(id);
        emit documentSymbolsReady(path, parseDocumentSymbols(message.value(QStringLiteral("result"))));
        return;
    }

    if (message.contains(QStringLiteral("id")) && completionRequestIds_.remove(message.value(QStringLiteral("id")).toInt())) {
        QJsonValue result = message.value(QStringLiteral("result"));
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
        return;
    }

    const QString method = message.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("textDocument/publishDiagnostics")) {
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        emit diagnosticsReady(QStringLiteral("esbonio"), parseLspDiagnostics(
            params.value(QStringLiteral("uri")).toString(),
            params.value(QStringLiteral("diagnostics")).toArray()));
        return;
    }

    if (method == QStringLiteral("workspace/configuration") && message.contains(QStringLiteral("id"))) {
        const int id = message.value(QStringLiteral("id")).toInt();
        const QJsonArray items = message.value(QStringLiteral("params")).toObject().value(QStringLiteral("items")).toArray();
        QJsonArray result;
        for (qsizetype i = 0; i < items.size(); ++i) {
            result.push_back(QJsonObject{});
        }
        write(writer_.response(id, result));
    }
}

void LspClient::write(const QByteArray& frame) {
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        process_->write(frame);
        process_->waitForBytesWritten(100);
    }
}

QString LspClient::pathToUri(const QString& path) const {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QJsonObject LspClient::textDocumentIdentifier(const QString& path) const {
    return {{QStringLiteral("uri"), pathToUri(path)}};
}

}  // namespace mrst



