#include "video_render.h"
#include <QPainter>
#include <QPaintEvent>

// Internal widget that paints a QImage via paintEvent
class FrameWidget : public QWidget
{
public:
    void setImage(const QImage& img)
    {
        QMutexLocker lock(&mutex_);
        img_ = img;
        update();
    }

    QImage image() const
    {
        QMutexLocker lock(&mutex_);
        return img_;
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        QPainter p(this);
        p.setBrush(Qt::black);
        p.drawRect(rect());

        QMutexLocker lock(&mutex_);
        if (!img_.isNull())
        {
            QImage scaled = img_.scaled(size(), Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
            int x = (width() - scaled.width()) / 2;
            int y = (height() - scaled.height()) / 2;
            p.drawImage(x, y, scaled);
        }
    }

private:
    QImage img_;
    mutable QMutex mutex_;
};

VideoRender::VideoRender(QObject* parent)
    : MediaDevice(parent)
{
}

VideoRender::~VideoRender()
{
    stop();
}

bool VideoRender::start()
{
    if (running_) return true;

    displayWidget_ = new FrameWidget();
    displayWidget_->setMinimumSize(320, 240);
    displayWidget_->setAttribute(Qt::WA_DeleteOnClose, false);

    running_ = true;
    return true;
}

void VideoRender::stop()
{
    delete displayWidget_;
    displayWidget_ = nullptr;
    running_ = false;
}

bool VideoRender::isRunning() const
{
    return running_;
}

QWidget* VideoRender::videoWidget() const
{
    return displayWidget_;
}

void VideoRender::renderFrame(const QImage& frame)
{
    if (displayWidget_)
    {
        qDebug() << "[VideoRender] rendering frame" << frame.size();
        static_cast<FrameWidget*>(displayWidget_)->setImage(frame);
    }
    else
        qDebug() << "[VideoRender] no displayWidget_!";
}
