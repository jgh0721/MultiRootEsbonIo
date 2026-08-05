#include "TestRunner.hpp"

#include <QCoreApplication>
#include <QTest>

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
        failures += QTest::qExec( testObject, argc, argv );

    return failures;
}
