#pragma once

#include "baseEdit.hpp"

class MarkdownEdit : public BaseEdit
{
    Q_OBJECT
public:
    explicit MarkdownEdit( QWidget* Parent = nullptr );
    ~MarkdownEdit() override;

    QString                             EditorType() const override;
};

