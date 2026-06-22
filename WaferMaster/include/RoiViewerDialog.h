#pragma once

#include <QDialog>

class QLabel;
class QImage;

/// @brief 观察 ROI 弹窗，MainWindow.cpp的eventFilter()中首次框选触发创建
class RoiViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RoiViewerDialog(QWidget* parent = nullptr);
    ~RoiViewerDialog() override = default;

    /// @brief MainWindow 框选完成后调用，传入已裁切并转换好的 QImage
    void setImages(const QImage& original, const QImage& result);

private:
    QLabel* m_titleOriginal = nullptr; // ROI 原图 标题
    QLabel* m_titleResult   = nullptr; // ROI 结果图 标题
    QLabel* m_labelOriginal = nullptr; // ROI 原图 图片
    QLabel* m_labelResult   = nullptr; // ROI 平坦图 图片
};