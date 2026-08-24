#include "TestRunner.hpp"

#include <QCoreApplication>
#include <QTest>

#include <cstdio>

namespace mrst::tests {

QList< QObject* >& registry()
{
    static QList< QObject* > instances;
    return instances;
}

}  // namespace mrst::tests

int main( int argc, char** argv )
{
    QCoreApplication app( argc, argv );

    int failures = 0;
    for( QObject* testObject : mrst::tests::registry() )
    {
        const int result = QTest::qExec( testObject, argc, argv );
        // Windows 에서 QTest 의 기본 로거는 stdout 이 리다이렉트되면 출력을
        // OutputDebugString 으로 보낸다. 셸에서 돌리면 어느 클래스가 깨졌는지
        // 알 길이 없으므로 여기서 stderr 로 한 줄 남긴다.
        if( result != 0 )
            std::fprintf( stderr, "FAILED: %s (%d)\n", testObject->metaObject()->className(), result );
        failures += result;
    }

    return failures;
}
