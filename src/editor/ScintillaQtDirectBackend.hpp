#pragma once

#include "ScintillaDocument.hpp"

#include <QFont>
#include <QPointer>

class ScintillaEditBase;

namespace Scintilla {
class ILexer5;
}

struct ScintillaEditorSettings;

class ScintillaQtDirectBackend : public QObject
{
	Q_OBJECT

public:
	explicit ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent = nullptr);
	~ScintillaQtDirectBackend() override;

	QWidget* widget() const;
	QAbstractScrollArea* scrollArea() const;

	void applySettings(const ScintillaEditorSettings& settings);
	void setText(const QString& text);
	QString text() const;
	void clear();

	void setModified(bool modified);
	bool isModified() const;
	int lineCount() const;

	void setReadOnly(bool readOnly);
	bool isReadOnly() const;

	bool hasSelectedText() const;
	QString selectedText() const;
	bool getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const;
	void setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo);
	void setSelectionByPos(int startPos, int endPos);
	void copy();

	void setCursorPosition(int line, int index);
	bool findFirst(const QString& text,
					   bool regex,
					   bool caseSensitive,
					   bool wholeWords,
					   bool wrap,
					   bool forward);
	void replace(const QString& replacement);

	int countMatches(const QString& text,
					 bool regex,
					 bool caseSensitive,
					 bool wholeWords);
	int countMatchesInRange(const QString& text,
							bool regex,
							bool caseSensitive,
							bool wholeWords,
							int startPos,
							int endPos);
	void setIndicatorStyle(int indicatorId, int style, const QColor& color);
	void applyIndicator(int indicatorId, int startPos, int length);
	void clearIndicator(int indicatorId, int startPos, int length);
	void clearAllIndicator(int indicatorId);
	int selectionStartPos() const;
	int selectionEndPos() const;
	int documentLength() const;
	int currentPos() const;
	int matchStartPos() const;
	int matchEndPos() const;
	void scrollRangeToView(int startPos, int endPos);

	void setSearchTargetRange(int startPos, int endPos);
	int searchInTarget(const QString& text, int flags);
	int targetEnd() const;
	void replaceInTarget(const QString& text);

	void setEditorFont(const QFont& font);
	QFont editorFont() const;

	int firstVisibleLine() const;
	void restoreViewState(int caretPosition, int firstVisibleLine);
	int textHeight(int line) const;
	int leftMarginWidth() const;
	int changeHistoryFlags() const;
	void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting);
	bool applyLanguage(const QString& displayName);
	void applyThemeColors(bool dark);

signals:
	void modificationChanged(bool modified);
	void cursorPositionChanged(int line, int index);
	void linesChanged();
	void textChanged();
	void selectionChanged();

private:
	void configureCodeFolding(bool enabled);
	void configureBraceHighlightIndicators();
	void syncLineCountState(bool emitSignal);
	void updateLineNumberMargin(int minimumDigits = 0);
	void emitCursorAndSelectionState();
	void updateBraceHighlight();
	void clearLexer();
	void applySyntaxStyles(bool dark);
	void setKeywordsForLexer(const QString& lexerKey);

	QPointer<ScintillaEditBase> m_editor;
	Scintilla::ILexer5*         m_currentLexer = nullptr; // Non-owning after SCI_SETILEXER while the editor is alive; cleared when the editor dies.
	QFont               m_editorFont;
	QString             m_currentLexerKey;
	bool                m_darkTheme = false;
	bool                m_codeFoldingEnabled = true;
	bool                m_braceMatchingEnabled = true;
	bool                m_highlightCurrentLine = true;
	bool                m_forcedModified = false;
	int                 m_lineNumberMarginDigits = 0;
	int                 m_lastKnownLineCount = 1;
};

