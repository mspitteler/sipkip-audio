#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/ledc_periph.h"

#include "muxed-gpio.h"

#define GPIO_NUM_MUX_BUTTONS_OUT  GPIO_NUM_4
#define GPIO_NUM_MUX_CLIPS_OUT    GPIO_NUM_5        

/* These are all the buttons on the tail. */
#define GPIO_NUM_STAR_L_INOUT     GPIO_NUM_21
#define GPIO_NUM_TRIANGLE_L_INOUT GPIO_NUM_19
#define GPIO_NUM_SQUARE_L_INOUT   GPIO_NUM_27
#define GPIO_NUM_HEART_L_INOUT    GPIO_NUM_14
#define GPIO_NUM_HEART_R_INOUT    GPIO_NUM_12
#define GPIO_NUM_SQUARE_R_INOUT   GPIO_NUM_13
#define GPIO_NUM_TRIANGLE_R_INOUT GPIO_NUM_18
#define GPIO_NUM_STAR_R_INOUT     GPIO_NUM_16
/* This is the beak button. */
#define GPIO_NUM_BEAK_IN          GPIO_NUM_15
/* These are the inputs for the switch. */
#define GPIO_NUM_SWITCH_LEARN_IN  GPIO_NUM_34
#define GPIO_NUM_SWITCH_PLAY_IN   GPIO_NUM_35
/* These are the outputs for the LEDs. */
#define GPIO_NUM_LED_L_OUT        GPIO_NUM_22
#define GPIO_NUM_LED_M_OUT        GPIO_NUM_26
#define GPIO_NUM_LED_R_OUT        GPIO_NUM_17

#define GPIO_PIN_BIT_MASK_OUT     ((1ULL << GPIO_NUM_LED_L_OUT)        | (1ULL << GPIO_NUM_LED_M_OUT)        |     \
                                   (1ULL << GPIO_NUM_LED_R_OUT)        | (1ULL << GPIO_NUM_MUX_BUTTONS_OUT)  |     \
                                   (1ULL << GPIO_NUM_MUX_CLIPS_OUT))
#define GPIO_PIN_BIT_MASK_INOUT   ((1ULL << GPIO_NUM_STAR_L_INOUT)     | (1ULL << GPIO_NUM_TRIANGLE_L_INOUT) |     \
                                   (1ULL << GPIO_NUM_SQUARE_L_INOUT)   | (1ULL << GPIO_NUM_HEART_L_INOUT)    |     \
                                   (1ULL << GPIO_NUM_HEART_R_INOUT)    | (1ULL << GPIO_NUM_SQUARE_R_INOUT)   |     \
                                   (1ULL << GPIO_NUM_TRIANGLE_R_INOUT) | (1ULL << GPIO_NUM_STAR_R_INOUT))
#define GPIO_PIN_BIT_MASK_IN      ((1ULL << GPIO_NUM_BEAK_IN)          | (1ULL << GPIO_NUM_SWITCH_LEARN_IN)  |     \
                                   (1ULL << GPIO_NUM_SWITCH_PLAY_IN))

#define MUX_PWM_FREQUENCY   200UL                    /* 200 Hz. */
#define MUX_PWM_BITS        8                        /* Dutycycle is max 255. */

#define MUX_PWM_MAX                                                                                                \
    ((1UL << MUX_PWM_BITS) - 1UL)
#define MUX_TIMER_BITS_TMP(bits)                                                                                   \
    LEDC_TIMER_##bits##_BIT
#define MUX_TIMER_BITS(bits)                                                                                       \
    MUX_TIMER_BITS_TMP(bits)
    
struct interrupt_handler_args {
    gpio_num_t gpio_num;
    muxed_inputs_on_changed_fn fn;
};

