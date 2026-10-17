/* 主机替身：iot_pwm.h（真机来自 base/iot_hardware/peripheral/interfaces/kits） */
#ifndef GW_STUB_IOT_PWM_H
#define GW_STUB_IOT_PWM_H
unsigned int IoTPwmInit(unsigned int port);
unsigned int IoTPwmStart(unsigned int port, unsigned short duty, unsigned int freq);
unsigned int IoTPwmStop(unsigned int port);
unsigned int IoTPwmDeinit(unsigned int port);
#endif
