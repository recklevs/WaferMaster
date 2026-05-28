#pragma once

#include <QObject>

class WaferAlgorithm : public QObject
{
    Q_OBJECT

public:
    explicit WaferAlgorithm(QObject *parent = nullptr);
    ~WaferAlgorithm();
};