static const enum muxed_inputs gpio_num_to_muxed_inouts[][GPIO_NUM_MAX] = {{
    /* 0..7 are inputs for the buttons that can be read if GPIO_NUM_MUX_BUTTONS_OUT is set high. */
    [GPIO_NUM_STAR_L_INOUT] = MUXED_INPUT_STAR_L_BUTTON, [GPIO_NUM_TRIANGLE_L_INOUT] = MUXED_INPUT_TRIANGLE_L_BUTTON,
    [GPIO_NUM_SQUARE_L_INOUT] = MUXED_INPUT_SQUARE_L_BUTTON, [GPIO_NUM_HEART_L_INOUT] = MUXED_INPUT_HEART_L_BUTTON,
    [GPIO_NUM_HEART_R_INOUT] = MUXED_INPUT_HEART_R_BUTTON, [GPIO_NUM_SQUARE_R_INOUT] = MUXED_INPUT_SQUARE_R_BUTTON,
    [GPIO_NUM_TRIANGLE_R_INOUT] = MUXED_INPUT_TRIANGLE_R_BUTTON, [GPIO_NUM_STAR_R_INOUT] = MUXED_INPUT_STAR_R_BUTTON,
    /* 8..15 are inputs for the clips that can be read if GPIO_NUM_MUX_CLIPS_OUT is set high. */
    [GPIO_NUM_BEAK_IN] = MUXED_INPUT_BEAK_SWITCH,
    [GPIO_NUM_SWITCH_LEARN_IN] = MUXED_INPUT_LEARN_SWITCH, [GPIO_NUM_SWITCH_PLAY_IN] = MUXED_INPUT_PLAY_SWITCH
}, {
    /* 0..7 are inputs for the buttons that can be read if GPIO_NUM_MUX_BUTTONS_OUT is set high. */
    [GPIO_NUM_STAR_L_INOUT] = MUXED_INPUT_STAR_L_CLIP, [GPIO_NUM_TRIANGLE_L_INOUT] = MUXED_INPUT_TRIANGLE_L_CLIP,
    [GPIO_NUM_SQUARE_L_INOUT] = MUXED_INPUT_SQUARE_L_CLIP, [GPIO_NUM_HEART_L_INOUT] = MUXED_INPUT_HEART_L_CLIP,
    [GPIO_NUM_HEART_R_INOUT] = MUXED_INPUT_HEART_R_CLIP, [GPIO_NUM_SQUARE_R_INOUT] = MUXED_INPUT_SQUARE_R_CLIP,
    [GPIO_NUM_TRIANGLE_R_INOUT] = MUXED_INPUT_TRIANGLE_R_CLIP, [GPIO_NUM_STAR_R_INOUT] = MUXED_INPUT_STAR_R_CLIP,
    /* 8..15 are inputs for the clips that can be read if GPIO_NUM_MUX_CLIPS_OUT is set high. */
    [GPIO_NUM_BEAK_IN] = MUXED_INPUT_BEAK_SWITCH,
    [GPIO_NUM_SWITCH_LEARN_IN] = MUXED_INPUT_LEARN_SWITCH, [GPIO_NUM_SWITCH_PLAY_IN] = MUXED_INPUT_PLAY_SWITCH
}};
static const gpio_num_t muxed_inouts_to_gpio_num[] = {
    /* 0..7 are inputs for the buttons that can be read if GPIO_NUM_MUX_BUTTONS_OUT is set high. */
    [MUXED_INPUT_STAR_L_BUTTON] = GPIO_NUM_STAR_L_INOUT, [MUXED_INPUT_TRIANGLE_L_BUTTON] = GPIO_NUM_TRIANGLE_L_INOUT,
    [MUXED_INPUT_SQUARE_L_BUTTON] = GPIO_NUM_SQUARE_L_INOUT, [MUXED_INPUT_HEART_L_BUTTON] = GPIO_NUM_HEART_L_INOUT,
    [MUXED_INPUT_HEART_R_BUTTON] = GPIO_NUM_HEART_R_INOUT, [MUXED_INPUT_SQUARE_R_BUTTON] = GPIO_NUM_SQUARE_R_INOUT,
    [MUXED_INPUT_TRIANGLE_R_BUTTON] = GPIO_NUM_TRIANGLE_R_INOUT, [MUXED_INPUT_STAR_R_BUTTON] = GPIO_NUM_STAR_R_INOUT,
    /* 8..15 are inputs for the clips that can be read if GPIO_NUM_MUX_CLIPS_OUT is set high. */
    [MUXED_INPUT_STAR_L_CLIP] = GPIO_NUM_STAR_L_INOUT, [MUXED_INPUT_TRIANGLE_L_CLIP] = GPIO_NUM_TRIANGLE_L_INOUT,
    [MUXED_INPUT_SQUARE_L_CLIP] = GPIO_NUM_SQUARE_L_INOUT, [MUXED_INPUT_HEART_L_CLIP] = GPIO_NUM_HEART_L_INOUT,
    [MUXED_INPUT_HEART_R_CLIP] = GPIO_NUM_HEART_R_INOUT, [MUXED_INPUT_SQUARE_R_CLIP] = GPIO_NUM_SQUARE_R_INOUT,
    [MUXED_INPUT_TRIANGLE_R_CLIP] = GPIO_NUM_TRIANGLE_R_INOUT, [MUXED_INPUT_STAR_R_CLIP] = GPIO_NUM_STAR_R_INOUT,
    /* The following GPIOs are only ever used as inputs. */
    [MUXED_INPUT_BEAK_SWITCH] = GPIO_NUM_BEAK_IN,
    [MUXED_INPUT_LEARN_SWITCH] = GPIO_NUM_SWITCH_LEARN_IN, [MUXED_INPUT_PLAY_SWITCH] = GPIO_NUM_SWITCH_PLAY_IN
};

