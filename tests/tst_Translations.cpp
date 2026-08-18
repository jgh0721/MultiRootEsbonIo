#include "TestRunner.hpp"

#include "core/solLanguageManager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTest>
#include <QTranslator>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>

#ifndef MRST_I18N_TS_DIR
#    error "MRST_I18N_TS_DIR 이 정의되지 않았다 (CMakeLists.txt 의 target_compile_definitions)"
#endif

namespace
{
    struct Message
    {
        QString     context;
        QString     source;
        QString     comment;        ///< tr( s, comment ) 의 동음이의 구분자
        /// 복수형(%n) 메시지는 형이 여럿이다. 각 형을 따로 검사해야 한다.
        QStringList translations;
        bool        unfinished = false;

        [[nodiscard]] bool translated() const
        {
            return std::any_of( translations.cbegin(), translations.cend(),
                                []( const QString& t ) { return !t.trimmed().isEmpty(); } );
        }
    };

    /// 실패 메시지에 넣을 사람이 읽을 이름.
    QString describe( const Message& m )
    {
        return m.comment.isEmpty() ? QStringLiteral( "%1 / \"%2\"" ).arg( m.context, m.source )
                                   : QStringLiteral( "%1 / \"%2\" [%3]" ).arg( m.context, m.source, m.comment );
    }

    /// .ts 를 읽는다. Qt 의 TS writer 는 개행을 이스케이프하지 않고 그대로 쓰므로
    /// (qttools/shared/ts.cpp) 텍스트 노드에 \n 이 살아 있다.
    QList< Message > loadTs( const QString& path, QString* error )
    {
        QList< Message > messages;
        QFile            file( path );
        if( !file.open( QIODevice::ReadOnly ) )
        {
            *error = QStringLiteral( "열 수 없음: %1" ).arg( path );
            return messages;
        }

        QXmlStreamReader reader( &file );
        QString          context;
        Message          current;
        bool             inMessage = false;

        while( !reader.atEnd() )
        {
            reader.readNext();
            if( reader.isStartElement() )
            {
                const QStringView name = reader.name();
                if( name == QLatin1String( "name" ) && !inMessage )
                    context = reader.readElementText();
                else if( name == QLatin1String( "message" ) )
                {
                    inMessage = true;
                    current   = Message{};
                    current.context = context;
                }
                else if( inMessage && name == QLatin1String( "source" ) )
                    current.source = reader.readElementText();
                else if( inMessage && name == QLatin1String( "comment" ) )
                    current.comment = reader.readElementText();
                else if( inMessage && name == QLatin1String( "translation" ) )
                {
                    current.unfinished =
                        reader.attributes().value( QLatin1String( "type" ) ) == QLatin1String( "unfinished" );

                    // 복수형이면 <numerusform> 자식이 여럿 들어 있다.
                    // readElementText() 는 자식 요소를 만나면 오류를 내므로 직접 훑는다.
                    QString buffer;
                    bool    sawNumerus = false;
                    while( !reader.atEnd() )
                    {
                        reader.readNext();
                        if( reader.isEndElement() && reader.name() == QLatin1String( "translation" ) )
                            break;
                        if( reader.isStartElement() && reader.name() == QLatin1String( "numerusform" ) )
                        {
                            sawNumerus = true;
                            current.translations << reader.readElementText();
                        }
                        else if( reader.isCharacters() )
                        {
                            buffer += reader.text().toString();
                        }
                    }
                    if( !sawNumerus && !buffer.trimmed().isEmpty() )
                        current.translations << buffer;
                }
            }
            else if( reader.isEndElement() && reader.name() == QLatin1String( "message" ) )
            {
                inMessage = false;
                messages.append( current );
            }
        }

        if( reader.hasError() )
            *error = QStringLiteral( "%1: %2" ).arg( path, reader.errorString() );
        return messages;
    }

    /// qttools 의 validator.cpp 와 같은 규칙 — '%' + 연속 숫자를 인덱스별로 센다.
    QMap< int, int > placeMarkers( const QString& text )
    {
        QMap< int, int > counts;
        for( int i = 0; i < text.size(); ++i )
        {
            if( text.at( i ) != QLatin1Char( '%' ) )
                continue;
            int j = i + 1;
            while( j < text.size() && text.at( j ).isDigit() )
                ++j;
            if( j > i + 1 )
            {
                const int index = QStringView( text ).mid( i + 1, j - i - 1 ).toInt();
                counts[ index ] += 1;
                i = j - 1;
            }
        }
        return counts;
    }

