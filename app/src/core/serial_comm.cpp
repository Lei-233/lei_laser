#include "serial_comm.h"
#include <QDebug>

SerialComm::SerialComm(QObject *parent) : QObject(parent)
{
    m_serial = new QSerialPort(this);
    connect(m_serial, &QSerialPort::readyRead, this, &SerialComm::onReadyRead);
}

SerialComm::~SerialComm()
{
    if (m_serial->isOpen()) {
        m_serial->close();
    }
}

bool SerialComm::openVirtualPort(const QString &portName)
{
    m_serial->setPortName(portName);
    
    // Qt 中即便是针对 /tmp/ttyGalvo 这种 Linux 内存虚终端，也需要设置波特率为固定值（对虚终端无意义，但是必须合法化其传输管道特性）
    m_serial->setBaudRate(QSerialPort::Baud115200);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        qDebug() << "【核心发射报错😱】无法打开打标下发管线通道:" << portName << m_serial->errorString();
        return false;
    }
    
    qDebug() << "🟢 [高速管道锁死]: 本机 Qt 已与下位机底座" << portName << "实现双工跨进程直连！";
    return true;
}

void SerialComm::sendGcodeCommand(const QString &cmd)
{
    if (m_serial && m_serial->isOpen()) {
        QByteArray data = cmd.toUtf8();
        m_serial->write(data);
        // 使用非常微弱的安全阻塞机制（也可以直接挂接缓冲让它异构化甩出）
        m_serial->waitForBytesWritten(20); 
    } else {
        qDebug() << "[虚终端断开! 仅做本机 TX 脱机预览]:" << cmd;
    }
}

void SerialComm::onReadyRead()
{
    QByteArray readData = m_serial->readAll();
    // 攫取到底层 C 代码发回的诸如 "ok" 或者 WPos 汇报的字符串
    emit receivedStatus(QString::fromUtf8(readData));
}
