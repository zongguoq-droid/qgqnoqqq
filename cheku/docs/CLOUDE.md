# 智能车载终端项目 - Cloude Code 全局配置

## 开发者身份
大三学生，嵌入式Linux应用层开发学习阶段。
要求：代码注释详细、变量命名规范、逻辑清晰易懂、每个模块配测试demo，便于学习理解。
## 开发板设备树文件
@C:\Users\qgqnoqqq\Desktop\LINUX\cheku\Linux-4.9.88\arch\arm\boot\dts\100ask_imx6ull-14x14.dts(这是设备树文件，先按照这里面的配置完成开发，然后再去修改，禁止修改这个文件！)
## 硬件平台（开发板原生 + 外接模块）
### 开发板原生资源
- 主控：NXP i.MX6ULL（ARM Cortex-A7 单核，主频 528MHz）
- 内存：512MB DDR3L
- 存储：4GB eMMC Flash（板载） + micro SD卡（外接）
	- 交叉编译工具链：arm-buildroot-linux-gnueabihf-gcc
	  工具链路径: /home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/
	  环境变量:
	    export ARCH=arm
	    export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
	    export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin
- 显示：7 寸 1024600TN-RGB LCD接口 
- 音频：WM8960 编解码芯片，喇叭输出 + MIC1咪头
- 网络：双以太网口(RJ45) + 板载WIFI/蓝牙(RTL8723) + mini PCIE 4G + nano SIM
- 总线：CAN接口 + RS485接口
- 输入：KEY1 / KEY2 按键 + 复位KEY4 + 开关机KEY3
- LED：4G LED + 用户LED + 电源指示灯
- USB：USB OTG + 2路USB HOST
- RTC：RTC后备电池接口
## Linux虚拟机工具链
工具链地址如下
export ARCH=arm
export CROSS_COMPILE=arm-buildroot-linux-gnueabihf
export PATH=$PATH:/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueab
ihf_sdk-buildroot/bin
### 外接模块
| 模块 | 接口 | 设备节点 | 用途 |
|------|------|---------|------|
| GPS串口模块 | USB转串口 / UART | /dev/ttyUSB0 或 /dev/ttymxcx | 定位、车速、里程 |
| DHT11温湿度传感器 | GPIO | GPIO编号（如GPIO1_IO03） | 车内温湿度监测 |
| USB摄像头 | USB HOST（UVC协议） | /dev/video0 | 行车录像、倒车影像 |

## 项目定位
智能车载终端（简化版），聚焦 **Linux 应用层开发**，驱动使用原厂/开发板提供的，应用层通过设备节点/标准接口操作硬件。

## 整体架构（四层）
1. 硬件底层：i.MX6ULL 开发板 + 外接 GPS / DHT11 / USB摄像头
2. 内核驱动层：使用原厂驱动（应用层直接操作设备节点）
3. 应用服务层：C语言多进程后台服务，UNIX Socket 通信
4. UI交互层：Qt 5.14.2嵌入式（QWidget），通过本地Socket与后台进程交互

## 应用层模块划分（8个后台进程 + 1个UI进程）
| 进程名 | 功能 | 核心技术点 | 对应硬件 |
|--------|------|-----------|---------|
| gps_daemon | GPS定位数据采集与解析 | 串口termios、NMEA-0183协议解析、里程计算 | 外接GPS串口模块 |
| dvr_daemon | 行车录像DVR | V4L2(UVC)采集、MJPEG直存、循环存储 | 外接USB摄像头 (/dev/video1) |
| sensor_daemon | 车内温湿度监测 | 字符设备驱动读取、滑动平均滤波、阈值告警 | 外接DHT11 (/dev/mydht11) |
| net_daemon | 网络服务 | HTTP天气请求、JSON解析、网络接口监控、链路切换 | 双ETH + WIFI(RTL8723) + 4G模组 |
| canbus_daemon | CAN总线通信 | SocketCAN、CAN报文收发、错误帧处理 | CAN接口 (flexcan1) |
| av_daemon | 音频播放服务 | ALSA PCM播放(WM8960)、WAV解码、混音控制 | WM8960 + 喇叭 |
| input_daemon | 按键输入管理 | input子系统、按键事件监听、长按短按识别 | KEY1/KEY2按键 + 用户LED |
| guard_daemon | 系统守护进程 | 进程保活、日志管理、存储监控、异常重启 | - |
| qt_ui | Qt车载触控界面 | QWidget、QPainter绘图、QtMultimedia、本地Socket | LCD触摸屏 |

## 进程间通信方式
- **统一使用 UNIX Domain Socket（本地Socket）**：Qt UI 与各后台服务之间双向通信
- 消息格式：自定义简单二进制协议（帧头0xAA55 + 消息类型 + 数据长度 + 数据 + CRC8校验）
- 各后台服务启动时创建各自的Socket服务端，UI作为客户端连接
- 守护进程负责监控所有子进程状态，异常时重启

## 编码强制约束
1. 后台服务全部使用 **标准C语言（C99）**，禁止C++特性
2. 每个进程独立目录、独立main函数、独立编译
3. 所有IO操作（串口、Socket、文件、CAN、GPIO）使用 **非阻塞 + epoll多路复用**
4. 函数拆分合理，单函数不超过80行
5. 每个.c文件对应一个.h头文件，头文件只放声明
6. 内存申请必须配套释放，禁止内存泄漏
7. 错误处理必须完善：每个系统调用都检查返回值，打印perror
8. 日志分级：DEBUG / INFO / WARN / ERROR，写入eMMC日志文件，支持日志轮转
9. 代码注释：每个函数写功能说明、入参、返回值；关键逻辑行内注释
10. 考虑528MHz单核性能，避免高频轮询，全部事件驱动

## 目录结构
car_terminal/
├── cloud.md # 本配置文件
├── Makefile # 总编译脚本
├── src/
│ ├── common/ # 公共库：日志、Socket 封装、协议解析、链表、环形缓冲区
│ ├── gps_daemon/ # GPS 定位进程
│ ├── dvr_daemon/ # DVR 录像进程（USB 摄像头）
│ ├── sensor_daemon/ # 温湿度监测进程（DHT11）
│ ├── net_daemon/ # 网络服务进程
│ ├── canbus_daemon/ # CAN 总线通信进程
│ ├── av_daemon/ # 音视频服务进程（WM8960 ALSA）
│ ├── input_daemon/ # 按键输入管理进程
│ └── guard_daemon/ # 系统守护进程
├── qt_ui/ # Qt 界面工程
├── config/ # 配置文件
├── scripts/ # 启动脚本、部署脚本
└── docs/ # 文档
## 禁止行为
1. 不要擅自新增未提及的第三方库
2. 不要修改目录结构
3. 不要写内核驱动代码，应用层通过设备节点操作硬件
4. 不要使用C++写后台服务进程
5. 不要省略错误处理和日志
6. 不要使用阻塞式轮询，全部事件驱动

## 交付要求
1. 每个模块附带 Makefile，支持虚拟机Linux系统x86本地编译和ARM交叉编译（CROSS_COMPILE=arm-buildroot-linux-gnueabihf-）
2. 每个模块附带测试demo和使用说明
3. 总Makefile一键编译全部模块
4. 启动脚本一键拉起全部后台进程
5. 部署方式：通过虚拟机adb发送文件至开发板