#include "cam_engine.h"
#include <QDebug>
#include <QPolygonF>
#include <QPointF>
#include <QTransform>

CamEngine::CamEngine()
{
}

QList<QString> CamEngine::generateGcodeFromText(const QString &text, 
                                                const QString &fontFamily, 
                                                int pointSize, 
                                                double xOffset, 
                                                double yOffset)
{
    QList<QString> gcodeOutput;
    
    // 1. 设置系统字体环境
    QFont font(fontFamily, pointSize);
    
    // 2. 构建一个数学笔画收集器 (将无形的文字转化为有绝对坐标线的轮廓集)
    QPainterPath path;
    path.addText(0, 0, font, text);

    // * 注意：在 Qt 中 Y 轴正方向是向下的，但在 CNC/GRBL 机床中通常正 Y 朝上。
    // 为了防止打标打出来的字是上下颠倒的，我们在这套一个简单的上下翻转矩阵
    QTransform flipY;
    flipY.scale(1.0, -1.0); 
    QPainterPath flippedPath = flipY.map(path);
    
    // 3. 将包含大量圆滑贝塞尔曲线（机床看不懂）转化成机器能懂的多边形线段集合 (也就是打碎细化)
    // toSubpathPolygons() 是非常强大的重算方法，它会根据平滑度把复杂的轮廓统统变成离散单线的列表！
    QList<QPolygonF> strokes = flippedPath.toSubpathPolygons();
    
    gcodeOutput << "; === 由纯血统原生 Qt CAM 引擎构建的 G-code 开始 ===";
    gcodeOutput << "; 文本内容: " + text;
    gcodeOutput << "G90 ; 切换至绝对坐标模式";
    
    // 4. 重建 G-code 激光走跑流水线
    foreach (const QPolygonF &poly, strokes) {
        if (poly.isEmpty()) continue;
        
        QPointF startPt = poly.first();
        
        // 4a. 抬光（关掉激光），急速飞针移动至这一笔画的初始起笔点 [G0: 空移]
        gcodeOutput << QString("G0 X%.3f Y%.3f S0").arg(startPt.x() + xOffset).arg(startPt.y() + yOffset);
        
        // 4b. 落笔（接通激光），沿着多边形逐个顶点雕刻 [G1: 匀速走线]
        for (int i = 1; i < poly.size(); ++i) {
            QPointF p = poly.at(i);
            // S 参数可根据实际调节功率，F 为雕刻进给速度
            gcodeOutput << QString("G1 X%.3f Y%.3f S200 F4000").arg(p.x() + xOffset).arg(p.y() + yOffset);
        }
        
        // 如果这是一个闭合字母内圈 (例如 'O', 'D' 等)，补刻头尾重合段
        if (poly.isClosed() && poly.size() > 2) {
            gcodeOutput << QString("G1 X%.3f Y%.3f S200 F4000").arg(startPt.x() + xOffset).arg(startPt.y() + yOffset);
        }
        
        // 4c. 画完这一内环或者笔画后，即刻关闭激光阀门
        gcodeOutput << "G0 S0";
    }
    
    gcodeOutput << "M5 ; 任务终末安全关光";
    gcodeOutput << "G0 X0 Y0 S0 ; 回归机械零点";
    gcodeOutput << "; === 生成终了 ===";
    
    return gcodeOutput;
}
