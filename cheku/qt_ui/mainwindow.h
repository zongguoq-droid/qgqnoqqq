/**
 * @file    mainwindow.h
 * @brief   智能车载终端 Qt UI — 主窗口
 *
 * ============================================================================
 * 架构概览
 * ============================================================================
 * MainWindow 是 Qt UI 的顶层窗口，通过 QTabWidget 组织四个功能页面:
 *   Tab 0 — 仪表盘 (Dashboard):  速度表、卫星信息、温湿度
 *   Tab 1 — DVR 行车记录:         预览画面、录像控制、状态显示
 *   Tab 2 — 音乐播放:             曲目显示、播放/暂停、音量
 *   Tab 3 — CAN 总线:             消息收发、日志
 *
 * 与后台守护进程通信:
 *   使用 DaemonClient (QLocalSocket 封装) 连接 5 个 Unix domain socket
 *   守护进程: gps, input, can, av, dvr
 *   协议帧格式: [0xAA][0x55][TYPE][LEN_H][LEN_L][DATA...][CRC8][0x55]
 *
 * ============================================================================
 * DVR 录像状态机 (m_dvrState)
 * ============================================================================
 *   State 0 — IDLE (待机)
 *     初始状态。停止录像后回到此状态。
 *     btnRecord 显示"开始录像"，btnPause/btnStop 禁用。
 *
 *   State 1 — RECORDING (录像中)
 *     点击"开始录像"后进入。m_durationTimer 每秒自增 m_dvrElapsedSec。
 *     时长标签闪烁红/深红交替。btnRecord 禁用(防重复)，btnPause/btnStop 启用。
 *
 *   State 2 — PAUSED (已暂停)
 *     录像中点击"暂停"后进入。m_dvrElapsedSec 冻结不再增长。
 *     时长标签显示橙色。btnRecord 变为"继续录像"(绿色)，btnPause 禁用。
 *
 *   状态转换:
 *     IDLE   --[开始录像]--> RECORDING
 *     RECORDING --[暂停]-->  PAUSED
 *     PAUSED  --[继续录像]--> RECORDING
 *     RECORDING/PAUSED --[停止]--> IDLE (elapsed 归零)
 *
 *   daemon 端通过 MSG_DVR_STATUS 消息回传实际状态，UI 同步更新。
 *
 * ============================================================================
 * 物理按键分发 (Key Dispatch)
 * ============================================================================
 *   input 守护进程通过 MSG_KEY_EVENT 上报物理按键事件。
 *   MainWindow 根据当前选中的 Tab 将按键路由到不同的功能模块:
 *     - DVR Tab (index 1):  KEY1 → 拍照(MSG_DVR_SNAPSHOT)
 *                           KEY2 → 暂停/继续(MSG_DVR_PAUSE)
 *     - Music Tab (index 2): KEY1 → 播放(MSG_AV_PLAY)
 *                           KEY2 → 暂停(MSG_AV_PAUSE)
 *   仅短按(event_type==1)触发功能；长按和双击仅记录到 statusbar 用于诊断。
 *
 * ============================================================================
 * CAN 消息发送格式
 * ============================================================================
 *   发送帧固定 15 字节 (对应 CanMsg 结构体):
 *     bytes[0..3]   can_id       (uint32, little-endian)
 *     bytes[4]      can_dlc      (数据长度, 0-8)
 *     bytes[5..12]  data[8]      (CAN 数据字段)
 *     bytes[13]     is_extended  (扩展帧标志)
 *     bytes[14]     is_remote    (远程帧标志)
 *   若用户输入不足 15 字节，尾部用 0x00 填充。
 *
 * ============================================================================
 * 已移除的功能
 * ============================================================================
 *   - LED 轮询:     原通过 QTimer 周期查询 daemon 的 LED 状态，
 *                    因用不到已移除相关代码和定时器。
 *   - 输入设备 Tab:  原独立的"输入"页面已移除，物理按键改为
 *                    根据当前活跃 Tab 自动分发到 DVR/Music。
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QTimer>
#include "daemon_client.h"
#include "pages/speed_gauge.h"
#include "sensor_thread.h"

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    /** @brief 构造主窗口: 初始化 UI、仪表盘、守护进程连接、定时器 */
    explicit MainWindow(QWidget *parent = nullptr);

    /** @brief 析构: 停止定时器、请求 SensorThread 退出并等待 */
    ~MainWindow();