/*                          { GPIO_NUM_LED_*_OUT, GPIO_NUM_*_*_INOUT } */
static const struct { ledc_channel_t lmr_channel, bc_channel; } muxed_inouts_to_ledc_channel[] = {
    [MUXED_OUTPUT_STAR_L_LED] = { .lmr_channel = LEDC_CHANNEL_0, .bc_channel = LEDC_CHANNEL_4 },
    [MUXED_OUTPUT_TRIANGLE_L_LED] = { .lmr_channel = LEDC_CHANNEL_0, .bc_channel = LEDC_CHANNEL_5 },
    [MUXED_OUTPUT_SQUARE_L_LED] = { .lmr_channel = LEDC_CHANNEL_1, .bc_channel = LEDC_CHANNEL_4 },
    [MUXED_OUTPUT_HEART_L_LED] = { .lmr_channel = LEDC_CHANNEL_1, .bc_channel = LEDC_CHANNEL_5 },
    [MUXED_OUTPUT_HEART_R_LED] = { .lmr_channel = LEDC_CHANNEL_1, .bc_channel = LEDC_CHANNEL_6 },
    [MUXED_OUTPUT_SQUARE_R_LED] = { .lmr_channel = LEDC_CHANNEL_1, .bc_channel = LEDC_CHANNEL_7 },
    [MUXED_OUTPUT_TRIANGLE_R_LED] = { .lmr_channel = LEDC_CHANNEL_0, .bc_channel = LEDC_CHANNEL_6 },
    [MUXED_OUTPUT_STAR_R_LED] = { .lmr_channel = LEDC_CHANNEL_0, .bc_channel = LEDC_CHANNEL_7 }
};

/**
 *                           |___               ______               ______               ______
 * GPIO_NUM_LED_*_OUT:       |   |             |      |             |      |             |      |
 *                           |    ^^^^^^^^^^^^^        ^^^^^^^^^^^^^        ^^^^^^^^^^^^^        ^^^
 *                           |    ______               ______               ______               ___
 * GPIO_NUM_MUX_BUTTONS_OUT: |   |      |             |      |             |      |             |
 *                           |^^^        ^^^^^^^^^^^^^        ^^^^^^^^^^^^^        ^^^^^^^^^^^^^
 *                           |           ______               ______               ______
 * GPIO_NUM_MUX_CLIPS_OUT:   |          |      |             |      |             |      |
 *                           |^^^^^^^^^^        ^^^^^^^^^^^^^       ^^^^^^^^^^^^^^        ^^^^^^^^^^
 */

static volatile bool muxed_input_levels[MUXED_INPUT_N];
    
static const ledc_timer_config_t ledc_timer0_config = {
    .speed_mode       = LEDC_HIGH_SPEED_MODE,
    .timer_num        = LEDC_TIMER_0, 
    .duty_resolution  = MUX_TIMER_BITS(MUX_PWM_BITS),
    .freq_hz          = MUX_PWM_FREQUENCY,
    .clk_cfg          = LEDC_USE_REF_TICK
};

static const ledc_channel_config_t ledc_channel_configs[] = {
    {
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = GPIO_NUM_LED_L_OUT,
        .duty           = MUX_PWM_MAX / 3,
        .hpoint         = 0 /* Set the timer value at which the output will be latched. */
    },
    {
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_1,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = GPIO_NUM_LED_M_OUT,
        .duty           = MUX_PWM_MAX / 3,
        .hpoint         = 0 /* Set the timer value at which the output will be latched. */
    },
    {
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = GPIO_NUM_LED_R_OUT,
        .duty           = MUX_PWM_MAX / 3,
        .hpoint         = 0 /* Set the timer value at which the output will be latched. */
    },
    {
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_2,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = GPIO_NUM_MUX_BUTTONS_OUT,
        .duty           = MUX_PWM_MAX / 3,
        .hpoint         = MUX_PWM_MAX / 3, /* Set the timer value at which the output will be latched. */
    },
    {
        .speed_mode     = LEDC_HIGH_SPEED_MODE,
        .channel        = LEDC_CHANNEL_3,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = GPIO_NUM_MUX_CLIPS_OUT,
        .duty           = MUX_PWM_MAX / 3,
        .hpoint         = MUX_PWM_MAX * 2 / 3, /* Set the timer value at which the output will be latched. */
    }
};

