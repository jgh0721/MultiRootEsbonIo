#include "TestRunner.hpp"

#include <QApplication>
#include <QTest>

#include <cstdio>

namespace mrst::tests {

QList< QObject* >& registry()
{
    static QList< QObject* > instances;
    return instances;
}

}  // namespace mrst::tests

/// 위젯을 실제로 만들어 두드리는 테스트의 진입점.
///
/// `mrst_tests` 는 QCoreApplication 이라 위젯을 만들 수 없다. 그 규칙을 깨지
/// 않으려고 타깃을 따로 둔다 — 순수 로직 스위트는 계속 빠르고 항상 켜져 있고,
/// 여기만 QApplication 값을 치른다.
///
/// 헤드리스로 돌리려면 `-platform offscreen` 을 준다. QWidget::grab() 은 컴포지터를
/// 거치지 않으므로 그 상태에서도 그림을 검사할 수 있다.
int main( int argc, char** argv )
{
    QApplication app( argc, argv );
    // ThemeManager 가 QSettings 를 읽는다. 이름을 주지 않으면 빈 조직/앱으로
    // 엉뚱한 곳을 본다.
    QCoreApplication::setOrganizationName( QStringLiteral( "myHouse" ) );
    QCoreApplication::setApplicationName( QStringLiteral( "MultiRoot reST Editor UiTests" ) );

    int failures = 0;
    for( QObject* testObject : mrst::tests::registry() )
    {
        const int result = QTest::qExec( testObject, argc, argv );
        // Windows 에서 QTest 의 기본 로거는 stdout 이 리다이렉트되면 출력을
        // OutputDebugString 으로 보낸다. ctest 로 돌리면 어느 클래스가 깨졌는지
        // 알 길이 없으므로 여기서 stderr 로 한 줄 남긴다 (tests/main.cpp 와 같다).
        if( result != 0 )
            std::fprintf( stderr, "FAILED: %s (%d)\n", testObject->metaObject()->className(), result );
        failures += result;
    }

    return failures;
}
