#include "stdafx.h"
#include "EditorTabWidget.hpp"

#include "markdownEdit.hpp"
#include "pythonEdit.hpp"
#include "reSTEdit.hpp"

EditorTabWidget::EditorTabWidget( QWidget* Parent )
    : QTabWidget( Parent )
{
    setTabsClosable( true );
    setMovable( true );
    setDocumentMode( true );

    connect( this, &QTabWidget::currentChanged, this, &EditorTabWidget::onCurrentChanged );
    connect( this, &QTabWidget::tabCloseRequested, this, &EditorTabWidget::onTabCloseRequested );
}

EditorTabWidget::~EditorTabWidget() = default;

void EditorTabWidget::InitializeEmpty()
{
    while( count() > 0 )
    {
        QWidget* tab = widget( 0 );
        removeTab( 0 );
        delete tab;
    }
}

BaseEdit* EditorTabWidget::OpenFile( const QString& FilePath )
{
    const QFileInfo info( FilePath );
    if( !info.exists() || !info.isFile() )
    {
        emit fileOpenFailed( FilePath, tr( "The path does not exist or is not a regular file." ) );
        return nullptr;
    }

    const QString normalized = normalizeFilePath( FilePath );
    const int existingIndex = findTabByNormalizedPath( normalized );
    if( existingIndex >= 0 )
    {
        const bool alreadyCurrent = currentIndex() == existingIndex;
        setCurrentIndex( existingIndex );
        BaseEdit* existingEditor = editorAt( existingIndex );
        emit fileTabReused( existingEditor ? existingEditor->FilePath() : FilePath, existingEditor );
        if( alreadyCurrent )
        {
            emit activeEditorChanged( existingEditor );
            emit editorTabActivated( existingIndex, existingEditor );
        }
        return existingEditor;
    }

    BaseEdit* editor = createEditorForFile( FilePath );
    QString errorMessage;
    if( !editor->LoadFile( FilePath, &errorMessage ) )
    {
        emit fileOpenFailed( FilePath, errorMessage );
        delete editor;
        return nullptr;
    }

    const int newIndex = addTab( editor, editor->DisplayName() );
    setTabToolTip( newIndex, QDir::toNativeSeparators( editor->FilePath() ) );
    const bool alreadyCurrent = currentIndex() == newIndex;
    setCurrentIndex( newIndex );

    connect( editor, &BaseEdit::filePathChanged, this, [this, editor]( const QString& ) {
        const int index = indexOf( editor );
        if( index < 0 )
            return;

        setTabText( index, editor->DisplayName() );
        setTabToolTip( index, QDir::toNativeSeparators( editor->FilePath() ) );
    } );

    emit fileOpened( editor->FilePath(), editor );
    if( alreadyCurrent )
    {
        emit activeEditorChanged( editor );
        emit editorTabActivated( newIndex, editor );
    }
    return editor;
}

BaseEdit* EditorTabWidget::CurrentEditor() const
{
    return editorAt( currentIndex() );
}

void EditorTabWidget::SetRulerVisibleForAllEditors( bool Visible )
{
    for( int i = 0; i < count(); ++i )
    {
        BaseEdit* editor = editorAt( i );
        if( editor )
            editor->SetRulerVisible( Visible );
    }
}

void EditorTabWidget::onCurrentChanged( int Index )
{
    BaseEdit* editor = editorAt( Index );
    emit activeEditorChanged( editor );
    emit editorTabActivated( Index, editor );
}

void EditorTabWidget::onTabCloseRequested( int Index )
{
    QWidget* tab = widget( Index );
    removeTab( Index );
    delete tab;
}

QString EditorTabWidget::normalizeFilePath( const QString& FilePath )
{
    const QFileInfo info( FilePath );
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
}

BaseEdit* EditorTabWidget::createEditorForFile( const QString& FilePath )
{
    const QString suffix = QFileInfo( FilePath ).suffix().toLower();
    if( suffix == QLatin1String( "rst" ) )
        return new reSTEdit( this );
    if( suffix == QLatin1String( "md" ) )
        return new MarkdownEdit( this );
    if( suffix == QLatin1String( "py" ) )
        return new PythonEdit( this );

    return new BaseEdit( this );
}

BaseEdit* EditorTabWidget::editorAt( int Index ) const
{
    return qobject_cast< BaseEdit* >( widget( Index ) );
}

int EditorTabWidget::findTabByNormalizedPath( const QString& NormalizedFilePath ) const
{
    for( int i = 0; i < count(); ++i )
    {
        const BaseEdit* editor = editorAt( i );
        if( editor && editor->NormalizedFilePath() == NormalizedFilePath )
            return i;
    }

    return -1;
}



