#include "ui/mainwindow.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDir>
#include <QDebug>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // --- 消除嵌入式裸板的字体"方块绝症" ---
    // 程序启动时强行在当前它所处的目录找一块儿自带包 "font.ttf" 用来绘制和重切片中文激光轨迹
    int fontId = QFontDatabase::addApplicationFont(QCoreApplication::applicationDirPath() + "/font.ttf");
    if (fontId != -1) {
        QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
        if (!fontFamilies.isEmpty()) {
            QFont loadedFont(fontFamilies.at(0));
            a.setFont(loadedFont);
            qDebug() << "✅ 终极闭环破壳: 自驱动微型字库注入成功，全系统界面级接管!";
        }
    } else {
        qDebug() << "💥 [高危警报]: 未挂载字弹！请丢入任意有效的 .ttf 并命名为 font.ttf 放在程序身边！";
    }

    // 配置让Qt原生支持高分屏等特性的常规初始化
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    
    // 主界面必须死锁为您板子的液晶特性 480 宽 × 800 高
    MainWindow w;
    w.setWindowFlags(Qt::FramelessWindowHint); // 在没有窗口管理器的开发板上，不需要标题栏
    w.setFixedSize(480, 800);
    w.show();

    return a.exec();
}
