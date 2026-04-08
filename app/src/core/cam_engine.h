#ifndef CAMENGINE_H
#define CAMENGINE_H

#include <QString>
#include <QList>
#include <QFont>
#include <QPainterPath>

class CamEngine
{
public:
    CamEngine();
    
    // 引擎总入口：输入文字内容、字体属性及平移参数，直接返回严格的 G-code 动作组
    // 参数说明：
    // text: 要打标的字符串
    // fontFamily: 字体风格 (默认可以是系统自带黑体之类)
    // pointSize: 字号大小 (直接决定了打标出来的物理尺寸映射)
    // xOffset/yOffset: 相当于 LightBurn 里的起点偏移 (定位光斑点)
    QList<QString> generateGcodeFromText(const QString &text, 
                                         const QString &fontFamily, 
                                         int pointSize, 
                                         double xOffset = 0.0, 
                                         double yOffset = 0.0);
};

#endif // CAMENGINE_H
