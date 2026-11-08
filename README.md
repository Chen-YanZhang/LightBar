# LightBar — Matter over Wi-Fi 双色温灯带控制器

基于 **ESP32-C6** 的 Matter 灯带控制器，用两颗 **TI TPS92390** 分别驱动冷/暖两路 LED 灯串，
支持 Matter 调亮度与色温、温湿度传感、日光跟随、板温保护、网页控制台和 OTA 升级。

> 本仓库**只包含源码**。不提供预编译固件，请按下文自行编译。

## 特性

- **Matter over Wi-Fi**（esp-matter），支持 Apple Home / Google Home / Home Assistant 配网
- **2 × TPS92390** LED 驱动：U12 暖白 2700 K @ I²C `0x3A`，U4 冷白 5000 K @ `0x3B`
- 每颗驱动 3 路恒流灯串，每路 150 mA（`CONFIG_LB_STRINGS_PER_CHIP` 可改）
- **Hybrid 调光**：亮度走 `DISPLAY_BRT` 寄存器，电流上限走 `LED_CURR_CONFIG`，互不干扰
- **SHT40** 板温/环境温湿度，超温自动降额
- **日光跟随**：按经纬度与日期计算太阳高度角，映射到色温
- **单颗 WS2812** 状态指示（网络、配网、故障）
- **网页控制台**：亮度/色温/模式、网络设置、寄存器诊断、OTA 上传
- **串口 CLI**：`lb>` 提示符，可直接读写驱动寄存器
- A/B 双分区 OTA，带校验与自动回滚

## 硬件

| 项目 | 说明 |
|---|---|
| MCU | ESP32-C6-MINI-1U（4 MB Flash） |
| LED 驱动 | 2 × TPS92390（I²C，`0x3A` / `0x3B`） |
| 灯串 | 3 串 × 150 mA / 每颗驱动 |
| 供电 | USB-PD 20 V |
| 传感器 | SHT40（I²C） |
| 状态灯 | WS2812，DIN = IO15 |

引脚定义见 [`firmware/main/board_pins.h`](firmware/main/board_pins.h)。

> ⚠️ **共享电流检测脚的问题**：两路驱动共用 `VSENSE_P` 采样网络时，两通道同时大幅变化
> （如 0 ↔ 100%）会让高开关频率的那颗进入 `VINOCP → FAULT_RECOVERY` 循环。
> 修改硬件时建议为每颗驱动做独立的 Kelvin 采样，不要共用采样网络。

## 目录结构

```
firmware/
├── main/                 应用层
│   ├── app_main.c        启动流程
│   ├── app_matter.cpp    Matter 数据模型与回调
│   ├── safety_sm.c       安全状态机（过温、故障、恢复）
│   ├── lightbar_out.c    亮度/色温 → 寄存器映射
│   ├── solar_policy.c    日光跟随策略
│   ├── tps_bus.c         I²C 总线与驱动访问
│   ├── tps_selftest.c    上电自检
│   ├── rgb_status.c      WS2812 状态指示
│   ├── wifi_prov.c       Wi-Fi 配网与扫描
│   ├── lb_ota.c          OTA
│   ├── web_ui.c          内置 HTTP 服务与 REST API
│   ├── evlog.c           事件环形日志
│   └── web/              前端（HTML/CSS/JS，编译时嵌入固件）
├── components/
│   ├── tps92390/         TPS92390 驱动
│   ├── lightbar_mix/     色温混合算法
│   ├── sht41/            SHT40 驱动
│   ├── ws2812/           WS2812 驱动
│   ├── lb_solar/         太阳位置计算
│   └── lb_time/          SNTP 对时
├── sdkconfig.defaults    Kconfig 默认值
├── partitions.csv        分区表
└── version.txt           版本号（编译进 app descriptor）
```

## 编译

需要 **ESP-IDF v6.1**（`esp_matter ~1.6` 由 component manager 自动拉取，首次编译会下载，
约需十几分钟）。

```bash
# 1. 安装并激活 ESP-IDF v6.1
#    https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/

# 2. 编译
cd firmware
idf.py set-target esp32c6
idf.py build
```

首次 `idf.py build` 会通过 `main/idf_component.yml` 下载 esp-matter 及其依赖。
构建产物在 `firmware/build/lightbar.bin`。

### 主要 Kconfig 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CONFIG_LB_STRINGS_PER_CHIP` | 3 | 每颗驱动接的灯串数 |
| `CONFIG_LB_REV_NEW` | n | 新板（PCB4）硬件特性开关 |
| `CONFIG_LB_BOOT_SELFTEST` | n | 上电写寄存器自检 |

`idf.py menuconfig` → `LightBar Configuration`。

## 烧录与升级

**首次烧录（串口）**

```bash
idf.py -p COM4 flash
```

**后续升级（网页 OTA）**

```bash
curl -X POST --data-binary "@build/lightbar.bin" http://<设备IP>/update
```

或打开 `http://<设备IP>/` → 更新标签页上传。设备写入备用分区、校验、重启，
新固件启动失败会自动回滚。

## 配网

首次上电设备进入配网模式，串口日志会打印 Matter 配对码与二维码链接。
也可以在网页控制台「网络设置」里直接填入 SSID/密码。

如需打印二维码：

```bash
# 串口日志中查找 "Manual pairing code" 或 "SetupQRCode"
idf.py -p COM4 monitor
```

## HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 版本、状态机、板温、IP |
| GET | `/api/diag` | 两颗 TPS92390 的寄存器读数与故障位 |
| GET | `/api/log` | 事件环形日志 |
| POST | `/api/light` | 亮度/色温 |
| POST | `/api/prov` | `ssid=&pass=` 加入 Wi-Fi |
| POST | `/update` | OTA 固件上传 |

## ⚠️ 安全与认证声明

1. **VID/PID 是测试值**（`0xFFF1` / `0x8000`），固件使用 esp-matter 的**测试 DAC**。
   仅可用于开发与自用，**不可用于量产或销售**。商用需加入 CSA 联盟、取得分配的
   VID/PID 与认证证书。
2. **`/update` 接口没有密码**。设备所在局域网内任何人都能刷入固件。
   请勿部署在不可信网络。生产版本应加上鉴权。
3. 网页控制台与 OTA 均为明文 HTTP，不提供机密性保护。

## 已知问题

- 两通道同时大幅变化时，高开关频率那路可能触发 `VINOCP` 保护循环（硬件共享采样网络所致）
- 日光跟随依赖 SNTP 对时，断网时回退到上次同步的时间
- `Mode Select` 聚类需要在 Matter 控制器侧重新 interview 才会出现

## 许可证

[MIT](LICENSE)。

本项目链接的 ESP-IDF 与 esp-matter 使用 Apache-2.0，与 MIT 兼容。
