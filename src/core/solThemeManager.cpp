#include "stdafx.h"
#include "core/solThemeManager.hpp"
#include "core/solAppSettings.hpp"
#include "core/solQlementineTheme.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
    constexpr auto kThemeOverridesKey              = "theme/colorOverridesJson";
    constexpr auto kThemeOverridesSchemaVersionKey = "theme/colorOverridesSchemaVersion";
    /// main() 이 시작할 때 읽는 키. 설정 대화상자와 "보기 > 테마 전환" 이
    /// 같은 값을 써야 하므로 저장은 ThemeManager::setTheme() 한 곳에서만 한다.
    constexpr auto kThemeKey                       = "theme";
    constexpr int  kThemeOverridesSchemaVersion    = 4;

    QJsonObject colorMapToJson( const QHash< QString, QColor >& colors )
    {
        QJsonObject object;
        const auto  keys = colors.keys();
        for( const QString& key : keys )
        {
            const QColor color = colors.value( key );
            if( color.isValid() )
                object.insert( key, color.name( QColor::HexArgb ) );
        }
        return object;
    }

    QHash< QString, QColor > jsonToColorMap( const QJsonObject& object )
    {
        QHash< QString, QColor > colors;
        for( auto it = object.constBegin(); it != object.constEnd(); ++it )
        {
            const QColor color( it.value().toString() );
            if( color.isValid() )
                colors.insert( it.key(), color );
        }
        return colors;
    }
} // namespace

ThemeManager& ThemeManager::instance()
{
    static ThemeManager mgr;
    return mgr;
}

ThemeManager::ThemeManager()
{
//     loadOverrides();
}

ThemeManager::Theme ThemeManager::currentTheme() const
{
    return m_theme;
}

void ThemeManager::setTheme( Theme theme )
{
    // 값이 같아도 저장까지 건너뛰지는 않는다. 설정 파일에 키가 아직 없을 수
    // 있는데, "지금 기본값과 같으니 안 써도 된다" 고 두면 나중에 기본값이
    // 바뀌었을 때 사용자가 고른 적 없는 테마로 조용히 바뀐다.
    AppSettings settings;
    settings.setValue( QString::fromLatin1( kThemeKey ), static_cast< int >( theme ) );

    if( m_theme == theme ) return;
    m_theme = theme;
    applyToApplication();
    emit themeChanged( theme );
}

ThemeManager::Theme ThemeManager::savedTheme()
{
    AppSettings settings;
    const int stored =
        settings.value( QString::fromLatin1( kThemeKey ), static_cast< int >( Dark ) ).toInt();

    // 설정 파일은 사람이 고칠 수 있다. 범위를 벗어난 값을 그대로 enum 으로
    // 캐스팅하면 색 조회가 전부 기본값(마젠타)으로 떨어진다.
    return static_cast< Theme >(
        qBound( static_cast< int >( Light ), stored, static_cast< int >( Dark ) ) );
}

QColor ThemeManager::color( const QString& key ) const
{
    const auto  defaults  = defaultColors( m_theme );
    const auto& overrides = ( m_theme == Dark ) ? m_darkOverrides : m_lightOverrides;
    return overrides.value( key, defaults.value( key, QColor( Qt::magenta ) ) );
}

bool ThemeManager::hasColor( const QString& key ) const
{
    const auto  defaults  = defaultColors( m_theme );
    const auto& overrides = ( m_theme == Dark ) ? m_darkOverrides : m_lightOverrides;
    return overrides.contains( key ) || defaults.contains( key );
}

bool ThemeManager::hasColorOverride(const QString& key) const
{
    const auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    return overrides.contains(key);
}

void ThemeManager::setColorOverride( const QString& key, const QColor& color )
{
    if( key.isEmpty() || !color.isValid() )
        return;

    auto& overrides = ( m_theme == Dark ) ? m_darkOverrides : m_lightOverrides;
    overrides.insert( key, color );
    saveOverrides();
    applyToApplication();
    emit themeChanged( m_theme );
}

void ThemeManager::setColorOverrides( const QHash< QString, QColor >& colors )
{
    auto& overrides = ( m_theme == Dark ) ? m_darkOverrides : m_lightOverrides;
    overrides       = colors;
    saveOverrides();
    applyToApplication();
    emit themeChanged( m_theme );
}

void ThemeManager::resetColorOverrides( Theme theme )
{
    if( theme == Dark )
        m_darkOverrides.clear();
    else
        m_lightOverrides.clear();
    saveOverrides();
    if( m_theme == theme )
    {
        applyToApplication();
        emit themeChanged( m_theme );
    }
}

QHash< QString, QColor > ThemeManager::colorOverrides( Theme theme ) const
{
    return theme == Dark ? m_darkOverrides : m_lightOverrides;
}

QHash< QString, QColor > ThemeManager::effectiveColors( Theme theme ) const
{
    auto       colors    = defaultColors( theme );
    const auto overrides = colorOverrides( theme );
    for( auto it = overrides.constBegin(); it != overrides.constEnd(); ++it )
        colors.insert( it.key(), it.value() );
    return colors;
}

