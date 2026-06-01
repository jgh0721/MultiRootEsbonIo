#pragma once

#include <ScintillaEditBase.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>

class QScintillaEdit : public QObject
{
    Q_OBJECT
public:
    explicit QScintillaEdit( QWidget* EditorParent, QObject* Parent = nullptr );
    ~QScintillaEdit() override;

    QWidget*                            Editor() const;
    ScintillaEditBase*                  ScintillaWidget() const;

    sptr_t                              Send( unsigned int Message, uptr_t WParam = 0, sptr_t LParam = 0 ) const;
    sptr_t                              Sends( unsigned int Message, uptr_t WParam = 0, const char* Text = nullptr ) const;

    void                                SetText( const QByteArray& Text );
    QByteArray                          Text() const;
    void                                Copy();

    int                                 GetLineCount() const;

    void                                SetReadOnly( bool ReadOnly );
    bool                                IsReadOnly() const;

    bool                                HasSelectedText() const;
    QString                             SelectedText() const;
    bool                                GetSelectionRange( int& lineFrom, int& indexFrom, int& lineTo, int& indexTo ) const;
    void                                SetSelectionRange( int lineFrom, int indexFrom, int lineTo, int indexTo );
    void                                SetSelectionByPos( int startPos, int endPos );
    int                                 SelectionStartPos() const;
    int                                 SelectionEndPos() const;

    void                                SetCursorPosition( int line, int index );
    bool                                FindFirst( const QString& text, bool regex, bool caseSensitive, bool wholeWords, bool wrap, bool forward );
    void                                Replace( const QString& replacement );
    int                                 CountMatches( const QString& text, bool regex, bool caseSensitive, bool wholeWords );
    int                                 CountMatchesInRange( const QString& text, bool regex, bool caseSensitive, bool wholeWords, int startPos, int endPos );
    void                                SetSearchTargetRange( int startPos, int endPos );
    int                                 SearchInTarget( const QString& text, int flags );
    int                                 TargetEnd() const;
    void                                ReplaceInTarget( const QString& text );

    int                                 DocumentLength() const;

	int									RowHeight( int Line ) const;
	int									LeftMarginWidth() const;
	void								SetLingEnding( Scintilla::EndOfLine Type, bool ConvertExisting );
	int									ChangeHistoryFlags() const;

    void                                SetIndicatorStyle( int indicatorId, int style, const QColor& color );
    void                                ApplyIndicator( int indicatorId, int startPos, int length );
    void                                ClearIndicator( int indicatorId, int startPos, int length );
    void                                ClearAllIndicator( int indicatorId );

    void                                ApplyThemeColors( bool dark );
    void                                ScrollRangeToView( int startPos, int endPos );

signals:
	void                                modificationChanged( bool modified );
	void                                cursorPositionChanged( int line, int index );
	void                                linesChanged();
	void                                textChanged();
	void                                selectionChanged();

private:
    void                                configureCodeFolding( bool enabled );
    void                                configureBraceHighlightIndicators();
    void                                updateLineNumberMargin( int minimumDigits = 0 );
    void                                emitCursorAndSelectionState();
    void                                updateBraceHighlight();
    void                                applySyntaxStyles( bool dark );

    QPointer< ScintillaEditBase >       m_editor;
    QFont                               m_editorFont;
    QFont                               m_editorRulerFont;
    QFont                               m_editorMarginFont;
    bool                                m_braceMatchingEnabled = false;
    bool                                m_darkTheme = false;
};