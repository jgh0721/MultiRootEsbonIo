#include "TestRunner.hpp"

#include "core/solExternalChangeWatcher.hpp"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace mrst;

namespace {

/// 테스트에서 쓰는 안정화 대기. 기본값(250ms)보다 짧게 잡아 스위트를 빠르게
/// 유지하되, 파일 시스템이 크기/시각을 반영할 시간은 남긴다.
constexpr int kTestSettleMs = 60;

void writeFile( const QString& path, const QByteArray& content )
{
    QFile file( path );
    QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    QCOMPARE( file.write( content ), static_cast< qint64 >( content.size() ) );
    file.close();
}

}  // namespace

/// 외부 파일 편집 인식.
///
/// 여기서 지키는 것은 "바뀌면 알린다" 하나가 아니다. 그것만 지키는 감시자는
/// 쓸 수 없다 — 우리 저장에 반응하고, 다른 편집기의 임시 파일 + rename 저장을
/// 삭제로 오해하고, 반쯤 쓰인 파일을 알린다. 그 셋이 각각 테스트로 있다.
class TestExternalChangeWatch : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ── 지문 ──
    void fingerprintTellsMissingFromPresent();
    void fingerprintChangesWithContent();

    // ── 발견 ──
    void notifyModeReportsExternalWrite();
    void pollModeReportsExternalWrite();
    void oneWriteProducesOneSignal();
    void unwatchedFileIsSilent();
    void ignoreActionStopsWatching();

    // ── 오탐 ──
    void settleIntervalIsRespected();
    void selfWriteIsNotReported();
    void atomicReplaceIsChangeNotDeletion();
    void deletionIsReportedWhenFileStaysGone();

private:
    QString path( const QString& name ) const { return dir_->filePath( name ); }
    /// 감시자를 만들고 dir_ 안의 파일 하나를 지켜본다.
    ExternalChangeWatcher* makeWatcher( ExternalChangeWatcher::Detection detection,
                                      const QStringList& files );

    std::unique_ptr< QTemporaryDir >         dir_;
    std::unique_ptr< ExternalChangeWatcher > watcher_;
};

void TestExternalChangeWatch::init()
{
    dir_ = std::make_unique< QTemporaryDir >();
    QVERIFY( dir_->isValid() );
}

void TestExternalChangeWatch::cleanup()
{
    watcher_.reset();
    dir_.reset();
}

ExternalChangeWatcher* TestExternalChangeWatch::makeWatcher( ExternalChangeWatcher::Detection detection,
                                                           const QStringList& files )
{
    watcher_ = std::make_unique< ExternalChangeWatcher >();
    // 설정 파일이 무엇을 담고 있든 테스트는 자기 상태를 명시한다.
    watcher_->setAction( ExternalChangeWatcher::Action::Reload );
    watcher_->setDetection( detection );
    watcher_->setPollSeconds( ExternalChangeWatcher::minPollSeconds() );
    watcher_->setSettleInterval( kTestSettleMs );
    watcher_->setWatchedFiles( files );
    return watcher_.get();
}

// ═══════════════════════════════════════════════════════════
// 지문
// ═══════════════════════════════════════════════════════════
void TestExternalChangeWatch::fingerprintTellsMissingFromPresent()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    QVERIFY( !ExternalChangeWatcher::fingerprintOf( file ).exists );

    writeFile( file, QByteArrayLiteral( "hello" ) );
    const auto present = ExternalChangeWatcher::fingerprintOf( file );
    QVERIFY( present.exists );
    QCOMPARE( present.size, 5 );

    QVERIFY( QFile::remove( file ) );
    QVERIFY( !ExternalChangeWatcher::fingerprintOf( file ).exists );

    // 디렉터리는 파일이 아니다. 경로를 잘못 넘겨도 "있다" 고 답하면 안 된다.
    QVERIFY( !ExternalChangeWatcher::fingerprintOf( dir_->path() ).exists );
    QVERIFY( !ExternalChangeWatcher::fingerprintOf( QString() ).exists );
}

void TestExternalChangeWatch::fingerprintChangesWithContent()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "one" ) );
    const auto before = ExternalChangeWatcher::fingerprintOf( file );

    writeFile( file, QByteArrayLiteral( "one two three" ) );
    QVERIFY( !( ExternalChangeWatcher::fingerprintOf( file ) == before ) );
}

// ═══════════════════════════════════════════════════════════
// 발견
// ═══════════════════════════════════════════════════════════
void TestExternalChangeWatch::notifyModeReportsExternalWrite()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );

    writeFile( file, QByteArrayLiteral( "after the change" ) );
    QVERIFY( changed.wait( 5000 ) );
    QCOMPARE( changed.first().first().toString(), QDir::cleanPath( file ) );
}

void TestExternalChangeWatch::pollModeReportsExternalWrite()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    // 폴링은 OS 알림을 아예 걸지 않는다. 발견의 유일한 경로가 타이머다.
    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Poll, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );

    writeFile( file, QByteArrayLiteral( "after the change" ) );
    QVERIFY( changed.wait( 5000 ) );
}

void TestExternalChangeWatch::oneWriteProducesOneSignal()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );

    writeFile( file, QByteArrayLiteral( "after the change" ) );
    QVERIFY( changed.wait( 5000 ) );

    // 알림 한 번을 여러 신호로 부풀리지 않는다. 자동 불러오기에서 그것은
    // 파일을 두 번 읽고 캐럿을 두 번 되돌린다는 뜻이다.
    QTest::qWait( kTestSettleMs * 6 );
    QCOMPARE( changed.count(), 1 );
}

