#pragma once

#include "baseEdit.hpp"

class reSTEdit : public BaseEdit
{
    Q_OBJECT
public:
    explicit reSTEdit( QWidget* Parent = nullptr );
    ~reSTEdit() override;

    QString                             EditorType() const override;
};

