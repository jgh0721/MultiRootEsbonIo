#include "stdafx.h"
#include "reSTEdit.hpp"

reSTEdit::reSTEdit( QWidget* Parent )
    : BaseEdit( Parent )
{
}

reSTEdit::~reSTEdit() = default;

QString reSTEdit::EditorType() const
{
    return QStringLiteral( "reSTEdit" );
}