    /// validator.cpp 와 같은 규칙 — '&' 뒤에 '&', 공백, '#' 이 아닌 문자.
    int mnemonicCount( const QString& text )
    {
        int count = 0;
        for( int i = 0; i + 1 < text.size(); ++i )
        {
            if( text.at( i ) != QLatin1Char( '&' ) )
                continue;
            const QChar next = text.at( i + 1 );
            if( next == QLatin1Char( '&' ) )
            {
                ++i;   // "&&" 는 리터럴 앰퍼샌드다
                continue;
            }
            if( !next.isSpace() && next != QLatin1Char( '#' ) )
                ++count;
        }
        return count;
    }

    QString leadingWs( const QString& s )
    {
        int i = 0;
        while( i < s.size() && s.at( i ).isSpace() )
            ++i;
        return s.left( i );
    }

    QString trailingWs( const QString& s )
    {
        int i = s.size();
        while( i > 0 && s.at( i - 1 ).isSpace() )
            --i;
        return s.mid( i );
    }

    bool hasHangul( const QString& s )
    {
        for( const QChar c : s )
        {
            const char16_t u = c.unicode();
            if( ( u >= 0xAC00 && u <= 0xD7A3 ) || ( u >= 0x1100 && u <= 0x11FF )
                || ( u >= 0x3130 && u <= 0x318F ) )
                return true;
        }
        return false;
    }

    const char* const kLanguages[] = { "en", "ja" };
}   // namespace

/// 번역 파이프라인 전체를 지킨다.
///
/// 두 가지를 본다. (1) translations/*.ts 의 번역문이 원문과 기계적으로
/// 아귀가 맞는가 — 플레이스홀더, 니모닉, 앞뒤 공백, 파일 필터 구조. LLM 이
/// 채운 번역이 저장소에 들어오기 전에 여기서 걸러야 한다. (2) CMake 의
/// qt_add_translations 배선이 살아 있는가 — 특히 MERGE_QT_TRANSLATIONS 는
/// 카탈로그를 못 찾아도 경고만 내고 조용히 넘어가므로 회귀 감시가 필요하다.
class TranslationsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ── .ts 무결성 ────────────────────────────────────────
    void placeholdersMatch();
    void surroundingWhitespaceMatches();
    void newlineCountMatches();
    void mnemonicCountMatches();
    void fileDialogFilterStructureKept();
    void noHangulLeftInTranslation();
    void reportUntranslated();

    // ── .qm 파이프라인 ────────────────────────────────────
    void embeddedQmLoads_data();
    void embeddedQmLoads();
    void qtBaseCatalogIsMerged_data();
    void qtBaseCatalogIsMerged();

    // ── 런타임 매핑 ───────────────────────────────────────
    void systemLanguageIsSupported();
    void unknownSavedLanguageFallsBackToSystem();

private:
    /// 번역이 채워진 항목을 전부 돌며 rule 을 적용하고, 어긋난 것을 모아 한 번에
    /// 실패시킨다.
    ///
    /// 데이터 기반(_data)으로 쓰지 않는 이유가 둘 있다. 번역이 아직 하나도 없는
    /// 단계에서는 데이터 표가 비어 QTest 가 "no testdata available" 로 죽고,
    /// 번역을 채운 뒤에는 위반이 한 번에 하나씩만 보여 700개를 고치는 데
    /// 700번을 돌려야 한다.
    void checkAll( const std::function< bool( const Message& ) >& ok,
                   const QString&                                 rule ) const;

    QMap< QString, QList< Message > >    m_byLang;
};

void TranslationsTest::checkAll( const std::function< bool( const Message& ) >& ok,
                                 const QString&                                 rule ) const
{
    QStringList problems;
    for( auto it = m_byLang.constBegin(); it != m_byLang.constEnd(); ++it )
    {
        for( const Message& m : it.value() )
        {
            if( !m.translated() )
                continue;
            if( !ok( m ) )
                problems << QStringLiteral( "  [%1] %2 -> \"%3\"" )
                                .arg( it.key(), describe( m ),
                                      m.translations.join( QLatin1String( " | " ) ) );
        }
    }
    if( !problems.isEmpty() )
        QFAIL( qPrintable( QStringLiteral( "%1 (%2건)\n%3" )
                               .arg( rule )
                               .arg( problems.size() )
                               .arg( problems.join( QLatin1Char( '\n' ) ) ) ) );
}

