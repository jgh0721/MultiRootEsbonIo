#include "stdafx.h"
#include "markdownEdit.hpp"

MarkdownEdit::MarkdownEdit( QWidget* Parent )
    : BaseEdit( Parent )
{
}

MarkdownEdit::~MarkdownEdit() = default;

QString MarkdownEdit::EditorType() const
{
    return QStringLiteral( "MarkdownEdit" );
}

