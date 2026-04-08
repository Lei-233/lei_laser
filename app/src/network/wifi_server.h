#ifndef WIFISERVER_H
#define WIFISERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QList>

class WifiServer : public QObject
{
    Q_OBJECT
public:
    explicit WifiServer(QObject *parent = nullptr);
    bool startServer(quint16 port = 8080);

signals:
    // 将手机解析出的目标文字与字号推给 UI主控执行
    void remotePrintRequested(const QString &text, int size);

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    QTcpServer *m_server;
    QList<QTcpSocket*> m_clients;
};

#endif // WIFISERVER_H
