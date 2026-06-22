#include "RoiViewerDialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QFont>

RoiViewerDialog::RoiViewerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("观察 ROI"));
    resize(800, 420);

    // --- 左侧：ROI 原图 ---
    m_titleOriginal = new QLabel(QStringLiteral("ROI 原图"), this);
    m_titleOriginal->setAlignment(Qt::AlignCenter);
    QFont boldFont = m_titleOriginal->font();
    boldFont.setBold(true);
    boldFont.setPointSize(10);
    m_titleOriginal->setFont(boldFont);
    m_titleOriginal->setFixedHeight(22);

    m_labelOriginal = new QLabel(this);
    m_labelOriginal->setScaledContents(true);
    m_labelOriginal->setMinimumSize(320, 240);
    m_labelOriginal->setAlignment(Qt::AlignCenter);

    QVBoxLayout* leftLayout = new QVBoxLayout;
    leftLayout->addWidget(m_titleOriginal);
    leftLayout->addWidget(m_labelOriginal);

    // --- 右侧：ROI 结果图 ---
    m_titleResult = new QLabel(QStringLiteral("ROI 结果图"), this);
    m_titleResult->setAlignment(Qt::AlignCenter);
    m_titleResult->setFont(boldFont);
    m_titleResult->setFixedHeight(22);

    m_labelResult = new QLabel(this);
    m_labelResult->setScaledContents(true);
    m_labelResult->setMinimumSize(320, 240);
    m_labelResult->setAlignment(Qt::AlignCenter);

    QVBoxLayout* rightLayout = new QVBoxLayout;
    rightLayout->addWidget(m_titleResult);
    rightLayout->addWidget(m_labelResult);

    // --- 水平组合 ---
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->addLayout(leftLayout);
    layout->addLayout(rightLayout);
}

void RoiViewerDialog::setImages(const QImage& original, const QImage& result)
{
    if (!original.isNull())
        m_labelOriginal->setPixmap(QPixmap::fromImage(original));//fromImage()把QImage 转换成 QPixmap
    if (!result.isNull())
        m_labelResult->setPixmap(QPixmap::fromImage(result));
}