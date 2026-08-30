/**
 * @file    mainwindow.cpp
 * @brief   智能车载终端 Qt UI — 主窗口实现
 *
 * ============================================================================
 * DVR 录像状态机 (m_dvrState)
 * ============================================================================
 *   State 0 — IDLE (待机):      初始状态, 停止后回到此状态。
 *                                btnRecord="开始录像", Pause/Stop 禁用。
 *   State 1 — RECORDING (录像中): 正在录制, m_durationTimer 每秒自增
 *                                m_dvrElapsedSec。时长标签红/深红交替闪烁。
 *   State 2 — PAUSED (已暂停):  录像暂停, m_dvrElapsedSec 冻结。
 *                                btnRecord="继续录像"(绿色), 时长标签橙色。
 *
 *   状态转换:
 *     IDLE  --[开始录像]--> RECORDING
 *     RECORDING --[暂停]--> PAUSED
 *     PAUSED --[继续录像]--> RECORDING
 *     RECORDING/PAUSED --[停止]--> IDLE (elapsed 归零, UI 重置)
 *
 *   daemon 通过 MSG_DVR_STATUS 回传实际状态和已录制时长, UI 据此同步。
 *
 * DVR 控制消息:
 *   - MSG_DVR_START   (0x20): 开始/继续录像
 *   - MSG_DVR_STOP    (0x21): 停止录像, 关闭文件
 *   - MSG_DVR_STATUS  (0x22): daemon → UI 状态回传
 *   - MSG_DVR_SNAPSHOT(0x23): 保存当前帧为 JPEG 到 /record/
 *   - MSG_DVR_PAUSE   (0x26): 暂停/恢复录像
 *
 * ============================================================================
 * 物理按键分发 (Key Dispatch)
 * ============================================================================
 *   input 守护进程上报物理按键 (MSG_KEY_EVENT), 事件类型:
 *     1=短按, 2=长按, 3=双击 (KeyEvent.event_type)
 *
 *   根据当前活跃 Tab (ui->tabWidget->currentIndex()) 分发:
 *     Tab 1 (DVR):   KEY1 → 拍照      (MSG_DVR_SNAPSHOT)
 *                    KEY2 → 暂停/继续  (MSG_DVR_PAUSE)
 *     Tab 2 (Music): KEY1 → 播放      (MSG_AV_PLAY)
 *                    KEY2 → 暂停      (MSG_AV_PAUSE)
 *
 *   仅短按触发功能; 长按和双击仅显示在 statusbar 用于诊断。
 *   所有按键事件都会在 statusbar 显示 3 秒的诊断信息。
 *
 * ============================================================================
 * CAN 消息格式 (can_msg_t = 15 bytes)
 * ============================================================================
 *   struct CanMsg {              // 对应字节偏移:
 *       uint32_t can_id;         // [0..3]   little-endian
 *       uint8_t  can_dlc;        // [4]      数据长度 (0-8)
 *       uint8_t  data[8];        // [5..12]  CAN 数据字段
 *       uint8_t  is_extended;    // [13]     扩展帧标志
 *       uint8_t  is_remote;      // [14]     远程帧标志
 *   };
 *
 *   发送时从 UI 控件读取 ID 和十六进制数据, 组装为 15 字节。
 *   不足 15 字节时尾部用 0x00 填充, 确保 daemon 收到的帧格式一致。
 *
 * ============================================================================
 * 已移除的功能
 * ============================================================================
 *   - LED 轮询:     原通过 QTimer 周期查询 daemon 的 LED 状态 (MSG_LED_CONTROL),
 *                    因项目不需要已移除相关定时器和处理代码。
 *   - 输入设备 Tab:  原 UI 有独立的"输入"页面用于显示按键事件,
 *                    已移除; 物理按键改为根据当前活跃 Tab 自动分发到 DVR/Music,
 *                    诊断信息改在 statusbar 显示。
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "protocol_handler.h"
#include <QVBoxLayout>
#include <QPixmap>
#include <QFile>
#include <QDebug>
#include <cmath>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow),
      m_connected(0), m_dvrState(0), m_dvrElapsedSec(0)
{
    ui->setupUi(this);
    setStyleSheet("QMainWindow{background:#f5f5f5;} QTabBar::tab{padding:10px 20px; font-size:16px; font-weight:bold;} QTabBar::tab:selected{background:#2196F3; color:white;}");

    /* 仪表盘: 速度表 */
    auto *dashLayout = new QVBoxLayout(ui->tabDashboard);
    m_gauge = new SpeedGauge(this);
    dashLayout->addWidget(m_gauge);
    m_satsLabel = new QLabel("🛰 0  |  未定位");
    m_satsLabel->setAlignment(Qt::AlignCenter);
    m_satsLabel->setStyleSheet("color:#888; font-size:14px;");
    dashLayout->addWidget(m_satsLabel);

    connectDaemons();

    /* DVR 预览定时器: 每秒刷新 */
    m_previewTimer = new QTimer(this);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::refreshPreview);
    m_previewTimer->start(1000);
    refreshPreview();

    /* DVR 时长定时器: 每秒更新 */
    m_durationTimer = new QTimer(this);
    connect(m_durationTimer, &QTimer::timeout, this, &MainWindow::updateDvrDuration);
    m_durationTimer->start(1000);
    ui->dvrDuration->setText("⏱ 00:00");

    /* 音乐曲目名 (读取 daemon 写入的 /tmp/av_track) */
    QTimer *trackTimer = new QTimer(this);
    connect(trackTimer, &QTimer::timeout, this, [=]() {
        QFile f("/tmp/av_track");
        if (f.open(QIODevice::ReadOnly)) {
            QString name = QString::fromUtf8(f.readAll()).trimmed();
            if (!name.isEmpty()) ui->musicTitle->setText(name);
            f.close();
        }
    });
    trackTimer->start(1000);

    /* 温湿度: SensorThread 独占用 /dev/mydht11 */
    m_sensorThread = new SensorThread;
    m_sensorThread->setLabels(ui->sensorHum, ui->sensorTemp);
    connect(m_sensorThread, &SensorThread::dataReady, this,
            [=](int t, int h) {
                ui->sensorTemp->setText(QString("%1°C").arg(t));
                ui->sensorHum->setText(QString("%1%").arg(h));
            });
    m_sensorThread->start();

}


