

/*
--------------- ALARM CLOCK PROJECT FOR RASPBERRY PI PICO ----------------

SSD1306 128x64 OLED display is used as screen
It uses builtin RTC to handle time operations

While Core 0 displays texts on the screen,
Core 1 handles mainly logical operations and user inputs

Alarm is disabled by default
Multiple alarms cannot be set
Alarm keeps firing at most 60 seconds
Setting alarm automatically enables it
When the current mode is CLOCK, it activate SLEEP MODE after a while
In SLEEP MODE, the display is blank. To awake the machine, press a button
The communication between cores is maintained by global variables
Builtin LED indicates that Pico gets power and starts both cores
Instead of sleep, busy_wait is used because of the interrupt of alarm

--------------------------------------------------------------------------
*/


#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/rtc.h"
#include "OLED.h"


#define HIGH                1
#define LOW                 0

#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_FREQ           400000
#define OLED_SCL            19
#define OLED_SDA            18

#define LEFT_BUTTON         28
#define RIGHT_BUTTON        22
#define BACK_BUTTON         7
#define SELECT_BUTTON       11
#define BUZZER              13
#define LED                 12

#define MENU                0
#define CLOCK               1
#define SET_CLOCK_YEAR      2
#define SET_CLOCK_MONTH     3
#define SET_CLOCK_DAY       4
#define SET_CLOCK_WEEKDAY   5
#define SET_CLOCK_HOUR      6
#define SET_CLOCK_MIN       7
#define SET_CLOCK_SEC       8
#define SET_CLOCK_FINAL     9
#define ALARM_MENU          10
#define DISABLE_ALARM       11
#define SET_ALARM_HOUR      12
#define SET_ALARM_MIN       13
#define SET_ALARM_SEC       14
#define SET_ALARM_FINAL     15
#define SLEEP_MODE          16

#define WAIT_DURATION_MS                20
#define BUZZER_FREQ                     466 // NOTE_AS4
#define MAX_ALARM_TIME_SEC              60
#define SLEEP_MODE_ACTIVATION_TIME_MS   10000

enum Months {JAN=1, FEB, MAR, APR, MAY, JUN, JUL, AUG, SEP, OCT, NOV, DEC};
const char weekdays[7][10] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char months[12][4]   = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const uint8_t weekday_pixel_offset[7] = {28, 24, 22, 8, 18, 32, 20};

// Global variables reachable by both cores
bool datetime_set = false;
bool alarm_enabled = false; 
bool alarm_fired = false;
uint8_t current_mode = MENU;
uint8_t menu_index = 0;
uint8_t alarm_count = 0;
uint16_t sleep_mode_count = 0;
datetime_t alarm_settime;
datetime_t set_date;

datetime_t alarmtime = {
    .year  = -1, // doesnt matter
    .month = -1, // doesnt matter
    .day   = -1, // doesnt matter
    .dotw  = -1, // doesnt matter
    .hour  =  8, // The alarm fires whenever hour, min, and sec match with those of the current time
    .min   = 00,
    .sec   = 00
};

datetime_t date = {
    .year  = 2022,
    .month = 07,
    .day   = 01,
    .dotw  = 5,
    .hour  = 00,
    .min   = 00,
    .sec   = 00
};

// 0 <= year <= 4095
// 1 <= month <= 12 
// Returns the number of days on given month in given year
uint8_t number_of_days(uint16_t year, uint8_t month) {
    if (month==JAN || month==MAR || month==MAY || month==JUL || month==AUG || month==OCT || month==DEC)
        return 31;
    else if (month==APR || month==JUN || month==SEP || month==NOV) // the month must be FEB if this is false
        return 30;
    else if (year%400==0)
        return 29;
    else if (year%100==0)
        return 28;
    else if (year%4==0)
        return 29;
    else
        return 28;
}

// One period of sound at the given frequency from the buzzer
// If the frequency is zero, then it does nothing
void buzz(uint64_t freq) {
    if (freq == 0)  return;
    gpio_put(BUZZER, HIGH);
    busy_wait_us((uint64_t)500000/freq);
    gpio_put(BUZZER, LOW);
    busy_wait_us((uint64_t)500000/freq);
}

// The function that is called when the alarm is fired
// It waits user to press a button 
// If any button is not pressed for 1 min, then alarm is stopped
static void alarm_callback() {
    gpio_put(LED, HIGH);
    alarm_fired = true;
    uint64_t start = time_us_64();
    uint64_t duration_us = MAX_ALARM_TIME_SEC*1000000;
    uint64_t end = start + duration_us;
    while (!gpio_get(SELECT_BUTTON) && time_us_64()<end) buzz(BUZZER_FREQ); // Wait until button is pressed or 1 min
    while  (gpio_get(SELECT_BUTTON) && time_us_64()<end) buzz(BUZZER_FREQ); // Wait until button is released or 1 min period ends
    gpio_put(LED, LOW);
    gpio_put(BUZZER, LOW);
    alarm_fired = false;
    alarm_count = 0;
    while (gpio_get(SELECT_BUTTON)); // Wait Select Button to be released, if it is still pressed
    busy_wait_ms(WAIT_DURATION_MS); // Wait a bit to prevent the button from bouncing
}

