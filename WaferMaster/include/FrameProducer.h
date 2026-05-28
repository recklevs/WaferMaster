#pragma once

#include <QObject>

class FrameProducer : public QObject
{
    Q_OBJECT

public:
    explicit FrameProducer(QObject *parent = nullptr);
    ~FrameProducer();
};