#pragma once
#include "baseEdit.hpp"
class PythonEdit : public BaseEdit
{
    Q_OBJECT
public:
    explicit PythonEdit( QWidget* Parent = nullptr );
    ~PythonEdit() override;
    QString                             EditorType() const override;
};
