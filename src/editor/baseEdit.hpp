#pragma once

#include "ScintillaEdit.hpp"

class BaseEdit : public QWidget
{
    Q_OBJECT
public:
    explicit BaseEdit( QWidget* Parent = nullptr );
    ~BaseEdit() override;

    QScintillaEdit*                     Scintilla() const;
    QWidget*                            EditorWidget() const;

    QString                             FilePath() const;
    QString                             NormalizedFilePath() const;
    QString                             DisplayName() const;

    bool                                LoadFile( const QString& FilePath, QString* ErrorMessage = nullptr );
    bool                                SaveFile( QString* ErrorMessage = nullptr );
    bool                                SaveFileAs( const QString& FilePath, QString* ErrorMessage = nullptr );

    virtual QString                     EditorType() const;

    bool                                IsAutoCompletionAvailable() const;
    bool                                IsPreviewAvailable() const;
    bool                                IsOutlineAvailable() const;
    bool                                IsDiagnosticsAvailable() const;

    void                                SetReadOnly( bool ReadOnly );
    bool                                IsReadOnly() const;

signals:
    void                                filePathChanged( const QString& filePath );
    void                                modificationChanged( bool modified );
    void                                cursorPositionChanged( int line, int index );
    void                                linesChanged();
    void                                textChanged();
    void                                selectionChanged();

protected:
    void                                SetAutoCompletionAvailable( bool Available );
    void                                SetPreviewAvailable( bool Available );
    void                                SetOutlineAvailable( bool Available );
    void                                SetDiagnosticsAvailable( bool Available );

private:
    static QString                      normalizeFilePath( const QString& FilePath );
    void                                setFilePath( const QString& FilePath );

    QScintillaEdit*                     m_scintilla = nullptr;
    QString                             m_filePath;
    QString                             m_normalizedFilePath;
    bool                                m_autoCompletionAvailable = false;
    bool                                m_previewAvailable = false;
    bool                                m_outlineAvailable = false;
    bool                                m_diagnosticsAvailable = false;
};

