#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QPainter>
#include <QFont>
#include <QDebug>

// --- PreviewCanvas 白板显示器的具体展现方式 ---
PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent) {
    // 设置它看起来像一个画布内嵌在黑框手机上
    setStyleSheet("background-color: #FAFAFA; border: 2px dashed #999; border-radius: 5px;");
}

void PreviewCanvas::setPreviewPath(const QPainterPath &path) {
    m_path = path;
    update(); // 触发底盘重绘，实现毫无延迟地拖动响应
}

void PreviewCanvas::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing); // 开启抗锯齿，使曲线极其平滑

    // 偏移中心，否则文字将绘制在左上角的 (0,0) 并被裁剪掉
    painter.translate(rect().center());
    
    // 给打标机的激光刀路设计一套拉风又警告感十足的红色画笔
    painter.setPen(QPen(Qt::red, 2));
    painter.drawPath(m_path);
}

// --- MainWindow (整个 UI 的统合) ---
MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    // 初始化隐藏引擎、通信管道组件和手机接发网关伺服器
    m_camEngine = new CamEngine();
    m_serialComm = new SerialComm(this);
    m_wifiServer = new WifiServer(this);

    initUI();
    
    // 后台网络静默绑定启动 (开启在开发板本机 8080 端口)
    connect(m_wifiServer, &WifiServer::remotePrintRequested, this, &MainWindow::onRemotePrintRequested);
    m_wifiServer->startServer(8080);
    
    // 假设绑定我们在后台即将开启的虚拟管道入口
    m_serialComm->openVirtualPort("/tmp/ttyGalvo");
    
    // 一启动，立刻自动调用生成一版开机字体的预处理
    onGeneratePreview();
}

MainWindow::~MainWindow()
{
    delete m_camEngine;
}

void MainWindow::initUI()
{
    // 全局纵向直板型重力排布
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(10);

    // 1. 系统标头组件 (让您感觉这是个顶级定制软件)
    QLabel *headerLabel = new QLabel("雷电激光·上位机工作站");
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setStyleSheet("font-size: 24px; font-weight: bold; background-color: #2E3B4E; color: #FFF; border-radius: 8px;");
    headerLabel->setFixedHeight(50);
    mainLayout->addWidget(headerLabel);

    // 2. 文字编排/设定调参组件栏 (放置在一排)
    QHBoxLayout *inputLayout = new QHBoxLayout();
    m_textInput = new QLineEdit("雷电打标机");
    m_textInput->setStyleSheet("font-size: 18px; padding: 5px;");
    m_sizeInput = new QSpinBox();
    m_sizeInput->setRange(10, 300); // 最大可把字撑满屏
    m_sizeInput->setValue(60);
    m_sizeInput->setStyleSheet("font-size: 18px; padding: 5px;");
    
    inputLayout->addWidget(new QLabel("喷印内容:"));
    inputLayout->addWidget(m_textInput, 1);
    inputLayout->addWidget(new QLabel("字号:"));
    inputLayout->addWidget(m_sizeInput);
    mainLayout->addLayout(inputLayout);

    // [事件挂载!] - 一旦输入框发生哪怕敲进一个字的动作，或是数字拨轮滚了一下，它都连向切片引擎
    connect(m_textInput, &QLineEdit::textChanged, this, &MainWindow::onGeneratePreview);
    connect(m_sizeInput, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onGeneratePreview);

    // 3. 画板预览呈现层 (占据 480x800 的中间极大版面)
    m_canvas = new PreviewCanvas(this);
    m_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainLayout->addWidget(m_canvas);

    // 4. 重火力打击起爆层 (包含试探操作和发射按键集群)
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnFrame = new QPushButton("🎯 框选激光寻址");
    btnFrame->setFixedHeight(65);
    btnFrame->setStyleSheet("QPushButton { font-size: 18px; font-weight: bold; background-color: #FFA000; color: #333; border-radius: 8px; } "
                            "QPushButton:pressed { background-color: #FF8F00; }");
    
    QPushButton *btnStart = new QPushButton("🚀 开始极速雕刻");
    btnStart->setFixedHeight(65);
    btnStart->setStyleSheet("QPushButton { font-size: 18px; font-weight: bold; background-color: #D32F2F; color: white; border-radius: 8px; } "
                            "QPushButton:pressed { background-color: #C62828; }");
    
    btnLayout->addWidget(btnFrame);
    btnLayout->addWidget(btnStart);
    mainLayout->addLayout(btnLayout);

    // 绑定发射按钮触发射网
    connect(btnFrame, &QPushButton::clicked, this, &MainWindow::onFrameBox);
    connect(btnStart, &QPushButton::clicked, this, &MainWindow::onStartEngraving);
}

// “字->图画展现->切片计算入队” 一体机动作流
void MainWindow::onGeneratePreview()
{
    QString txt = m_textInput->text();
    int size = m_sizeInput->value();
    
    // 先产生给我们的白板用于显形的数学虚影图层
    QFont f("sans-serif", size);
    QPainterPath path;
    // (简单的 -x 偏移能让文字差不多画在画布最中间，否则它会从 0 撑出去)
    path.addText(-size * txt.length() / 2, size / 2, f, txt); 
    
    m_currentPath = path;
    m_canvas->setPreviewPath(path); 
    
    // 第二部，暗中调用咱们的牛逼切片机 CAM_Engine 生成所有运动编码阵列，预存！
    m_currentGcode = m_camEngine->generateGcodeFromText(txt, "sans-serif", size);
}

// 模拟 LightBurn 核心：空光框选画出极大矩形！
void MainWindow::onFrameBox()
{
    // 用数学直接包裹文字的所有死角
    QRectF bounds = m_currentPath.boundingRect();
    
    qDebug() << "开始发送走边框指令，寻查雕刻占地范围...";
    // 发包给底层一条顺次围绕四方的粗放边界路线 (保持极低光强和最快移速，不留伤痕)
    QString frameCmd = QString("G0 X%.2f Y%.2f S0\n"
                               "G1 X%.2f Y%.2f S10 F6000\n"
                               "G1 X%.2f Y%.2f S10 F6000\n"
                               "G1 X%.2f Y%.2f S10 F6000\n"
                               "G1 X%.2f Y%.2f S10 F6000\n"
                               "G0 S0\nM5").arg(bounds.left()).arg(bounds.top())
                               .arg(bounds.right()).arg(bounds.top())
                               .arg(bounds.right()).arg(bounds.bottom())
                               .arg(bounds.left()).arg(bounds.bottom())
                               .arg(bounds.left()).arg(bounds.top());
                               
    m_serialComm->sendGcodeCommand(frameCmd);
}

// 狂飙喷发，射出切好的所有代码弹幕
void MainWindow::onStartEngraving()
{
    qDebug() << "准备突击发射合计:" << m_currentGcode.size() << "行 G-code 到振镜...";
    
    foreach (const QString &cmd, m_currentGcode) {
        m_serialComm->sendGcodeCommand(cmd + "\n");
    }
}

// 模拟幽灵手：不用触屏，全自动完成局域网下发的雕刻指令！
void MainWindow::onRemotePrintRequested(const QString &text, int size)
{
    // 修改原界面的输入框 (因为咱们早就绑定了 textChanged 到重切片方法，所以这里一改，画板和 G-code 会同步刷新！)
    m_textInput->setText(text);
    m_sizeInput->setValue(size);
    
    qDebug() << "[网络端接管] 数据更新完成，UI 切片已完成，开始实施远程雕刻...";
    // 图省事暴力开光：直接调用发射按键的函数
    onStartEngraving();
}