void TestExternalChangeWatch::unwatchedFileIsSilent()
{
    const QString watched = path( QStringLiteral( "a.rst" ) );
    const QString other = path( QStringLiteral( "b.rst" ) );
    writeFile( watched, QByteArrayLiteral( "a" ) );
    writeFile( other, QByteArrayLiteral( "b" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { watched } );
    QCOMPARE( watcher->watchedFiles(), QStringList{ QDir::cleanPath( watched ) } );

    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );
    writeFile( other, QByteArrayLiteral( "b changed a lot" ) );
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );

    // 목록에서 빠진 파일도 조용해야 한다 — 탭을 닫은 뒤 그 파일이 바뀌는 것은
    // 우리 일이 아니다.
    watcher->setWatchedFiles( {} );
    QVERIFY( watcher->watchedFiles().isEmpty() );
    writeFile( watched, QByteArrayLiteral( "a changed a lot" ) );
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );
}

void TestExternalChangeWatch::ignoreActionStopsWatching()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    watcher->setAction( ExternalChangeWatcher::Action::Ignore );
    QVERIFY( !watcher->isEnabled() );

    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );
    writeFile( file, QByteArrayLiteral( "after the change" ) );
    watcher->recheckAll();
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );

    // 다시 켜면 그때부터를 기준으로 삼는다. 꺼 둔 동안의 변경을 몰아서 알리면
    // 설정을 켠 순간 대화상자가 줄줄이 뜬다.
    watcher->setAction( ExternalChangeWatcher::Action::Reload );
    watcher->recheckAll();
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );

    writeFile( file, QByteArrayLiteral( "and one more change here" ) );
    QVERIFY( changed.wait( 5000 ) );
}

// ═══════════════════════════════════════════════════════════
// 오탐
// ═══════════════════════════════════════════════════════════
void TestExternalChangeWatch::settleIntervalIsRespected()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    watcher->setSettleInterval( 400 );

    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );
    writeFile( file, QByteArrayLiteral( "after the change" ) );

    // 안정화 대기 안에는 알리지 않는다. 이것이 반쯤 쓰인 파일을 불러오지 않게
    // 막아 주는 성질이고, 없으면 큰 파일을 복사해 넣을 때마다 잘린 본문이 뜬다.
    QVERIFY( !changed.wait( 200 ) );
    QVERIFY( changed.wait( 5000 ) );
}

void TestExternalChangeWatch::selfWriteIsNotReported()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );

    watcher->beginSelfWrite( file );
    writeFile( file, QByteArrayLiteral( "saved by us, not by them" ) );
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );

    // 구간을 닫으면 그 내용이 새 기준이 된다. 저장 직후에 "밖에서 바뀌었습니다"
    // 를 띄우는 것만큼 신뢰를 깎는 것이 없다.
    watcher->endSelfWrite( file );
    watcher->recheckAll();
    QVERIFY( !changed.wait( kTestSettleMs * 6 ) );

    // 그 뒤의 남의 편집은 다시 보인다.
    writeFile( file, QByteArrayLiteral( "now somebody else wrote this" ) );
    QVERIFY( changed.wait( 5000 ) );
}

void TestExternalChangeWatch::atomicReplaceIsChangeNotDeletion()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    const QString temp = path( QStringLiteral( "a.rst.tmp" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );
    QSignalSpy vanished( watcher, &ExternalChangeWatcher::sigFileVanished );

    // 다른 편집기 다수(그리고 Qt 의 QSaveFile)가 이렇게 저장한다. 파일이 잠깐
    // 사라지므로, 삭제를 곧바로 믿는 감시자는 여기서 반드시 틀린다.
    writeFile( temp, QByteArrayLiteral( "replaced by another editor" ) );
    QVERIFY( QFile::remove( file ) );
    QVERIFY( QFile::rename( temp, file ) );

    QVERIFY( changed.wait( 5000 ) );
    QCOMPARE( vanished.count(), 0 );

    // rename 으로 파일 감시가 풀렸어도 그다음 편집을 놓치지 않아야 한다.
    writeFile( file, QByteArrayLiteral( "and then edited again, longer" ) );
    QVERIFY( changed.wait( 5000 ) );
}

void TestExternalChangeWatch::deletionIsReportedWhenFileStaysGone()
{
    const QString file = path( QStringLiteral( "a.rst" ) );
    writeFile( file, QByteArrayLiteral( "before" ) );

    ExternalChangeWatcher* watcher = makeWatcher( ExternalChangeWatcher::Detection::Notify, { file } );
    QSignalSpy changed( watcher, &ExternalChangeWatcher::sigFileChanged );
    QSignalSpy vanished( watcher, &ExternalChangeWatcher::sigFileVanished );

    QVERIFY( QFile::remove( file ) );
    QVERIFY( vanished.wait( 5000 ) );
    QCOMPARE( changed.count(), 0 );

    // 되돌아오면 변경으로 잡힌다. 삭제를 알린 뒤 손을 놓아 버리면, 다른 도구가
    // 옮기는 중이던 파일이 제자리로 온 것을 영원히 모른다.
    writeFile( file, QByteArrayLiteral( "it came back" ) );
    QVERIFY( changed.wait( 5000 ) );
}

MRST_REGISTER_TEST( TestExternalChangeWatch );

#include "tst_ExternalChangeWatch.moc"
