#pragma once

#include "baseEdit.hpp"

class EditorTabWidget : public QTabWidget
{
    Q_OBJECT
public:
    explicit EditorTabWidget( QWidget* Parent = nullptr );
    ~EditorTabWidget() override;

    void                                InitializeEmpty();
    BaseEdit*                           OpenFile( const QString& FilePath );
    BaseEdit*                           CurrentEditor() const;

signals:
    void                                activeEditorChanged( BaseEdit* editor );
    void                                editorTabActivated( int index, BaseEdit* editor );
    void                                fileOpened( const QString& filePath, BaseEdit* editor );
    void                                fileOpenFailed( const QString& filePath, const QString& reason );
    void                                fileTabReused( const QString& filePath, BaseEdit* editor );

private slots:
    void                                onCurrentChanged( int Index );
    void                                onTabCloseRequested( int Index );

private:
    static QString                      normalizeFilePath( const QString& FilePath );

    BaseEdit*                           createEditorForFile( const QString& FilePath );
    BaseEdit*                           editorAt( int Index ) const;
    int                                 findTabByNormalizedPath( const QString& NormalizedFilePath ) const;
};

