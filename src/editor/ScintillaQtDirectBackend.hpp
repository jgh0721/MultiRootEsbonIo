#pragma once

#include "ScintillaDocument.hpp"
#include "RstContainerLexer.hpp"

#include <QFont>
#include <QPointer>

#include <memory>

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
	/// 자동 줄넘김 관련 메시지만 보내고, **화면 맨 위 문서 줄을 지킨다.**
	///
	/// 줄넘김을 켜고 끄면 문서 전체가 다시 배치되면서 화면이 튄다. 그것을 되돌리는
	/// 것이 이 함수의 본래 이유이며 applySettings() 에는 없는 동작이다.
	///
	/// 예전 주석은 "applySettings() 가 끝에서 SCI_COLOURISE 로 문서 전체를 다시
	/// 렉싱하므로" 를 이유로 들었으나 그 호출은 제거되었다. 남은 것은 접기 깊이
	/// 재주입인데 그것도 이제 값이 달라진 줄만 보낸다 — 성능 논거는 더 이상
	/// 이 함수를 지탱하지 않는다. 스크롤 위치 보존이 지탱한다.
	void applyWrapSettings(const ScintillaEditorSettings& settings);
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

	/// 문서 전체의 **UTF-16 코드 유닛 수** (상태바의 "문자 N").
	///
	/// 캐시된 값이다. 예전 구현은 부를 때마다 `text().size()` 로 문서를 통째로
	/// 복사·디코딩했고, 그 경로는 **캐럿을 움직일 때마다** 돌았다. 실측(1095줄
	/// 45 KB)으로 상태바 갱신 한 번이 0.19 ms 였고 그중 대부분이 이것이다.
	///
	/// 값은 편집 통지에서 편집분만 더하고 빼서 유지한다(Utf16Length.hpp 의 판단).
	/// 캐시가 비어 있으면 여기서 한 번 전량 계산한다 — 그 계산도 SCI_GETRANGEPOINTER
	/// 로 갭 버퍼를 직접 훑으므로 복사가 없다.
	int characterCount() const;

	/// 선택 범위의 UTF-16 코드 유닛 수.
	///
	/// 여기는 캐시하지 않는다. 선택은 보통 작고, 캐시하면 무효화 계기가 편집이
	/// 아니라 선택 변경이라 사실상 매번 다시 세게 된다.
	int selectedCharacterCount() const;
	bool getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const;
	void setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo);
	void setSelectionByPos(int startPos, int endPos);
	void copy();
	void paste();
	bool canPaste() const;

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
	/// 화면 맨 위 행이 속한 문서 줄 (0-based). 줄넘김이 켜져 있어도 창 폭에
	/// 흔들리지 않으므로 세션/핫 엑시트 저장에는 이 값을 쓴다.
	int topDocumentLine() const;
	void setFirstVisibleLine(int line);
	int linesOnScreen() const;

	// ── 뷰포트 비율 기준 스크롤 ──
	//
	// 전부 픽셀 좌표로 계산한다. 문서 줄 번호로 계산하면 자동 줄바꿈이나 코드
	// 접기가 있을 때 어긋난다 (한 문서 줄이 화면에서는 여러 줄을 차지하거나
	// 아예 안 보일 수 있다).
	//
	// 반환/입력은 "소수 줄 번호" 다. 12.5 는 12번 줄의 세로 중간 지점을 뜻하며,
	// 줄바꿈으로 여러 행을 차지하는 줄 안에서의 위치까지 표현할 수 있다.
	double fractionalLineAtViewportRatio(double ratio) const;
	void scrollFractionalLineToViewportRatio(double fractionalLine, double ratio);
	void restoreViewState(int caretPosition, int firstVisibleLine);

	// ── 위치 변환 ──
	// Scintilla 위치는 UTF-8 바이트 오프셋이고 열은 코드 유닛 단위다.
	int positionFromLine(int line) const;
	int lineEndPosition(int line) const;
	int lineFromPosition(int position) const;
	int columnFromPosition(int position) const;
	int positionFromLineColumn(int line, int column) const;
	QByteArray textRangeUtf8(int startPos, int endPos) const;
	QString lineText(int line) const;
	QPoint pointFromPosition(int position) const;
	/// 뷰포트 좌표 -> 문서 위치. 글자 위가 아니면 -1.
	int positionFromPoint(const QPoint& viewportPos) const;

	// ── 컨테이너 렉싱 지원 ──
	// ILexer 를 비운 상태에서 styleNeeded 시그널을 받아 직접 스타일을 칠할 때 쓴다.
	int endStyled() const;
	void startStyling(int position);
	void setStylingEx(const QByteArray& styleBytes);
	void setStyleForeground(int style, const QColor& color);
	void setStyleBackground(int style, const QColor& color);
	void setStyleBold(int style, bool bold);
	void setStyleItalic(int style, bool italic);
	void setStyleUnderline(int style, bool underline);
	int textHeight(int line) const;
	int leftMarginWidth() const;
	int changeHistoryFlags() const;
	void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting);
	bool applyLanguage(const QString& displayName);
	void applyThemeColors(bool dark);

	/// reST 컨테이너 렉서의 메타데이터 캐시. Esbonio 자동완성 결과를 여기에
	/// 넣으면 directive/role 이 UNKNOWN -> VALID/INVALID 로 바뀐다.
	/// 컨테이너 렉싱 중이 아니면 nullptr.
	[[nodiscard]] mrst::rst::RstMetadataCache* rstMetadataCache() const;
	/// 스타일을 무효화하고 다음 페인트에 다시 칠하게 한다.
	///
	/// SCI_COLOURISE 를 쓰지 않는다 — 컨테이너 렉싱에서 그것은 문서 전체를 한 번에
	/// 동기 렉싱하게 만들어 SC_IDLESTYLING_ALL 의 시간 예산을 우회한다.
	void invalidateStyling();

	// ── 코드 접기 ──
	/// 그 문서 줄이 지금 화면에 보이는가 (접혀 있지 않은가).
	[[nodiscard]] bool isLineVisible(int line) const;
	/// 접혀서 안 보이는 줄이면 그 줄을 감추고 있는, 화면에 보이는 조상 줄.
	/// 보이는 줄이면 그대로 돌려준다.
	[[nodiscard]] int visibleAnchorLine(int line) const;
	/// 그 줄이 접힌 블록 안에 있으면 펼쳐서 보이게 한다.
	void ensureLineVisible(int line);
	/// 전체 접기/펼치기. 개요나 단축키에서 부른다.
	void foldAll(bool contract);

