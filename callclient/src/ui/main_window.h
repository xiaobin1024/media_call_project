#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>

class CallManager;
class CallWindow;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

signals:
    void loginRequested(const QString& username);
    void callRequested(const QString& target);

public slots:
    void onLoginSuccess();
    void onLoginFailed(const QString& reason);
    void onUserListUpdated(const QStringList& users);
    void onIncomingCall(const QString& from);
    void onCallConnected();
    void onCallDisconnected();

private slots:
    void onLoginClicked();
    void onCallClicked();
    void onLogoutClicked();
    void onCallWindowHangUp();

private:
    void setupUI();

    QWidget* loginWidget_;
    QLineEdit* usernameEdit_;
    QPushButton* loginBtn_;
    QLabel* statusLabel_;

    QWidget* mainWidget_;
    QListWidget* userList_;
    QPushButton* callBtn_;
    QPushButton* logoutBtn_;
    QLabel* selfLabel_;

    CallWindow* callWindow_ = nullptr;
};

#endif
