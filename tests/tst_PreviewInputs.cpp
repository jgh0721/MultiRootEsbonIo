#include "TestRunner.hpp"
#include "core/solPreviewInputs.hpp"
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace {
void put(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}
QString fingerprint(const QTemporaryDir& directory)
{
    QJsonArray files;
    const auto add = [&](const QString& path) {
        const QFileInfo info(path);
        files.append(QJsonObject{{"p", path}, {"s", info.size()},
            {"m", info.lastModified().toMSecsSinceEpoch() * 1000000}});
    };
    add(directory.filePath("conf.py"));
    QJsonArray roots;
    for (const auto& name : {"_templates", "theme", "missing"}) {
        const QString path = directory.filePath(name);
        QJsonArray names;
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto file = it.next();
            names.append(QDir(path).relativeFilePath(file));
            add(file);
        }
        roots.append(QJsonObject{{"p", path}, {"files", names}});
    }
    const auto path = directory.filePath("inputs.json");
    put(path, QJsonDocument(QJsonObject{{"schema", 2}, {"templatesComplete", true},
        {"templateDirs", roots}, {"files", files}}).toJson());
    return path;
}
}

class TestPreviewInputs : public QObject
{
    Q_OBJECT
private slots:
    void templateChangesInvalidate_data();
    void templateChangesInvalidate();
    void legacyAndIncompleteFingerprintsAreRejected();
};

void TestPreviewInputs::templateChangesInvalidate_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("edit") << 0;
    QTest::newRow("nested-add") << 1;
    QTest::newRow("delete") << 2;
    QTest::newRow("rename") << 3;
    QTest::newRow("inherited-theme") << 4;
    QTest::newRow("missing-override-created") << 5;
}

void TestPreviewInputs::templateChangesInvalidate()
{
    QFETCH(int, change);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    put(directory.filePath("conf.py"), "templates_path=['_templates']");
    put(directory.filePath("_templates/layout.html"), "original");
    put(directory.filePath("_templates/nested/existing.html"), "existing");
    put(directory.filePath("theme/layout.html"), "theme");
    const auto inputs = fingerprint(directory);
    QVERIFY(!mrst::previewInputsChanged(inputs));
    if (change == 0) put(directory.filePath("_templates/layout.html"), "changed template contents");
    if (change == 1) put(directory.filePath("_templates/nested/new.html"), "new override");
    if (change == 2) QVERIFY(QFile::remove(directory.filePath("_templates/layout.html")));
    if (change == 3) QVERIFY(QFile::rename(directory.filePath("_templates/layout.html"), directory.filePath("_templates/renamed.html")));
    if (change == 4) put(directory.filePath("theme/layout.html"), "changed inherited theme");
    if (change == 5) put(directory.filePath("missing/layout.html"), "newly created root");
    QVERIFY(mrst::previewInputsChanged(inputs));
}

void TestPreviewInputs::legacyAndIncompleteFingerprintsAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    put(directory.filePath("conf.py"), "");
    const auto inputs = fingerprint(directory);
    QFile file(inputs);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto json = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    json["schema"] = 1;
    put(inputs, QJsonDocument(json).toJson());
    QVERIFY(mrst::previewInputsChanged(inputs));
    json["schema"] = 2;
    json["templatesComplete"] = false;
    put(inputs, QJsonDocument(json).toJson());
    QVERIFY(mrst::previewInputsChanged(inputs));
}

MRST_REGISTER_TEST(TestPreviewInputs);
#include "tst_PreviewInputs.moc"
