#include "lifeline_ui.h"
#include "lifeline_config.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "lcd.h"

void lifeline_ui_init(void)
{
    lcd_init();
    lcd_fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    lcd_show_string(8, 6, (const uint8_t *)"YADX LIFELINE", LCD_CYAN, LCD_BLACK, 24, 0);
    lcd_show_string(8, 32, (const uint8_t *)NODE_ID, LCD_WHITE, LCD_BLACK, 16, 0);
    lcd_draw_line(0, 50, LCD_W, 50, LCD_GRAY);
    lcd_show_string(8, 56, (const uint8_t *)"GAS", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 76, (const uint8_t *)"T/H", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 96, (const uint8_t *)"TILT/VIB", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 116, (const uint8_t *)"LUX/FLOOD", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_draw_line(0, 140, LCD_W, 140, LCD_GRAY);
    lcd_show_string(8, 146, (const uint8_t *)"LEVEL", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 166, (const uint8_t *)"SRC", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 186, (const uint8_t *)"VALVE/AUTO", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 206, (const uint8_t *)"MQTT/KEY", LCD_LGRAY, LCD_BLACK, 16, 0);
    lcd_show_string(8, 222, (const uint8_t *)"K5-K6 keys say: Tongxiao", LCD_GRAY, LCD_BLACK, 16, 0);
}

static uint16_t level_color(alarm_level_t level)
{
    switch (level)
    {
        case ALARM_LEVEL_RED:
            return LCD_RED;
        case ALARM_LEVEL_ORANGE:
            return LCD_BRRED;
        case ALARM_LEVEL_YELLOW:
            return LCD_YELLOW;
        default:
            return LCD_GREEN;
    }
}

static uint16_t high_color(float v, float yellow, float orange, float red)
{
    if (v >= red)
    {
        return LCD_RED;
    }
    if (v >= orange)
    {
        return LCD_BRRED;
    }
    if (v >= yellow)
    {
        return LCD_YELLOW;
    }
    return LCD_WHITE;
}

static uint16_t low_lux_color(float lux)
{
    if (lux <= LUX_ORANGE)
    {
        return LCD_BRRED;
    }
    if (lux <= LUX_YELLOW)
    {
        return LCD_YELLOW;
    }
    return LCD_WHITE;
}

void lifeline_ui_update(const lifeline_report_t *report, unsigned int mqtt_ok)
{
    char buf[48];
    uint16_t lc = level_color(report->level);
    uint8_t ok = report->sensor.sensor_ok;
    uint16_t gc;

    gc = high_color(report->sensor.gas_ppm, GAS_PPM_YELLOW, GAS_PPM_ORANGE, GAS_PPM_RED);
    snprintf(buf, sizeof(buf), "%7.1f ppm   ", report->sensor.gas_ppm);
    lcd_show_string(88, 56, (uint8_t *)buf, gc, LCD_BLACK, 16, 0);

    if (ok & SENSOR_OK_SHT30)
    {
        uint16_t tc = high_color(report->sensor.temperature, TEMP_YELLOW, TEMP_ORANGE, TEMP_RED);
        uint16_t hc = high_color(report->sensor.humidity, HUMID_YELLOW, HUMID_ORANGE, HUMID_RED);
        snprintf(buf, sizeof(buf), "%5.1fC ", report->sensor.temperature);
        lcd_show_string(88, 76, (uint8_t *)buf, tc, LCD_BLACK, 16, 0);
        snprintf(buf, sizeof(buf), "%5.1f%%  ", report->sensor.humidity);
        lcd_show_string(160, 76, (uint8_t *)buf, hc, LCD_BLACK, 16, 0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "  --.-C  --.-%%  ");
        lcd_show_string(88, 76, (uint8_t *)buf, LCD_WHITE, LCD_BLACK, 16, 0);
    }

    if (ok & SENSOR_OK_MPU)
    {
        uint16_t tic = high_color(report->sensor.tilt_deg, TILT_YELLOW, TILT_ORANGE, TILT_RED);
        uint16_t vc = high_color(report->sensor.vibration, VIB_YELLOW, VIB_ORANGE, VIB_RED);
        snprintf(buf, sizeof(buf), "%5.1fdeg ", report->sensor.tilt_deg);
        lcd_show_string(88, 96, (uint8_t *)buf, tic, LCD_BLACK, 16, 0);
        snprintf(buf, sizeof(buf), "%5.0f  ", report->sensor.vibration);
        lcd_show_string(176, 96, (uint8_t *)buf, vc, LCD_BLACK, 16, 0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "  --.-deg    --  ");
        lcd_show_string(88, 96, (uint8_t *)buf, LCD_WHITE, LCD_BLACK, 16, 0);
    }

    if (ok & SENSOR_OK_BH1750)
    {
        snprintf(buf, sizeof(buf), "%5.0f f=%d   ",
                 report->sensor.lux, report->sensor.drain_src);
        lcd_show_string(88, 116, (uint8_t *)buf,
                        low_lux_color(report->sensor.lux), LCD_BLACK, 16, 0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "   -- f=%d   ", report->sensor.drain_src);
        lcd_show_string(88, 116, (uint8_t *)buf, LCD_WHITE, LCD_BLACK, 16, 0);
    }

    snprintf(buf, sizeof(buf), "%-8s", lifeline_level_en(report->level));
    lcd_show_string(88, 146, (uint8_t *)buf, lc, LCD_BLACK, 16, 0);

    snprintf(buf, sizeof(buf), "%-12s", report->source ? report->source : "none");
    lcd_show_string(88, 166, (uint8_t *)buf, lc, LCD_BLACK, 16, 0);

    snprintf(buf, sizeof(buf), "%s / %s   ",
             report->valve_state ? "OPEN " : "CLOSE",
             report->auto_state ? "AUTO" : "MAN ");
    lcd_show_string(120, 186, (uint8_t *)buf, LCD_WHITE, LCD_BLACK, 16, 0);

    snprintf(buf, sizeof(buf), "%s mute=%d beep=%d",
             mqtt_ok ? "ON " : "OFF", report->mute_state, report->beep_state);
    lcd_show_string(120, 206, (uint8_t *)buf,
                    report->mute_state ? LCD_RED : (mqtt_ok ? LCD_GREEN : LCD_WHITE),
                    LCD_BLACK, 16, 0);
}
