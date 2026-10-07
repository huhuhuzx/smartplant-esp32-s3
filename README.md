# SmartPlant S3

ESP32-S3-LCD-EV-Board-2 **V1.5 主板 + SUB3 800×480 触摸屏**智能浇花工程。基于 [原 C3 工程](https://github.com/huhuhuzx/smartplant-esp32) 独立改造。使用 ESP-IDF 6.0.2、官方板级 BSP 4.1.0、LVGL 9.4.0。

## 当前双传感器方案的硬件

- ESP32-S3-LCD-EV-Board-2 V1.5，16 MB Flash / 16 MB 八线 PSRAM。
- 原来的三线模拟土壤传感器与三线条纹式模拟水位传感器。
- **新增一块 ADS1115 I²C ADC 模块**，VDD 接 3.3 V、ADDR 接 GND，地址为 `0x48`。ADS1015 不可代替。
- 输入高电平启动、3.3 V 信号兼容的水泵 MOSFET 驱动模块，以及匹配水泵电压的独立电源。
- 开发板建议使用 5 V / 2 A 稳定电源，水泵电源独立支路并共地。

ESP32-S3 自带 ADC；采用 ADS1115 是本工程为了同时采集两路模拟信号并保留板载功能所选的方案，并不是这两只传感器只能通过 ADS1115 使用。J5 第 11 针 IO4 对应 GPIO4 / ADC1_CH3，可直接采集土壤传感器模拟输出。V1.5 原理图中 GPIO4 共用板载 WS2812 彩灯控制线；直连时不能同时驱动该彩灯，需要改写土壤采样代码并重新校准。当前固件仍从 ADS1115 A0 读取土壤信号，改接 IO4 不会自动切换采样源。

本工程不占用 RGB 屏、USB 和音频 GPIO 来采集传感器。ADS1115 与板载触摸、I/O 扩展器共用官方 BSP 初始化的 I²C 总线。

## V1.5 接线

按主板 **J5（22 针用户扩展接口）** 的第 1 针标记数针。左右位置以实板为准；下表数字为原理图针号，不是 ESP32 GPIO 编号。

| 连接目标 | 接到哪里 |
|---|---|
| ADS1115 SDA | J5 第 13 针，IO47 / I²C SDA |
| ADS1115 SCL | J5 第 15 针，IO48 / I²C SCL |
| ADS1115 VDD / VCC | J5 第 17 或 18 针，3V3_IO |
| ADS1115 GND、ADDR | J5 第 1、7、19 或 20 针，GND |
| 土壤传感器 S / 黄线 | ADS1115 A0 / AIN0 |
| 土壤传感器 V / 红线 | 3.3 V |
| 土壤传感器 G / 黑线 | 公共 GND |
| 条纹水位传感器 S / 黄线 | ADS1115 A1 / AIN1 |
| 水位传感器 V / 红线 | 3.3 V（先验证模块在该电压下的原始读数） |
| 水位传感器 G / 黑线 | 公共 GND |
| 水泵 MOSFET IN | J5 第 12 针，EX_IO4 / TCA9554 P4 |
| MOSFET GND | 公共 GND |

J5 第 12 针是 **I²C 扩展器输出**，不是 GPIO4；不要接到 J5 第 11 针 IO4，也不要沿用 C3 的 GPIO20 控泵接线。GPIO19/20 保留给 USB。不要把 5 V 传感器输出直接送入供电为 3.3 V 的 ADS1115。

驱动 IN 到 GND 接 10 kΩ 下拉电阻（模块自带可省略）；扩展口只提供控制信号，不能给水泵供电。水泵需续流保护，接线先断电。

官方资料：[V1.5 用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s3/esp32-s3-lcd-ev-board/user_guide.html)、[V1.5 主板原理图](https://dl.espressif.com/dl/schematics/SCH_ESP32-S3-LCD-Ev-Board-MB_V1.5_20231009.pdf)。

## 土壤传感器直接接 IO4 的硬件方案

按 V1.5 官方原理图，J5 第 11 针 IO4 支持 ADC1_CH3；GPIO4 共用板载 WS2812 控制线，使用模拟采样时不要驱动该彩灯。音频主时钟是 GPIO5。

| 土壤传感器线 | J5 接线 |
| --- | --- |
| S / 黄线 | 第 11 针 IO4 |
| V / 红线 | 第 17 或 18 针 3V3_IO |
| G / 黑线 | 第 1、7、19 或 20 针 GND |

这张表说明可行的片上 ADC 接法，当前已烧录固件仍使用 ADS1115 A0，须改写采样代码后才能按此表使用；改换采集电路后重新记录干燥、湿润值。J5 第 12 针 EX_IO4 为数字扩展器端口；第 10 针实际是 EX_IO5。保留 USB 时不使用 IO19/IO20 采集传感器。传感器供电采用 3.3 V，输出不能向 GPIO4 送入 5 V。

依据：[V1.5 官方原理图](https://dl.espressif.com/dl/schematics/SCH_ESP32-S3-LCD-Ev-Board-MB_V1.5_20231009.pdf)，第 3、7 页；ADC 通道见 ESP-IDF `soc/esp32s3/include/soc/adc_channel.h`。

## 水位传感器的使用范围

三线条纹式模拟水位模块可按模拟输出接到 ADS1115 A1。**先显示和校准，不恢复缺水锁定**。水位为条纹区浸入程度的相对值，不是整个水箱的剩余升数或百分比；只适合观察探头附近水位。

只浸入条纹区，不超过板上标注的水位线，插头和上方电路保持干燥。这类裸露电极板长期通电浸水会腐蚀，建议验证后使用适合长期浸水的探头；目前固件没有自动断电供电电路。未接 ADS1115 会显示采集失败；仅凭模拟电压无法可靠区分探头未接与某些真实空水读数。

## 构建与烧录

在安装了 **ESP-IDF 6.0.2 和 Xtensa ESP32-S3 工具链**的 ESP-IDF 终端打开本文件夹：

```powershell
git clone https://github.com/huhuhuzx/smartplant-esp32-s3.git watering-s3
cd watering-s3
idf.py -B build build
idf.py -B build -p COM端口 flash monitor
```

目标在 `sdkconfig.defaults` 中设为 `esp32s3`。不要复制原 C3 的 `sdkconfig`、构建目录或固件到这里。首次构建需要联网下载声明组件；已生成的中文字体源码随工程提供，不需要本机安装字体即可编译。固件为 `build/smartplant_s3.bin`。

建议通过 USB-to-USB 接口的 USB Serial/JTAG COM 端口执行上面的下载和日志命令。USB-to-UART 也可用于下载，但本固件的应用日志输出在 USB-to-USB，使用 UART 下载后需换到 USB Serial/JTAG 的 COM 端口运行 `idf.py -p COM号 monitor`。首次验证请保持水泵电源断开，先确认屏幕、触摸、ADC 和控制信号。

## 界面与使用

触摸屏底部有六个页面：概览、历史、手动浇水、自动化、传感器、网络；右上角随时可停止水泵。黑底界面使用绿 / 橙 / 红分别表示开启运行、关闭等待、故障，蓝色表示水位读数。

1. 在“传感器”页分别记录土壤干燥、湿润值；更换采集电路后必须重新校准。
2. 水位探头分别保持空水、预期最高浸入水位，记录两端。差值至少为 100 才启用百分比显示。
3. 在“自动化”页编辑六个参数及开关，点“保存设置”后生效。目标湿度需比启动湿度至少高 5%。自动浇水默认关闭。
4. 手动可选 3 / 5 / 10 秒，到时自动停止。水位读数不会限制启动。
5. 倒计时表示最早允许再次自动浇水；到时仍需土壤低于启动阈值。

ADS1115 输出归一为 0..4095，对应约 0..3.3 V，与原网页参数范围一致；这里不是 ESP32 片上 ADC 原始值。土壤百分比是两端校准相对值，不是实验室测量的体积含水率。

### 网络与网页

可以直接在 ESP32 屏幕上配网：

1. 打开底部“网络”，点击“扫描网络”。正在连接旧网络时请等待连接结束再扫描。
2. 在附近 Wi-Fi 列表点选家庭 **2.4 GHz** 网络，列表可上下滑动，中文网络名可直接选择。
3. 使用屏幕键盘输入密码，可切换大小写、数字和符号；默认隐藏密码，“显示密码”可临时查看。开放网络可留空。
4. 点击“连接网络”，等待“已连接并保存”。点击“返回”查看网页地址。密码错误或找不到网络时会给出提示，可以修改密码后重试。
5. 隐藏网络可点击“手动输入网络名”；键盘提供英文、数字和符号，没有中文输入法，中文名称请从扫描列表选择。

输入键盘展开时，右上角“停止水泵”仍然可用。扫描与连接请求在界面锁之外执行，触摸和浇水控制继续运行。网络设置保存在本机 NVS，下次启动自动连接；源码默认不含个人 Wi-Fi 凭据。

也可连接 `SmartPlant_Setup`，访问 `http://192.168.4.1`，在网页“设置”中配置 Wi-Fi。连入家庭网络后，电脑和手机连接同一路由器，再打开屏幕显示的地址；也可尝试 `http://smartplant.local`。连接成功后配网热点自动关闭。ESP32-S3 不支持 5 GHz Wi-Fi。

Wi-Fi 配置仍可在 menuconfig 的 SmartPlant 菜单预设，保存在仅本机的 `sdkconfig` 中。硬件不支持软件调整背光，网页已隐藏亮度控件。

### 历史

联网或网页校时后每分钟记录一条**有效土壤湿度**。历史分区约 10 MiB、文件上限约 4 MB，达到上限后保留后半段；不能无限保留。24 小时 / 7 天 / 30 天使用实际时间范围。屏幕曲线按 96 个时间区间计算均值，缺失区间留空，不制造记录。网页最多返回 500 点，保留起止样本并均匀抽取；数据不足时不同范围会包含相同记录。

## 模块

- `board.c`：官方 BSP 共用 I²C、ADS1115 和 TCA9554 控泵。
- `plant.c`：浇水策略与校准，水位只监测。
- `screen.c`：LVGL 界面；控制请求与网页使用同一套 plant 接口。
- `store.c`：NVS 配置与 SPIFFS 历史；`net.c`、`web.c`：网络和网页服务。
- `main.c`：统一初始化与任务编排。

RGB 屏采用 PSRAM 双帧缓冲和 bounce buffer，并启用 PSRAM XIP，避免保存 NVS 参数、写入每分钟历史时关闭外部缓存而中断显示。配置依据[官方 RGB 驱动说明](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html#bounce-buffer-with-single-psram-frame-buffer)，显示效果仍需实板确认。

## 中文字体

字体固定为 Medium 500 字重，包含常用中文标点。20px 字体包含 ASCII 和基本汉字区，支持中文 Wi-Fi 名称；28px 字体包含界面使用的汉字。更新大字号文案后，在装有 Noto Sans SC 的 Windows 机器运行：

```powershell
python -m pip install fonttools
npm install --prefix .local/font-tools lv_font_conv@1.5.3
./tools/generate_ui_fonts.ps1
```

字库源自 Noto Sans SC，采用 SIL OFL 1.1，全文见 `licenses/NotoSansSC-OFL.txt`。构建缓存、字体转换工具、测试证据和私人配置不上传。复制保留的规格 PDF 和架构参考属于原项目资料；本 README 为 S3 接线与使用依据。

## 验证边界

构建成功仅证明组件接口、源码和分区可以编译；实际 RGB 颜色、方向、触摸位置、ADC 电压和水泵输出需在 V1.5 实板上确认。未经实板验证不能宣称已烧录或所有硬件工作正常。