MainWindow::~MainWindow() {
    m_durationTimer->stop();
    m_sensorThread->requestInterruption();
    m_sensorThread->wait(3000);
    delete ui;
}

/* ================================================================
 *  DVR 预览
 * ================================================================ */
void MainWindow::refreshPreview()
{
    QPixmap img("/tmp/dvr_preview.jpg");
    if (!img.isNull()) {
        ui->dvrPreview->setPixmap(img.scaled(640, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

/* ================================================================
 *  DVR 时长更新 (每秒触发)
 *
 * 仅在 RECORDING 状态时自增 m_dvrElapsedSec。
 * PAUSED/IDLE 状态保持当前值不变。
 *
 * 显示格式:
 *   < 1 小时: "⏱ MM:SS"
 *   >= 1 小时: "⏱ HH:MM:SS"
 *
 * 视觉反馈:
 *   RECORDING: 红/深红交替闪烁 (每秒切换, 模拟录制指示灯)
 *   PAUSED:    橙色背景
 *   IDLE:      灰色背景
 * ================================================================ */
void MainWindow::updateDvrDuration()
{
    if (m_dvrState == 1) {
        /* 录像中: 自增 */
        m_dvrElapsedSec++;
    }
    /* 已暂停 / 待机: 保持不变 */

    uint32_t hh = m_dvrElapsedSec / 3600;
    uint32_t mm = (m_dvrElapsedSec % 3600) / 60;
    uint32_t ss = m_dvrElapsedSec % 60;

    if (hh > 0)
        ui->dvrDuration->setText(QString("⏱ %1:%2:%3")
            .arg(hh, 2, 10, QChar('0'))
            .arg(mm, 2, 10, QChar('0'))
            .arg(ss, 2, 10, QChar('0')));
    else
        ui->dvrDuration->setText(QString("⏱ %1:%2")
            .arg(mm, 2, 10, QChar('0'))
            .arg(ss, 2, 10, QChar('0')));

    /* 录像中闪烁红点 */
    if (m_dvrState == 1) {
        ui->dvrDuration->setStyleSheet(
            (ss % 2) ?
                "font-size:20px; font-weight:bold; color:#fff; background:#F44336; border-radius:4px; padding:4px 12px;" :
                "font-size:20px; font-weight:bold; color:#fff; background:#c62828; border-radius:4px; padding:4px 12px;");
    } else if (m_dvrState == 2) {
        ui->dvrDuration->setStyleSheet(
            "font-size:20px; font-weight:bold; color:#fff; background:#FF9800; border-radius:4px; padding:4px 12px;");
    } else {
        ui->dvrDuration->setStyleSheet(
            "font-size:20px; font-weight:bold; color:#fff; background:#333; border-radius:4px; padding:4px 12px;");
    }
}

/* ================================================================
 *  DVR 按钮状态管理
 *
 * 根据 DVR 状态机切换三个控制按钮的启用状态、文字和颜色:
 *
 *   State 0 (IDLE):
 *     btnRecord: "开始录像" (红色, 启用)
 *     btnPause:  "暂停"     (橙色, 禁用)
 *     btnStop:   "停止"     (禁用)
 *
 *   State 1 (RECORDING):
 *     btnRecord: "开始录像" (禁用 — 防止重复开始)
 *     btnPause:  "暂停"     (橙色, 启用)
 *     btnStop:   "停止"     (启用)
 *
 *   State 2 (PAUSED):
 *     btnRecord: "继续录像" (绿色, 启用)
 *     btnPause:  "已暂停"   (灰色, 禁用)
 *     btnStop:   "停止"     (启用)
 * ================================================================ */
void MainWindow::updateDvrButtons(int state)
{
    m_dvrState = state;
    switch (state) {
    case 0: /* 待机 */
        ui->btnRecord->setText("开始录像");
        ui->btnRecord->setStyleSheet("background:#F44336; color:white; font-size:16px; font-weight:bold; border-radius:6px;");
        ui->btnRecord->setEnabled(true);
        ui->btnPause->setText("暂停");
        ui->btnPause->setEnabled(false);
        ui->btnPause->setStyleSheet("background:#FF9800; color:white; font-size:16px; font-weight:bold; border-radius:6px;");
        ui->btnStop->setEnabled(false);
        break;
    case 1: /* 录像中 */
        ui->btnRecord->setText("开始录像");
        ui->btnRecord->setEnabled(false);
        ui->btnPause->setText("暂停");
        ui->btnPause->setEnabled(true);
        ui->btnPause->setStyleSheet("background:#FF9800; color:white; font-size:16px; font-weight:bold; border-radius:6px;");
        ui->btnStop->setEnabled(true);
        break;
    case 2: /* 已暂停 */
        ui->btnRecord->setText("继续录像");
        ui->btnRecord->setStyleSheet("background:#4CAF50; color:white; font-size:16px; font-weight:bold; border-radius:6px;");
        ui->btnRecord->setEnabled(true);
        ui->btnPause->setText("已暂停");
        ui->btnPause->setEnabled(false);
        ui->btnPause->setStyleSheet("background:#888; color:white; font-size:16px; font-weight:bold; border-radius:6px;");
        ui->btnStop->setEnabled(true);
        break;
    }
}

/* ================================================================
 *  守护进程连接
 *
 * 创建 5 个 DaemonClient 实例，分别连接:
 *   gps   → /tmp/car_gps.sock   (GPS 定位数据)
 *   input → /tmp/car_input.sock (物理按键事件)
 *   can   → /tmp/car_can.sock   (CAN 总线消息)
 *   av    → /tmp/car_av.sock    (音视频播放控制)
 *   dvr   → /tmp/car_dvr.sock   (行车记录仪控制)
 *
 * 每个客户端注册三个信号:
 *   connected:     自增 m_connected 计数器, 更新状态栏 "连接: N/5"
 *   disconnected:  自减计数器 (daemon_client 内部自动重连)
 *   messageReceived: 统一路由到 onDaemonMessage(name, msgType, data)
 * ================================================================ */
void MainWindow::connectDaemons()
{
    struct { QString name; QString path; } ds[] = {
        {"gps","/tmp/car_gps.sock"},
        {"input","/tmp/car_input.sock"}, {"can","/tmp/car_can.sock"},
        {"av","/tmp/car_av.sock"}, {"dvr","/tmp/car_dvr.sock"},
    };
    for (auto &d : ds) {
        auto *c = new DaemonClient(d.name, d.path, this);
        c->setObjectName(d.name);
        connect(c, &DaemonClient::connected, this, [=](const QString&){ m_connected++; ui->statusbar->showMessage(QString("连接: %1/5").arg(m_connected)); });
        connect(c, &DaemonClient::disconnected, this, [=](const QString&){ m_connected--; });
        QString name = d.name;
        connect(c, &DaemonClient::messageReceived, this, [=](uint8_t t, const QByteArray &d){ onDaemonMessage(name, t, d); });
        m_clients.append(c);
        c->connectToServer();
    }
}

/* ================================================================
 *  消息处理 — 统一入口
 *
 * 根据 (name, msgType) 组合分发到对应处理逻辑:
 *
 *   gps   + MSG_GPS_DATA   → 解析 GpsData 结构体, 更新速度表和卫星信息
 *   dvr   + MSG_DVR_STATUS → 解析 DVR 状态字节流, 同步 state/elapsed/resolution/frames
 *   can   + MSG_CAN_DATA   → 解析 CAN ID + DLC + 数据, 追加到 CAN 日志
 *   input + MSG_KEY_EVENT  → 解析 KeyEvent, 根据当前 Tab 分发到 DVR 或 Music
 *
 * DVR 状态同步逻辑 (MSG_DVR_STATUS):
 *   数据格式 (≥13 bytes):
 *     [0]      state     (uint8)      — 0=IDLE, 1=RECORDING, 2=PAUSED
 *     [1..4]   elapsed   (uint32 LE)  — 已录制秒数
 *     [5..6]   width     (uint16 LE)  — 视频宽度
 *     [7..8]   height    (uint16 LE)  — 视频高度
 *     [9..12]  frames    (uint32 LE)  — 已录制帧数
 *
 *   若 daemon 端 state 与 UI 不一致, 调用 updateDvrButtons 同步。
 *   若 daemon 端 state==0 而 UI 仍为非 0, 说明 daemon 端已停止,
 *   重置 elapsed 并回到 IDLE。
 *
 * 按键分发逻辑 (MSG_KEY_EVENT):
 *   见文件头 Key Dispatch 文档。所有按键在 statusbar 显示 3 秒诊断。
 * ================================================================ */
void MainWindow::onDaemonMessage(const QString &name, uint8_t msgType, const QByteArray &data)
{
    /* ---- GPS ---- */
    if (name == "gps" && msgType == MSG_GPS_DATA) {
        if (data.size() < (int)sizeof(GpsData)) return;
        const GpsData *g = (const GpsData*)data.constData();
        m_gauge->setSpeed(g->speed);
        QString fix = g->fix_quality==2?"3D":g->fix_quality==1?"2D":"NO";
        m_satsLabel->setText(QString("🛰 %1  |  %2  |  %3:%4:%5 UTC")
            .arg(g->satellites).arg(fix).arg(g->hour,2,10,QChar('0')).arg(g->min,2,10,QChar('0')).arg(g->sec,2,10,QChar('0')));
    }

    /* ---- DVR 状态 ---- */
    if (name == "dvr" && msgType == MSG_DVR_STATUS) {
        if (data.size() >= 13) {
            uint8_t  state   = (uint8_t)data[0];
            uint32_t elapsed = (uint8_t)data[1] | ((uint8_t)data[2] << 8)
                             | ((uint8_t)data[3] << 16) | ((uint8_t)data[4] << 24);
            uint16_t width   = (uint8_t)data[5] | ((uint8_t)data[6] << 8);
            uint16_t height  = (uint8_t)data[7] | ((uint8_t)data[8] << 8);
            uint32_t frames  = (uint8_t)data[9] | ((uint8_t)data[10] << 8)
                             | ((uint8_t)data[11] << 16) | ((uint8_t)data[12] << 24);

            /* 同步状态和时长 */
            if (state != (uint8_t)m_dvrState) {
                updateDvrButtons((int)state);
            }
            if (state == 1 || state == 2) {
                m_dvrElapsedSec = elapsed;
            }
            if (state == 0 && m_dvrState != 0) {
                /* daemon 端停止 */
                m_dvrElapsedSec = 0;
                updateDvrButtons(0);
            }

            /* 状态文字 */
            const char *stStr = (state==1)?"● 录像中":(state==2)?"⏸ 已暂停":"待机";
            ui->dvrStatus->setText(QString("状态: %1  |  %2x%3  |  帧: %4  |  保存: /record/")
                .arg(stStr).arg(width).arg(height).arg(frames));
        }
    }

    /* ---- CAN ---- */
    if (name == "can" && msgType == MSG_CAN_DATA) {
        uint32_t id = (uint8_t)data[0]|((uint8_t)data[1]<<8)|((uint8_t)data[2]<<16)|((uint8_t)data[3]<<24);
        uint8_t dlc = (uint8_t)data[4];
        QString hex;
        for (int i=5; i<5+dlc && i<data.size(); i++) hex += QString("%1 ").arg((uint8_t)data[i],2,16,QChar('0'));
        ui->canLog->append(QString("<span style='color:#0f0;'>RX:</span> 0x%1 [%2]")
            .arg(id,3,16,QChar('0')).arg(hex.trimmed()));
    }

    /* ---- 按键 (根据当前 Tab 分发功能) ---- */
    if (name == "input" && msgType == MSG_KEY_EVENT) {
        if (data.size() >= (int)sizeof(KeyEvent)) {
            const KeyEvent *k = (const KeyEvent*)data.constData();

            /* 诊断: 任何按键事件都显示在 statusbar */
            const char *evt = k->event_type==1?"短按":k->event_type==2?"长按":"双击";
            ui->statusbar->showMessage(QString("[Key%1 %2] tab=%3")
                .arg(k->key_id).arg(evt).arg(ui->tabWidget->currentIndex()), 3000);

            /* 仅短按触发功能 */
            if (k->event_type == 1) {
                int tab = ui->tabWidget->currentIndex();
                DaemonClient *dvr = nullptr;
                DaemonClient *av  = nullptr;
                for (auto *c : m_clients) {
                    if (c->objectName() == "dvr") dvr = c;
                    if (c->objectName() == "av")  av  = c;
                }

                if (tab == 1) {
                    /* DVR: KEY1→拍照, KEY2→暂停/继续 */
                    if (k->key_id == 1 && dvr) {
                        dvr->send(MSG_DVR_SNAPSHOT);
                        ui->statusbar->showMessage("Key1 📷 拍照", 2000);
                    } else if (k->key_id == 2 && dvr) {
                        dvr->send(MSG_DVR_PAUSE);
                        ui->statusbar->showMessage("Key2 ⏯ 暂停/继续", 2000);
                    }
                } else if (tab == 2) {
                    /* 音乐: KEY1→播放, KEY2→暂停 */
                    if (k->key_id == 1 && av) {
                        av->send(MSG_AV_PLAY);
                        ui->statusbar->showMessage("Key1 ▶ 播放", 2000);
                    } else if (k->key_id == 2 && av) {
                        av->send(MSG_AV_PAUSE);
                        ui->statusbar->showMessage("Key2 ⏸ 暂停", 2000);
                    }
                }
            }
        }
    }
}

/* ================================================================
 *  DVR 按钮槽函数 — 通过 daemon 发送控制消息
 *
 * 四个按钮共享"DVR"守护进程连接 (objectName="dvr"):
 *   btnRecord: 开始/继续 → MSG_DVR_START
 *   btnPause:  暂停      → MSG_DVR_PAUSE
 *   btnStop:   停止      → MSG_DVR_STOP (elapsed 归零, 回 IDLE)
 *   btnSnap:   拍照      → MSG_DVR_SNAPSHOT
 *
 * 注意: 点击按钮后先乐观更新 UI 状态 (updateDvrButtons),
 *       daemon 随后通过 MSG_DVR_STATUS 回传实际状态同步。
 * ================================================================ */

/** @brief 开始录像 (IDLE→RECORDING) 或 继续录像 (PAUSED→RECORDING) */
void MainWindow::on_btnRecord_clicked()
{
    for (auto *c : m_clients) {
        if (c->objectName() == "dvr") {
            if (m_dvrState == 2) {
                /* 已暂停 → 发送"开始"表示继续 */
                c->send(MSG_DVR_START);
            } else if (m_dvrState == 0) {
                /* 待机 → 开始录像 */
                m_dvrElapsedSec = 0;
                c->send(MSG_DVR_START);
                updateDvrButtons(1);
            }
        }
    }
}

/** @brief 暂停录像 (RECORDING→PAUSED), 发送 MSG_DVR_PAUSE */
void MainWindow::on_btnPause_clicked()
{
    for (auto *c : m_clients) {
        if (c->objectName() == "dvr") {
            if (m_dvrState == 1) {
                /* 录像中 → 暂停 */
                c->send(MSG_DVR_PAUSE);
                updateDvrButtons(2);
            }
        }
    }
}

/** @brief 停止录像 (RECORDING/PAUSED→IDLE), elapsed 归零, 重置 UI */
void MainWindow::on_btnStop_clicked()
{
    for (auto *c : m_clients) {
        if (c->objectName() == "dvr") {
            c->send(MSG_DVR_STOP);
        }
    }
    m_dvrElapsedSec = 0;
    updateDvrButtons(0);
    ui->dvrDuration->setText("⏱ 00:00");
}

/** @brief 拍照: 请求 daemon 保存当前预览帧为 JPEG 到 /record/ */
void MainWindow::on_btnSnap_clicked()
{
    for (auto *c : m_clients) {
        if (c->objectName() == "dvr") {
            c->send(MSG_DVR_SNAPSHOT);
        }
    }
    ui->dvrStatus->setText(QString("状态: 📷 拍照已请求 (保存到 /record/)  |  %1")
        .arg(ui->dvrStatus->text().contains("录像中")?"● 录像中":"待机"));
}

/* ================================================================
 *  Music 槽函数 — 通过 AV 守护进程控制音乐播放
 *
 * btnPlay:       发送 MSG_AV_PLAY  开始播放
 * btnPauseMusic: 发送 MSG_AV_PAUSE 暂停播放 (独立按钮, 不与 DVR btnPause 冲突)
 * volSlider:     写入 /tmp/av_volume 文件并发送 MSG_AV_VOLUME
 *
 * 曲目名显示: 构造函数中的 trackTimer 每秒读取 /tmp/av_track,
 *            由 AV daemon 写入当前播放文件名。
 * ================================================================ */
void MainWindow::on_btnPlay_clicked()
{
    for (auto *c : m_clients) if (c->objectName() == "av") c->send(MSG_AV_PLAY);
}

/** @brief 音乐暂停: 发送 MSG_AV_PAUSE (独立按钮, 不与 DVR btnPause 冲突) */
void MainWindow::on_btnPauseMusic_clicked()
{
    for (auto *c : m_clients) if (c->objectName() == "av") c->send(MSG_AV_PAUSE);
}

void MainWindow::on_volSlider_valueChanged(int v)
{
    FILE *fp = fopen("/tmp/av_volume", "w");
    if (fp) { fprintf(fp, "%d", v); fclose(fp); }
    for (auto *c : m_clients) if (c->objectName() == "av") {
        QByteArray d; d.append((char)v); c->send(MSG_AV_VOLUME, d);
    }
}

/* ================================================================
 *  CAN 槽函数 — CAN 总线消息发送
 *
 * 从 UI 控件读取参数:
 *   canId   (QSpinBox):    CAN 标识符 (hex)
 *   canData (QLineEdit):   十六进制数据字节, 空格分隔 (如 "11 22 33")
 *
 * 组装为 15 字节 can_msg_t 结构体:
 *   [0..3]   can_id       (uint32 LE)
 *   [4]      can_dlc      (数据字节数)
 *   [5..12]  data[8]      (CAN 数据, 从用户输入填充, 不足 8 字节剩余为 0)
 *   [13]     is_extended  (扩展帧标志, 默认 0)
 *   [14]     is_remote    (远程帧标志, 默认 0)
 *
 * 尾部不够 15 字节时用 0x00 填充, 确保 daemon 解析 CanMsg 结构体正确。
 * 发送后追加 TX 日志到 canLog 控件。
 * ================================================================ */
void MainWindow::on_btnCanSend_clicked()
{
    uint32_t id = (uint32_t)ui->canId->value();
    QStringList parts = ui->canData->text().split(' ', QString::SkipEmptyParts);
    QByteArray d;
    d.append((char)(id&0xFF)); d.append((char)((id>>8)&0xFF));
    d.append((char)((id>>16)&0xFF)); d.append((char)((id>>24)&0xFF));
    d.append((char)parts.size());
    for (int i=0; i<qMin(parts.size(),8); i++) { bool ok; d.append((char)parts[i].toInt(&ok,16)); }
    /* can_msg_t = 15 bytes: can_id(4) + dlc(1) + data[8] + is_extended(1) + is_remote(1) */
    while (d.size() < 15) d.append('\0');
    for (auto *c : m_clients) if (c->objectName() == "can") {
        c->send(MSG_CAN_SEND, d);
        ui->canLog->append(QString("<b style='color:#f80;'>TX:</b> 0x%1 [%2]")
            .arg(id,3,16,QChar('0')).arg(ui->canData->text()));
    }
}
