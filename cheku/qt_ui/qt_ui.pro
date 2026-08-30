QT       += core gui widgets network
TARGET    = car_ui
TEMPLATE  = app
CONFIG   += c++11

SOURCES += main.cpp mainwindow.cpp protocol_handler.cpp daemon_client.cpp sensor_thread.cpp
HEADERS += mainwindow.h protocol_handler.h daemon_client.h pages/speed_gauge.h sensor_thread.h

FORMS   += mainwindow.ui

# ARM 交叉编译时取消下行注释
# target.path = /usr/bin
# INSTALLS += target
