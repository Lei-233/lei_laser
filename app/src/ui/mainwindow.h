#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QString>
#include <QPainterPath>
#include <QList>

#include "core/cam_engine.h"
#include "core/serial_comm.h"
#include "network/wifi_server.h"

// 前置免包含声明，减小编译体积
class QLineEdit;
class QPushButton;
class QSpinBox;

// [专属UI控件] - 极简画图白板，用来指引用户看到即被打出的刀路轨迹
class PreviewCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PreviewCanvas(QWidget *parent = nullptr);
    void setPreviewPath(const QPainterPath &path);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPainterPath m_path;
};

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onGeneratePreview();
    void onStartEngraving();
    void onFrameBox();
    
    // 连接到局域网手机服务的自动代理扳机
    void onRemotePrintRequested(const QString &text, int size);

private:
    void initUI();

    // 交互控件组
    QLineEdit *m_textInput;
    QSpinBox *m_sizeInput;
    PreviewCanvas *m_canvas;
    
    // 底层引擎总线及网络监听组
    CamEngine *m_camEngine;
    SerialComm *m_serialComm;
    WifiServer *m_wifiServer;

    // 当前在内存中缓冲的、即将被发射的数据队列
    QList<QString> m_currentGcode;
    QPainterPath m_currentPath; // 用于画图板展示的虚影
};

#endif // MAINWINDOW_H
