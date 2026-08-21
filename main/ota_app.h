#pragma once

#include "esp_err.h"
#include "esp_ota_ops.h"

typedef enum {
    OTA_IDLE = 0,
    OTA_DOWNLOADING,
    OTA_DONE,
    OTA_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int percent;
    char err_msg[128];
} ota_status_t;

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    int total;
    int received;
} ota_writer_t;

// 启动一个 URL OTA 后台任务；支持 http:// 和 https://。
esp_err_t ota_start_url(const char *url);
// 获取当前 OTA 状态和百分比，用于 Web 状态页。
ota_status_t ota_get_status(void);
// 应用完成基础服务初始化后，向 Bootloader 确认当前固件有效。
void ota_mark_valid(void);

// 初始化 OTA 写入并选择当前运行分区之外的备用分区。
esp_err_t ota_writer_begin(ota_writer_t *w, int total);
// 向 OTA 分区写入一块数据并更新进度。
esp_err_t ota_writer_write(ota_writer_t *w, const void *data, int len);
// 结束写入、验证镜像并设置下次启动分区。
esp_err_t ota_writer_finish(ota_writer_t *w);
// 中止 OTA 写入并记录错误原因。
void ota_writer_abort(ota_writer_t *w, const char *reason);