private slots:
    /**
     * @brief 处理来自守护进程的协议消息
     * @param name    守护进程名称 (gps/input/can/av/dvr)
     * @param msgType 消息类型 (MSG_* 宏定义)
     * @param data    消息载荷字节数组
     *
     * 根据 (name, msgType) 组合分发到对应处理逻辑:
     *   - gps + MSG_GPS_DATA  → 更新速度表、卫星信息
     *   - dvr + MSG_DVR_STATUS → 同步录像状态、时长、分辨率、帧数
     *   - can + MSG_CAN_DATA  → 追加到 CAN 日志
     *   - input + MSG_KEY_EVENT → 根据当前 Tab 分发按键到 DVR/Music
     */
    void onDaemonMessage(const QString &name, uint8_t msgType, const QByteArray &data);

    /** @brief DVR 预览刷新: 每秒读取 /tmp/dvr_preview.jpg 并显示 */
    void refreshPreview();

    /** @brief DVR 时长更新: 每秒自增 m_dvrElapsedSec (仅 RECORDING 状态) */
    void updateDvrDuration();

    /** @brief 开始录像 / 继续录像 (同一按钮, IDLE→RECORDING 或 PAUSED→RECORDING) */
    void on_btnRecord_clicked();

    /** @brief 暂停录像 (RECORDING→PAUSED) */
    void on_btnPause_clicked();

    /** @brief 停止录像 (RECORDING/PAUSED→IDLE, elapsed 归零) */
    void on_btnStop_clicked();

    /** @brief 拍照: 请求 daemon 保存当前帧为 JPEG 到 /record/ */
    void on_btnSnap_clicked();

    /** @brief 音乐播放: 发送 MSG_AV_PLAY */
    void on_btnPlay_clicked();

    /** @brief 音乐暂停: 发送 MSG_AV_PAUSE (独立按钮, 不与 DVR Pause 冲突) */
    void on_btnPauseMusic_clicked();

    /** @brief 音量调节: 写入 /tmp/av_volume 并发送 MSG_AV_VOLUME */
    void on_volSlider_valueChanged(int v);

    /**
     * @brief CAN 消息发送
     *
     * 从 UI 控件读取 CAN ID 和十六进制数据，组装为 15 字节 CanMsg 结构体，
     * 不足部分用 0x00 填充，通过 daemon 发送 MSG_CAN_SEND。
     */
    void on_btnCanSend_clicked();

private:
    /**
     * @brief 创建并连接 5 个守护进程客户端 (gps/input/can/av/dvr)
     *
     * 每个客户端连接到对应的 Unix domain socket，注册 connected/disconnected/
     * messageReceived 信号。连接计数显示在状态栏 (m_connected/5)。
     */
    void connectDaemons();

    /**
     * @brief 更新 DVR 控制按钮的启用状态和文字
     * @param state DVR 状态: 0=IDLE, 1=RECORDING, 2=PAUSED
     *
     * 根据状态切换 btnRecord/btnPause/btnStop 的文字、样式和启用状态。
     */
    void updateDvrButtons(int state);

    Ui::MainWindow *ui;              /**< Qt Designer 生成的 UI 对象 */
    SpeedGauge     *m_gauge;         /**< 仪表盘速度表控件 */
    QLabel         *m_satsLabel;     /**< 卫星状态标签 (数量/定位类型/UTC时间) */
    QTimer         *m_previewTimer;  /**< DVR 预览刷新定时器 (1秒) */
    QTimer         *m_durationTimer; /**< DVR 录像时长更新定时器 (1秒) */
    int            m_connected;      /**< 已连接的守护进程数量 (目标 5) */
    int            m_dvrState;       /**< DVR 状态机: 0=IDLE, 1=RECORDING, 2=PAUSED */
    uint32_t       m_dvrElapsedSec;  /**< 当前录像已录制时长 (秒), 暂停时冻结 */
    SensorThread   *m_sensorThread;  /**< 温湿度传感器读取线程 (独占 /dev/mydht11) */
    QVector<DaemonClient*> m_clients; /**< 守护进程客户端列表 (5 个实例) */
};

#endif
