#include "RoiViewerDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>

RoiViewerDialog::RoiViewerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("观察 ROI"));
    resize(800, 400);

    m_labelOriginal = new QLabel(this);
    m_labelOriginal->setScaledContents(true);// 图片自动缩放填满 label 区域
    m_labelOriginal->setMinimumSize(320, 240);
    m_labelOriginal->setAlignment(Qt::AlignCenter);
    m_labelOriginal->setText(QStringLiteral("ROI 原图"));

    m_labelResult = new QLabel(this);
    m_labelResult->setScaledContents(true);
    m_labelResult->setMinimumSize(320, 240);
    m_labelResult->setAlignment(Qt::AlignCenter);
    m_labelResult->setText(QStringLiteral("ROI 结果图"));

    QHBoxLayout* layout = new QHBoxLayout(this);// 水平布局
    layout->addWidget(m_labelOriginal);
    layout->addWidget(m_labelResult);
}

void RoiViewerDialog::setImages(const QImage& original, const QImage& result)
{
    if (!original.isNull())
        m_labelOriginal->setPixmap(QPixmap::fromImage(original));//fromImage()把QImage 转换成 QPixmap
    if (!result.isNull())
        m_labelResult->setPixmap(QPixmap::fromImage(result));
}