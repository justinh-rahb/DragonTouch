#pragma once

/* Clean-room K-Touch/PandaTouch hardware contract. Provenance and confidence are
 * recorded in docs/HARDWARE.md. These are facts, not copied driver code. */

#define DT_LCD_H_RES 800
#define DT_LCD_V_RES 480

#define DT_LCD_PCLK_HZ 14800000
#define DT_LCD_HSYNC_PULSE_WIDTH 4
#define DT_LCD_HSYNC_BACK_PORCH 16
#define DT_LCD_HSYNC_FRONT_PORCH 16
#define DT_LCD_VSYNC_PULSE_WIDTH 4
#define DT_LCD_VSYNC_BACK_PORCH 32
#define DT_LCD_VSYNC_FRONT_PORCH 32

#define DT_LCD_PCLK_GPIO 5
#define DT_LCD_DE_GPIO 38
#define DT_LCD_RESET_GPIO 46
#define DT_LCD_BACKLIGHT_GPIO 21

#define DT_LCD_R3_GPIO 6
#define DT_LCD_R4_GPIO 7
#define DT_LCD_R5_GPIO 8
#define DT_LCD_R6_GPIO 9
#define DT_LCD_R7_GPIO 10

#define DT_LCD_G2_GPIO 11
#define DT_LCD_G3_GPIO 12
#define DT_LCD_G4_GPIO 13
#define DT_LCD_G5_GPIO 14
#define DT_LCD_G6_GPIO 15
#define DT_LCD_G7_GPIO 16

#define DT_LCD_B3_GPIO 17
#define DT_LCD_B4_GPIO 18
#define DT_LCD_B5_GPIO 48
#define DT_LCD_B6_GPIO 47
#define DT_LCD_B7_GPIO 39

#define DT_TOUCH_I2C_PORT 0
#define DT_TOUCH_I2C_SCL_GPIO 1
#define DT_TOUCH_I2C_SDA_GPIO 2
#define DT_TOUCH_INTERRUPT_GPIO 40
#define DT_TOUCH_RESET_GPIO 41
#define DT_TOUCH_I2C_HZ 400000

#define DT_BACKLIGHT_PWM_HZ 30000

/* Every known signal is unique. A collision should fail at compile time rather
 * than present as a mysterious display/touch failure on hardware. */
enum {
    DT_BOARD_KNOWN_SIGNAL_COUNT = 24,
    DT_BOARD_KNOWN_SIGNAL_MASK_LO =
        (1ULL << DT_TOUCH_I2C_SCL_GPIO) |
        (1ULL << DT_TOUCH_I2C_SDA_GPIO) |
        (1ULL << DT_LCD_PCLK_GPIO) |
        (1ULL << DT_LCD_R3_GPIO) |
        (1ULL << DT_LCD_R4_GPIO) |
        (1ULL << DT_LCD_R5_GPIO) |
        (1ULL << DT_LCD_R6_GPIO) |
        (1ULL << DT_LCD_R7_GPIO) |
        (1ULL << DT_LCD_G2_GPIO) |
        (1ULL << DT_LCD_G3_GPIO) |
        (1ULL << DT_LCD_G4_GPIO) |
        (1ULL << DT_LCD_G5_GPIO) |
        (1ULL << DT_LCD_G6_GPIO) |
        (1ULL << DT_LCD_G7_GPIO) |
        (1ULL << DT_LCD_B3_GPIO) |
        (1ULL << DT_LCD_B4_GPIO) |
        (1ULL << DT_LCD_BACKLIGHT_GPIO),
    DT_BOARD_KNOWN_SIGNAL_MASK_HI =
        (1ULL << (DT_LCD_DE_GPIO - 32)) |
        (1ULL << (DT_LCD_B7_GPIO - 32)) |
        (1ULL << (DT_TOUCH_INTERRUPT_GPIO - 32)) |
        (1ULL << (DT_TOUCH_RESET_GPIO - 32)) |
        (1ULL << (DT_LCD_RESET_GPIO - 32)) |
        (1ULL << (DT_LCD_B6_GPIO - 32)) |
        (1ULL << (DT_LCD_B5_GPIO - 32))
};

_Static_assert(__builtin_popcountll(DT_BOARD_KNOWN_SIGNAL_MASK_LO) +
               __builtin_popcountll(DT_BOARD_KNOWN_SIGNAL_MASK_HI) ==
               DT_BOARD_KNOWN_SIGNAL_COUNT,
               "DragonTouch board GPIO assignments collide");