void TranslationsTest::initTestCase()
{
    const QDir dir{ QStringLiteral( MRST_I18N_TS_DIR ) };
    QVERIFY2( dir.exists(), qPrintable( dir.absolutePath() ) );

    for( const char* lang : kLanguages )
    {
        QString    error;
        const auto path = dir.filePath( QStringLiteral( "mrst_%1.ts" ).arg( QLatin1String( lang ) ) );
        auto       msgs = loadTs( path, &error );
        QVERIFY2( error.isEmpty(), qPrintable( error ) );
        QVERIFY2( !msgs.isEmpty(), qPrintable( QStringLiteral( "메시지가 하나도 없다: %1" ).arg( path ) ) );
        m_byLang.insert( QString::fromLatin1( lang ), msgs );
    }
}

namespace
{
    /// 복수형이면 형이 여럿이다. 모든 형이 규칙을 지켜야 통과다.
    bool everyForm( const Message& m, const std::function< bool( const QString& ) >& ok )
    {
        return std::all_of( m.translations.cbegin(), m.translations.cend(),
                            [&]( const QString& t ) { return t.trimmed().isEmpty() || ok( t ); } );
    }
}   // namespace

void TranslationsTest::placeholdersMatch()
{
    // %1 이 하나 빠지면 뒤따르는 .arg() 가 조용히 아무 일도 하지 않는다.
    // %n 도 같이 본다 — 복수형 메시지에서 빠지면 개수가 사라진다.
    checkAll(
        []( const Message& m ) {
            const bool wantsN = m.source.contains( QLatin1String( "%n" ) );
            return everyForm( m, [&]( const QString& t ) {
                return placeMarkers( t ) == placeMarkers( m.source )
                       && t.contains( QLatin1String( "%n" ) ) == wantsN;
            } );
        },
        QStringLiteral( "플레이스홀더(%1..%9 / %n)가 원문과 다르다" ) );
}

void TranslationsTest::surroundingWhitespaceMatches()
{
    // QSpinBox::setSuffix(" MB") 처럼 앞 공백이 곧 의미인 문자열이 있다.
    checkAll(
        []( const Message& m ) {
            return everyForm( m, [&]( const QString& t ) {
                return leadingWs( t ) == leadingWs( m.source ) && trailingWs( t ) == trailingWs( m.source );
            } );
        },
        QStringLiteral( "앞뒤 공백이 원문과 다르다" ) );
}

void TranslationsTest::newlineCountMatches()
{
    checkAll(
        []( const Message& m ) {
            return everyForm( m, [&]( const QString& t ) {
                return t.count( QLatin1Char( '\n' ) ) == m.source.count( QLatin1Char( '\n' ) );
            } );
        },
        QStringLiteral( "개행 개수가 원문과 다르다" ) );
}

void TranslationsTest::mnemonicCountMatches()
{
    // 원문에 니모닉이 있으면 번역에도 정확히 하나 있어야 한다. 영어는 위치가
    // 바뀌고(&File) 일본어는 괄호형을 유지하지만(ファイル(&F)), 개수는 같다.
    checkAll(
        []( const Message& m ) {
            return everyForm( m, [&]( const QString& t ) {
                return mnemonicCount( t ) == mnemonicCount( m.source );
            } );
        },
        QStringLiteral( "니모닉(&) 개수가 원문과 다르다" ) );
}

void TranslationsTest::fileDialogFilterStructureKept()
{
    // ";;" 는 QFileDialog 필터 구분자다. 하나라도 사라지면 파일이 안 보인다.
    checkAll(
        []( const Message& m ) {
            if( !m.source.contains( QLatin1String( ";;" ) ) )
                return true;
            return everyForm( m, [&]( const QString& t ) {
                return t.count( QLatin1String( ";;" ) ) == m.source.count( QLatin1String( ";;" ) )
                       && t.count( QLatin1Char( '*' ) ) == m.source.count( QLatin1Char( '*' ) );
            } );
        },
        QStringLiteral( "파일 대화상자 필터 구조(';;' / 글롭)가 깨졌다" ) );
}

void TranslationsTest::noHangulLeftInTranslation()
{
    // 가장 값싼 누락 탐지다. 원문이 한국어이므로 번역문에 한글이 남아 있으면
    // LLM 이 그 항목을 흘렸거나 일부만 옮긴 것이다.
    checkAll( []( const Message& m ) { return everyForm( m, []( const QString& t ) { return !hasHangul( t ); } ); },
              QStringLiteral( "번역문에 한글이 남아 있다" ) );
}

