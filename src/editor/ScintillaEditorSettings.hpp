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

    bool  useUtf8 = true;
    bool  showLineNumbers = true;
    bool  showIndentationGuides = true;
    bool  highlightCurrentLine = true;
    bool  autoIndent = true;
    bool  useTabs = true;
    bool  braceMatching = true;
    bool  readOnly = false;
    bool  wordWrap = false;
    bool  showWhitespace = false;
    bool  showCodeFolding = true;
    int   tabWidth = 4;
    double lineSpacingScale = 1.1;
    int   lineNumberMarginDigits = 5;
    FontRenderingMode fontRendering = Antialiased;
    ChangeHistoryMode changeHistoryMode = ChangeHistoryBoth;
    IndentGuideStyle indentGuideStyle = IndentGuideReal;

    static ScintillaEditorSettings standard();
    static ScintillaEditorSettings limitedPreview();

};

