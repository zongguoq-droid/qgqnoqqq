/**
 * @file    main.cpp
 * @brief   智能车载终端 Qt UI — 主入口
 *
 * 启动流程:
 *   1. QApplication 初始化
 *   2. 设置全屏/窗口模式 (嵌入式 LCD 1024x600)
 *   3. 创建 MainWindow → 自动连接 8 个后台守护进程
 *   4. 进入事件循环
 *
 * 运行方式:
 *   ./car_ui                          — 窗口模式 (开发调试)
 *   ./car_ui -fullscreen              — 全屏模式 (车载LCD)
 *   ./car_ui -platform linuxfb        — Linux framebuffer (无 X11)
 */

#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    /* 全局样式 (车载触控优化: 大按钮/大字体) */
    app.setStyleSheet(
        "QMainWindow { background: #f5f5f5; }"
        "QWidget { font-family: 'Noto Sans CJK SC', 'DejaVu Sans', sans-serif; }"
        "QPushButton { min-height: 40px; }"
    );

    MainWindow window;

    /* 全屏模式 (嵌入式 LCD) */
    bool fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (QString(argv[i]) == "-fullscreen") fullscreen = true;
    }

    if (fullscreen) {
        window.showFullScreen();
    } else {
        window.show();
    }

    return app.exec();
}