static const uint64_t gpio_pin_bit_mask_inout = GPIO_PIN_BIT_MASK_INOUT;
static const uint64_t gpio_pin_bit_mask_in = GPIO_PIN_BIT_MASK_IN;

static const gpio_config_t gpio_configs[] = {
    {
        /* Set as output mode. */
        .mode         = GPIO_MODE_OUTPUT,
        /* Bit mask of the pins that you want to set as outputs. */
        .pin_bit_mask = GPIO_PIN_BIT_MASK_OUT,
        /* Disable pull-down mode. */
        .pull_down_en = 0,
        /* Disable interrupts. */
        .intr_type    = GPIO_INTR_DISABLE
    },
    {
        /* Bit mask of the pins, use inputs here. */
        .pin_bit_mask = gpio_pin_bit_mask_in,
        /* Set as input mode. */
        .mode         = GPIO_MODE_INPUT,
        /* Enable pull-down mode. */
        .pull_down_en = 1,
        /* Enable interrupt on rising edge. */
        .intr_type    = GPIO_INTR_POSEDGE
    },
    {
        /* Bit mask of the pins, use input/open drain output here. */
        .pin_bit_mask = gpio_pin_bit_mask_inout,
        /* Set as input/output mode. */
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        /* Enable pull-down mode. */
        .pull_down_en = 1,
        /* Enable interrupt on rising edge. */
        .intr_type    = GPIO_INTR_POSEDGE
    }
};

static void IRAM_ATTR gpio_buttons_and_clips_interrupt_handler(void *arg) {
    /* Get input from buttons if true, and from clips if false. */
    bool muxed_input_buttons;
    struct interrupt_handler_args *args = arg;
    
    if ((muxed_input_buttons = gpio_get_level(GPIO_NUM_MUX_BUTTONS_OUT)) || gpio_get_level(GPIO_NUM_MUX_CLIPS_OUT)) {
        /* Get input. */
        bool level;
        if (muxed_input_levels[gpio_num_to_muxed_inouts[!muxed_input_buttons][args->gpio_num]]
            != (level = gpio_get_level(args->gpio_num))) {
            muxed_input_levels[gpio_num_to_muxed_inouts[!muxed_input_buttons][args->gpio_num]] = level;
            args->fn(&muxed_input_levels);
        }
    }
}

static void IRAM_ATTR gpio_switches_interrupt_handler(void *arg) {
    struct interrupt_handler_args *args = arg;
    bool level;
    /* Get input. */
    /* Whether MUX_BUTTONS or MUX_CLIPS is high doesn't matter here. */
    if (muxed_input_levels[gpio_num_to_muxed_inouts[0][args->gpio_num]] != (level = gpio_get_level(args->gpio_num))) {
        muxed_input_levels[gpio_num_to_muxed_inouts[0][args->gpio_num]] = level;
        args->fn(&muxed_input_levels);
    }
}

