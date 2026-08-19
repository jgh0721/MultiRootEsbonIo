#include "TestRunner.hpp"

#include "core/solThemeManager.hpp"

#include <QSet>
#include <QStringList>
#include <QTest>

/// 색상 항목 표와 기본값 표가 어긋나지 않는지 지킨다.
///
/// `ThemeManager::color()` 는 없는 키에 **자홍색**을 돌려준다. 그래서 항목 표에만
/// 키를 넣고 기본값을 빠뜨리면 그 색을 쓰는 화면이 통째로 자홍색이 되고, 설정
/// 대화상자의 "기본값 복원" 도 자홍색을 복원한다. 눈에는 잘 보이지만 **어느 테마의
/// 어느 키를 빠뜨렸는지**는 화면만 보고 알 수 없다.
///
/// 그리고 `scopeLabel()` 에 분기를 넣는 것을 잊으면 그 함수가 식별자를 그대로
/// 되돌리는데, 설정 대화상자가 그 값으로 범위를 거르므로 **그 범위 전체가 UI 에서
/// 사라진다.** 색을 다 넣고도 화면에 아무 것도 안 나오는 상태가 된다.
///
/// 두 함정은 범위를 새로 추가할 때마다 되돌아온다. 여기서 한 번 막아 둔다.
class ThemeColorKeysTest : public QObject
{
    Q_OBJECT

private slots:
    void                                everyEditableKeyHasDefaults();
    void                                everyScopeHasLabel();
    void                                noDefaultIsPlaceholderMagenta();
};

void ThemeColorKeysTest::everyEditableKeyHasDefaults()
{
    const QList< ThemeManager::ColorEntry > entries = ThemeManager::editableColorEntries();
    QVERIFY( !entries.isEmpty() );

    QStringList problems;
    for( const ThemeManager::Theme theme : { ThemeManager::Dark, ThemeManager::Light } )
    {
        const QHash< QString, QColor > defaults = ThemeManager::defaultColors( theme );
        const QString themeName = ThemeManager::themeName( theme );
        for( const ThemeManager::ColorEntry& entry : entries )
        {
            if( !defaults.contains( entry.key ) )
                problems << QStringLiteral( "  [%1] %2" ).arg( themeName, entry.key );
        }
    }

    if( !problems.isEmpty() )
        QFAIL( qPrintable( QStringLiteral( "editableColorEntries() 의 키에 defaultColors() 기본값이 "
                                           "없다 — 그 색은 자홍색으로 나온다 (%1건)\n%2" )
                               .arg( problems.size() )
                               .arg( problems.join( QLatin1Char( '\n' ) ) ) ) );
}

void ThemeColorKeysTest::everyScopeHasLabel()
{
    QSet< QString > groupIds;
    for( const ThemeManager::ColorEntry& entry : ThemeManager::editableColorEntries() )
        groupIds.insert( entry.groupId );
    QVERIFY( !groupIds.isEmpty() );

    QStringList problems;
    for( const QString& groupId : groupIds )
    {
        // scopeLabel() 은 모르는 식별자를 그대로 되돌린다. 그것이 곧 "분기를
        // 넣는 것을 잊었다" 는 신호다.
        if( ThemeManager::scopeLabel( groupId ) == groupId )
            problems << QStringLiteral( "  %1" ).arg( groupId );
    }

    if( !problems.isEmpty() )
        QFAIL( qPrintable( QStringLiteral( "scopeLabel() 에 분기가 없는 범위가 있다 — 설정 "
                                           "대화상자에서 그 범위가 통째로 사라진다 (%1건)\n%2" )
                               .arg( problems.size() )
                               .arg( problems.join( QLatin1Char( '\n' ) ) ) ) );
}

void ThemeColorKeysTest::noDefaultIsPlaceholderMagenta()
{
    // 자홍색은 "키가 없다" 는 표시에만 쓰는 색이다. 기본값 표에 그 색이 들어
    // 있으면 없는 키와 구별할 수 없어진다.
    QStringList problems;
    for( const ThemeManager::Theme theme : { ThemeManager::Dark, ThemeManager::Light } )
    {
        const QHash< QString, QColor > defaults = ThemeManager::defaultColors( theme );
        for( auto it = defaults.constBegin(); it != defaults.constEnd(); ++it )
        {
            if( it.value() == QColor( Qt::magenta ) )
                problems << QStringLiteral( "  [%1] %2" ).arg( ThemeManager::themeName( theme ), it.key() );
        }
    }

    if( !problems.isEmpty() )
        QFAIL( qPrintable( QStringLiteral( "기본값이 자홍색인 키가 있다 (%1건)\n%2" )
                               .arg( problems.size() )
                               .arg( problems.join( QLatin1Char( '\n' ) ) ) ) );
}

MRST_REGISTER_TEST( ThemeColorKeysTest );

#include "tst_ThemeColorKeys.moc"