signals:
	void modificationChanged(bool modified);
	void cursorPositionChanged(int line, int index);
	void linesChanged();
	void textChanged();
	void selectionChanged();
	/// 컨테이너 렉싱 모드에서 Scintilla 가 스타일을 요구하는 구간의 끝 위치.
	/// SCI_SETIDLESTYLING 때문에 문서 전체가 아니라 청크 단위로 도착한다.
	void styleNeeded(int endPosition);
	/// 본문이 삽입되거나 삭제되었다. 좌표는 **LSP 규약 그대로** — 0-based 줄과
	/// 줄머리로부터의 **바이트 수**다(SCI_GETCOLUMN 의 표시 열이 아니다).
	///
	/// 삽입이면 oldEnd 가 start 와 같고 newText 가 넣은 본문이다.
	/// 삭제면 oldEnd 가 지워지기 전의 끝이고 newText 가 비어 있다.
	///
	/// 위치 인코딩을 utf-8 로 협상하면 이 값이 LSP Position 에 그대로 들어간다.
	void documentEdited(int startLine, int startColumn, int oldEndLine, int oldEndColumn,
						const QByteArray& newText);
	/// 사용자가 문자를 입력했다. 자동완성 트리거 감지에 쓴다.
	void charAdded(int ch);
	/// 세로 스크롤이 변했다. 프리뷰 동기화에 쓴다.
	void viewportScrolled();
	/// 마우스가 글자 위에서 멈췄다. position 은 문서 위치, viewportPos 는 뷰포트 좌표.
	void dwellStarted(int position, const QPoint& viewportPos);
	void dwellEnded();

private:
	void configureCodeFolding(bool enabled);
	void configureBraceHighlightIndicators();
	/// characterCount() 의 캐시. -1 은 "모른다" 이고 다음 조회에서 전량 계산한다.
	mutable int m_characterCount = -1;
	/// 갭 버퍼를 직접 훑어 전량 계산한다. 복사도 할당도 하지 않는다.
	int recountCharacters() const;

	void syncLineCountState(bool emitSignal);
	void updateLineNumberMargin(int minimumDigits = 0);
	void emitCursorAndSelectionState();
	void updateBraceHighlight();
	void clearLexer();
	void applySyntaxStyles(bool dark);
	void applyRstSyntaxStyles();
	void applyMarkdownSyntaxStyles();
	void setKeywordsForLexer(const QString& lexerKey);
	void handleStyleNeeded(int endPosition);
	/// 렉싱 창의 시작 줄을 리터럴 블록 상태가 확실한 곳까지 위로 넓힌다.
	[[nodiscard]] int literalSeedLine(int contextFirstLine) const;
	/// 접기 깊이 캐시를 버린다. 본문을 통째로 갈아 끼웠을 때 부른다.
	void invalidateFoldLevelCache();
	void emitDocumentEdited(bool inserted, int position, int length, int linesAdded,
							const QByteArray& changedText);
	/// 접기 깊이를 누가 채우는가.
	///
	/// Lexilla 렉서에 folder 함수가 있으면 Scintilla 가 알아서 하지만 둘은 그렇지
	/// 않다 — reST 는 렉서 자체가 없고, Markdown 은 LexMarkdown 이 3인자
	/// LexerModule 로 등록되어 fnFolder 가 nullptr 이다. 그 둘만 우리가 문서를
	/// 훑어 SCI_SETFOLDLEVEL 로 직접 넣는다.
	///
	/// 이 값이 아니라 m_rstLexer 로 게이트를 두면 Markdown 은 한 줄도 주입되지
	/// 않는다(그 포인터는 reST 컨테이너 렉싱에서만 만들어진다).
	enum class FoldSource
	{
		Lexer,          ///< Lexilla 렉서가 채운다. 우리는 손대지 않는다
		RstContainer,
		Markdown
	};

	/// reST 와 Markdown 은 접기 깊이를 우리가 계산해 넣는다. 섹션 깊이가 문서
	/// 앞부분 전체에 의존하므로 문서 단위로 다시 센다.
	void updateContainerFoldLevels();
	void scheduleContainerFoldUpdate();

	QPointer<ScintillaEditBase> m_editor;
	/// 직전에 Scintilla 에 넣은 접기 깊이. SCI_GETFOLDLEVEL 왕복을 없애고
	/// 실제로 바뀐 줄만 보내기 위한 것이다.
	std::vector<int> m_foldLevelCache;
	std::unique_ptr<mrst::rst::RstContainerLexer> m_rstLexer;   // 컨테이너 렉싱 중에만 유효
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
	/// 편집 중에 매 글자 문서 전체의 접기 깊이를 다시 세지 않는다.
	class QTimer*       m_containerFoldTimer = nullptr;
	/// 접기 깊이의 출처. clearLexer() 가 Lexer 로 되돌린다.
	FoldSource          m_foldSource = FoldSource::Lexer;
};

