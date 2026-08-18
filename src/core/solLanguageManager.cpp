#include "stdafx.h"
#include "core/solLanguageManager.hpp"
#include "core/solAppSettings.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>

namespace
{
    /// theme 과 같은 최상위 키다. 그룹(`general/` 등)을 두지 않는 이유는,
    /// 기존 그룹들이 전부 "어느 기능의 설정인가" 로 나뉘어 있기 때문이다.
    /// 언어는 theme 과 같은 범주 — main() 이 위젯 생성 전에 읽고 프로세스 전역
    /// 싱글톤이 소유하는 값 — 이라 같은 자리에 둔다.
    constexpr auto kLanguageKey = "language";

    /// .qm 은 exe 안에 있다. windeployqt 를 --no-translations 로 돌리므로
    /// (cmake/MrstDeployFlags.cmake) 배포본에는 translations/ 폴더 자체가 없고,
    /// QLibraryInfo::TranslationsPath 는 개발 머신의 Qt 설치 경로를 가리킨다.
    constexpr auto kQmPrefix = ":/i18n/";

    /// 지원 언어. 여기에 코드를 넣고 CMakeLists.txt 의
    /// I18N_TRANSLATED_LANGUAGES 에 같은 코드를 넣으면 언어가 하나 늘어난다.
    /// 소스 원어인 ko 도 목록에 있다 — mrst_ko.qm 은 우리 문자열은 비어 있지만
    /// qtbase_ko 카탈로그를 실어 나른다(표준 대화상자의 확인/취소 등).
    const QStringList& supportedCodes()
    {
        static const QStringList codes{
            QStringLiteral( "ko" ),
            QStringLiteral( "en" ),
            QStringLiteral( "ja" ),
        };
        return codes;
    }
} // namespace

LanguageManager& LanguageManager::instance()
{
    static LanguageManager mgr;
    return mgr;
}

bool LanguageManager::isSupported( const QString& code )
{
    return supportedCodes().contains( code );
}

QString LanguageManager::nativeName( const QString& code )
{
    // 각 언어를 **그 언어로** 적는다. 언어를 바꾸려는 사람은 정의상 지금 화면에
    // 뜬 언어를 못 읽는 사람이라, 현재 UI 언어로 번역해 두면 정작 그 사용자가
    // 자기 언어를 찾지 못한다. 그래서 tr() 로 감싸지 않는다 — 감싸면 번역자가
    // "日本語" 를 "일본어" 로 바꿔 버릴 수 있고 그 순간 성질이 깨진다.
    // (Windows / Chrome / VS Code 가 모두 같은 규칙을 쓴다.)
    if( code == QStringLiteral( "ko" ) ) return QStringLiteral( "한국어" );
    if( code == QStringLiteral( "en" ) ) return QStringLiteral( "English" );
    if( code == QStringLiteral( "ja" ) ) return QStringLiteral( "日本語" );
    return code;
}

QString LanguageManager::systemLanguage()
{
    // uiLanguages() 를 쓴다. QLocale::system().name() 은 Windows 의
    // "국가 또는 지역 > 형식"(GetUserDefaultLocaleName) 에서 오는 값이라
    // **표시 언어와 다를 수 있다** — 한국에서 영어판 Windows 를 쓰면 형식은
    // ko-KR 인데 메뉴는 영어로 나온다. Qt 는 uiLanguages() 를 물을 때만
    // GetUserPreferredUILanguages() 를 보므로, "메뉴가 무슨 말로 나오는가" 는
    // 이쪽에만 들어 있다.
    //
    // 목록을 앞에서부터 훑어 우리가 줄 수 있는 첫 언어를 고른다. 영어도 후보에
    // 넣어야 [fr, en, ko] 같은 목록에서 ko 가 아니라 en 이 잡힌다.
    const QStringList preferred = QLocale::system().uiLanguages();
    for( const QString& tag : preferred )
    {
        // "ja-JP", "en-Latn-US" 처럼 하위 태그가 붙어 오므로 문자열을 직접
        // 비교하지 않고 QLocale 로 파싱해 언어만 본다.
        switch( QLocale( tag ).language() )
        {
            case QLocale::Korean:   return QStringLiteral( "ko" );
            case QLocale::Japanese: return QStringLiteral( "ja" );
            case QLocale::English:  return QStringLiteral( "en" );
            default:                break;
        }
    }
    return QStringLiteral( "en" );
}