QString ThemeManager::scopeLabel( const QString& groupId )
{
    // 범위 이름을 만드는 곳은 여기 하나다. 항목 표에서 tr() 로 되풀이하면 같은
    // 원문이 94번 반복되고, 설정 대화상자가 만드는 상세 항목과 문자열이
    // 어긋나는 순간 그 범위가 통째로 걸러진다.
    if( groupId == QLatin1String( ThemeScopeIds::kCommon ) )          return tr( "공통" );
    if( groupId == QLatin1String( ThemeScopeIds::kPdf ) )             return tr( "PDF" );
    if( groupId == QLatin1String( ThemeScopeIds::kImage ) )           return tr( "IMAGE" );
    if( groupId == QLatin1String( ThemeScopeIds::kMarkdown ) )        return tr( "Markdown" );
    if( groupId == QLatin1String( ThemeScopeIds::kText ) )            return tr( "TEXT" );
    if( groupId == QLatin1String( ThemeScopeIds::kTextLexer ) )       return tr( "TEXT Lexer" );
    if( groupId == QLatin1String( ThemeScopeIds::kTextLexerDetail ) ) return tr( "TEXT Lexer 상세" );
    if( groupId == QLatin1String( ThemeScopeIds::kTextLexerRst ) )    return tr( "TEXT Lexer reST" );
    // 모르는 식별자는 그대로 보여 준다. 빈 칸보다는 낫고, 새 범위를 추가하고
    // 여기 넣는 것을 잊었다는 사실이 화면에 바로 드러난다.
    return groupId;
}

