#include "wifi_server.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

WifiServer::WifiServer(QObject *parent) : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &WifiServer::onNewConnection);
}

bool WifiServer::startServer(quint16 port)
{
    // 在全网卡接管 8080 指定端口收发流
    if (!m_server->listen(QHostAddress::Any, port)) {
        qDebug() << "WiFi Server 启动监听失败，端口:" << port;
        return false;
    }
    qDebug() << "WiFi 激光极速云端伺服器已上线，正在监听端口:" << port;
    return true;
}

void WifiServer::onNewConnection()
{
    QTcpSocket *clientSocket = m_server->nextPendingConnection();
    // 有手机或客户端连进来，就在这里收他的字
    connect(clientSocket, &QTcpSocket::readyRead, this, &WifiServer::onReadyRead);
    connect(clientSocket, &QTcpSocket::disconnected, clientSocket, &QTcpSocket::deleteLater);
    m_clients.append(clientSocket);
}

void WifiServer::onReadyRead()
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) return;

    QByteArray data = clientSocket->readAll();
    
    // 按照微信小程序或前端浏览器送来的轻量封包解开找目标
    // 假设手机扔过来的命令包为: {"text":"立创泰山派", "size":120}
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isNull() && doc.isObject()) {
        QJsonObject obj = doc.object();
        QString text = obj.value("text").toString();
        int size = obj.value("size").toInt(60); // 未传尺寸默认 60 标号大字
        
        if (!text.isEmpty()) {
            qDebug() << "\n[Network Alert⚡] 成功捕获局域网发来的全自动打标指令:" << text;
            
            // 下放通知告诉核心总线
            emit remotePrintRequested(text, size);
            
            // 发回馈信
            clientSocket->write("{\"status\":\"Laser Engraving Instantiated!\"}\n");
        }
    }
}