// Core 1 Main
// Handles inputs given via buttons
// Sets and fires alarms
void core1_main() {
    // Initialise the buttons
    gpio_init(LEFT_BUTTON);
    gpio_set_dir(LEFT_BUTTON, GPIO_IN);
    gpio_init(RIGHT_BUTTON);
    gpio_set_dir(RIGHT_BUTTON, GPIO_IN);
    gpio_init(BACK_BUTTON);
    gpio_set_dir(BACK_BUTTON, GPIO_IN);
    gpio_init(SELECT_BUTTON);
    gpio_set_dir(SELECT_BUTTON, GPIO_IN);
    
    // Initialise the LED and the buzzer
    gpio_init(BUZZER);
    gpio_set_dir(BUZZER, GPIO_OUT);
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);

    // Initialise the builtin LED and power it
    // It indicates that both cores of Pico are running without any problem
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, HIGH);

    // Core 1 Main Loop
    while (true) {
        if (current_mode == CLOCK) {
            if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON)); // wait button to be released
                sleep_mode_count = 0; // reset sleep count
                current_mode = MENU;
            }
            // if there is no activity for a while, then activate sleep mode
            sleep_mode_count++;
            if (sleep_mode_count==SLEEP_MODE_ACTIVATION_TIME_MS/WAIT_DURATION_MS) {
                sleep_mode_count = 0;
                current_mode = SLEEP_MODE;
            }
        }
        else if (current_mode == SLEEP_MODE) {
            while (!gpio_get(SELECT_BUTTON) && !gpio_get(BACK_BUTTON) && \
                     !gpio_get(LEFT_BUTTON) && !gpio_get(RIGHT_BUTTON)) {
                        busy_wait_ms(WAIT_DURATION_MS);
            }
            while (gpio_get(SELECT_BUTTON) || gpio_get(BACK_BUTTON) || \
                     gpio_get(LEFT_BUTTON) || gpio_get(RIGHT_BUTTON));  // Wait buttons to be released
            current_mode = CLOCK; // After the button is released, go back to CLOCK mode
        }
        else if (current_mode == MENU) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                menu_index = (menu_index==0)?2:menu_index-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                menu_index = (menu_index==2)?0:menu_index+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                if (menu_index == 0) {
                    current_mode = CLOCK;
                }
                else if (menu_index == 1) {
                    current_mode = SET_CLOCK_YEAR;
                    rtc_get_datetime(&set_date);
                }
                else if (menu_index == 2) {
                    current_mode = ALARM_MENU;
                }
                menu_index = 0; 
            }
        }
        else if (current_mode == SET_CLOCK_YEAR) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.year = (set_date.year==0)?4095:set_date.year-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.year = (set_date.year==4095)?0:set_date.year+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_CLOCK_MONTH;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = MENU;
            }
        }
        else if (current_mode == SET_CLOCK_MONTH) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.month = (set_date.month==1)?12:set_date.month-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.month = (set_date.month==12)?1:set_date.month+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                // The number of days in a month depends on the month and the year
                uint8_t day_num = number_of_days(set_date.year, set_date.month);
                set_date.day = (set_date.day>day_num)?day_num:set_date.day;
                current_mode = SET_CLOCK_DAY;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_YEAR;
            }
        }
        else if (current_mode == SET_CLOCK_DAY) {
            uint8_t day_num = number_of_days(set_date.year, set_date.month);
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.day = (set_date.day==1)?day_num:set_date.day-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.day = (set_date.day==day_num)?1:set_date.day+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_CLOCK_WEEKDAY;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_MONTH;
            }
        }
        else if (current_mode == SET_CLOCK_WEEKDAY) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.dotw = (set_date.dotw==0)?6:set_date.dotw-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.dotw = (set_date.dotw==6)?0:set_date.dotw+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_CLOCK_HOUR;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_DAY;
            }
        }
        else if (current_mode == SET_CLOCK_HOUR) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.hour = (set_date.hour==0)?23:set_date.hour-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.hour = (set_date.hour==23)?0:set_date.hour+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_CLOCK_MIN;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_WEEKDAY;
            }
        }
        else if (current_mode == SET_CLOCK_MIN) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.min = (set_date.min==0)?59:set_date.min-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.min = (set_date.min==59)?0:set_date.min+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_CLOCK_SEC;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_HOUR;
            }
        }
        else if (current_mode == SET_CLOCK_SEC) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                set_date.sec = (set_date.sec==0)?59:set_date.sec-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                set_date.sec = (set_date.sec==59)?0:set_date.sec+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                datetime_set = rtc_set_datetime(&set_date);
                current_mode = SET_CLOCK_FINAL;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_CLOCK_MIN;
            }
        }
        else if (current_mode == ALARM_MENU) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                menu_index = (menu_index==0)?1:0;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                menu_index = (menu_index==1)?0:1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                if (menu_index == 0) {
                    if (alarm_enabled)
                        rtc_disable_alarm();
                    else
                        rtc_set_alarm(&alarmtime, &alarm_callback);
                    alarm_enabled = !alarm_enabled;
                    current_mode = DISABLE_ALARM;
                }
                else if (menu_index == 1) {
                    current_mode = SET_ALARM_HOUR;
                    alarm_settime = alarmtime;
                }
                menu_index = 0;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = MENU;
                menu_index = 0;
            }
        }
        else if (current_mode == SET_ALARM_HOUR) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                alarm_settime.hour = (alarm_settime.hour==0)?23:alarm_settime.hour-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                alarm_settime.hour = (alarm_settime.hour==23)?0:alarm_settime.hour+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_ALARM_MIN;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = MENU;
            }
        }
        else if (current_mode == SET_ALARM_MIN) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                alarm_settime.min = (alarm_settime.min==0)?59:alarm_settime.min-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                alarm_settime.min = (alarm_settime.min==59)?0:alarm_settime.min+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = SET_ALARM_SEC;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_ALARM_HOUR;
            }
        }
        else if (current_mode == SET_ALARM_SEC) {
            if (gpio_get(LEFT_BUTTON)) {
                while (gpio_get(LEFT_BUTTON));
                alarm_settime.sec = (alarm_settime.sec==0)?59:alarm_settime.sec-1;
            }
            else if (gpio_get(RIGHT_BUTTON)) {
                while (gpio_get(RIGHT_BUTTON));
                alarm_settime.sec = (alarm_settime.sec==59)?0:alarm_settime.sec+1;
            }
            else if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                alarmtime = alarm_settime;
                rtc_set_alarm(&alarmtime, &alarm_callback);
                alarm_enabled = true;
                current_mode = SET_ALARM_FINAL;
            }
            else if (gpio_get(BACK_BUTTON)) {
                while (gpio_get(BACK_BUTTON));
                current_mode = SET_ALARM_MIN;
            }
        }
        else if (current_mode == SET_ALARM_FINAL || \
                 current_mode == SET_CLOCK_FINAL || \
                 current_mode == DISABLE_ALARM) {
            if (gpio_get(SELECT_BUTTON)) {
                while (gpio_get(SELECT_BUTTON));
                current_mode = CLOCK;
            }
        }
        busy_wait_ms(WAIT_DURATION_MS); // to prevent buttons from bouncing
    } // end of while loop
}

