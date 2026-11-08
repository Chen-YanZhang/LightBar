# Lightbar firmware

CCT 环形灯固件(ESP32-C6 + 2×TPS92390 + Matter over Wi-Fi)。

## 构建

```powershell
. $env:IDF_PATH\export.ps1  # 先激活 ESP-IDF v6.1 环境
idf.py set-target esp32c6   # 仅首次
idf.py build                # 产物 build\lightbar.bin
```

依赖:`espressif/esp_matter ~1.6`(managed_components 由组件管理器拉取)。

## 升级

- 网页:浏览器打开 `http://<ip>/`,上传 `build\lightbar.bin`(POST /update,原始字节流)。
- 串口 CLI:`ota http://<pc>:8000/lightbar.bin`(后台任务)。
- Matter OTA Requestor(cluster 0x0019)已启用。
- 新镜像启动后必须通过 `lb_mix` / `lb_solar` 自检且至少一颗驱动 ACK,否则立即回滚。

## 关键设计约束(2026-11-06 审查后)

- **TPS92390 工作在 Current Dimming Mode**(MODE=42.2k/5.76k,1Eh MODE_SEL=6/2):
  `DISPLAY_BRT` 已是模拟调光(经 200 ms sloper 进 12-bit 电流 DAC),**亮度只走 BRT**;
  `LED_CURR_CONFIG` 是电流上限,每颗芯片配置一次(默认 0xD00=130 mA,上限 0xF00=150 mA)。
- 驱动由 VBUS 供电、寄存器跨 MCU 复位保持:`safety_sm_recorder_boot()` 记录原始状态后**立即写 BRT=0**。
- 启动不再跑寄存器自检(`CONFIG_LB_BOOT_SELFTEST=n`),CLI `m1` 按需运行;自检全程 BRT=0 且电流码 ≤ 0xF00。
- 监视任务每秒比对 `USER_CONFIG1`,芯片掉电重启(brownout/POR)会被识别并自动重新配置。
- 旧板 PCB3_3:EN 硬接 VDD33,软件门禁只能把 BRT 清零;物理断电仍靠拔 VBUS。新板把 EN/INT/PG 接 MCU 后开 `CONFIG_LB_REV_NEW`。

## 串口 CLI

`st hist clr m1 diag regs probe ledclr sn ot [degC] ota <url> ver wifi <ssid> <pass> ip prov [ap|clear] webtest out set <0|1> <0-254> [mireds] cur [hex] sens rgb <r> <g> <b> sun [day] [hour] time [epoch] auto <0|1> help`