void muxed_gpio_setup(muxed_inputs_on_changed_fn fn) {
    /* Configure GPIO with the given settings. */
    for (int i = 0; i < sizeof(gpio_configs) / sizeof(*gpio_configs); i++)
        ESP_ERROR_CHECK(gpio_config(&gpio_configs[i]));

    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer0_config));
    
    for (int i = 0; i < sizeof(ledc_channel_configs) / sizeof(*ledc_channel_configs); i++) {
        gpio_num_t gpio_num = ledc_channel_configs[i].gpio_num;
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_configs[i]));
        if (gpio_num == GPIO_NUM_MUX_BUTTONS_OUT || gpio_num == GPIO_NUM_MUX_CLIPS_OUT) {
            /**
             * We also need these two GPIOs to function as inputs, since we're getting the level of them
             * in the interrupt for the clips and buttons.
             */
            ESP_ERROR_CHECK(gpio_set_direction(gpio_num, GPIO_MODE_INPUT_OUTPUT));
            esp_rom_gpio_connect_out_signal(gpio_num,
                                            ledc_periph_signal[ledc_channel_configs[i].speed_mode].sig_out0_idx + 
                                                ledc_channel_configs[i].channel, 
                                            ledc_channel_configs[i].flags.output_invert, 0);
        }
        ESP_ERROR_CHECK(ledc_update_duty(ledc_channel_configs[i].speed_mode, ledc_channel_configs[i].channel));
    }
   
    /* Output the same signal on the star, triangle... in/outputs as on the LED_* outputs, but then inverted. */
    for (enum muxed_outputs i = MUXED_OUTPUT_MIN; i <= MUXED_OUTPUT_MAX; i++) {
       ledc_channel_config_t config = ledc_channel_configs[0];
       config.gpio_num = muxed_inouts_to_gpio_num[i];
       config.channel = muxed_inouts_to_ledc_channel[i].bc_channel;
       config.flags.output_invert = 1;
       ESP_ERROR_CHECK(ledc_channel_config(&config));
       /**
        * Hack to use open drain outputs for the star, triangle... in/outputs, since ledc_channel_config()
        * always sets the pin to regular outputs. But if we would only call gpio_set_direction() here, the
        * signal would get disconnected from the pin, so we have to call esp_rom_gpio_connect_out_signal()
        * again (which is normally called in ledc_channel_config()).
        */
       ESP_ERROR_CHECK(gpio_set_direction(config.gpio_num, GPIO_MODE_INPUT_OUTPUT_OD));
       esp_rom_gpio_connect_out_signal(config.gpio_num,
                                       ledc_periph_signal[config.speed_mode].sig_out0_idx + config.channel, 
                                       config.flags.output_invert, 0);
       ESP_ERROR_CHECK(ledc_update_duty(config.speed_mode, config.channel));
    }
    
    /* Register interrupt callbacks for the buttons and clips in/outputs. */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    uint64_t bit_mask = gpio_pin_bit_mask_inout;
    for (int test_bit = 0; test_bit < sizeof(gpio_pin_bit_mask_inout) * 8; test_bit++, bit_mask >>= 1) {
        if (bit_mask & 1) {
            struct interrupt_handler_args *args = malloc(sizeof(*args));
            args->gpio_num = test_bit;
            args->fn = fn;
            ESP_ERROR_CHECK(gpio_isr_handler_add(args->gpio_num, &gpio_buttons_and_clips_interrupt_handler, args));
        }
    }
    
    /* Register interrupt callbacks for the switches inputs. */
    bit_mask = gpio_pin_bit_mask_in;
    for (int test_bit = 0; test_bit < sizeof(gpio_pin_bit_mask_in) * 8; test_bit++, bit_mask >>= 1) {
        if (bit_mask & 1) {
            struct interrupt_handler_args *args = malloc(sizeof(*args));
            args->gpio_num = test_bit;
            args->fn = fn;
            ESP_ERROR_CHECK(gpio_isr_handler_add(args->gpio_num, &gpio_switches_interrupt_handler, args));
        }
    }

#if 0
    // Turn off LEDC or PWM
    for (int i = 0; i < sizeof(ledc_channels) / sizeof(*ledc_channels); i++)
        ESP_ERROR_CHECK(ledc_stop(ledc_channels[i].speed_mode, ledc_channels[i].channel, 0));
    gpio_uninstall_isr_service();
#endif
}

void muxed_gpio_set_output_levels(bool (*levels)[MUXED_OUTPUT_N]) {
    /* Set output to LEDs. */
    bool lmr_channel_on[] = { [LEDC_CHANNEL_0] = false, [LEDC_CHANNEL_1] = false };
    bool bc_channel_on[] = {
        [LEDC_CHANNEL_4] = false, [LEDC_CHANNEL_5] = false, [LEDC_CHANNEL_6] = false, [LEDC_CHANNEL_7] = false
    };
    
    for (enum muxed_outputs i = MUXED_OUTPUT_MIN; i <= MUXED_OUTPUT_MAX; i++)
        if ((*levels)[i])
            lmr_channel_on[muxed_inouts_to_ledc_channel[i].lmr_channel] = 
                bc_channel_on[muxed_inouts_to_ledc_channel[i].bc_channel] = true;
    for (ledc_channel_t i = LEDC_CHANNEL_0; i <= LEDC_CHANNEL_1; i++) {
        ledc_set_duty(ledc_channel_configs[0].speed_mode, i, lmr_channel_on[i] ? ledc_channel_configs[0].duty : 0);
        ledc_update_duty(ledc_channel_configs[0].speed_mode, i);
    }
    for (ledc_channel_t i = LEDC_CHANNEL_4; i <= LEDC_CHANNEL_7; i++) {
        ledc_set_duty(ledc_channel_configs[0].speed_mode, i, bc_channel_on[i] ? ledc_channel_configs[0].duty : 0);
        ledc_update_duty(ledc_channel_configs[0].speed_mode, i);
    }
}

void muxed_gpio_get_input_switch_levels(bool (*levels)[MUXED_INPUT_N]) {
    for (enum muxed_inputs i = MUXED_INPUT_SWITCH_MIN; i <= MUXED_INPUT_SWITCH_MAX; i++)
        (*levels)[i] = gpio_get_level(muxed_inouts_to_gpio_num[i]);
}
