#ifndef SERIALCOMM_H
#define SERIALCOMM_H

#include <QObject>
#include <QString>
#include <QSerialPort>

class SerialComm : public QObject
{
    Q_OBJECT
public:
    explicit SerialComm(QObject *parent = nullptr);
    ~SerialComm();
    
    bool openVirtualPort(const QString &portName);
    void sendGcodeCommand(const QString &cmd);

signals:
    void receivedStatus(const QString &statusJSON);

private slots:
    void onReadyRead();

private:
    QSerialPort *m_serial;
};

#endif // SERIALCOMM_H
