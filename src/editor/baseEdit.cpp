#include "stdafx.h"
#include "baseEdit.hpp"

BaseEdit::BaseEdit( QWidget* Parent )
    : QWidget( Parent )
    , m_scintilla( new QScintillaEdit( this, this ) )
{
    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 0 );
    layout->addWidget( m_scintilla->Editor() );

    connect( m_scintilla, &QScintillaEdit::modificationChanged, this, &BaseEdit::modificationChanged );
    connect( m_scintilla, &QScintillaEdit::cursorPositionChanged, this, &BaseEdit::cursorPositionChanged );
    connect( m_scintilla, &QScintillaEdit::linesChanged, this, &BaseEdit::linesChanged );
    connect( m_scintilla, &QScintillaEdit::textChanged, this, &BaseEdit::textChanged );
    connect( m_scintilla, &QScintillaEdit::selectionChanged, this, &BaseEdit::selectionChanged );
}

BaseEdit::~BaseEdit() = default;

QScintillaEdit* BaseEdit::Scintilla() const
{
    return m_scintilla;
}

QWidget* BaseEdit::EditorWidget() const
{
    return m_scintilla ? m_scintilla->Editor() : nullptr;
}

QString BaseEdit::FilePath() const
{
    return m_filePath;
}

QString BaseEdit::NormalizedFilePath() const
{
    return m_normalizedFilePath;
}

QString BaseEdit::DisplayName() const
{
    if( m_filePath.isEmpty() )
        return tr( "Untitled" );

    const QString fileName = QFileInfo( m_filePath ).fileName();
    return fileName.isEmpty() ? m_filePath : fileName;
}

bool BaseEdit::LoadFile( const QString& FilePath, QString* ErrorMessage )
{
    QFile file( FilePath );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    const QByteArray contents = file.readAll();
    if( file.error() != QFileDevice::NoError )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    setFilePath( FilePath );
    if( m_scintilla )
        m_scintilla->SetText( contents );

    emit modificationChanged( false );
    return true;
}

bool BaseEdit::SaveFile( QString* ErrorMessage )
{
    if( m_filePath.isEmpty() )
    {
        if( ErrorMessage )
            *ErrorMessage = tr( "저장할 파일 경로가 없습니다." );
        return false;
    }

    return SaveFileAs( m_filePath, ErrorMessage );
}

bool BaseEdit::SaveFileAs( const QString& FilePath, QString* ErrorMessage )
{
    QFile file( FilePath );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    const QByteArray contents = m_scintilla ? m_scintilla->Text() : QByteArray();
    if( file.write( contents ) != contents.size() )
    {
        if( ErrorMessage )
            *ErrorMessage = file.errorString();
        return false;
    }

    setFilePath( FilePath );
    if( m_scintilla )
        m_scintilla->Send( SCI_SETSAVEPOINT );

    emit modificationChanged( false );
    return true;
}

QString BaseEdit::EditorType() const
{
    return QStringLiteral( "BaseEdit" );
}

bool BaseEdit::IsAutoCompletionAvailable() const
{
    return m_autoCompletionAvailable;
}

bool BaseEdit::IsPreviewAvailable() const
{
    return m_previewAvailable;
}

bool BaseEdit::IsOutlineAvailable() const
{
    return m_outlineAvailable;
}

bool BaseEdit::IsDiagnosticsAvailable() const
{
    return m_diagnosticsAvailable;
}

void BaseEdit::SetReadOnly( bool ReadOnly )
{
    if( m_scintilla )
        m_scintilla->SetReadOnly( ReadOnly );
}

bool BaseEdit::IsReadOnly() const
{
    return m_scintilla ? m_scintilla->IsReadOnly() : false;
}

void BaseEdit::SetAutoCompletionAvailable( bool Available )
{
    m_autoCompletionAvailable = Available;
}

void BaseEdit::SetPreviewAvailable( bool Available )
{
    m_previewAvailable = Available;
}

void BaseEdit::SetOutlineAvailable( bool Available )
{
    m_outlineAvailable = Available;
}

void BaseEdit::SetDiagnosticsAvailable( bool Available )
{
    m_diagnosticsAvailable = Available;
}

QString BaseEdit::normalizeFilePath( const QString& FilePath )
{
    const QFileInfo info( FilePath );
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
}

void BaseEdit::setFilePath( const QString& FilePath )
{
    const QString normalized = normalizeFilePath( FilePath );
    const QString absolute = QFileInfo( FilePath ).absoluteFilePath();

    if( m_filePath == absolute && m_normalizedFilePath == normalized )
        return;

    m_filePath = absolute;
    m_normalizedFilePath = normalized;
    emit filePathChanged( m_filePath );
}


