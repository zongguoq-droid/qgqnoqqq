# qt_ui 模块完整学习文档

> 面向对象：熟悉 C/嵌入式、但**不熟悉 Qt** 的开发者。
> 本文档循序渐进，从"Qt 是什么"讲起，逐层拆解 qt_ui 模块的架构设计、使用逻辑与代码设计，最后用真实案例讲透**数据如何与 UI 绑定**这一核心问题。

---

## 目录

1. [模块定位：qt_ui 在整个系统里扮演什么角色](#1-模块定位)
2. [Qt 基础速成（15 分钟看懂本项目所需的全部 Qt 知识）](#2-qt-基础速成)
3. [架构设计：五层结构与数据流](#3-架构设计)
4. [文件清单与职责总览](#4-文件清单与职责总览)
5. [模块详解①：入口 main.cpp — 程序是怎么跑起来的](#5-模块详解-入口-maincpp)
6. [模块详解②：协议层 protocol_handler — 二进制帧的打包与拆包](#6-模块详解-协议层-protocol_handler)
7. [模块详解③：网络层 daemon_client — Socket 客户端与自动重连](#7-模块详解-网络层-daemon_client)
8. [模块详解④：传感器线程 sensor_thread — QThread 与跨线程通信](#8-模块详解-传感器线程-sensor_thread)
9. [模块详解⑤：自定义控件 speed_gauge — QPainter 手绘表盘](#9-模块详解-自定义控件-speed_gauge)
10. [模块详解⑥：界面布局 mainwindow.ui — Designer 与代码生成](#10-模块详解-界面布局-mainwindowui)
11. [模块详解⑦：主窗口 mainwindow — 总装配车间与消息分发](#11-模块详解-主窗口-mainwindow)
12. [核心专题：数据如何与 UI 绑定（三条绑定路径全解析）](#12-核心专题数据如何与-ui-绑定)
13. [构建系统：.pro 文件与编译流程](#13-构建系统)
14. [总结与进阶学习建议](#14-总结与进阶学习建议)

---

## 1. 模块定位

### 1.1 整个车载系统长什么样

这是一个嵌入式智能车载终端项目，硬件是 ARM 开发板 + 1024×600 LCD 触摸屏。系统分成两大部分：

```
┌─────────────────────────────────────────────────────────┐
│                    嵌入式 ARM 开发板                      │
│                                                         │
│  ┌─────────────────────┐      ┌──────────────────────┐  │
│  │   C 后端 (守护进程)   │      │   qt_ui (本项目)      │  │
│  │                     │      │                      │  │
│  │  gps_daemon         │◄────►│   触控界面            │  │
│  │  dvr_daemon         │ Unix │   (Qt 应用程序)       │  │
│  │  av_daemon          │Socket│                      │  │
│  │  canbus_daemon      │      │                      │  │
│  │  input_daemon       │      │                      │  │
│  └─────────────────────┘      └──────────────────────┘  │
│         ▲                             ▲                  │
│         │ 直接操作硬件                 │ 显示给用户看        │
│  ┌──────┴─────────────────────────────┴──────────────┐   │
│  │  GPS模组 摄像头 喇叭 CAN总线 按键 DHT11传感器 /dev  │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

- **C 后端**：一堆守护进程（daemon），各自负责与硬件打交道——GPS 模组、摄像头、CAN 总线、物理按键、音频等。它们常驻后台，不关心界面。
- **qt_ui（本项目）**：纯界面层，负责把后端的数据**显示给人看**，把人的操作（触屏/物理按键）**转发给后端**。

### 1.2 qt_ui 的职责边界（很重要）

qt_ui **不直接碰任何硬件**。它只做两件事：

| 方向              | 做什么        | 手段                     |
| --------------- | ---------- | ---------------------- |
| **显示**（数据 → UI） | 把后端数据渲染成界面 | Unix Socket 收消息 → 更新控件 |
| **控制**（用户 → 后端） | 把用户操作转成命令  | 按钮点击/物理按键 → Socket 发消息 |

唯一的例外是温湿度传感器：`SensorThread` 直接读 `/dev/mydht11` 设备文件（因为该传感器没有独立的守护进程，Qt 端自己起了个线程读）。

### 1.3 功能一览

5 个标签页，对应 5 类车载功能：

| 标签页 | 功能              | 数据来源            |
| --- | --------------- | --------------- |
| 仪表盘 | 速度表盘、卫星数、UTC 时间 | gps_daemon      |
| DVR | 摄像头预览、录像/拍照控制   | dvr_daemon      |
| 音乐  | 播放/暂停、音量、曲目名    | av_daemon       |
| CAN | CAN 报文实时监控与手动发送 | canbus_daemon   |
| 温湿度 | 大字体温湿度实时显示      | 直读 /dev/mydht11 |

---

## 2. Qt 基础速成

> 这一节是**阅读本项目源码的前置知识**。每个概念都对应项目里的真实用法，不展开无关内容。

### 2.1 Qt 是什么

Qt 是一个 C++ 图形界面框架（类比：Java 的 Swing、Python 的 PyQt——PyQt 就是 Qt 的 Python 绑定）。写 Qt 程序的典型流程：

1. **创建控件**（按钮、标签、输入框……）
2. **布局排列**它们
3. **连接信号与槽**：让"用户点了按钮"触发"某段代码"
4. **进入事件循环**：程序开始响应鼠标、键盘、网络、定时器

### 2.2 事件循环（Event Loop）— Qt 程序的心脏

```cpp
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);   // ① 初始化 Qt 框架
    MainWindow window;              // ② 创建窗口
    window.show();                  // ③ 显示窗口
    return app.exec();              // ④ 进入事件循环 ★关键★
}
```

`app.exec()` 是一个**永不返回的循环**（直到窗口关闭）。它在后台不停地轮询：

- 有没有鼠标点击？→ 找到被点的控件，发出 `clicked` 信号
- 有没有网络数据到达？→ 触发对应 socket 的 `readyRead` 信号
- 定时器到点了吗？→ 触发 `timeout` 信号

**理解这一点是关键**：Qt 程序不是"从头执行到尾"的线性程序，而是"初始化一次，然后靠事件驱动回调函数"。本项目所有功能本质上都是**注册回调，等事件来**。

### 2.3 信号与槽（Signals & Slots）— Qt 最核心的机制

这是 Qt 区别于其他 UI 框架的根本设计，本项目从头到尾都在用它，必须彻底搞懂。

**类比**：信号是广播电台，槽是收音机。`connect()` 就是"调台"——把某个电台的某个频道和某台收音机绑定起来。电台广播时不知道谁在听；收音机也不关心广播来自哪。

```cpp
// 语法：connect(发送者, &发送者类::信号, 接收者, &接收者类::槽)
connect(m_socket, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
//      └发送者    └信号：有数据到达              └接收者  └槽：我的处理函数
```

当 socket 收到数据，Qt 自动调用 `onReadyRead()`。**双方不需要知道对方的存在**，只需要 `connect()` 时牵一次线。

**信号声明**用 `signals:` 关键字，**发出信号**用 `emit`：

```cpp
class DaemonClient : public QObject {
    Q_OBJECT                          // ← 凡是用信号槽的类必须加这个宏
signals:
    void messageReceived(uint8_t msgType, const QByteArray &data);  // 声明信号
};

// 在代码里发出信号：
emit messageReceived(msgType, data);  // 所有已连接的槽会被依次调用
```

**重要性质**：

- 一个信号可以连接多个槽（广播）
- 多个信号可以连接同一个槽（汇总）
- 信号可以带参数，槽的参数要与之匹配
- 连接可以跨线程（后面讲传感器时会用到这个特性）

### 2.4 QObject、父子关系与内存管理

所有 Qt 对象都继承自 `QObject`，形成一棵**对象树**：

```cpp
m_socket = new QLocalSocket(this);   // this 是父对象
```

创建对象时传入 `parent`，父对象析构时会**自动删除所有子对象**。所以 Qt 代码里几乎见不到 `delete`——这是 Qt 的内存管理约定：

> **谁 new 出来并指定了 parent，就由 parent 负责回收。**

本项目中 `MainWindow` 是最顶层的父对象，`DaemonClient`、`QTimer`、`SpeedGauge` 都挂在它下面，窗口销毁时全部自动释放。

### 2.5 常用控件速查表（本项目用到的）

| 控件            | 作用              | 项目中的实例                 |
| ------------- | --------------- | ---------------------- |
| `QMainWindow` | 主窗口（含菜单栏、状态栏）   | `MainWindow` 本身        |
| `QWidget`     | 一切控件的基类/空白容器    | 每个 Tab 页面              |
| `QTabWidget`  | 标签页容器           | `tabWidget`（5 个页签）     |
| `QLabel`      | 显示文字或图片         | 时长标签、预览画面 `dvrPreview` |
| `QPushButton` | 按钮              | 开始录像、播放、发送 CAN         |
| `QSlider`     | 滑块              | 音量 `volSlider`         |
| `QSpinBox`    | 数字输入框（支持十六进制显示） | CAN ID 输入              |
| `QLineEdit`   | 单行文本输入          | CAN 数据输入               |
| `QTextEdit`   | 多行富文本显示         | CAN 日志 `canLog`        |
| `QStatusBar`  | 窗口底部状态栏         | 连接计数、按键诊断              |

### 2.6 控件的常用操作

```cpp
ui->btnRecord->setText("继续录像");        // 改文字
ui->btnRecord->setEnabled(false);          // 禁用（变灰不可点）
ui->dvrDuration->setStyleSheet("color:red; background:#fff;");  // 改样式(类似CSS)
ui->dvrPreview->setPixmap(image);          // 显示图片
ui->statusbar->showMessage("已连接", 3000); // 状态栏提示3秒
ui->tabWidget->currentIndex();             // 当前是第几个标签页
```

`setStyleSheet()` 是 Qt 的样式表机制，语法与 CSS 几乎一致，本项目用它实现车载大屏的配色。

### 2.7 布局（Layout）

控件怎么摆放有两种方式：

1. **绝对定位**：在 `.ui` 文件里直接给每个控件指定 `geometry`（x, y, 宽, 高）。本项目大量使用——因为目标屏幕是固定 1024×600，不需要自适应。
2. **布局管理器**：`QVBoxLayout`（垂直）/ `QHBoxLayout`（水平）等自动排列。本项目的仪表盘页用了 `QVBoxLayout`，因为速度表盘要自适应大小。

```cpp
auto *dashLayout = new QVBoxLayout(ui->tabDashboard);  // 给仪表盘页装一个垂直布局
dashLayout->addWidget(m_gauge);       // 速度表盘
dashLayout->addWidget(m_satsLabel);   // 卫星信息标签，自动排在表盘下方
```

### 2.8 定时器（QTimer）— Qt 的闹钟

```cpp
m_previewTimer = new QTimer(this);
connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::refreshPreview);
m_previewTimer->start(1000);   // 每 1000 毫秒触发一次 timeout 信号
```

本项目有 3 个定时器：预览刷新（1秒）、录像时长自增（1秒）、曲目名轮询（1秒）。
定时器到期 → 发 `timeout` 信号 → 执行槽函数。依然是"注册回调，等事件来"的模式。

### 2.9 .pro 项目文件与 moc

Qt 项目用 **qmake** 构建（类似 Makefile 生成器）。`.pro` 文件声明源码、依赖模块：

```qmake
QT      += core gui widgets network   # 依赖的 Qt 模块
SOURCES += main.cpp mainwindow.cpp ...
HEADERS += mainwindow.h ...
FORMS   += mainwindow.ui              # Designer 界面文件
```

**moc（元对象编译器）**：信号/槽不是标准 C++，qmake 构建前会先让 `moc` 工具扫描带 `Q_OBJECT` 宏的头文件，生成额外代码（`moc_xxx.cpp`）来实现信号槽机制。所以你会在头文件顶部看到 `Q_OBJECT`——少了它信号槽就不工作。

---

## 3. 架构设计

### 3.1 分层架构

qt_ui 内部是一个清晰的**四层结构**，自底向上：

```
┌────────────────────────────────────────────────────────────┐
│  第4层  展示层 (UI)                                          │
│  mainwindow.ui 定义的控件 + SpeedGauge 自定义控件            │
│  职责: 把数据显示出来，接收用户点击                            │
├────────────────────────────────────────────────────────────┤
│  第3层  协调层 (MainWindow)                                  │
│  职责: 消息分发中心——按(来源,类型)路由消息到对应控件;           │
│        管理 DVR 状态机; 物理按键按当前标签页分发               │
├────────────────────────────────────────────────────────────┤
│  第2层  通信层 (DaemonClient × 5)                            │
│  职责: 管理 5 条 Unix Socket 连接、自动重连、收发缓冲           │
├────────────────────────────────────────────────────────────┤
│  第1层  协议层 (ProtocolHandler)                             │
│  职责: 二进制帧的打包(组帧)与解包(拆帧)、CRC8                  │
└────────────────────────────────────────────────────────────┘
```

**为什么要分层？** 每层只依赖下一层，改动互不影响：

- 协议格式变了？只改 `ProtocolHandler`，上层无感。
- 想换个通信方式（比如 TCP）？只改 `DaemonClient`，协议和 UI 都不用动。
- 想加一个新页面？在 UI 层加控件，在协调层加一条消息路由即可。

### 3.2 五条连接：一个客户端实例对应一个守护进程

```
MainWindow
  └── m_clients (QVector<DaemonClient*>)
        ├── DaemonClient("gps",   "/tmp/car_gps.sock")   ──► gps_daemon
        ├── DaemonClient("input", "/tmp/car_input.sock") ──► input_daemon
        ├── DaemonClient("can",   "/tmp/car_can.sock")   ──► canbus_daemon
        ├── DaemonClient("av",    "/tmp/car_av.sock")    ──► av_daemon
        └── DaemonClient("dvr",   "/tmp/car_dvr.sock")   ──► dvr_daemon
```

每个 `DaemonClient` 实例内部结构完全相同：

```
DaemonClient
  ├── QLocalSocket*      m_socket          ← 实际的 socket 连接
  ├── QTimer*            m_reconnectTimer  ← 断线重连定时器(1秒)
  ├── QByteArray         m_recvBuffer      ← 接收缓冲(累积不完整的帧)
  └── ProtocolHandler    m_proto           ← 拆帧状态机
```

**Unix domain socket 是什么？** 它是同一台机器上进程间通信（IPC）的方式，走内核内存，不经过网卡，比 TCP 快。`/tmp/car_gps.sock` 是文件系统里的一个特殊文件，作为"会合点"：后端先创建它（listen），Qt 连接它（connect）。

### 3.3 全局数据流

**上行（数据显示）**：

```
后端 daemon
   │ 写入 socket（二进制协议帧）
   ▼
QLocalSocket 收到字节 ──触发──► readyRead 信号
   ▼
DaemonClient::onReadyRead()
   │ 累积到 m_recvBuffer
   │ 调用 m_proto.unpack() 循环拆帧
   ▼ 每拆出一个完整帧
emit messageReceived(msgType, data)      ← DaemonClient 的信号
   ▼ (MainWindow 构造时已 connect)
MainWindow::onDaemonMessage(name, msgType, data)
   │ 按 (name, msgType) 分发
   ▼
reinterpret_cast 成 C 结构体 → 更新对应 UI 控件
   ▼
Qt 自动重绘界面 → 用户看到新数据
```

**下行（用户控制）**：

```
用户点按钮 / 按物理按键
   ▼
Qt 触发槽函数（如 on_btnRecord_clicked）
   ▼
找到对应的 DaemonClient（objectName == "dvr"）
   ▼
client->send(MSG_DVR_START)
   │ ProtocolHandler::pack() 组帧
   │ socket->write() + flush()
   ▼
后端 daemon 收到命令并执行
   ▼
daemon 回传 MSG_DVR_STATUS → UI 同步最新状态（闭环）
```

### 3.4 信号槽连接全景图（本项目的"电路图"）

理解这张图就理解了整个程序的运行机制：

```
[Qt 框架事件]                    [槽函数]                     [效果]

QLocalSocket::connected    ──►  DaemonClient::onConnected   停止重连、清缓冲
QLocalSocket::disconnected ──►  DaemonClient::onDisconnected 启动1秒重连定时器
QLocalSocket::readyRead    ──►  DaemonClient::onReadyRead   收数据、拆帧
QLocalSocket::error        ──►  DaemonClient::onError       启动重连定时器
QTimer::timeout(重连)       ──►  DaemonClient::onReconnect   重新连接

DaemonClient::connected        ──►  lambda: m_connected++   状态栏"连接: N/5"
DaemonClient::disconnected     ──►  lambda: m_connected--
DaemonClient::messageReceived  ──►  MainWindow::onDaemonMessage  消息分发中心★

QTimer::timeout(1s)        ──►  MainWindow::refreshPreview  读jpg刷新预览
QTimer::timeout(1s)        ──►  MainWindow::updateDvrDuration 时长自增
QTimer::timeout(1s)        ──►  lambda: 读 /tmp/av_track    更新曲目名

QPushButton::clicked       ──►  on_btnRecord_clicked 等     发送控制命令
QSlider::valueChanged      ──►  on_volSlider_valueChanged   发送音量

SensorThread::dataReady    ──►  lambda: setText()           温湿度显示（跨线程!）
```

---

## 4. 文件清单与职责总览

```
qt_ui/
├── qt_ui.pro               # 构建配置: 依赖模块、源文件清单
├── main.cpp                # 程序入口: QApplication → MainWindow → exec()
├── mainwindow.ui           # 界面布局: Qt Designer 可视化设计(5个标签页)
├── mainwindow.h/.cpp       # 主窗口: 消息分发、DVR状态机、按键路由、定时器
├── protocol_handler.h/.cpp # 协议层: 组帧/拆帧状态机/CRC8/消息类型定义
├── daemon_client.h/.cpp    # 通信层: QLocalSocket封装、自动重连、解帧
├── sensor_thread.h/.cpp    # 传感器: QThread 直读 /dev/mydht11
├── pages/
│   └── speed_gauge.h       # 自定义控件: QPainter 手绘速度表盘
└── README.md               # 项目说明
```

**依赖关系**（编译顺序与调用关系一致）：

```
main.cpp ──► mainwindow ──► daemon_client ──► protocol_handler
                 │                ▲
                 ├──► sensor_thread
                 ├──► speed_gauge
                 └──► ui_mainwindow.h (由 mainwindow.ui 自动生成)
```

---

## 5. 模块详解① 入口 main.cpp

入口只有 46 行，但包含了 Qt 程序的标准骨架：

```cpp
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);        // ① 初始化 Qt
    app.setStyleSheet(...);              // ② 全局样式
    MainWindow window;                   // ③ 创建主窗口
    if (fullscreen) window.showFullScreen(); else window.show();  // ④ 显示
    return app.exec();                   // ⑤ 进入事件循环，永不返回
}
```

### 逐点解析

**① `QApplication`**：每个 Qt 图形程序有且只有一个。它管理事件循环、全局状态、命令行参数。**必须在创建任何控件之前构造**。

**② 全局样式表**：`setStyleSheet` 对整个应用生效，类似网页的全局 CSS。这里设置了字体和按钮最小高度 40px——车载触控屏要手指好点。

**③ `MainWindow window;`**：构造主窗口时，所有初始化连锁发生——连接 5 个守护进程、启动传感器线程、启动定时器。**窗口构造完成 = 系统就绪**。

**④ 全屏开关**：通过命令行参数 `-fullscreen` 控制。开发时窗口模式方便调试，部署到开发板时全屏。还有 `-platform linuxfb` 参数（Qt 自带），让程序不依赖 X11 图形系统，直接往 Linux 的 framebuffer 上画——嵌入式部署的关键。

**⑤ `app.exec()`**：事件循环启动。此后程序的生命完全由事件驱动：用户点击、socket 数据、定时器到期……都变成信号回调。

---

## 6. 模块详解② 协议层 protocol_handler

### 6.1 为什么需要协议层

socket 是**字节流**，没有"消息"概念。发送方写两次 7 字节，接收方可能一次读到 14 字节（粘包），也可能读到 3+11 字节（半包）。必须约定帧格式，让接收方能从连续字节流中**切出一条条消息**。

### 6.2 帧格式

```
偏移   大小   字段       说明
0     1     HEAD0      固定 0xAA ─┐
1     1     HEAD1      固定 0x55 ─┴─ 双字节帧头，降低误判
2     1     TYPE       消息类型 (MSG_* 宏)
3     1     LEN_H      数据长度高字节 (大端序)
4     1     LEN_L      数据长度低字节
5..N  可变   DATA       数据载荷 (LEN 字节)
N+1   1     CRC8       校验 (当前固定 0x00，未启用)
N+2   1     TAIL       帧尾，固定 0x55
```

例：拍照命令（`MSG_DVR_SNAPSHOT=0x23`，无数据）= `AA 55 23 00 00 00 55`，共 7 字节。

**设计要点**：

- **帧头+帧尾**双重定界，帧损坏可检测（帧尾不对就丢弃）。
- **长度字段**告诉接收方"还要再收多少字节"，这是流式拆包的核心依据。
- **CRC 未启用**是有意为之：Unix socket 走内核内存，不会有比特错误，留字段是为将来可能的 TCP 中继扩展。

### 6.3 消息类型定义（.h 里的宏）

```cpp
#define MSG_GPS_DATA     0x10  // GPS 数据上报
#define MSG_DVR_START    0x20  // 开始录像
#define MSG_DVR_PAUSE    0x26  // 暂停/恢复
#define MSG_CAN_DATA     0x50  // CAN 数据上报
#define MSG_AV_PLAY      0x60  // 播放
#define MSG_KEY_EVENT    0x70  // 物理按键事件
...
```

按功能域分段编址（0x1x=GPS、0x2x=DVR、0x5x=CAN、0x6x=音频、0x7x=输入），与 C 后端的 `common.h` **逐字节一致**——这是前后端能对话的基础契约。

### 6.4 数据结构定义（`#pragma pack(push, 1)`）

```cpp
#pragma pack(push, 1)          // 关键: 1字节对齐，禁止编译器填充
struct GpsData {
    double   latitude;
    double   longitude;
    float    speed;
    ...
};
#pragma pack(pop)
```

**为什么必须 `#pragma pack(1)`？** 编译器默认会在结构体成员间插入填充字节以优化对齐（比如把 `uint8_t` 后面补 3 个字节让下一个 `uint32_t` 对齐到 4 字节边界）。那样 Qt 端和 C 后端的结构体内存布局就不同了。`pack(1)` 强制紧凑布局，保证两边**逐字节一致**，才能直接内存拷贝/指针转换解析。

### 6.5 组帧 `pack()` — 发送方向

```cpp
static QByteArray pack(uint8_t msgType, const QByteArray &data)
{
    QByteArray frame;
    uint16_t len = data.size();
    frame.append((char)0xAA);                    // 帧头
    frame.append((char)0x55);
    frame.append((char)msgType);                 // 类型
    frame.append((char)((len >> 8) & 0xFF));     // 长度高字节(大端)
    frame.append((char)(len & 0xFF));            // 长度低字节
    frame.append(data);                          // 载荷
    frame.append((char)0x00);                    // CRC占位
    frame.append((char)0x55);                    // 帧尾
    return frame;
}
```

静态方法，不需要实例。`QByteArray` 是 Qt 的字节数组（类似 `std::vector<char>`），自动管理内存，可任意 `append`。

### 6.6 拆帧状态机 `feed()` — 接收方向（重点）

这是协议层最精彩的部分。难点：数据可能**一次来半个帧**，也可能**一次来三个帧**。解决办法是**状态机**——把"解析一个帧"拆成几个阶段，每来一个字节推进一次状态：

```
IDLE ──[0xAA]──► HEAD1 ──[0x55]──► HEADER ──[收齐3字节]──► DATA ──[收齐LEN+1]──► TAIL ──[0x55]──► 成功,回IDLE
```

| 状态       | 等待                | 动作                                                |
| -------- | ----------------- | ------------------------------------------------- |
| `IDLE`   | 帧头第一字节            | 收到 0xAA 进入 HEAD1；其他字节丢弃                           |
| `HEAD1`  | 帧头第二字节            | 收到 0x55 进入 HEADER；又收到 0xAA 保持（处理连续 0xAA）；其他回 IDLE |
| `HEADER` | TYPE + LEN 共 3 字节 | 收齐后算出还要收多少数据，进入 DATA                              |
| `DATA`   | LEN+1 字节（数据+CRC）  | 收齐后进入 TAIL                                        |
| `TAIL`   | 帧尾 0x55           | 收到则一帧完整，返回 true；否则整帧丢弃回 IDLE                      |

```cpp
bool ProtocolHandler::feed(uint8_t byte, uint8_t &msgType, QByteArray &data)
{
    switch (m_state) {
    case IDLE:
        if (byte == 0xAA) { m_state = HEAD1; /*清空临时缓冲*/ }
        break;
    case HEAD1:
        if (byte == 0x55) m_state = HEADER;
        else if (byte == 0xAA) { /*保持*/ }
        else m_state = IDLE;
        break;
    case HEADER:
        m_headerBuf.append(byte);
        if (m_headerBuf.size() >= 3) {
            m_msgType = m_headerBuf[0];
            m_dataLen = (m_headerBuf[1] << 8) | m_headerBuf[2];  // 大端拼装
            m_state = DATA;
        }
        break;
    case DATA:
        m_dataBuf.append(byte);
        if (++m_dataReceived >= m_dataLen + 1) {   // +1 是 CRC 字节
            data = m_dataBuf.left(m_dataLen);      // 取出纯数据(不含CRC)
            msgType = m_msgType;
            m_state = TAIL;
        }
        break;
    case TAIL:
        if (byte == 0x55) { m_state = IDLE; return true; }  // ★完整帧★
        m_state = IDLE;   // 帧尾错误，丢弃
        break;
    }
    return false;
}
```

**状态机的好处**：不管字节以什么粒度到达（一个一个来、半帧半帧来、一次来五帧），状态机都能正确切分。状态保存在成员变量（`m_state` 等）里，跨越多次函数调用依然连续。

**容错设计**：中途收到垃圾字节不报错，只是静默回到 IDLE 重新找 0xAA。因为流数据一旦错位，唯一可靠的恢复手段就是重新同步帧头。

### 6.7 外层循环 `unpack()`

```cpp
bool ProtocolHandler::unpack(QByteArray &buffer, uint8_t &msgType, QByteArray &data)
{
    while (!buffer.isEmpty()) {
        uint8_t byte = buffer.at(0);
        buffer.remove(0, 1);        // 消费一个字节
        if (feed(byte, msgType, data))
            return true;            // 拆出一帧就返回，交给调用方处理
    }
    return false;
}
```

调用方这样用（见下一模块）：

```cpp
while (proto.unpack(buffer, msgType, data)) {   // 有帧就一直拆
    处理这一帧;
}   // 剩下不够一帧的字节留在 buffer，等下次数据到达
```

---

## 7. 模块详解③ 网络层 daemon_client

`DaemonClient` = **一个守护进程的完整连接管理**。封装四件事：连接、发送、接收解帧、断线重连。

### 7.1 构造函数：装配 + 接线

```cpp
DaemonClient::DaemonClient(const QString &name, const QString &sockPath, QObject *parent)
{
    m_socket = new QLocalSocket(this);           // socket，父对象是 this
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(1000);         // 1秒后重连
    m_reconnectTimer->setSingleShot(true);       // 只触发一次(不循环)

    // 接线: socket 的 4 个事件 → 我的 4 个槽
    connect(m_socket, &QLocalSocket::connected,    this, &DaemonClient::onConnected);
    connect(m_socket, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
    connect(m_socket, &QLocalSocket::readyRead,    this, &DaemonClient::onReadyRead);
    connect(m_socket, &QLocalSocket::error,        this, &DaemonClient::onError);
    connect(m_reconnectTimer, &QTimer::timeout,    this, &DaemonClient::onReconnect);
}
```

注意：**构造函数里只"接线"，不做实际连接**。所有行为都推迟到事件发生。这是 Qt 的典型写法。

### 7.2 接收路径：`onReadyRead()`（数据 → 信号 的关键一环）

```cpp
void DaemonClient::onReadyRead()
{
    m_recvBuffer.append(m_socket->readAll());    // ① 读出所有可用字节，累积
    uint8_t msgType; QByteArray data;
    while (m_proto.unpack(m_recvBuffer, msgType, data)) {  // ② 循环拆帧
        emit messageReceived(msgType, data);     // ③ 每帧发一次信号
    }
    // ④ 剩余的不完整字节留在 m_recvBuffer，等下次 readyRead
}
```

三步走：**先累积，再循环拆，拆一帧广播一次**。这正是处理流式粘包/半包的标准范式。`messageReceived` 是本类对外的唯一数据出口。

### 7.3 发送路径：`send()`

```cpp
void DaemonClient::send(uint8_t msgType, const QByteArray &data)
{
    QByteArray frame = ProtocolHandler::pack(msgType, data);  // 组帧
    m_socket->write(frame);      // 写入 socket
    m_socket->flush();           // 立即发送
}
```

上层调用时完全不用关心字节格式：`dvrClient->send(MSG_DVR_START)` 即可。协议细节被封装在协议层。

### 7.4 自动重连机制（健壮性设计）

车载环境下守护进程可能重启、崩溃。重连逻辑：

```
连接断开/出错
   │
   ▼
emit disconnected  ──►  (上层更新连接计数)
   │
   ▼
m_reconnectTimer->start()      启动1秒单次定时器
   │  (1秒后)
   ▼
onReconnect() → connectToServer()   重试连接
   │
   ├─ 成功 → onConnected(): 停定时器、清缓冲、重置解帧状态机
   └─ 失败 → onError(): 再启动定时器，1秒后再试
```

细节：

- `setSingleShot(true)` + 每次手动 `start()`，形成"失败后固定间隔重试"，不会疯狂轮询。
- 连接成功后必须 `m_proto.reset()` 和 `m_recvBuffer.clear()`——上次连接残留的半帧数据会污染新连接的解析。

### 7.5 这个类教给我们的设计思想

1. **封装变化**：把"一个连接"的所有状态（socket、缓冲、重连器、解帧器）打包成一个对象，上层只管收发消息。5 条连接 = 5 个独立实例，互不干扰。
2. **事件驱动**：类内部没有一个死循环，全部行为由信号触发。
3. **接口最小化**：对外只暴露 `connectToServer()`、`send()`、`isConnected()` 三个方法 + `messageReceived`/`connected`/`disconnected` 三个信号。

---

## 8. 模块详解④ 传感器线程 sensor_thread

### 8.1 为什么需要独立线程

DHT11 温湿度传感器通过内核驱动暴露为 `/dev/mydht11` 字符设备。`read()` 它可能阻塞等待采样完成。如果在主线程（UI 线程）里读：

> **UI 线程一旦阻塞，整个界面冻结**——按钮不响应、画面不刷新。这是 GUI 编程的第一铁律：**永远不要在 UI 线程做耗时操作**。

解决办法：把读取放到独立线程，读到数据后通知主线程更新界面。

### 8.2 实现：QThread 子类

```cpp
class SensorThread : public QThread {
    Q_OBJECT
public:
    void setLabels(QLabel *humi, QLabel *temp);   // 传入UI控件指针
    void run() override;                          // 线程主体
signals:
    void dataReady(int temp, int humi);           // 数据就绪信号
};
```

重写 `run()` 就定义了线程做什么：

```cpp
void SensorThread::run()
{
    int fd = open("/dev/mydht11", O_RDWR | O_NONBLOCK);
    if (fd < 0) return;               // 设备不存在就静默退出

    char buf[2];
    while (!isInterruptionRequested()) {      // 外部可请求退出
        if (read(fd, buf, 2) == 2) {
            emit dataReady(buf[1], buf[0]);   // buf[1]=温度, buf[0]=湿度
            msleep(2000);                     // 2秒读一次
        }
    }
    close(fd);
}
```

要点：

- `O_NONBLOCK` 非阻塞打开，`read()` 无数据时立即返回，线程空转重试而不卡死。
- `isInterruptionRequested()` 是 QThread 的协作式退出机制：外部调用 `requestInterruption()` 设置标志，循环检测到就干净退出。
- **工作线程里绝对不碰 UI 控件**，只 `emit dataReady(...)` 发信号。

### 8.3 跨线程信号：数据如何安全回到 UI（重要！）

```cpp
// MainWindow 构造函数里:
connect(m_sensorThread, &SensorThread::dataReady, this,
        [=](int t, int h) {
            ui->sensorTemp->setText(QString("%1°C").arg(t));
            ui->sensorHum->setText(QString("%1%").arg(h));
        });
```

发送者（SensorThread）在**工作线程**，接收者（MainWindow）在**主线程**。Qt 默认的连接类型 `Qt::AutoConnection` 会自动检测到跨线程，把信号调用**转成事件投递到主线程的事件队列**：

```
工作线程: emit dataReady(25, 60)
    │  (不直接调用槽！而是打包成一个事件)
    ▼
主线程事件队列: [ ..., dataReady事件, ... ]
    │  (主线程的事件循环 app.exec() 取出它)
    ▼
主线程执行: lambda 槽函数 → setText() 更新界面
```

这样**无需任何锁**就实现了线程安全的数据传递。这是 Qt 多线程编程最重要的模式，记住它：

> **工作线程干活 → emit 信号 → 主线程槽函数更新 UI。**

### 8.4 生命周期管理

```cpp
// 启动 (MainWindow 构造函数):
m_sensorThread = new SensorThread;
m_sensorThread->start();          // 创建线程并执行 run()

// 退出 (MainWindow 析构函数):
m_sensorThread->requestInterruption();   // 设标志
m_sensorThread->wait(3000);              // 最多等3秒线程结束
```

`wait()` 确保线程真的退出后再销毁窗口，否则线程可能访问已销毁的对象导致崩溃。

---

## 9. 模块详解⑤ 自定义控件 speed_gauge

标准控件里没有"汽车速度表盘"，Qt 的解法是**继承 QWidget，自己画**——重写 `paintEvent()`。

### 9.1 核心模式：数据成员 + update() + paintEvent()

```cpp
class SpeedGauge : public QWidget {
    float m_speed = 0, m_max = 120;
public:
    void setSpeed(float kmh) { m_speed = kmh; update(); }   // ① 外部设置数据
protected:
    void paintEvent(QPaintEvent *) override {                // ② Qt 回调绘制
        QPainter p(this);
        // 根据 m_speed 画表盘...
    }
};
```

这是自定义控件的**通用三段式**：

1. 提供 setter 接收数据；
2. `update()` 请求重绘（不是立即画，而是告诉 Qt "我脏了，下次绘制周期重画我"）；
3. Qt 在合适时机回调 `paintEvent()`，在这里用 `QPainter` 真正画。

**为什么不直接在 setter 里画？** 因为绘制必须在 `paintEvent` 里做——窗口被遮挡再露出、最小化再恢复时，Qt 都会回调 `paintEvent` 重画。把绘制逻辑集中在这里，任何场景下画面都正确。

### 9.2 QPainter 绘图基础

`QPainter` 是画笔对象，在 `paintEvent` 里构造并绑定到本控件：

```cpp
QPainter p(this);
p.setRenderHint(QPainter::Antialiasing);   // 抗锯齿，画弧线更平滑
p.setPen(QPen(color, width));              // 设线条颜色/粗细
p.setBrush(color);                         // 设填充色
p.drawArc(...);      // 弧线
p.drawLine(x1,y1,x2,y2);
p.drawEllipse(...);  // 圆
p.drawText(rect, flags, text);
```

### 9.3 表盘分解

本项目的表盘画了五层，由底到顶：

| 层    | 代码                            | 说明                |
| ---- | ----------------------------- | ----------------- |
| 背景弧  | `drawArc(..., 45*16, 270*16)` | 270° 深灰圆弧（仪表量程范围） |
| 彩色分段 | 4 段 `drawArc`                 | 绿→橙→红，速度越高颜色越警示   |
| 刻度线  | 循环 13 次 `drawLine`            | 用三角函数算每条刻度的端点坐标   |
| 指针   | 1 条 `drawLine` + 中心圆点         | 角度随 `m_speed` 变化  |
| 数字   | 2 次 `drawText`                | 大号速度值 + "km/h"    |

**角度换算**是表盘绘制的关键。Qt 的 `drawArc` 角度单位是 **1/16 度**（所以要 `*16`），且 0° 在 3 点钟方向逆时针。本项目把表盘设计成 225° 起始、扫过 270°：

```cpp
double ang = (225 - m_speed/m_max*270) * M_PI / 180.0;  // 速度→角度→弧度
int nx = cx + (r-40)*cos(ang);     // 极坐标→直角坐标
int ny = cy - (r-40)*sin(ang);
p.drawLine(cx, cy, nx, ny);        // 从圆心画指针
```

速度 0 → 指针在 225°（左下），速度满量程 → 转过 270° 到右下，正是真实汽车仪表的样式。

### 9.4 数据→表盘 的完整链路（先睹为快）

```
gps_daemon 发 GpsData 帧
  → DaemonClient 解帧 → messageReceived
  → MainWindow::onDaemonMessage
  → m_gauge->setSpeed(g->speed)     ← setter
  → update() 请求重绘
  → Qt 回调 paintEvent()
  → 按新速度重算指针角度并重画
  → 用户看到指针动了
```

这就是"数据驱动 UI"的最典型例子，第 12 章会系统展开。

---

## 10. 模块详解⑥ 界面布局 mainwindow.ui

### 10.1 .ui 文件是什么

`.ui` 是 Qt Designer（可视化设计器）生成的 **XML 界面描述文件**。你在 Designer 里拖控件、摆位置，它保存成 XML；构建时 **uic 工具**把它编译成 C++ 代码 `ui_mainwindow.h`。

好处：**界面设计与代码分离**。改布局只需编辑 .ui，不碰业务代码。

### 10.2 本项目的界面结构

```
MainWindow (1024×600, 车载大屏)
├── centralwidget
│   └── tabWidget (QTabWidget, 5个标签页)
│       ├── tabDashboard  "仪表盘"   (空白, 代码里动态加表盘)
│       ├── tabDVR        "DVR"
│       │     ├── dvrPreview  (QLabel 显示摄像头图片)
│       │     ├── dvrDuration (QLabel 录像时长)
│       │     ├── btnRecord / btnPause / btnStop / btnSnap (4个按钮)
│       │     └── dvrStatus   (QLabel 状态文字)
│       ├── tabMusic      "音乐"
│       │     ├── musicTitle  (QLabel 曲目名)
│       │     ├── btnPlay / btnPauseMusic
│       │     └── volSlider   (QSlider 0-100)
│       ├── tabCAN        "CAN"
│       │     ├── canLog      (QTextEdit 滚动日志)
│       │     ├── canId       (QSpinBox 十六进制)
│       │     ├── canData     (QLineEdit)
│       │     └── btnCanSend
│       └── tabSensor     "温湿度"
│             ├── sensorTemp  (QLabel 72px 大字体)
│             └── sensorHum   (QLabel 72px 大字体)
└── statusbar (底部状态栏)
```

### 10.3 控件命名约定 → 自动槽连接（重要机制）

注意按钮的命名：`btnRecord`、`btnPause`、`btnStop`……再看 MainWindow 里的槽函数名：

```cpp
void on_btnRecord_clicked();
void on_btnPause_clicked();
```

这不是巧合！`ui->setupUi(this)` 会**自动扫描**名为 `on_<控件名>_<信号名>` 的槽并连接。所以：

> 控件叫 `btnRecord`，你写一个 `on_btnRecord_clicked()` 槽，**不需要手动 connect**，点击就自动生效。

本项目 7 个按钮/滑块槽全靠这个约定连接。这是 Qt 的"约定优于配置"设计。

### 10.4 代码里如何访问 .ui 控件

```cpp
#include "ui_mainwindow.h"     // uic 生成的头文件

namespace Ui { class MainWindow; }

MainWindow::MainWindow(...) : ui(new Ui::MainWindow) {
    ui->setupUi(this);         // 根据 .ui 创建所有控件并挂到窗口上
    ui->dvrDuration->setText("⏱ 00:00");   // 之后通过 ui->控件名 访问
}
```

`ui->xxx` 是贯穿整个 mainwindow.cpp 的访问模式，看到它就知道是在操作 .ui 里定义的控件。

---

## 11. 模块详解⑦ 主窗口 mainwindow

`MainWindow` 是总装配车间，也是整个模块的"大脑"。构造函数做了六件事，逐一解析。

### 11.1 构造函数逐段解析

**① 加载界面 + 全局样式**

```cpp
ui->setupUi(this);          // 创建 .ui 里定义的全部控件
setStyleSheet(...);          // 标签页选中高亮等样式
```

**② 装配仪表盘页（.ui 里留白，代码动态填充）**

```cpp
auto *dashLayout = new QVBoxLayout(ui->tabDashboard);
m_gauge = new SpeedGauge(this);      // 自定义速度表盘
dashLayout->addWidget(m_gauge);
m_satsLabel = new QLabel("🛰 0  |  未定位");
dashLayout->addWidget(m_satsLabel);
```

为什么仪表盘页在 .ui 里是空的？因为 `SpeedGauge` 是自定义类，Designer 不认识它，只能代码创建。这是 Designer 与代码混合使用的常见模式。

**③ 建立 5 条守护进程连接（`connectDaemons()`）**

```cpp
void MainWindow::connectDaemons()
{
    struct { QString name; QString path; } ds[] = {
        {"gps","/tmp/car_gps.sock"}, {"input","/tmp/car_input.sock"},
        {"can","/tmp/car_can.sock"}, {"av","/tmp/car_av.sock"},
        {"dvr","/tmp/car_dvr.sock"},
    };
    for (auto &d : ds) {
        auto *c = new DaemonClient(d.name, d.path, this);
        c->setObjectName(d.name);    // 起名，之后按名字找客户端
        // 信号接线:
        connect(c, &DaemonClient::connected,    this, [=](const QString&){ m_connected++; ... });
        connect(c, &DaemonClient::disconnected, this, [=](const QString&){ m_connected--; });
        QString name = d.name;
        connect(c, &DaemonClient::messageReceived, this,
                [=](uint8_t t, const QByteArray &d){ onDaemonMessage(name, t, d); });
        m_clients.append(c);
        c->connectToServer();
    }
}
```

三个要点：

- **表驱动**：用一个数组描述 5 条连接，循环创建，加一条连接只需加一行表项。
- **lambda 表达式**：`[=](...)` 是 C++11 匿名函数，`[=]` 表示按值捕获外部变量。Qt5 的信号槽可以直接连 lambda，省得为每个小逻辑写一个槽函数。
- **消息汇聚**：5 个客户端的 `messageReceived` 都连到同一个 `onDaemonMessage`，并带上各自的 `name` 标识来源——这就是"消息总线"式的分发设计。

**④ 启动三个定时器**（预览刷新 / 录像时长 / 曲目名轮询，见 2.8 节模式）

**⑤ 启动传感器线程**（见第 8 章）

### 11.2 消息分发中心 `onDaemonMessage()`（全模块最关键的函数）

所有上行数据的唯一入口，按 `(来源名, 消息类型)` 分发：

```cpp
void MainWindow::onDaemonMessage(const QString &name, uint8_t msgType, const QByteArray &data)
{
    /* ---- GPS ---- */
    if (name == "gps" && msgType == MSG_GPS_DATA) {
        if (data.size() < sizeof(GpsData)) return;               // 防御: 长度校验
        const GpsData *g = (const GpsData*)data.constData();     // 字节流→结构体
        m_gauge->setSpeed(g->speed);                             // ★数据→UI★
        m_satsLabel->setText(...);
    }

    /* ---- DVR 状态 ---- */
    if (name == "dvr" && msgType == MSG_DVR_STATUS) { ... }

    /* ---- CAN 数据 ---- */
    if (name == "can" && msgType == MSG_CAN_DATA) { ... }

    /* ---- 物理按键 ---- */
    if (name == "input" && msgType == MSG_KEY_EVENT) { ... }
}
```

**字节流转结构体的两种手法**（本项目都用了）：

手法一：**直接指针转换**（结构体整体传输时）：

```cpp
const GpsData *g = (const GpsData*)data.constData();
g->speed;   // 直接访问成员
```

零拷贝，前提是两端结构体布局一致（靠 `#pragma pack(1)` 保证）。

手法二：**逐字节拼装**（字段散排时，如 DVR 状态）：

```cpp
uint32_t elapsed = (uint8_t)data[1] | ((uint8_t)data[2] << 8)
                 | ((uint8_t)data[3] << 16) | ((uint8_t)data[4] << 24);
```

手动按小端序拼整数，逐字段提取，更灵活。

### 11.3 DVR 录像状态机（业务逻辑示例）

UI 需要跟踪录像状态来决定按钮的可用性——这是典型的**状态机**：

```
        开始录像              暂停
IDLE(0) ────────► RECORDING(1) ────────► PAUSED(2)
  ▲                    │                     │
  │        停止         │        继续录像       │
  ├────────────────────┴─────────────────────┘
  ◄─────────────────────停止────────────────────
```

三个状态对应三套界面：

| 状态    | btnRecord    | btnPause | btnStop | 时长标签 |
| ----- | ------------ | -------- | ------- | ---- |
| 0 待机  | "开始录像" 可用    | 禁用       | 禁用      | 灰色   |
| 1 录像中 | 禁用           | "暂停" 可用  | 可用      | 红色闪烁 |
| 2 已暂停 | "继续录像" 可用(绿) | "已暂停" 禁用 | 可用      | 橙色   |

`updateDvrButtons(state)` 是唯一的状态切换函数——**改状态必过此函数**，保证按钮/颜色/内部变量 `m_dvrState` 三者永远一致。这是状态机编码的关键纪律。

**双向同步**（一个精妙的设计）：

- 用户点按钮 → **乐观更新**：先假设成功，立即切状态（界面零延迟），同时发命令给后端；
- 后端每 2 秒回传 `MSG_DVR_STATUS` → **校准**：若后端真实状态与 UI 不一致（比如后端自己崩了/停了），以后端为准纠正。

这样既有即时响应，又不会"界面说在录，实际没录"。

### 11.4 物理按键分发（上下文感知路由）

开发板上有两个物理按键，input_daemon 把按键事件通过 `MSG_KEY_EVENT` 上报。同一个按键在不同页面要有不同功能：

```cpp
if (k->event_type == 1) {                       // 仅短按生效
    int tab = ui->tabWidget->currentIndex();    // 当前在哪个标签页?
    if (tab == 1) {                             // DVR 页
        if (k->key_id == 1) dvr->send(MSG_DVR_SNAPSHOT);  // KEY1=拍照
        if (k->key_id == 2) dvr->send(MSG_DVR_PAUSE);     // KEY2=暂停/继续
    } else if (tab == 2) {                      // 音乐页
        if (k->key_id == 1) av->send(MSG_AV_PLAY);        // KEY1=播放
        if (k->key_id == 2) av->send(MSG_AV_PAUSE);       // KEY2=暂停
    }
}
```

设计思想：**按键是全局资源，功能由当前上下文决定**。路由逻辑集中在 UI 层，后端完全不用关心"现在显示什么页面"。

### 11.5 下行控制槽函数

以"开始录像"为例：

```cpp
void MainWindow::on_btnRecord_clicked()
{
    for (auto *c : m_clients) {
        if (c->objectName() == "dvr") {          // 按名字找客户端
            if (m_dvrState == 2)      c->send(MSG_DVR_START);   // 暂停→继续
            else if (m_dvrState == 0) {
                m_dvrElapsedSec = 0;
                c->send(MSG_DVR_START);
                updateDvrButtons(1);             // 乐观更新状态
            }
        }
    }
}
```

模式固定：**找客户端 → 按当前状态决定发什么 → 发 → 乐观更新界面**。所有按钮槽（播放/暂停/拍照/停止）都是这个模式。

---

## 12. 核心专题：数据如何与 UI 绑定

这是你特别关心的部分，也是整个模块的灵魂。Qt 里没有"双向绑定框架"，**绑定的本质就是：在正确的时机，把数据写入控件的属性**。"正确的时机"由信号槽机制保证。

本节用项目里的三条真实链路，覆盖全部三种绑定模式。

### 12.1 绑定模式总览

```
模式A: 网络消息驱动     数据从socket来 → 解帧 → 直接写控件      (GPS速度表)
模式B: 跨线程信号驱动   工作线程读数据 → emit信号 → 主线程写控件 (温湿度)
模式C: 定时器轮询驱动   定时器周期性读文件/状态 → 写控件          (预览画面)
```

三种模式的共同终点都是同一个动作：**调用控件的 setter**（`setText` / `setPixmap` / `setSpeed` / `append`）。

### 12.2 案例一（模式A）：GPS 数据 → 速度表盘

**完整链路，逐跳追踪**：

```
第1跳: gps_daemon (C程序)
   填充 GpsData 结构体，按协议组帧，写入 /tmp/car_gps.sock
   字节序列: AA 55 10 [LEN] [结构体字节] 00 55

第2跳: QLocalSocket (Qt框架)
   内核通知有数据 → Qt 事件循环检测到 → 发出 readyRead 信号

第3跳: DaemonClient::onReadyRead()
   m_recvBuffer.append(m_socket->readAll());
   while (m_proto.unpack(...)) emit messageReceived(MSG_GPS_DATA, data);

第4跳: MainWindow::onDaemonMessage("gps", MSG_GPS_DATA, data)
   const GpsData *g = (const GpsData*)data.constData();
   m_gauge->setSpeed(g->speed);          // ← 数据写入控件 ★绑定点★
   m_satsLabel->setText(...);            // ← 卫星信息写入标签

第5跳: SpeedGauge::setSpeed()
   m_speed = kmh;   // 存进成员变量
   update();        // 请求重绘(不是立即画)

第6跳: Qt 框架
   下一个绘制周期回调 paintEvent()

第7跳: SpeedGauge::paintEvent()
   按新的 m_speed 计算指针角度，重画整个表盘

第8跳: 屏幕
   用户看到指针转到新位置
```

**关键理解**：数据不是"绑定"在控件上的，而是**每来一条新数据，就主动推给控件一次**。这种"数据推送→控件更新"的模式叫**命令式更新**。控件自己不知道数据从哪来，MainWindow 也不关心控件内部怎么画——双方通过 setter 这一个接触点解耦。

**防御性细节**：`if (data.size() < sizeof(GpsData)) return;` ——永远先校验数据长度再转换，防止后端发了短数据导致越界读取。

### 12.3 案例二（模式B）：温湿度 → 大字体标签（跨线程绑定）

```
第1跳: SensorThread::run() [工作线程]
   read(/dev/mydht11) 读到 2 字节: [湿度][温度]

第2跳: emit dataReady(temp, humi)  [工作线程发出]
   ★ 跨线程边界 ★
   Qt 检测到接收者在主线程 → 不直接调用槽，
   而是把 (temp, humi) 打包成事件，塞进主线程事件队列

第3跳: 主线程的 app.exec() 事件循环
   从队列取出事件 → 执行已连接的槽

第4跳: lambda 槽函数 [主线程执行]
   ui->sensorTemp->setText(QString("%1°C").arg(t));  // ← 绑定点★
   ui->sensorHum->setText(QString("%1%").arg(h));

第5跳: QLabel 内部
   setText 触发标签重绘 → 显示 "25°C" "60%"
```

**为什么不能在工作线程里直接 `setText`？**
Qt 的所有 UI 控件都只能在创建它的线程（主线程）里操作。工作线程直接碰控件属于未定义行为，轻则界面错乱，重则崩溃。跨线程信号是 Qt 提供的**唯一正确通道**。

注意 `setLabels()` 存了控件指针但 `run()` 里根本没用它——这是历史遗留接口，实际数据全走信号。这提醒我们：**数据通道要唯一且明确**，本项目选择了信号这条通道。

### 12.4 案例三（模式C）：摄像头预览 → 图片标签（轮询绑定）

DVR 后端把摄像头画面每秒写成一张 `/tmp/dvr_preview.jpg`。Qt 端怎么显示？用文件轮询：

```cpp
// 构造函数: 每秒触发一次
m_previewTimer->start(1000);

// 槽函数:
void MainWindow::refreshPreview()
{
    QPixmap img("/tmp/dvr_preview.jpg");      // 读图片文件
    if (!img.isNull()) {
        ui->dvrPreview->setPixmap(             // ← 绑定点★
            img.scaled(640, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}
```

`QLabel` 既能显示文字（`setText`）也能显示图片（`setPixmap`）。每秒重读文件、缩放、贴上——界面就"动"起来了。

**为什么用文件轮询而不是 socket？** 一帧 JPEG 几十 KB，走协议帧浪费且复杂；写文件+读文件是嵌入式下简单可靠的图像传输通道。这是**务实的架构取舍**：低频大数据用文件，高频小数据用 socket。

### 12.5 反向绑定：用户输入 → 数据（控制方向）

绑定是双向的。用户操作控件 → 读取控件值 → 发给后端：

```cpp
void MainWindow::on_volSlider_valueChanged(int v)   // v 就是滑块当前值
{
    // 滑块一动，Qt 立即把新值作为参数传进来 ← 反向绑定点★
    FILE *fp = fopen("/tmp/av_volume", "w");
    if (fp) { fprintf(fp, "%d", v); fclose(fp); }   // 持久化
    QByteArray d; d.append((char)v);
    avClient->send(MSG_AV_VOLUME, d);               // 发给后端
}
```

`valueChanged(int)` 信号自带新值参数——控件主动把状态变化推给代码，与数据方向的 `setText` 正好镜像。

### 12.6 绑定机制总结表

| 数据源   | 触发机制                | 数据通道          | 控件更新方式                        | 实例               |
| ----- | ------------------- | ------------- | ----------------------------- | ---------------- |
| 后端消息  | `readyRead` 信号      | socket→解帧→结构体 | `setSpeed`/`setText`/`append` | GPS、DVR状态、CAN、按键 |
| 传感器线程 | `dataReady` 信号(跨线程) | 信号参数直接带值      | `setText`                     | 温湿度              |
| 文件系统  | `QTimer` 1秒轮询       | 读文件           | `setPixmap`/`setText`         | 预览、曲目名           |
| 用户操作  | 控件事件信号              | 信号参数带控件当前值    | 不需要更新（它自己是源头）                 | 滑块、按钮            |

**一句话总结**：

> Qt 的数据-UI 绑定 = **信号槽保证"更新动作在正确的时机、正确的线程执行"，控件的 setter 完成"数据到像素"的最后一步**。所有链路殊途同归：数据 → (某种事件机制) → 主线程槽函数 → 控件 setter → 重绘。

---

## 13. 构建系统

### 13.1 .pro 文件解读

```qmake
QT       += core gui widgets network   # 依赖模块
TARGET    = car_ui                     # 生成可执行文件名
TEMPLATE  = app                        # 应用程序(区别于 lib)
CONFIG   += c++11                      # 启用 C++11 (lambda 需要)

SOURCES += main.cpp mainwindow.cpp protocol_handler.cpp daemon_client.cpp sensor_thread.cpp
HEADERS += mainwindow.h protocol_handler.h daemon_client.h pages/speed_gauge.h sensor_thread.h
FORMS   += mainwindow.ui               # uic 会处理它
```

依赖模块说明：

- `core`：QObject、信号槽、QByteArray、QTimer 等基础
- `gui`：QPainter、字体、图像
- `widgets`：按钮、标签等所有控件
- `network`：QLocalSocket（注意它属于 network 模块！）

### 13.2 完整构建流程

```
qmake
  │ ① uic: mainwindow.ui ──► ui_mainwindow.h (界面代码)
  │ ② moc: 扫描 Q_OBJECT 头文件 ──► moc_mainwindow.cpp 等 (信号槽实现)
  │ ③ 生成 Makefile
  ▼
make
  │ g++ 编译: 所有 .cpp + moc_*.cpp
  │ 链接 Qt 库
  ▼
car_ui (可执行文件)
```

### 13.3 运行方式

```bash
./car_ui                            # 窗口模式(开发调试)
./car_ui -fullscreen                # 全屏(车载)
./car_ui -fullscreen -platform linuxfb   # 无X11, 直写framebuffer(实机部署)
```

---

## 14. 总结与进阶学习建议

### 14.1 一图回顾整个模块

```
用户触摸 ──► 按钮槽函数 ──► DaemonClient.send() ──► pack()组帧 ──► socket ──► 后端
                                                                        │
后端 ──► socket ──► readyRead ──► 累积缓冲 ──► 状态机拆帧 ──► messageReceived
                                                              │
                                        ┌─────────────────────┤
                                        ▼                     ▼
                              onDaemonMessage 分发      跨线程信号(传感器)
                                        │                     │
                                        ▼                     ▼
                                控件 setter (setText/setPixmap/setSpeed)
                                        │
                                        ▼
                                   update() → paintEvent → 屏幕
```

### 14.2 本项目教会我们的 7 个设计思想

1. **分层解耦**：协议/通信/协调/展示四层，各管各的。
2. **事件驱动**：没有死循环，一切靠信号回调。
3. **对象树内存管理**：parent-child 自动释放，几乎不写 delete。
4. **状态机管理业务**：DVR 三态，改状态必过统一函数。
5. **乐观更新 + 后端校准**：界面即时响应，状态最终一致。
6. **跨线程只走信号**：工作线程干活，主线程碰界面。
7. **约定优于配置**：`on_控件名_信号名` 自动连接，表驱动批量建连。

### 14.3 建议的阅读顺序（如果你要重新走一遍代码）

1. `mainwindow.ui` ——先看界面长什么样、有哪些控件
2. `main.cpp` ——程序怎么启动
3. `protocol_handler.h` ——协议契约（宏定义+结构体）
4. `daemon_client.cpp` ——一条连接的完整生命周期
5. `mainwindow.cpp::connectDaemons` + `onDaemonMessage` ——数据怎么流进来、怎么分发
6. 挑一个按钮槽（如 `on_btnRecord_clicked`）——控制怎么流出去
7. `speed_gauge.h` 和 `sensor_thread.cpp` ——两个特色模块

### 14.4 动手练习（由易到难）

1. **入门**：在状态栏显示传感器线程的读取次数（提示：加计数器 + `dataReady` 槽里自增）。
2. **进阶**：给仪表盘页加一个"海拔高度"标签，从 `GpsData.altitude` 取数。
3. **挑战**：新增一个"网络状态"标签页，定义新的消息类型，走通完整链路（宏定义→后端模拟发送→onDaemonMessage 分发→新控件显示）。

做完第 3 题，你就完全掌握了这个模块从协议到像素的全部链路。

---

*文档基于项目当前代码生成。建议配合源码阅读，遇到不解处用 Qt Assistant 或官方文档查询类名（如 QLocalSocket、QPainter）可获得最权威说明。*
