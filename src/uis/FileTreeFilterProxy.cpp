#include "stdafx.h"
#include "uis/FileTreeFilterProxy.hpp"

#include <QDir>
#include <QFileSystemModel>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <lm.h>
#endif

namespace mrst {

namespace {

bool looksLikeWildcard( const QString& text )
{
    return text.contains( QLatin1Char( '*' ) ) || text.contains( QLatin1Char( '?' ) );
}

#ifdef Q_OS_WIN
bool isDisconnectedMappedDrive( const QString& driveName )
{
    // NetUseGetInfo 의 오래된 시그니처는 입력 문자열도 LPWSTR 로 받는다.
    // 원본 QString 을 const_cast 하지 않고 호출 전용 가변 버퍼를 만든다.
    QString mutableDriveName = driveName;
    LPBYTE buffer = nullptr;
    const NET_API_STATUS result = ::NetUseGetInfo(
        nullptr, reinterpret_cast< LPWSTR >( mutableDriveName.data() ), 1, &buffer );
    if( result != NERR_Success || buffer == nullptr )
    {
        if( buffer != nullptr )
            ::NetApiBufferFree( buffer );
        return false;
    }

    const auto* useInfo = reinterpret_cast< const USE_INFO_1* >( buffer );
    const bool disconnected = useInfo->ui1_status != USE_OK;
    ::NetApiBufferFree( buffer );
    return disconnected;
}
#endif

}  // namespace

bool isDisconnectedRemoteDrivePath( const QString& path )
{
#ifdef Q_OS_WIN
    // 파일 시스템 API 로 경로를 정규화하지 않는다. 연결이 끊긴 드라이브라면
    // 정규화 자체가 재연결을 기다릴 수 있으므로 문자열에서 드라이브 이름만 뽑는다.
    if( path.size() < 2 || !path.at( 0 ).isLetter() || path.at( 1 ) != QLatin1Char( ':' ) )
        return false;

    return isDisconnectedMappedDrive( path.left( 2 ).toUpper() );
#else
    Q_UNUSED( path );
    return false;
#endif
}

FileTreeFilterProxy::FileTreeFilterProxy( QObject* parent )
    : QSortFilterProxyModel( parent )
{
    // 자손이 하나라도 통과하면 조상도 남긴다. 이것이 없으면 "conf" 를 쳤을 때
    // conf.py 를 담은 폴더까지 사라져 결과가 통째로 보이지 않는다.
    setRecursiveFilteringEnabled( true );
    setDynamicSortFilter( true );

    collator_.setCaseSensitivity( Qt::CaseInsensitive );
    collator_.setNumericMode( true );
}

void FileTreeFilterProxy::setFilterText( const QString& text )
{
    const QString trimmed = text.trimmed();
    if( trimmed == filterText_ )
        return;

    filterText_ = trimmed;
    // 와일드카드는 **전체 일치**다. `*.rst` 가 "a.rst.bak" 까지 잡으면 그것은
    // 부분 일치이지 와일드카드가 아니다.
    wildcard_ = looksLikeWildcard( trimmed )
                    ? QRegularExpression( QRegularExpression::wildcardToRegularExpression( trimmed ),
                                         QRegularExpression::CaseInsensitiveOption )
                    : QRegularExpression{};
    invalidateFilter();
}

void FileTreeFilterProxy::setRootSourceIndex( const QModelIndex& index )
{
    rootSourceIndex_ = index;
    if( isFiltering() )
        invalidateFilter();
}

bool FileTreeFilterProxy::matches( const QModelIndex& sourceIndex ) const
{
    if( !sourceIndex.isValid() )
        return false;

    const QString name = sourceIndex.data( Qt::DisplayRole ).toString();
    if( name.isEmpty() )
        return false;

    return wildcard_.pattern().isEmpty() ? name.contains( filterText_, Qt::CaseInsensitive )
                                         : wildcard_.match( name ).hasMatch();
}

bool FileTreeFilterProxy::isRootOrAncestor( const QModelIndex& sourceIndex ) const
{
    if( !rootSourceIndex_.isValid() || !sourceIndex.isValid() )
        return false;

    for( QModelIndex node = rootSourceIndex_; node.isValid(); node = node.parent() )
    {
        if( node == sourceIndex )
            return true;
    }
    return false;
}

bool FileTreeFilterProxy::isDirectory( const QModelIndex& sourceIndex ) const
{
    const QAbstractItemModel* source = sourceModel();
    if( const auto* fileSystem = qobject_cast< const QFileSystemModel* >( source ) )
    {
        // hasChildren() 은 Windows 에서 QFileInfo::exists() 확인까지 덧붙인다.
        // 원격 드라이브는 앞서 필터링하고, 나머지는 모델의 형식 판정을 그대로 쓴다.
        return fileSystem->isDir( sourceIndex );
    }
    // 단위 테스트의 가상 모델 등 QFileSystemModel 이 아닌 모델도 지원한다.
    return source != nullptr && source->hasChildren( sourceIndex );
}

bool FileTreeFilterProxy::isDisconnectedRemoteDrive( const QModelIndex& sourceIndex ) const
{
#ifdef Q_OS_WIN
    const auto* fileSystem = qobject_cast< const QFileSystemModel* >( sourceModel() );
    if( fileSystem == nullptr || !sourceIndex.isValid() || sourceIndex.parent().isValid() )
        return false;

    // 파일 모델 최상위에 들어오는 "Z:/" 형태의 드라이브 루트만 검사한다.
    // 하위 경로에서 상태 API 를 반복 호출하지 않으며 UNC 워크스페이스도 건드리지 않는다.
    const QString path = QDir::fromNativeSeparators( fileSystem->filePath( sourceIndex ) );
    if( path.size() != 3 || path.at( 1 ) != QLatin1Char( ':' )
        || path.at( 2 ) != QLatin1Char( '/' ) )
    {
        return false;
    }

    // WNetGetConnection() 은 기억된 매핑이면 연결이 끊겨도 성공한다. net use 와 같은
    // 상태값을 주는 NetUseGetInfo() 로 실제 세션 상태를 판정한다.
    return isDisconnectedRemoteDrivePath( path );
#else
    Q_UNUSED( sourceIndex );
    return false;
#endif
}

bool FileTreeFilterProxy::filterAcceptsRow( const int sourceRow, const QModelIndex& sourceParent ) const
{
    const QAbstractItemModel* source = sourceModel();
    if( source == nullptr )
        return true;

    const QModelIndex index = source->index( sourceRow, 0, sourceParent );
    if( !index.isValid() )
        return false;

    // 연결이 끊긴 원격 드라이브는 빈 필터에서도 내보내지 않는다. 여기서 거르면
    // 뷰와 정렬 프록시가 그 루트의 자식/속성을 조회하지 않아 재연결 대기에 빠지지 않는다.
    if( isDisconnectedRemoteDrive( index ) )
        return false;

    if( !isFiltering() )
        return true;

    // 뷰의 뿌리와 그 조상은 무슨 일이 있어도 남긴다.
    //
    // 아무것도 걸리지 않는 문구("zzzz")를 치면 워크스페이스 행 자체가 조건을
    // 통과하지 못해 프록시에서 사라진다. 그러면 QTreeView 의 rootIndex 가
    // 무효해지는데, 그때 뷰는 그것을 **모델의 최상위**로 읽는다 — 트리에 드라이브
    // 전체가 나타나고, 필터를 위해 트리를 훑는 쪽이 그 순간 디스크를 훑기
    // 시작한다. "필터를 쳤더니 멎었다" 의 정체가 이것이었다.
    if( isRootOrAncestor( index ) )
        return true;

    if( matches( index ) )
        return true;

    // 아직 읽지 않은 **디렉터리**는 남긴다.
    //
    // QFileSystemModel 은 게을러서 펼치기 전에는 자식이 모델에 없다. 그것을
    // "걸린 자손이 없다" 로 보고 지우면 그 안을 들여다볼 길이 **영영** 막힌다 —
    // 뷰가 펼쳐야 모델이 읽고, 읽어야 자손을 판정할 수 있는데 지워진 항목은
    // 펼칠 수 없기 때문이다. 읽고 나면 canFetchMore 가 false 가 되어 아래의
    // 평범한 규칙으로 돌아간다. 그래서 이 예외는 스스로 끝난다.
    //
    // ⚠ hasChildren() 검사를 빼면 안 된다. QFileSystemModel::canFetchMore() 는
    //   디렉터리인지 보지 않고 "아직 훑지 않았는가" 만 보므로 **파일에도 true** 다.
    //   그것만 믿으면 예외가 모든 파일을 통과시킨다 — `.md` 로 걸렀는데 .py 와
    //   .txt 가 그대로 남는 것이 정확히 그 증상이었다.
    if( source->hasChildren( index ) && source->canFetchMore( index ) )
        return true;

    // 이름이 맞은 디렉터리 **아래**는 통째로 보여 준다. 폴더 이름을 쳤는데 그
    // 안이 비어 보이면 필터가 고장난 것처럼 읽힌다.
    for( QModelIndex ancestor = sourceParent;
         ancestor.isValid() && ancestor != rootSourceIndex_; ancestor = ancestor.parent() )
    {
        if( matches( ancestor ) )
            return true;
    }

    // 자손 일치는 setRecursiveFilteringEnabled 가 이미 봐 준다.
    return false;
}

bool FileTreeFilterProxy::lessThan( const QModelIndex& left, const QModelIndex& right ) const
{
    // QSortFilterProxyModel 이 동적 행 삽입 중 필터와 정렬을 연달아 수행한다. 연결이
    // 끊긴 루트가 잠깐 비교 대상으로 들어와도 hasChildren()/파일 속성을 묻지 않는다.
    if( isDisconnectedRemoteDrive( left ) || isDisconnectedRemoteDrive( right ) )
    {
        return collator_.compare( left.data( Qt::DisplayRole ).toString(),
                                  right.data( Qt::DisplayRole ).toString() ) < 0;
    }

    // 디렉터리가 언제나 파일 앞이다. 정렬 방향을 뒤집어도 그렇다 — 탐색기에서
    // 폴더가 파일 사이에 섞이면 트리를 훑는 눈이 갈 곳을 잃는다.
    const bool leftIsDir = isDirectory( left );
    if( leftIsDir != isDirectory( right ) )
        return sortOrder() == Qt::AscendingOrder ? leftIsDir : !leftIsDir;

    const QString leftName = left.data( Qt::DisplayRole ).toString();
    const QString rightName = right.data( Qt::DisplayRole ).toString();
    return collator_.compare( leftName, rightName ) < 0;
}

}  // namespace mrst
