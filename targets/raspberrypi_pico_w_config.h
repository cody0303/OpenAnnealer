#ifndef RASPBERRYPI_PICO_W_CONFIG_H_
#define RASPBERRYPI_PICO_W_CONFIG_H_

#include "pico/cyw43_arch.h"

/* Specify board PIN mapping
    Reference: https://github.com/eamars/RaspberryPi-Pico-Motor-Expansion-Board?tab=readme-ov-file#peripherals
*/

#define WATCHDOG_LED_PIN CYW43_WL_GPIO_LED_PIN

#define DISPLAY0_SPI spi0
#define DISPLAY0_RX_PIN 16
#define DISPLAY0_TX_PIN 19
#define DISPLAY0_CS_PIN 17
#define DISPLAY0_SCK_PIN 18
#define DISPLAY0_A0_PIN 20
#define DISPLAY0_RESET_PIN 21

#define BUTTON0_ENCODER_PIN1 15
#define BUTTON0_ENCODER_PIN2 14
#define BUTTON0_ENC_PIN 22
#define BUTTON0_RST_PIN 12
#define NEOPIXEL_PIN 13
#define NEOPIXEL_PWM3_PIN 28

#define MOTOR_UART uart1
#define MOTOR_UART_TX 4
#define MOTOR_UART_RX 5
#define MOTOR_PIO pio0

// Feeder motor: advances cases into the holder
#define FEEDER_MOTOR_ADDR 0
#define FEEDER_MOTOR_EN_PIN 6
#define FEEDER_MOTOR_STEP_PIN 3
#define FEEDER_MOTOR_DIR_PIN 2

// Spare motor: unused for now, reserved for a future case-feed enhancement
#define SPARE_MOTOR_ADDR 1
#define SPARE_MOTOR_EN_PIN 9
#define SPARE_MOTOR_STEP_PIN 8
#define SPARE_MOTOR_DIR_PIN 7

// GPIO 0/1 (formerly SCALE_UART) are free since the scale subsystem was removed
// (Milestone 8) - available for a future sensor, e.g. Milestone 11's I2C temp sensor.

#define EEPROM_I2C i2c1
#define EEPROM_SDA_PIN 10
#define EEPROM_SCL_PIN 11
#define EEPROM_ADDR 0x50

// Only one physical servo is used (the case holder) so only one PWM channel is claimed.
// GPIO26 (the pin the old dual-shutter design used as its second channel) is left free
// for other uses below.
#define SERVO_PWM_PIN 27
#define SERVO_PWM_SLICE_NUM 5

// Default induction heater trigger pin, used only to seed the EEPROM default the first
// time the board boots. The actual pin in use is runtime-configurable (see
// induction_heater.h) and stored in EEPROM, settable via /rest/induction_heater_config
// or the web UI, since the right pin depends on how each build is wired.
#define INDUCTION_TRIGGER_PIN_DEFAULT 26

#endif  // RASPBERRYPI_PICO_W_CONFIG_H_