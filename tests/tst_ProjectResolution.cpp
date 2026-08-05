#include "TestRunner.hpp"

#include "core/solSphinxScanner.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace mrst;

namespace {

/// conf.py 하나를 가진 프로젝트를 만든다.
void writeProject( const QString& directory, const QString& rootDoc )
{
    QDir().mkpath( directory );
    QFile conf( QDir( directory ).filePath( QStringLiteral( "conf.py" ) ) );
    QVERIFY2( conf.open( QIODevice::WriteOnly | QIODevice::Text ), "conf.py 를 쓸 수 없음" );
    conf.write( QStringLiteral( "project = 'x'\nroot_doc = '%1'\n" ).arg( rootDoc ).toUtf8() );
    conf.close();
}

void writeFile( const QString& path, const QByteArray& content )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile file( path );
    QVERIFY2( file.open( QIODevice::WriteOnly ), "파일을 쓸 수 없음" );
    file.write( content );
    file.close();
}

}  // namespace

class TestProjectResolution : public QObject
{
    Q_OBJECT

private slots:
    void resolvesFileInSameDirectory();
    /// Windows 에서 사용자가 대소문자가 다른 경로로 워크스페이스를 열 수 있다
    /// (예: Docs/Source vs docs/source). 그때도 같은 프로젝트로 해석돼야 한다.
    void resolvesDespiteCaseDifference();
    void prefersNearestProject();
    void returnsNullForUnrelatedFile();
    void detectsEmptyHtmlStyle();
};

void TestProjectResolution::resolvesFileInSameDirectory()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs/source" ) );
    writeProject( source, QStringLiteral( "index" ) );
    const QString file = QDir( source ).filePath( QStringLiteral( "client.rst" ) );
    writeFile( file, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 1 ) );

    const SphinxProject* resolved = resolveProjectForFile( toPath( file ), projects );
    QVERIFY2( resolved != nullptr, "같은 디렉터리의 파일이 프로젝트로 해석되지 않음" );
}

void TestProjectResolution::resolvesDespiteCaseDifference()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs/source" ) );
    writeProject( source, QStringLiteral( "index" ) );
    writeFile( QDir( source ).filePath( QStringLiteral( "client.rst" ) ), "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 1 ) );

    // 사용자가 대문자로 입력/선택한 경로. Windows 에서는 같은 파일을 가리킨다.
    const QString upperCased = QDir( temp.path() ).filePath( QStringLiteral( "Docs/Source/client.rst" ) );
    const SphinxProject* resolved = resolveProjectForFile( toPath( upperCased ), projects );

#ifdef Q_OS_WIN
    QVERIFY2( resolved != nullptr,
             "대소문자만 다른 경로가 같은 프로젝트로 해석되지 않음 "
             "(Windows 에서는 같은 파일이다)" );
#else
    Q_UNUSED( resolved );
#endif
}

void TestProjectResolution::prefersNearestProject()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString outer = QDir( temp.path() ).filePath( QStringLiteral( "outer" ) );
    const QString inner = QDir( outer ).filePath( QStringLiteral( "nested" ) );
    writeProject( outer, QStringLiteral( "index" ) );
    writeProject( inner, QStringLiteral( "index" ) );

    const QString file = QDir( inner ).filePath( QStringLiteral( "page.rst" ) );
    writeFile( file, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( outer ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 2 ) );

    const SphinxProject* resolved = resolveProjectForFile( toPath( file ), projects );
    QVERIFY( resolved != nullptr );
    // toQString 은 네이티브 구분자(\), QFileInfo 는 /. 비교 전에 맞춘다.
    QCOMPARE( QDir::fromNativeSeparators( toQString( resolved->rootPath ) ),
             QFileInfo( inner ).absoluteFilePath() );
}

void TestProjectResolution::returnsNullForUnrelatedFile()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs" ) );
    writeProject( source, QStringLiteral( "index" ) );

    const QString outside = QDir( temp.path() ).filePath( QStringLiteral( "elsewhere/orphan.rst" ) );
    writeFile( outside, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QVERIFY( resolveProjectForFile( toPath( outside ), projects ) == nullptr );
}

void TestProjectResolution::detectsEmptyHtmlStyle()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString path = QDir( temp.path() ).filePath( QStringLiteral( "conf.py" ) );
    writeFile( path, "project = 'x'\nhtml_style = ''\n" );
    QVERIFY( confDeclaresEmptyHtmlStyle( toPath( path ) ) );

    writeFile( path, "project = 'x'\nhtml_style = 'custom.css'\n" );
    QVERIFY( !confDeclaresEmptyHtmlStyle( toPath( path ) ) );

    // 마지막 대입이 이긴다.
    writeFile( path, "html_style = ''\nhtml_style = 'later.css'\n" );
    QVERIFY( !confDeclaresEmptyHtmlStyle( toPath( path ) ) );
}

MRST_REGISTER_TEST( TestProjectResolution );

#include "tst_ProjectResolution.moc"
