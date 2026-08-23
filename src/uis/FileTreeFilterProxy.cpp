#include "stdafx.h"
#include "uis/FileTreeFilterProxy.hpp"

#include <QRegularExpression>

namespace mrst {

namespace {

bool looksLikeWildcard( const QString& text )
{
    return text.contains( QLatin1Char( '*' ) ) || text.contains( QLatin1Char( '?' ) );
}

}  // namespace

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

bool FileTreeFilterProxy::isDirectory( const QModelIndex& sourceIndex ) const
{
    // QFileSystemModel::hasChildren() 은 그대로 isDir() 이다 — 빈 디렉터리에도
    // true 를 돌려준다. 그래서 qobject_cast 로 그 클래스를 못 박지 않아도 되고,
    // 덕분에 이 규칙을 파일 시스템 없이 검증할 수 있다.
    const QAbstractItemModel* source = sourceModel();
    return source != nullptr && source->hasChildren( sourceIndex );
}

bool FileTreeFilterProxy::filterAcceptsRow( const int sourceRow, const QModelIndex& sourceParent ) const
{
    if( !isFiltering() )
        return true;

    const QAbstractItemModel* source = sourceModel();
    if( source == nullptr )
        return true;

    const QModelIndex index = source->index( sourceRow, 0, sourceParent );
    if( !index.isValid() )
        return false;
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