void TranslationsTest::reportUntranslated()
{
    // 게이트가 아니라 계기판이다. 번역이 진행되는 동안 남은 양을 보여 준다.
    for( auto it = m_byLang.constBegin(); it != m_byLang.constEnd(); ++it )
    {
        int untranslated = 0;
        for( const Message& m : it.value() )
            if( !m.translated() )
                ++untranslated;
        qInfo( "%s: %d / %d 미번역", qPrintable( it.key() ), untranslated,
               static_cast< int >( it.value().size() ) );

        // 개수 자체는 게이트로 삼지 않는다 (문자열을 추가한 커밋이 번역 전에
        // 빌드를 막으면 곤란하다). 다만 절반 넘게 비어 있다면 그건 진행 상황이
        // 아니라 파이프라인이 깨진 것이다.
        QVERIFY2( untranslated * 2 < it.value().size(),
                  qPrintable( QStringLiteral( "%1 번역이 절반 이상 비어 있다" ).arg( it.key() ) ) );
    }
}

void TranslationsTest::embeddedQmLoads_data()
{
    QTest::addColumn< QString >( "lang" );
    // ko 도 임베드된다 — 우리 문자열은 비어 있지만 qtbase_ko 카탈로그를 실어 나른다.
    for( const char* lang : { "ko", "en", "ja" } )
        QTest::newRow( lang ) << QString::fromLatin1( lang );
}

void TranslationsTest::embeddedQmLoads()
{
    QFETCH( QString, lang );

    QTranslator translator;
    const QString path = QStringLiteral( ":/i18n/mrst_%1.qm" ).arg( lang );
    QVERIFY2( QFile::exists( path ), qPrintable( path ) );
    QVERIFY2( translator.load( path ), qPrintable( path ) );
}

void TranslationsTest::qtBaseCatalogIsMerged_data()
{
    QTest::addColumn< QString >( "lang" );
    // en 은 제외한다. qtbase_en.qm 은 33바이트짜리 빈 카탈로그다 — Qt 의 원문이
    // 이미 영어라 옮길 것이 없다.
    for( const char* lang : { "ko", "ja" } )
        QTest::newRow( lang ) << QString::fromLatin1( lang );
}

void TranslationsTest::qtBaseCatalogIsMerged()
{
    QFETCH( QString, lang );

    QTranslator translator;
    QVERIFY( translator.load( QStringLiteral( ":/i18n/mrst_%1.qm" ).arg( lang ) ) );

    // CMakeLists.txt 의 MERGE_QT_TRANSLATIONS 가 빠지거나 카탈로그를 못 찾으면
    // lrelease 는 경고만 내고 그냥 넘어간다. 여기서만 드러난다.
    // (표준 대화상자의 취소 버튼 — QPlatformTheme 컨텍스트.)
    QVERIFY2( !translator.translate( "QPlatformTheme", "Cancel" ).isEmpty(),
              "qtbase 카탈로그가 .qm 에 병합되지 않았다" );
}

void TranslationsTest::systemLanguageIsSupported()
{
    const QString code = LanguageManager::systemLanguage();
    QVERIFY2( LanguageManager::isSupported( code ), qPrintable( code ) );
}

void TranslationsTest::unknownSavedLanguageFallsBackToSystem()
{
    // ini 는 사람이 고칠 수 있다. 모르는 값이 적혀 있어도 앱이 멀쩡해야 한다.
    QVERIFY( !LanguageManager::isSupported( QStringLiteral( "kl" ) ) );
    QVERIFY( !LanguageManager::isSupported( QString() ) );
    QVERIFY( !LanguageManager::isSupported( QString::fromLatin1( LanguageManager::kFollowSystem ) ) );

    // availableLanguages() 는 항상 "시스템 따름" 을 맨 앞에 둔다.
    const auto entries = LanguageManager::availableLanguages();
    QCOMPARE( entries.size(), 4 );
    QCOMPARE( entries.first().code, QString::fromLatin1( LanguageManager::kFollowSystem ) );
    QCOMPARE( entries.at( 1 ).code, QStringLiteral( "ko" ) );
    // 언어 이름은 그 언어로 적는다 (번역하지 않는다).
    QCOMPARE( entries.at( 1 ).displayName, QStringLiteral( "한국어" ) );
    QCOMPARE( entries.at( 3 ).displayName, QStringLiteral( "日本語" ) );
}

MRST_REGISTER_TEST( TranslationsTest );

#include "tst_Translations.moc"