QString LanguageManager::savedLanguage()
{
    AppSettings   settings;
    const QString stored =
        settings.value( QString::fromLatin1( kLanguageKey ), QString::fromLatin1( kFollowSystem ) ).toString();
    // 설정 파일은 사람이 고칠 수 있다. 모르는 값이 적혀 있으면 시스템을 따른다.
    return isSupported( stored ) ? stored : QString::fromLatin1( kFollowSystem );
}

QString LanguageManager::selectedLanguage() const
{
    return savedLanguage();
}

QString LanguageManager::effectiveLanguage() const
{
    return m_effective;
}

QList< LanguageManager::Entry > LanguageManager::availableLanguages()
{
    QList< Entry > entries;
    // "시스템 설정 따름" 은 언어 이름이 아니라 동작에 대한 문장이라 번역한다.
    // 괄호에 실제 해석 결과를 보여 주면 고르기 전에 무엇이 될지 알 수 있다.
    entries.append( { QString::fromLatin1( kFollowSystem ),
                      tr( "시스템 설정 따름 (%1)" ).arg( nativeName( systemLanguage() ) ) } );
    for( const QString& code : supportedCodes() )
        entries.append( { code, nativeName( code ) } );
    return entries;
}

void LanguageManager::applyTranslator( const QString& code )
{
    auto* app = QCoreApplication::instance();
    if( app == nullptr )
        return;

    // 먼저 뗀다. removeTranslator() 는 비어 있지 않은 번역기를 뗄 때
    // LanguageChange 를 내보내고, installTranslator() 도 마찬가지다. Qt 는 같은
    // 수신자에 쌓인 LanguageChange 를 하나로 합치므로 실제 재번역은 한 번만
    // 돈다.
    app->removeTranslator( &m_translator );

    // load() 가 실패하면 붙이지 않는다. 비어 있는 번역기를 걸어 두면 설치
    // 목록과 실제 상태가 어긋나 나중에 원인을 찾기 어렵다.
    if( m_translator.load( QStringLiteral( "mrst_%1" ).arg( code ), QLatin1String( kQmPrefix ) ) )
        app->installTranslator( &m_translator );
    else
        qWarning( "번역 파일을 읽지 못했습니다: %s%s.qm", kQmPrefix, qPrintable( code ) );
}

void LanguageManager::installAtStartup()
{
    LanguageManager& mgr   = instance();
    const QString    saved = savedLanguage();

    mgr.m_effective = ( saved == QLatin1String( kFollowSystem ) ) ? systemLanguage() : saved;
    mgr.applyTranslator( mgr.m_effective );
    // languageChanged 는 내보내지 않는다. 아직 구독자도 위젯도 없다.
}

void LanguageManager::setLanguage( const QString& code )
{
    const QString normalized = isSupported( code ) ? code : QString::fromLatin1( kFollowSystem );

    // 값이 같아도 저장까지 건너뛰지는 않는다 (ThemeManager::setTheme 과 같은
    // 이유). 설정 파일에 키가 아직 없을 수 있는데, "지금 기본값과 같으니 안
    // 써도 된다" 고 두면 나중에 기본값이 바뀌었을 때 사용자가 고른 적 없는
    // 언어로 조용히 바뀐다.
    AppSettings settings;
    settings.setValue( QString::fromLatin1( kLanguageKey ), normalized );

    const QString effective =
        ( normalized == QLatin1String( kFollowSystem ) ) ? systemLanguage() : normalized;
    if( effective == m_effective )
        return;

    m_effective = effective;
    applyTranslator( effective );
    emit languageChanged( effective );
}
