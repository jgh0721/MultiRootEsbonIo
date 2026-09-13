#include "ExternalFilesProxy.hpp"
#include "core/solWorkspaceSession.hpp"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QItemSelection>

namespace mrst {
namespace {
QString normalizedPath( const QString& path )
{
    const QFileInfo info( path );
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
}

bool samePath( const QString& a, const QString& b )
{
#ifdef Q_OS_WIN
    return a.compare( b, Qt::CaseInsensitive ) == 0;
#else
    return a == b;
#endif
}
}

ExternalFilesProxy::ExternalFilesProxy( QObject* parent ) : QIdentityProxyModel( parent )
{
    // Identity's default layout handler would invalidate our synthetic indexes
    // because they intentionally have no corresponding source index.
    setHandleSourceLayoutChanges( false );
}

void ExternalFilesProxy::setSourceModel( QAbstractItemModel* source )
{
    for( const auto& connection : connections_ )
        disconnect( connection );
    connections_.clear();
    QIdentityProxyModel::setSourceModel( source );
    if( !source )
        return;
    connections_ << connect( source, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] {
        emit layoutAboutToBeChanged();
        layoutIndexes_ = persistentIndexList();
        layoutSources_.clear();
        for( const auto& index : layoutIndexes_ )
            layoutSources_.append( mapToSource( index ) );
    } );
    connections_ << connect( source, &QAbstractItemModel::layoutChanged, this, [this] {
        QModelIndexList updated;
        for( int i = 0; i < layoutIndexes_.size(); ++i )
        {
            const auto& old = layoutIndexes_.at( i );
            if( isExternalRoot( old ) )
                updated << externalRoot().siblingAtColumn( old.column() );
            else if( isExternalFile( old ) )
                updated << index( old.row(), old.column(), externalRoot() );
            else
                updated << mapFromSource( layoutSources_.at( i ) );
        }
        changePersistentIndexList( layoutIndexes_, updated );
        layoutIndexes_.clear();
        layoutSources_.clear();
        emit layoutChanged();
    } );
}

void ExternalFilesProxy::setWorkspace( const QString& path, const QModelIndex& root )
{
    beginResetModel();
    workspace_ = path;
    root_ = root;
    files_.clear();
    endResetModel();
}

bool ExternalFilesProxy::addFile( const QString& path )
{
    if( !sourceModel() || path.isEmpty() || isPathInWorkspace( path, workspace_ ) )
        return false;
    const QString normalized = normalizedPath( path );
    for( const auto& existing : files_ )
        if( samePath( existing, normalized ) )
            return false;
    if( files_.isEmpty() )
    {
        const int row = sourceModel()->rowCount( root_ );
        beginInsertRows( mapFromSource( root_ ), row, row );
        files_.append( normalized );
        endInsertRows();
    }
    else
    {
        const int row = static_cast<int>( files_.size() );
        beginInsertRows( externalRoot(), row, row );
        files_.append( normalized );
        endInsertRows();
    }
    return true;
}

void ExternalFilesProxy::removeFile( const QString& path )
{
    const QModelIndex item = indexForFile( path );
    if( !item.isValid() )
        return;
    if( files_.size() == 1 )
    {
        const int row = externalRoot().row();
        beginRemoveRows( mapFromSource( root_ ), row, row );
        files_.clear();
        endRemoveRows();
    }
    else
    {
        beginRemoveRows( externalRoot(), item.row(), item.row() );
        files_.removeAt( item.row() );
        endRemoveRows();
    }
}

bool ExternalFilesProxy::isExternalRoot( const QModelIndex& index ) const
{
    return index.isValid() && index.model() == this && index.internalPointer() == &rootTag_;
}

bool ExternalFilesProxy::isExternalFile( const QModelIndex& index ) const
{
    return index.isValid() && index.model() == this && index.internalPointer() == &fileTag_;
}

bool ExternalFilesProxy::atRoot( const QModelIndex& parent ) const
{
    return !isExternalRoot( parent ) && !isExternalFile( parent ) && mapToSource( parent ) == root_;
}

QModelIndex ExternalFilesProxy::externalRoot() const
{
    return files_.isEmpty() || !sourceModel() ? QModelIndex{}
        : createIndex( sourceModel()->rowCount( root_ ), 0, &rootTag_ );
}

QModelIndex ExternalFilesProxy::indexForFile( const QString& path ) const
{
    if( path.isEmpty() )
        return {};
    const QString normalized = normalizedPath( path );
    for( int row = 0; row < files_.size(); ++row )
        if( samePath( files_.at( row ), normalized ) )
            return index( row, 0, externalRoot() );
    return {};
}

QString ExternalFilesProxy::filePath( const QModelIndex& index ) const
{
    return isExternalFile( index ) ? files_.value( index.row() ) : QString{};
}

