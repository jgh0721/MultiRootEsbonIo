#include "stdafx.h"
#include "pythonEdit.hpp"

PythonEdit::PythonEdit( QWidget* Parent )
    : BaseEdit( Parent )
{
}

PythonEdit::~PythonEdit() = default;

QString PythonEdit::EditorType() const
{
    return QStringLiteral( "PythonEdit" );
}

