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
    virtual ~QScintillaEdit();

    QWidget*                            Editor() const;

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