QModelIndex ExternalFilesProxy::mapToSource( const QModelIndex& index ) const
{
    if( isExternalRoot( index ) || isExternalFile( index ) )
        return {};
    return QIdentityProxyModel::mapToSource( index );
}

QItemSelection ExternalFilesProxy::mapSelectionToSource( const QItemSelection& selection ) const
{
    // A selection can contain both filesystem rows and the synthetic branch.
    return QAbstractProxyModel::mapSelectionToSource( selection );
}

QModelIndex ExternalFilesProxy::buddy( const QModelIndex& index ) const
{
    return isExternalRoot( index ) || isExternalFile( index ) ? index : QIdentityProxyModel::buddy( index );
}

QMap<int, QVariant> ExternalFilesProxy::itemData( const QModelIndex& index ) const
{
    return isExternalRoot( index ) || isExternalFile( index )
        ? QAbstractItemModel::itemData( index ) : QIdentityProxyModel::itemData( index );
}

QModelIndex ExternalFilesProxy::index( int row, int column, const QModelIndex& parent ) const
{
    if( row < 0 || column < 0 || column >= columnCount( parent ) || row >= rowCount( parent ) )
        return {};
    if( isExternalRoot( parent ) )
        return createIndex( row, column, &fileTag_ );
    if( atRoot( parent ) && !files_.isEmpty() && row == sourceModel()->rowCount( root_ ) )
        return createIndex( row, column, &rootTag_ );
    return QIdentityProxyModel::index( row, column, parent );
}

QModelIndex ExternalFilesProxy::parent( const QModelIndex& index ) const
{
    if( isExternalRoot( index ) )
        return mapFromSource( root_ );
    if( isExternalFile( index ) )
        return externalRoot();
    return QIdentityProxyModel::parent( index );
}

QModelIndex ExternalFilesProxy::sibling( int row, int column, const QModelIndex& item ) const
{
    return index( row, column, parent( item ) );
}

int ExternalFilesProxy::rowCount( const QModelIndex& parent ) const
{
    if( parent.column() > 0 || isExternalFile( parent ) )
        return 0;
    if( isExternalRoot( parent ) )
        return static_cast<int>( files_.size() );
    return QIdentityProxyModel::rowCount( parent ) + ( atRoot( parent ) && !files_.isEmpty() ? 1 : 0 );
}

int ExternalFilesProxy::columnCount( const QModelIndex& parent ) const
{
    if( isExternalRoot( parent ) || isExternalFile( parent ) )
        return sourceModel() ? sourceModel()->columnCount( root_ ) : 0;
    return QIdentityProxyModel::columnCount( parent );
}

bool ExternalFilesProxy::hasChildren( const QModelIndex& parent ) const
{
    if( isExternalFile( parent ) || parent.column() > 0 )
        return false;
    return isExternalRoot( parent ) || ( atRoot( parent ) && !files_.isEmpty() )
        || QIdentityProxyModel::hasChildren( parent );
}

bool ExternalFilesProxy::canFetchMore( const QModelIndex& parent ) const
{
    return !isExternalRoot( parent ) && !isExternalFile( parent ) && QIdentityProxyModel::canFetchMore( parent );
}

void ExternalFilesProxy::fetchMore( const QModelIndex& parent )
{
    if( !isExternalRoot( parent ) && !isExternalFile( parent ) )
        QIdentityProxyModel::fetchMore( parent );
}

QVariant ExternalFilesProxy::data( const QModelIndex& index, int role ) const
{
    if( !isExternalRoot( index ) && !isExternalFile( index ) )
        return QIdentityProxyModel::data( index, role );
    if( index.column() != 0 )
        return {};
    if( role == Qt::DisplayRole )
        return isExternalRoot( index ) ? tr( "외부" ) : QFileInfo( filePath( index ) ).fileName();
    if( role == Qt::ToolTipRole )
        return isExternalRoot( index ) ? tr( "워크스페이스 외부 파일" ) : QDir::toNativeSeparators( filePath( index ) );
    if( role == Qt::DecorationRole )
        return QFileIconProvider{}.icon( isExternalRoot( index ) ? QFileIconProvider::Folder : QFileIconProvider::File );
    return {};
}

bool ExternalFilesProxy::setData( const QModelIndex& index, const QVariant& value, int role )
{
    return !isExternalRoot( index ) && !isExternalFile( index ) && QIdentityProxyModel::setData( index, value, role );
}

Qt::ItemFlags ExternalFilesProxy::flags( const QModelIndex& index ) const
{
    if( isExternalRoot( index ) || isExternalFile( index ) )
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return QIdentityProxyModel::flags( index );
}

} // namespace mrst