// Core 0 Main
// Renders texts for the display
int main() {
    // Initialise and clear the OLED display
    OLED oled(OLED_SCL, OLED_SDA, OLED_WIDTH, OLED_HEIGHT, OLED_FREQ, i2c1);
    oled.clear();
    oled.show();

    // Start RTC
    rtc_init();
    rtc_set_datetime(&date);

    // Start Core 1
    multicore_launch_core1(core1_main);

    // Create a buffer to print string to OLED display
    char oled_str[30];

    // Core 0 Main Loop
    while (true) {
        oled.clear();
        if (alarm_fired) { // Display alarm message
            if (alarm_count < 8) { // When alarm fires, the alarm message flicks
                oled.print(32, 8, (uint8_t *)"ALARM");
                sprintf(oled_str, "%02hi:%02hi:%02hi", (uint16_t)alarmtime.hour, (uint16_t)alarmtime.min, (uint16_t)alarmtime.sec);
                oled.print(20, 32, (uint8_t *)oled_str);
                alarm_count++;
            }
            else {
                oled.show(); // blank display
                busy_wait_ms(80);
                alarm_count = 0;
                continue;
            }
        }
        else if (current_mode == MENU) {
            oled.print(8, 0, (uint8_t *)"CLOCK");
            oled.print(8, 20, (uint8_t *)"SET CLOCK");
            oled.print(8, 40, (uint8_t *)"ALARM");
            oled.print(0, 20*menu_index, (uint8_t *)"-");
        }
        else if (current_mode == ALARM_MENU) {
            if (alarm_enabled)
                oled.print(8, 0, (uint8_t *)"DISABLE");
            else 
                oled.print(8, 0, (uint8_t *)"ENABLE");
            oled.print(8, 20, (uint8_t *)"SET");
            oled.print(0, 20*menu_index, (uint8_t *)"-");
        }
        else if (current_mode == DISABLE_ALARM) {
            oled.print(12, 8, (uint8_t *)"ALARM IS");
            if (alarm_enabled)
                oled.print(14, 32, (uint8_t *)"ENABLED");
            else
                oled.print(12, 32, (uint8_t *)"DISABLED");
        }
        else if (current_mode == CLOCK) {
            bool rtc_running = rtc_get_datetime(&date);
            if (rtc_running) {
                sprintf(oled_str, "%02hi %s %04hi", (uint16_t)date.day, months[date.month-1], date.year);
                oled.print(2, 0, (uint8_t *)oled_str);
                sprintf(oled_str, "%02hi:%02hi:%02hi", (uint16_t)date.hour, (uint16_t)date.min, (uint16_t)date.sec);
                oled.print(16, 20, (uint8_t *)oled_str);
                sprintf(oled_str, "%s", weekdays[date.dotw]);
                oled.print(weekday_pixel_offset[date.dotw], 40, (uint8_t *)oled_str);
            }
            else {
                oled.print(24, 8, (uint8_t *)"RTC NOT");
                oled.print(24, 32, (uint8_t *)"WORKING");
            }
        }
        else if (current_mode == SLEEP_MODE) {
            oled.show(); // Blank display
            while (current_mode == SLEEP_MODE && !alarm_fired) { // Wait until Core 1 changes the current mode or alarm fires
                tight_loop_contents();
            }
            continue;
        }
        else if (SET_CLOCK_YEAR <= current_mode && current_mode <= SET_CLOCK_SEC) {
            switch (current_mode) {
                case SET_CLOCK_YEAR:
                    sprintf(oled_str, "%04hi", set_date.year);
                    oled.print(8, 8, (uint8_t *)"YEAR");
                    break;
                case SET_CLOCK_MONTH:
                    sprintf(oled_str, "%s", months[set_date.month-1]);
                    oled.print(8, 8, (uint8_t *)"MONTH");
                    break;
                case SET_CLOCK_DAY:
                    sprintf(oled_str, "%02hi", (uint16_t)set_date.day);
                    oled.print(8, 8, (uint8_t *)"DAY");
                    break;
                case SET_CLOCK_WEEKDAY:
                    sprintf(oled_str, "%s", weekdays[set_date.dotw]);
                    oled.print(8, 8, (uint8_t *)"WEEKDAY");
                    break;
                case SET_CLOCK_HOUR:
                    sprintf(oled_str, "%02hi", (uint16_t)set_date.hour);
                    oled.print(8, 8, (uint8_t *)"HOUR");
                    break;
                case SET_CLOCK_MIN:
                    sprintf(oled_str, "%02hi", (uint16_t)set_date.min);
                    oled.print(8, 8, (uint8_t *)"MIN");
                    break;
                case SET_CLOCK_SEC:
                    sprintf(oled_str, "%02hi", (uint16_t)set_date.sec);
                    oled.print(8, 8, (uint8_t *)"SEC");
            }
            oled.print(8, 28, (uint8_t *)oled_str);
        }
        else if (current_mode == SET_CLOCK_FINAL) {
            if (datetime_set) {
                oled.print(30, 8, (uint8_t *)"CLOCK");
                oled.print(30, 32, (uint8_t *)"IS SET");
            }
            else {
                oled.print(8, 8, (uint8_t *)"INVALID");
                oled.print(24, 32, (uint8_t *)"DATE");
            }
        }
        else if (SET_ALARM_HOUR <= current_mode && current_mode <= SET_ALARM_SEC) {
            switch (current_mode) {
                case SET_ALARM_HOUR:
                    sprintf(oled_str, "%02hi", (uint16_t)alarm_settime.hour);
                    oled.print(0, 8, (uint8_t *)"ALARM HOUR");
                    break;
                case SET_ALARM_MIN:
                    sprintf(oled_str, "%02hi", (uint16_t)alarm_settime.min);
                    oled.print(0, 8, (uint8_t *)"ALARM MIN");
                    break;
                case SET_ALARM_SEC:
                    sprintf(oled_str, "%02hi", (uint16_t)alarm_settime.sec);
                    oled.print(0, 8, (uint8_t *)"ALARM SEC");
            }
            oled.print(0, 28, (uint8_t *)oled_str);
        }
        else if (current_mode == SET_ALARM_FINAL) {
            oled.print(30, 8, (uint8_t *)"ALARM");
            oled.print(30, 32, (uint8_t *)"IS SET");
        }
        oled.show();
    } // end of while loop
}


