#pragma once
#include <stdint.h>

// 启动 LED 闪烁任务。
void led_app_start(void);
// 设置 LED 完整闪烁周期，函数内部会进行上下限保护。
void led_set_period_ms(uint32_t period_ms);
// 获取当前 LED 完整闪烁周期。
uint32_t led_get_period_ms(void);
