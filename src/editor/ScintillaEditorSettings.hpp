#pragma once

#include <QFont>

struct ScintillaEditorSettings
{
    enum FontRenderingMode {
        Default = 0,
        NonAntialiased = 1,
        Antialiased = 2,       // GrayScale
        LcdOptimized = 3       // ClearType
    };

    enum ChangeHistoryMode {
        ChangeHistoryOff = 0,
        ChangeHistoryMarkers = 1,
        ChangeHistoryIndicators = 2,
        ChangeHistoryBoth = 3
    };

    enum IndentGuideStyle {
        IndentGuideReal = 1,
        IndentGuideLookForward = 2,
        IndentGuideLookBoth = 3
    };

    // 값은 Scintilla 의 SC_WRAP_* 와 같다.
    enum WrapMode {
        WrapNone = 0,
        WrapWord = 1,
        WrapChar = 2,
        WrapWhitespace = 3
    };

    // SC_WRAPVISUALFLAG_* 비트 조합. 필드는 enum 이 아니라 int 로 둔다.
    enum WrapVisualFlag {
        WrapFlagNone = 0x0000,
        WrapFlagEnd = 0x0001,
        WrapFlagStart = 0x0002,
        WrapFlagMargin = 0x0004
    };
    static constexpr int WrapFlagMask = 0x0007;

    // SC_WRAPINDENT_*
    enum WrapIndentMode {
        WrapIndentFixed = 0,
        WrapIndentSame = 1,
        WrapIndentIndent = 2,
        WrapIndentDeepIndent = 3
    };

    bool  useUtf8 = true;
    bool  showLineNumbers = true;
    bool  showIndentationGuides = true;
    bool  highlightCurrentLine = true;
    bool  autoIndent = true;
    bool  useTabs = true;
    bool  braceMatching = true;
    bool  readOnly = false;
    bool  showWhitespace = false;
    bool  showCodeFolding = true;
    int   tabWidth = 4;
    double lineSpacingScale = 1.1;
    int   lineNumberMarginDigits = 5;
    FontRenderingMode fontRendering = Antialiased;
    ChangeHistoryMode changeHistoryMode = ChangeHistoryBoth;
    IndentGuideStyle indentGuideStyle = IndentGuideReal;

    WrapMode wrapMode = WrapChar;
    /// 접힌 행 끝에 이어짐 표시를 그린다. 가로 자리를 차지하지 않으면서
    /// WrapIndentSame 의 약점(연속 행이 형제 줄처럼 보임)을 없애 준다.
    int      wrapVisualFlags = WrapFlagEnd;
    /// reST 는 들여쓰기가 곧 문법이다. Fixed 로 두면 접힌 뒷행이 0열에서
    /// 시작해 실제로 없는 블록 경계처럼 보인다.
    WrapIndentMode wrapIndentMode = WrapIndentSame;

    static ScintillaEditorSettings standard();
    static ScintillaEditorSettings limitedPreview();

};

