#ifndef CALL_WINDOW_H
#define CALL_WINDOW_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QImage>

class VideoRender;

class CallWindow : public QWidget
{
    Q_OBJECT
public:
    explicit CallWindow(const QString& peer, QWidget* parent = nullptr);

    void setRemoteVideoWidget(QWidget* widget);

    void showLocalFrame(const QImage& frame);

signals:
    void hangUp();
    void muteToggled(bool muted);

private:
    void setupUI();

    QLabel* peerLabel_;
    QPushButton* hangUpBtn_;
    QPushButton* muteBtn_;
    QWidget* remoteVideo_;
    QLabel* localPreview_;
    bool muted_ = false;
    QString peer_;
};

#endif
