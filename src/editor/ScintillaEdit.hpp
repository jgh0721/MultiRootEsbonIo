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

    int                                 GetLineCount() const;

    void                                SetReadOnly( bool ReadOnly );
    bool                                IsReadOnly() const;

    bool                                HasSelectedText() const;
    QString                             SelectedText() const;
    bool                                GetSelectionRange( int& lineFrom, int& indexFrom, int& lineTo, int& indexTo ) const;
    void                                SetSelectionRange( int lineFrom, int indexFrom, int lineTo, int indexTo );
    void                                SetSelectionByPos( int startPos, int endPos );

	int									RowHeight( int Line ) const;
	int									LeftMarginWidth() const;
	void								SetLingEnding( Scintilla::EndOfLine Type, bool ConvertExisting );
	int									ChangeHistoryFlags() const;

signals:
	void                                modificationChanged( bool modified );
	void                                cursorPositionChanged( int line, int index );
	void                                linesChanged();
	void                                textChanged();
	void                                selectionChanged();

private:
	void                                emitCursorAndSelectionState();
    void                                updateBraceHighlight();

    QPointer< ScintillaEditBase >       m_editor;
    bool                                m_braceMatchingEnabled = false;
};