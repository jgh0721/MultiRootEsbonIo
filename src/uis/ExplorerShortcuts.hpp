#pragma once

#include <QShortcut>
#include <QTreeView>
#include <functional>

namespace mrst {

// Only the tree owns these keys: an editor or filter field must keep Delete.
inline void installExplorerShortcuts( QTreeView* tree,
                                     const std::function<void()>& rename,
                                     const std::function<void()>& remove )
{
    const auto bind = [tree]( Qt::Key key, const std::function<void()>& callback ) {
        auto* shortcut = new QShortcut( QKeySequence( key ), tree );
        shortcut->setContext( Qt::WidgetShortcut );
        shortcut->setAutoRepeat( false );
        QObject::connect( shortcut, &QShortcut::activated, tree, [tree, callback] {
            if( tree->currentIndex().isValid() )
                callback();
        } );
    };
    bind( Qt::Key_F2, rename );
    bind( Qt::Key_Delete, remove );
}

} // namespace mrst
