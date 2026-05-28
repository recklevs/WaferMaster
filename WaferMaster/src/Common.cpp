#include "Common.h"

QString detectionLevelToString(DetectionLevel level)
{
    switch (level)
    {
    case DetectionLevel::Good:
        return QStringLiteral("Good");
    case DetectionLevel::Warning:
        return QStringLiteral("Warning");
    case DetectionLevel::Ng:
        return QStringLiteral("NG");
    default:
        return QStringLiteral("Unknown");
    }
}