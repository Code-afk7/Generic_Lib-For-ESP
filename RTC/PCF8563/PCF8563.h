#ifndef __PCF8563_H
#define __PCF8563_H

#include <stdio.h>
#include <time.h>
#include <stdbool.h>

/* ── RTC Configuration ───────────────────────────────────────────────────────── */
#define I2C_PORT        I2C_Port Num
#define PIN_SDA         SDA_Pin_Num
#define PIN_SCL         SCL_pin_Num

//Fuctions ===============================================================
void print_time(const struct tm *t, bool valid);
void set_rtc_time(void);
void setup_alarm(void);
void setup_timer(void);
void setup_clkout(void);
int* read_rtc_time(void);
void __RTC_main(void);
uint8_t dec_to_bcd(uint8_t val);

#endif /* __RTC_H */
