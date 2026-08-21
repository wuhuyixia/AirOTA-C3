# AirOTA-C3

## 快速完成一次 OTA

1. 使用 USB 数据线将初始固件烧录到开发板。

2. 手机或电脑连接开发板创建的 WiFi 热点：

```text
esp32c3_XXXX
```

3. 浏览器打开：

```text
http://192.168.100.1
```

填写 WiFi 名称和密码并保存。

4. 设备重启并连接 WiFi 后，从串口日志找到设备 IP，例如：

```text
192.168.*.*
```

5. 修改一个容易观察的功能，例如将 LED 闪烁周期从：

```c
1000
```

修改为：

```c
200
```

毫秒。

6. 重新编译：

```bash
idf.py build
```

生成：

```text
build/esp32c3_onenet_ota.bin
```

7. 浏览器访问设备 IP：

```text
http://192.168.*.*
```

在页面中找到：

```text
OTA - 本地 .bin 上传
```

选择：

```text
build/esp32c3_onenet_ota.bin
```

并开始升级。

8. OTA 成功后设备会自动重启。串口看到：

```text
OTA: Current firmware marked valid
```

说明升级后的固件已经正常运行。



> README 中局域网 IP 使用 `192.168.*.*` 模糊表示，实际地址请以设备串口日志为准。