QList< ThemeManager::ColorEntry > ThemeManager::editableColorEntries()
{
    QList< ColorEntry > entries = {
        { QStringLiteral( "common.background" ), tr( "앱 배경" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.surface" ), tr( "표면/패널" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.surfaceAlt" ), tr( "보조 표면" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.foreground" ), tr( "기본 글자" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.foregroundMuted" ), tr( "보조 글자" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.accent" ), tr( "강조색" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.border" ), tr( "테두리" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.toolbar" ), tr( "툴바" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.tabActive" ), tr( "활성 탭" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.tabInactive" ), tr( "비활성 탭" ), QLatin1String( ThemeScopeIds::kCommon ) },
        { QStringLiteral( "common.canvas" ), tr( "캔버스 영역" ), QLatin1String( ThemeScopeIds::kCommon ) },

        { QStringLiteral( "pdf.canvas" ), tr( "PDF 캔버스" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.pageGap" ), tr( "PDF 페이지 간격" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.searchHighlight" ), tr( "PDF 검색 하이라이트" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.searchCurrent" ), tr( "PDF 현재 검색 결과" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.textSelection" ), tr( "PDF 텍스트 선택" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.imageSelection" ), tr( "PDF 이미지 선택" ), QLatin1String( ThemeScopeIds::kPdf ) },
        { QStringLiteral( "pdf.annotation" ), tr( "PDF 기본 주석" ), QLatin1String( ThemeScopeIds::kPdf ) },

        { QStringLiteral( "image.canvas" ), tr( "이미지 캔버스" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.draw" ), tr( "이미지 기본 그리기" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.selection" ), tr( "이미지 선택/크롭" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.compositeOutline" ), tr( "이미지 합성 경계" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.handleFill" ), tr( "이미지 핸들 배경" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.handleBorder" ), tr( "이미지 핸들 테두리" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.accept" ), tr( "이미지 승인 버튼" ), QLatin1String( ThemeScopeIds::kImage ) },
        { QStringLiteral( "image.cancel" ), tr( "이미지 취소 버튼" ), QLatin1String( ThemeScopeIds::kImage ) },

        { QStringLiteral( "markdown.background" ), tr( "Markdown 문서 배경" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.foreground" ), tr( "Markdown 본문" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.link" ), tr( "Markdown 링크" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.heading" ), tr( "Markdown 제목" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.border" ), tr( "Markdown 테두리" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.codeBackground" ), tr( "Markdown 코드 배경" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.inlineCode" ), tr( "Markdown 인라인 코드" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.blockquote" ), tr( "Markdown 인용문" ), QLatin1String( ThemeScopeIds::kMarkdown ) },
        { QStringLiteral( "markdown.taskChecked" ), tr( "Markdown 체크 항목" ), QLatin1String( ThemeScopeIds::kMarkdown ) },

        { QStringLiteral( "text.background" ), tr( "Text 편집기 배경" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.foreground" ), tr( "Text 기본 글자" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.currentLine" ), tr( "Text 현재 줄" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.selection" ), tr( "Text 선택 배경" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.selectionForeground" ), tr( "Text 선택 글자" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.caret" ), tr( "Text 캐럿" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.marginBackground" ), tr( "Text 여백 배경" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.marginForeground" ), tr( "Text 줄 번호" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.indentGuide" ), tr( "Text 들여쓰기 가이드" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.foldMarker" ), tr( "Text 폴딩 마커" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.braceMatch" ), tr( "Text 괄호 일치" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.braceMismatch" ), tr( "Text 괄호 오류" ), QLatin1String( ThemeScopeIds::kText ) },
        { QStringLiteral( "text.lexer.comment" ), tr( "Lexer 주석" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.number" ), tr( "Lexer 숫자" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.keyword" ), tr( "Lexer 키워드" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.type" ), tr( "Lexer 타입/보조 키워드" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.string" ), tr( "Lexer 문자열" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.preprocessor" ), tr( "Lexer 전처리/속성" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.operator" ), tr( "Lexer 연산자" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.identifier" ), tr( "Lexer 식별자" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.function" ), tr( "Lexer 함수/클래스" ), QLatin1String( ThemeScopeIds::kTextLexer ) },
        { QStringLiteral( "text.lexer.variable" ), tr( "Lexer 변수" ), QLatin1String( ThemeScopeIds::kTextLexer ) },

        { QStringLiteral( "text.lexer.cpp.comment" ), tr( "C++ 주석" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.cpp.keyword" ), tr( "C++ 키워드" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.cpp.string" ), tr( "C++ 문자열" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.cpp.function" ), tr( "C++ 함수/클래스" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.python.comment" ), tr( "Python 주석" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.python.keyword" ), tr( "Python 키워드" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.python.string" ), tr( "Python 문자열" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.python.function" ), tr( "Python 함수/클래스" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.json.keyword" ), tr( "JSON true/false/null" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.json.string" ), tr( "JSON 문자열" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.json.function" ), tr( "JSON 속성명" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.xml.keyword" ), tr( "XML/HTML 태그" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.xml.preprocessor" ), tr( "XML/HTML 속성" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.css.keyword" ), tr( "CSS 선택자" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.css.preprocessor" ), tr( "CSS 속성" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.bash.variable" ), tr( "Bash 변수" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },
        { QStringLiteral( "text.lexer.sql.keyword" ), tr( "SQL 키워드" ), QLatin1String( ThemeScopeIds::kTextLexerDetail ) },

        // reStructuredText 는 Lexilla 렉서가 없어 자체 컨테이너 렉서로 칠한다.
        // directive/role 은 Esbonio 자동완성으로 확인되기 전까지 UNKNOWN 색을 쓴다.
        { QStringLiteral( "text.lexer.rst.title" ), tr( "reST 제목" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.transition" ), tr( "reST 구분선" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.comment" ), tr( "reST 주석" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.explicitMarkup" ), tr( "reST 명시적 마크업(..)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.directiveValid" ), tr( "reST directive (확인됨)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.directiveInvalid" ), tr( "reST directive (알 수 없음)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.directiveUnknown" ), tr( "reST directive (미확인)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.roleValid" ), tr( "reST role (확인됨)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.roleInvalid" ), tr( "reST role (알 수 없음)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.roleUnknown" ), tr( "reST role (미확인)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.literal" ), tr( "reST 리터럴 블록" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.inlineLiteral" ), tr( "reST 인라인 리터럴(``)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.emphasis" ), tr( "reST 강조(*)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.strong" ), tr( "reST 굵게(**)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.interpreted" ), tr( "reST 해석 텍스트" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.hyperlink" ), tr( "reST 하이퍼링크" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.substitution" ), tr( "reST 치환(|..|)" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
        { QStringLiteral( "text.lexer.rst.fieldName" ), tr( "reST 필드명" ), QLatin1String( ThemeScopeIds::kTextLexerRst ) },
    };

    for( ColorEntry& entry : entries )
        entry.group = scopeLabel( entry.groupId );
    return entries;
}

QHash< QString, QColor > ThemeManager::defaultColors( Theme theme )
{
    QHash< QString, QColor > c;
    if( theme == Dark )
    {
        // Modern Windows 11 Dark 기반 공통 팔레트
        c.insert( QStringLiteral( "common.background" ), QColor( QStringLiteral( "#202020" ) ) );
        c.insert( QStringLiteral( "common.surface" ), QColor( QStringLiteral( "#2B2B2B" ) ) );
        c.insert( QStringLiteral( "common.surfaceAlt" ), QColor( QStringLiteral( "#323232" ) ) );
        c.insert( QStringLiteral( "common.foreground" ), QColor( QStringLiteral( "#F3F3F3" ) ) );
        c.insert( QStringLiteral( "common.foregroundMuted" ), QColor( QStringLiteral( "#CFCFCF" ) ) );
        c.insert( QStringLiteral( "common.accent" ), QColor( QStringLiteral( "#60CDFF" ) ) );
        c.insert( QStringLiteral( "common.border" ), QColor( QStringLiteral( "#3A3A3A" ) ) );
        c.insert( QStringLiteral( "common.toolbar" ), QColor( QStringLiteral( "#2B2B2B" ) ) );
        c.insert( QStringLiteral( "common.tabActive" ), QColor( QStringLiteral( "#202020" ) ) );
        c.insert( QStringLiteral( "common.tabInactive" ), QColor( QStringLiteral( "#323232" ) ) );
        c.insert( QStringLiteral( "common.canvas" ), QColor( QStringLiteral( "#1C1C1C" ) ) );
    }
    else
    {
        // Modern Windows 11 Light 기반 공통 팔레트
        c.insert( QStringLiteral( "common.background" ), QColor( QStringLiteral( "#F3F3F3" ) ) );
        c.insert( QStringLiteral( "common.surface" ), QColor( QStringLiteral( "#FFFFFF" ) ) );
        c.insert( QStringLiteral( "common.surfaceAlt" ), QColor( QStringLiteral( "#F9F9F9" ) ) );
        c.insert( QStringLiteral( "common.foreground" ), QColor( QStringLiteral( "#1A1A1A" ) ) );
        c.insert( QStringLiteral( "common.foregroundMuted" ), QColor( QStringLiteral( "#5C5C5C" ) ) );
        c.insert( QStringLiteral( "common.accent" ), QColor( QStringLiteral( "#0078D4" ) ) );
        c.insert( QStringLiteral( "common.border" ), QColor( QStringLiteral( "#E5E5E5" ) ) );
        c.insert( QStringLiteral( "common.toolbar" ), QColor( QStringLiteral( "#F9F9F9" ) ) );
        c.insert( QStringLiteral( "common.tabActive" ), QColor( QStringLiteral( "#FFFFFF" ) ) );
        c.insert( QStringLiteral( "common.tabInactive" ), QColor( QStringLiteral( "#F3F3F3" ) ) );
        c.insert( QStringLiteral( "common.canvas" ), QColor( QStringLiteral( "#F3F3F3" ) ) );
    }

    c.insert( QStringLiteral( "pdf.canvas" ), c.value( QStringLiteral( "common.canvas" ) ) );
    c.insert( QStringLiteral( "pdf.pageGap" ), theme == Dark
                                                   ? QColor( QStringLiteral( "#2B2B2B" ) )
                                                   : QColor( QStringLiteral( "#E5E5E5" ) ) );
    c.insert( QStringLiteral( "pdf.searchHighlight" ), QColor( QStringLiteral( "#5AFFEB3B" ) ) );
    c.insert( QStringLiteral( "pdf.searchCurrent" ), QColor( QStringLiteral( "#6EFF8C00" ) ) );
    c.insert( QStringLiteral( "pdf.textSelection" ), QColor( QStringLiteral( "#462196F3" ) ) );
    c.insert( QStringLiteral( "pdf.imageSelection" ), QColor( QStringLiteral( "#E62ECC71" ) ) );
    c.insert( QStringLiteral( "pdf.annotation" ), QColor( QStringLiteral( "#80FFEB3B" ) ) );

    c.insert( QStringLiteral( "image.canvas" ), c.value( QStringLiteral( "common.canvas" ) ) );
    c.insert( QStringLiteral( "image.draw" ), QColor( QStringLiteral( "#D13438" ) ) );
    c.insert( QStringLiteral( "image.selection" ), c.value( QStringLiteral( "common.accent" ) ) );
    c.insert( QStringLiteral( "image.compositeOutline" ), theme == Dark
                                                              ? QColor( QStringLiteral( "#F3F3F3" ) )
                                                              : QColor( QStringLiteral( "#FFFFFF" ) ) );
    c.insert( QStringLiteral( "image.handleFill" ), QColor( QStringLiteral( "#FFFFFF" ) ) );
    c.insert( QStringLiteral( "image.handleBorder" ), QColor( QStringLiteral( "#1A1A1A" ) ) );
    c.insert( QStringLiteral( "image.accept" ), QColor( QStringLiteral( "#107C10" ) ) );
    c.insert( QStringLiteral( "image.cancel" ), QColor( QStringLiteral( "#D13438" ) ) );

    c.insert( QStringLiteral( "markdown.background" ), theme == Dark
                                                           ? QColor( QStringLiteral( "#0D1117" ) )
                                                           : QColor( QStringLiteral( "#FFFFFF" ) ) );
    c.insert( QStringLiteral( "markdown.foreground" ), theme == Dark
                                                           ? QColor( QStringLiteral( "#C9D1D9" ) )
                                                           : QColor( QStringLiteral( "#24292F" ) ) );
    c.insert( QStringLiteral( "markdown.link" ), theme == Dark
                                                     ? QColor( QStringLiteral( "#58A6FF" ) )
                                                     : QColor( QStringLiteral( "#0969DA" ) ) );
    c.insert( QStringLiteral( "markdown.heading" ), theme == Dark
                                                        ? QColor( QStringLiteral( "#E6EDF3" ) )
                                                        : QColor( QStringLiteral( "#24292F" ) ) );
    c.insert( QStringLiteral( "markdown.border" ), theme == Dark
                                                       ? QColor( QStringLiteral( "#30363D" ) )
                                                       : QColor( QStringLiteral( "#D0D7DE" ) ) );
    c.insert( QStringLiteral( "markdown.codeBackground" ), theme == Dark
                                                               ? QColor( QStringLiteral( "#161B22" ) )
                                                               : QColor( QStringLiteral( "#F6F8FA" ) ) );
    c.insert( QStringLiteral( "markdown.inlineCode" ), theme == Dark
                                                           ? QColor( QStringLiteral( "#FF7B72" ) )
                                                           : QColor( QStringLiteral( "#CF222E" ) ) );
    c.insert( QStringLiteral( "markdown.blockquote" ), theme == Dark
                                                           ? QColor( QStringLiteral( "#8B949E" ) )
                                                           : QColor( QStringLiteral( "#57606A" ) ) );
    c.insert( QStringLiteral( "markdown.taskChecked" ), theme == Dark
                                                            ? QColor( QStringLiteral( "#238636" ) )
                                                            : QColor( QStringLiteral( "#2DA44E" ) ) );

    if( theme == Dark )
    {
        // Monokai 원형에 가까운 Dark 코드 팔레트
        c.insert( QStringLiteral( "text.background" ), QColor( QStringLiteral( "#272822" ) ) );
        c.insert( QStringLiteral( "text.foreground" ), QColor( QStringLiteral( "#F8F8F2" ) ) );
        c.insert( QStringLiteral( "text.currentLine" ), QColor( QStringLiteral( "#3E3D32" ) ) );
        c.insert( QStringLiteral( "text.selection" ), QColor( QStringLiteral( "#804A90E2" ) ) );
        c.insert( QStringLiteral( "text.selectionForeground" ), QColor( QStringLiteral( "#FFFFFFFF" ) ) );
        c.insert( QStringLiteral( "text.caret" ), QColor( QStringLiteral( "#F8F8F0" ) ) );
        c.insert( QStringLiteral( "text.marginBackground" ), QColor( QStringLiteral( "#1E1F1C" ) ) );
        c.insert( QStringLiteral( "text.marginForeground" ), QColor( QStringLiteral( "#90908A" ) ) );
        c.insert( QStringLiteral( "text.indentGuide" ), QColor( QStringLiteral( "#5A5A52" ) ) );
        c.insert( QStringLiteral( "text.foldMarker" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.braceMatch" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.braceMismatch" ), QColor( QStringLiteral( "#F92672" ) ) );
        c.insert( QStringLiteral( "text.lexer.comment" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.lexer.number" ), QColor( QStringLiteral( "#AE81FF" ) ) );
        c.insert( QStringLiteral( "text.lexer.keyword" ), QColor( QStringLiteral( "#F92672" ) ) );
        c.insert( QStringLiteral( "text.lexer.type" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.lexer.string" ), QColor( QStringLiteral( "#E6DB74" ) ) );
        c.insert( QStringLiteral( "text.lexer.preprocessor" ), QColor( QStringLiteral( "#A6E22E" ) ) );
        c.insert( QStringLiteral( "text.lexer.operator" ), QColor( QStringLiteral( "#F92672" ) ) );
        c.insert( QStringLiteral( "text.lexer.identifier" ), QColor( QStringLiteral( "#F8F8F2" ) ) );
        c.insert( QStringLiteral( "text.lexer.function" ), QColor( QStringLiteral( "#A6E22E" ) ) );
        c.insert( QStringLiteral( "text.lexer.variable" ), QColor( QStringLiteral( "#FD971F" ) ) );
    }
    else
    {
        // Windows 11 Light 표면 위에서 Monokai 계열 강조색을 유지한 Light 코드 팔레트
        c.insert( QStringLiteral( "text.background" ), QColor( QStringLiteral( "#FFFFFF" ) ) );
        c.insert( QStringLiteral( "text.foreground" ), QColor( QStringLiteral( "#272822" ) ) );
        c.insert( QStringLiteral( "text.currentLine" ), QColor( QStringLiteral( "#FFF8D6" ) ) );
        c.insert( QStringLiteral( "text.selection" ), QColor( QStringLiteral( "#660078D4" ) ) );
        c.insert( QStringLiteral( "text.selectionForeground" ), QColor( QStringLiteral( "#FFFFFFFF" ) ) );
        c.insert( QStringLiteral( "text.caret" ), QColor( QStringLiteral( "#1A1A1A" ) ) );
        c.insert( QStringLiteral( "text.marginBackground" ), QColor( QStringLiteral( "#F3F3F3" ) ) );
        c.insert( QStringLiteral( "text.marginForeground" ), QColor( QStringLiteral( "#6E6E6E" ) ) );
        c.insert( QStringLiteral( "text.indentGuide" ), QColor( QStringLiteral( "#D0D0D0" ) ) );
        c.insert( QStringLiteral( "text.foldMarker" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.braceMatch" ), QColor( QStringLiteral( "#0078D4" ) ) );
        c.insert( QStringLiteral( "text.braceMismatch" ), QColor( QStringLiteral( "#D13438" ) ) );
        c.insert( QStringLiteral( "text.lexer.comment" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.lexer.number" ), QColor( QStringLiteral( "#7C4DFF" ) ) );
        c.insert( QStringLiteral( "text.lexer.keyword" ), QColor( QStringLiteral( "#D0005F" ) ) );
        c.insert( QStringLiteral( "text.lexer.type" ), QColor( QStringLiteral( "#0078A8" ) ) );
        c.insert( QStringLiteral( "text.lexer.string" ), QColor( QStringLiteral( "#8A6D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.preprocessor" ), QColor( QStringLiteral( "#4E8A00" ) ) );
        c.insert( QStringLiteral( "text.lexer.operator" ), QColor( QStringLiteral( "#D0005F" ) ) );
        c.insert( QStringLiteral( "text.lexer.identifier" ), QColor( QStringLiteral( "#272822" ) ) );
        c.insert( QStringLiteral( "text.lexer.function" ), QColor( QStringLiteral( "#3B7D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.variable" ), QColor( QStringLiteral( "#B85C00" ) ) );
    }

    const QStringList lexerKeys = {
        QStringLiteral( "cpp" ), QStringLiteral( "python" ), QStringLiteral( "json" ),
        QStringLiteral( "xml" ), QStringLiteral( "hypertext" ), QStringLiteral( "css" ),
        QStringLiteral( "bash" ), QStringLiteral( "sql" )
    };
    const QStringList tokenKeys = {
        QStringLiteral( "comment" ), QStringLiteral( "number" ), QStringLiteral( "keyword" ),
        QStringLiteral( "type" ), QStringLiteral( "string" ), QStringLiteral( "preprocessor" ),
        QStringLiteral( "operator" ), QStringLiteral( "identifier" ), QStringLiteral( "function" ),
        QStringLiteral( "variable" )
    };
    for( const QString& lexerKey : lexerKeys )
    {
        for( const QString& tokenKey : tokenKeys )
        {
            c.insert( QStringLiteral( "text.lexer.%1.%2" ).arg( lexerKey, tokenKey ),
                      c.value( QStringLiteral( "text.lexer.%1" ).arg( tokenKey ) ) );
        }
    }

    // reStructuredText: 토큰 구성이 프로그래밍 언어와 달라 일반 lexer 색을
    // 재활용하지 않고 별도로 지정한다.
    if( theme == Dark )
    {
        c.insert( QStringLiteral( "text.lexer.rst.title" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.transition" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.comment" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.explicitMarkup" ), QColor( QStringLiteral( "#9A9581" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveValid" ), QColor( QStringLiteral( "#A6E22E" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveInvalid" ), QColor( QStringLiteral( "#F92672" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveUnknown" ), QColor( QStringLiteral( "#AE81FF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleValid" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleInvalid" ), QColor( QStringLiteral( "#F92672" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleUnknown" ), QColor( QStringLiteral( "#AE81FF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.literal" ), QColor( QStringLiteral( "#E6DB74" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.inlineLiteral" ), QColor( QStringLiteral( "#E6DB74" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.emphasis" ), QColor( QStringLiteral( "#D8CFA0" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.strong" ), QColor( QStringLiteral( "#FD971F" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.interpreted" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.hyperlink" ), QColor( QStringLiteral( "#66D9EF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.substitution" ), QColor( QStringLiteral( "#A6E22E" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.fieldName" ), QColor( QStringLiteral( "#F92672" ) ) );
    }
    else
    {
        c.insert( QStringLiteral( "text.lexer.rst.title" ), QColor( QStringLiteral( "#0078A8" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.transition" ), QColor( QStringLiteral( "#8C8778" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.comment" ), QColor( QStringLiteral( "#75715E" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.explicitMarkup" ), QColor( QStringLiteral( "#8C8778" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveValid" ), QColor( QStringLiteral( "#3B7D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveInvalid" ), QColor( QStringLiteral( "#D0005F" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.directiveUnknown" ), QColor( QStringLiteral( "#7C4DFF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleValid" ), QColor( QStringLiteral( "#0078A8" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleInvalid" ), QColor( QStringLiteral( "#D0005F" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.roleUnknown" ), QColor( QStringLiteral( "#7C4DFF" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.literal" ), QColor( QStringLiteral( "#8A6D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.inlineLiteral" ), QColor( QStringLiteral( "#8A6D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.emphasis" ), QColor( QStringLiteral( "#6B5A00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.strong" ), QColor( QStringLiteral( "#B85C00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.interpreted" ), QColor( QStringLiteral( "#0078A8" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.hyperlink" ), QColor( QStringLiteral( "#0057B8" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.substitution" ), QColor( QStringLiteral( "#3B7D00" ) ) );
        c.insert( QStringLiteral( "text.lexer.rst.fieldName" ), QColor( QStringLiteral( "#D0005F" ) ) );
    }

    return c;
}

QString ThemeManager::themeName( Theme theme )
{
    return theme == Dark ? QStringLiteral( "Windows 11 Dark" ) : QStringLiteral( "Windows 11 Light" );
}

bool ThemeManager::importThemeFile( const QString& filePath, QString* errorMessage )
{
    QFile file( filePath );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        if( errorMessage ) *errorMessage = tr( "테마 파일을 열 수 없습니다: %1" ).arg( filePath );
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );
    if( !document.isObject() )
    {
        if( errorMessage ) *errorMessage = tr( "올바른 JSON 테마 파일이 아닙니다." );
        return false;
    }

    const QJsonObject root = document.object();
    const QString     mode = root.value( QStringLiteral( "mode" ) ).toString( QStringLiteral( "light" ) ).toLower();
    const Theme       importedTheme = mode == QStringLiteral( "dark" ) ? Dark : Light;
    const QJsonObject colors = root.value( QStringLiteral( "colors" ) ).toObject();
    if( colors.isEmpty() )
    {
        if( errorMessage ) *errorMessage = tr( "테마 파일에 colors 객체가 없습니다." );
        return false;
    }

    if( importedTheme == Dark )
        m_darkOverrides = jsonToColorMap( colors );
    else
        m_lightOverrides = jsonToColorMap( colors );
    m_theme = importedTheme;
    saveOverrides();
    applyToApplication();
    emit themeChanged( m_theme );
    return true;
}

bool ThemeManager::exportThemeFile( const QString& filePath, QString* errorMessage ) const
{
    QFile file( filePath );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( errorMessage ) *errorMessage = tr( "테마 파일을 저장할 수 없습니다: %1" ).arg( filePath );
        return false;
    }

    QJsonObject root;
    root.insert( QStringLiteral( "schema" ), QStringLiteral( "multiviewer.theme.v1" ) );
    root.insert( QStringLiteral( "name" ), themeName( m_theme ) );
    root.insert( QStringLiteral( "mode" ), m_theme == Dark ? QStringLiteral( "dark" ) : QStringLiteral( "light" ) );
    root.insert( QStringLiteral( "colors" ), colorMapToJson( effectiveColors( m_theme ) ) );
    file.write( QJsonDocument( root ).toJson( QJsonDocument::Indented ) );
    return true;
}

QColor ThemeManager::backgroundColor() const { return color( QStringLiteral( "common.background" ) ); }
QColor ThemeManager::foregroundColor() const { return color( QStringLiteral( "common.foreground" ) ); }
QColor ThemeManager::toolBarColor() const { return color( QStringLiteral( "common.toolbar" ) ); }
QColor ThemeManager::tabActiveColor() const { return color( QStringLiteral( "common.tabActive" ) ); }
QColor ThemeManager::tabInactiveColor() const { return color( QStringLiteral( "common.tabInactive" ) ); }
QColor ThemeManager::canvasAreaColor() const { return color( QStringLiteral( "common.canvas" ) ); }

void ThemeManager::applyToApplication()
{
    if( !qApp )
        return;

    // Qlementine QStyle 이 켜져 있으면 전역 스타일시트를 절대 걸지 않는다.
    // 이유가 두 가지인데, 두 번째가 치명적이다.
    //
    // 1) 아래 스타일시트는 `QWidget { background-color: ... }` 로 시작해서 QStyle 이
    //    그린 결과를 거의 전부 덮어쓴다. 그러면 스타일을 적용한 의미가 없다.
    //
    // 2) qApp 에 스타일시트가 있으면 Qt 는 모든 위젯을 QStyleSheetStyle 로 감싸고
    //    **생성 도중에** polish() 를 호출한다. QComboBox 의 경우 그 시점이
    //    QComboBoxPrivate::viewContainer() 안, `container` 변수에 대입되기 전이다.
    //    거기서 QlementineStyle::polish() 가 ComboboxItemViewFilter 를 콤보박스에
    //    설치하고, 그 필터는 ChildAdded 를 받으면 QComboBox::view() 를 부른다.
    //    view() → viewContainer() → container 는 아직 null → 컨테이너를 또 만들고
    //    → ChildAdded → ... 무한 재귀로 스택 오버플로(0xC00000FD)가 난다.
    //    실제로 .rst 파일을 열 때 QTextView 도구모음의 콤보박스에서 터졌다.
    //    (qlementine v1.4.2 ComboboxItemViewFilter.hpp:49 의 상류 재진입 버그)
    //
    // 예전에 남겨 두려 했던 QScrollArea#canvasScrollArea 규칙은 이 포팅에 해당
    // 위젯이 없어서 어차피 아무것도 매칭하지 않았다. 나중에 PDF/이미지 뷰를
    // 옮겨 올 때 캔버스 배경은 스타일시트가 아니라 QPalette 로 지정해야 한다.
    if( QlementineTheme::isActive() )
    {
        qApp->setStyleSheet( QString() );
        return;
    }

    const auto bg      = backgroundColor().name();
    const auto fg      = foregroundColor().name();
    const auto tb      = toolBarColor().name();
    const auto ta      = tabActiveColor().name();
    const auto ti      = tabInactiveColor().name();
    const auto cv      = canvasAreaColor().name();
    const auto border  = color( QStringLiteral( "common.border" ) ).name();
    const auto surface = color( QStringLiteral( "common.surface" ) ).name();
    const auto accent  = color( QStringLiteral( "common.accent" ) ).name();

    const QString ss = QStringLiteral(
                                      "QWidget { background-color: %1; color: %2; }"
                                      "QFrame, QGroupBox { border-color: %7; }"
                                      "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QTableWidget, QListWidget, QTreeWidget { background-color: %8; color: %2; border: 1px solid %7; }"
                                      "QPushButton { background-color: %8; color: %2; border: 1px solid %7; padding: 4px 10px; border-radius: 4px; }"
                                      "QPushButton:hover { border-color: %9; }"
                                      "QScrollArea#canvasScrollArea, "
                                      "QScrollArea#canvasScrollArea > QWidget#qt_scrollarea_viewport, "
                                      "QScrollArea#canvasScrollArea > QWidget#qt_scrollarea_viewport > QWidget { background-color: %6; }"
                                      "QToolBar { background-color: %3; border: none; spacing: 4px; }"
                                      "QTabBar::tab { background: %5; color: %2; padding: 6px 14px; margin-right: 2px; }"
                                      "QTabBar::tab:selected { background: %4; }"
                                      "QMenuBar { background-color: %3; color: %2; }"
                                      "QMenuBar::item:selected { background-color: %4; }"
                                      "QStatusBar { background-color: %3; color: %2; }"
                                      "QScrollBar { background-color: %3; }"
                                     ).arg( bg, fg, tb, ta, ti, cv, border, surface, accent );

    qApp->setStyleSheet( ss );
}

void ThemeManager::loadOverrides()
{
    AppSettings settings;
    const int schemaVersion = settings.value( QString::fromLatin1( kThemeOverridesSchemaVersionKey ), 1 ).toInt();
    const QJsonDocument document =
            QJsonDocument::fromJson( settings.value( QString::fromLatin1( kThemeOverridesKey ) ).toByteArray() );
    if( !document.isObject() )
        return;

    const QJsonObject root = document.object();
    m_lightOverrides       = jsonToColorMap( root.value( QStringLiteral( "light" ) ).toObject() );
    m_darkOverrides        = jsonToColorMap( root.value( QStringLiteral( "dark" ) ).toObject() );

    if( schemaVersion < kThemeOverridesSchemaVersion )
    {
        const auto legacyMonokaiDefaults = defaultColors( Dark );
        const auto keys                  = m_lightOverrides.keys();
        for( const QString& key : keys )
        {
            if( !key.startsWith( QStringLiteral( "text." ) ) )
                continue;
            const QColor overrideColor = m_lightOverrides.value( key );
            const QColor legacyColor   = legacyMonokaiDefaults.value( key );
            if( overrideColor.isValid() && legacyColor.isValid()
                && overrideColor.name( QColor::HexArgb ).compare( legacyColor.name( QColor::HexArgb ),
                                                                  Qt::CaseInsensitive ) == 0 )
            {
                m_lightOverrides.remove( key );
            }
        }
        if( schemaVersion < 3 )
        {
            const auto removeLegacySelectionOverride = []( QHash< QString, QColor >& overrides ) {
                const QColor  selection     = overrides.value( QStringLiteral( "text.selection" ) );
                const QString selectionName = selection.name( QColor::HexArgb ).toLower();
                if( selectionName == QStringLiteral( "#ff49483e" )
                    || selectionName == QStringLiteral( "#ffd7e8ff" )
                    || selectionName == QStringLiteral( "#ff0078d4" )
                    || selectionName == QStringLiteral( "#ff264f78" )
                    || selectionName == QStringLiteral( "#49483e" )
                    || selectionName == QStringLiteral( "#d7e8ff" ) )
                {
                    overrides.remove( QStringLiteral( "text.selection" ) );
                }
                if( !overrides.contains( QStringLiteral( "text.selectionForeground" ) ) )
                    overrides.remove( QStringLiteral( "text.selectionForeground" ) );
            };
            removeLegacySelectionOverride( m_lightOverrides );
            removeLegacySelectionOverride( m_darkOverrides );
        }
        saveOverrides();
    }
}

void ThemeManager::saveOverrides() const
{
    QJsonObject root;
    root.insert( QStringLiteral( "light" ), colorMapToJson( m_lightOverrides ) );
    root.insert( QStringLiteral( "dark" ), colorMapToJson( m_darkOverrides ) );
    AppSettings settings;
    settings.setValue( QString::fromLatin1( kThemeOverridesKey ),
                       QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
    settings.setValue( QString::fromLatin1( kThemeOverridesSchemaVersionKey ), kThemeOverridesSchemaVersion );
}

