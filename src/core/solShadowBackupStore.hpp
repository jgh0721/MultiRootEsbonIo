#pragma once

#include "editor/ScintillaDocument.hpp"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

class TextShadowBackupStore
{
    struct WriteState;
    struct WriteRegistry;
public:
    struct Snapshot
    {
        bool isUntitled = false;
        QString untitledId;
        QString displayTitle;
        QString originalFilePath;
        QString text;
        QString encoding;
        QString detectedEncoding;
        ScintillaDocument::LineEnding lineEnding = ScintillaDocument::LF;
        int caretPosition = 0;
        int firstVisibleLine = 0;
        qint64 originalSize = -1;
        qint64 originalLastModifiedUtcMs = 0;
        QDateTime savedAtUtc;

    private:
        friend class TextShadowBackupStore;
        // 현재 프로세스의 예약 순서다. 백업 파일에는 직렬화하지 않는다.
        std::shared_ptr<WriteState> writeState_;
        quint64 writeGeneration_ = 0;
    };

    static QString backupDirectoryPath();
    static QString backupPathForFile(const QString& originalFilePath);
    static QString backupPathForUntitled(const QString& untitledId);
    /// 워커에 넘기기 전에 호출하여 저장 순서를 예약한다.
    static bool reserveSnapshotWrite(Snapshot& snapshot);
    static bool saveSnapshot(const Snapshot& snapshot, QString* errorMessage = nullptr);
    static bool loadSnapshot(const QString& originalFilePath, Snapshot* snapshot, QString* errorMessage = nullptr);
    static bool loadUntitledSnapshot(const QString& untitledId, Snapshot* snapshot, QString* errorMessage = nullptr);
    static QList<Snapshot> restorableSnapshots(bool includeText = false);
    static QStringList restorableFilePaths();
    static bool hasSnapshot(const QString& originalFilePath);
    static bool deleteUntitledSnapshot(const QString& untitledId, QString* errorMessage = nullptr);
    static bool deleteSnapshot(const QString& originalFilePath, QString* errorMessage = nullptr);
    static bool deleteAllBackups(QString* errorMessage = nullptr);
    static bool originalFileMatchesSnapshot(const Snapshot& snapshot);

private:
    static WriteRegistry& writeRegistry();
    static std::shared_ptr<WriteState> writeStateForPath(const QString& path);
    static bool deleteSnapshotFile(const QString& path, QString* errorMessage);
    static bool loadSnapshotFile(const QString& backupFilePath,
                                 Snapshot* snapshot,
                                 QString* errorMessage = nullptr,
                                 bool includeText = true);
    static QString backupPathForKey(const QString& key);
    static QString normalizedUntitledId(const QString& untitledId);
    static QString normalizedFilePath(const QString& filePath);
};




