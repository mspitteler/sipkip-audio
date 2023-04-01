#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

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
#define GPIO_NUM_SWITCH_MUSIC_IN  GPIO_NUM_35
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
                                   (1ULL << GPIO_NUM_SWITCH_MUSIC_IN))

static const enum muxed_inputs gpio_num_to_muxed_inouts[] = {
    [GPIO_NUM_STAR_L_INOUT] = MUXED_INPUT_STAR_L_BUTTON, [GPIO_NUM_TRIANGLE_L_INOUT] = MUXED_INPUT_TRIANGLE_L_BUTTON,
    [GPIO_NUM_SQUARE_L_INOUT] = MUXED_INPUT_SQUARE_L_BUTTON, [GPIO_NUM_HEART_L_INOUT] = MUXED_INPUT_HEART_L_BUTTON,
    [GPIO_NUM_HEART_R_INOUT] = MUXED_INPUT_HEART_R_BUTTON, [GPIO_NUM_SQUARE_R_INOUT] = MUXED_INPUT_SQUARE_R_BUTTON,
    [GPIO_NUM_TRIANGLE_R_INOUT] = MUXED_INPUT_TRIANGLE_R_BUTTON, [GPIO_NUM_STAR_R_INOUT] = MUXED_INPUT_STAR_R_BUTTON,
    /* 8..15 are virtual inputs for the clips, that can be read if gpio_set_mux_clips is set to true. */
    [GPIO_NUM_BEAK_IN] = MUXED_INPUT_BEAK_SWITCH,
    [GPIO_NUM_SWITCH_LEARN_IN] = MUXED_INPUT_LEARN_SWITCH, [GPIO_NUM_SWITCH_MUSIC_IN] = MUXED_INPUT_MUSIC_SWITCH
};
static const gpio_num_t muxed_inouts_to_gpio_num[] = {
    [MUXED_INPUT_STAR_L_BUTTON] = GPIO_NUM_STAR_L_INOUT, [MUXED_INPUT_TRIANGLE_L_BUTTON] = GPIO_NUM_TRIANGLE_L_INOUT,
    [MUXED_INPUT_SQUARE_L_BUTTON] = GPIO_NUM_SQUARE_L_INOUT, [MUXED_INPUT_HEART_L_BUTTON] = GPIO_NUM_HEART_L_INOUT,
    [MUXED_INPUT_HEART_R_BUTTON] = GPIO_NUM_HEART_R_INOUT, [MUXED_INPUT_SQUARE_R_BUTTON] = GPIO_NUM_SQUARE_R_INOUT,
    [MUXED_INPUT_TRIANGLE_R_BUTTON] = GPIO_NUM_TRIANGLE_R_INOUT, [MUXED_INPUT_STAR_R_BUTTON] = GPIO_NUM_STAR_R_INOUT,
    /* 8..15 are virtual inputs for the clips, that can be read if gpio_set_mux_clips is set to true. */
    [MUXED_INPUT_STAR_L_CLIP] = GPIO_NUM_STAR_L_INOUT, [MUXED_INPUT_TRIANGLE_L_CLIP] = GPIO_NUM_TRIANGLE_L_INOUT,
    [MUXED_INPUT_SQUARE_L_CLIP] = GPIO_NUM_SQUARE_L_INOUT, [MUXED_INPUT_HEART_L_CLIP] = GPIO_NUM_HEART_L_INOUT,
    [MUXED_INPUT_HEART_R_CLIP] = GPIO_NUM_HEART_R_INOUT, [MUXED_INPUT_SQUARE_R_CLIP] = GPIO_NUM_SQUARE_R_INOUT,
    [MUXED_INPUT_TRIANGLE_R_CLIP] = GPIO_NUM_TRIANGLE_R_INOUT, [MUXED_INPUT_STAR_R_CLIP] = GPIO_NUM_STAR_R_INOUT,
    /* The following GPIOs are only ever used as inputs. */
    [MUXED_INPUT_BEAK_SWITCH] = GPIO_NUM_BEAK_IN,
    [MUXED_INPUT_LEARN_SWITCH] = GPIO_NUM_SWITCH_LEARN_IN, [MUXED_INPUT_MUSIC_SWITCH] = GPIO_NUM_SWITCH_MUSIC_IN
};

volatile bool muxed_outputs[8] = {0};

void muxed_gpio_setup(void) {
    /* Zero-initialize the config structure. */
    gpio_config_t io_conf = {};
   
    /* Set as output mode. */
    io_conf.mode = GPIO_MODE_OUTPUT;
    /* Bit mask of the pins that you want to set as outputs. */
    io_conf.pin_bit_mask = GPIO_PIN_BIT_MASK_OUT;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);
   
    /* Bit mask of the pins, use inputs here. */
    io_conf.pin_bit_mask = GPIO_PIN_BIT_MASK_IN;
    /* Set as input mode. */
    io_conf.mode = GPIO_MODE_INPUT;
    /* Enable pull-down mode. */
    io_conf.pull_down_en = 1;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);
    
    /* Bit mask of the pins, use input/open drain output here. */
    io_conf.pin_bit_mask = GPIO_PIN_BIT_MASK_INOUT;
    /* Set as input/output mode. */
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    /* Configure GPIO with the given settings. */
    gpio_config(&io_conf);
}

void muxed_gpio_update(void *arg) {
    static bool states[MUXED_INPUT_N];
    muxed_inputs_on_changed_fn on_changed = arg;
    bool input = false, gpio_mux_clips = false;
    
    /* TODO: Attach clock to mux pins, instead of this bodged approach. */
    for (;;) {
        if (input) {
            for (enum muxed_inputs i = MUXED_INPUT_BUTTONS_MIN; i <= MUXED_INPUT_BUTTONS_MAX; i++) {
                gpio_num_t gpio_num = muxed_inouts_to_gpio_num[i];
                gpio_set_level(gpio_num, 1);
            }
            /* If we're reading the inputs; alternate between buttons and clips. */
            gpio_mux_clips = !gpio_mux_clips;
            gpio_set_level(GPIO_NUM_MUX_BUTTONS_OUT, !gpio_mux_clips);
            gpio_set_level(GPIO_NUM_MUX_CLIPS_OUT, gpio_mux_clips);
            
            gpio_set_level(GPIO_NUM_LED_L_OUT, 0);
            gpio_set_level(GPIO_NUM_LED_M_OUT, 0);
            gpio_set_level(GPIO_NUM_LED_R_OUT, 0);
            
            for (enum muxed_inputs i = MUXED_INPUT_MIN; i <= MUXED_INPUT_MAX; i++) {
                gpio_num_t gpio_num = muxed_inouts_to_gpio_num[i];
                int level;
                if (i >= MUXED_INPUT_CLIPS_MIN && i <= MUXED_INPUT_CLIPS_MAX) /* These are not real GPIO inputs. */
                    continue;

                if (i <= MUXED_INPUT_BUTTONS_MAX)
                    gpio_pulldown_en(gpio_num);
                if (states[i + (i >= MUXED_INPUT_SWITCH_MIN ? 0 : ((uint8_t)gpio_mux_clips << 3))]
                    != (level = gpio_get_level(gpio_num)))
                    on_changed(&states);
                
                states[i + (i >= MUXED_INPUT_SWITCH_MIN ? 0 : ((uint8_t)gpio_mux_clips << 3))] = level;
                if (i <= MUXED_INPUT_BUTTONS_MAX)
                    gpio_pulldown_dis(gpio_num);
            }
        } else {
            gpio_set_level(GPIO_NUM_LED_L_OUT, 1);
            gpio_set_level(GPIO_NUM_LED_M_OUT, 1);
            gpio_set_level(GPIO_NUM_LED_R_OUT, 1);
            
            gpio_set_level(GPIO_NUM_MUX_BUTTONS_OUT, 0);
            gpio_set_level(GPIO_NUM_MUX_CLIPS_OUT, 0);
            
            for (enum muxed_inputs i = MUXED_INPUT_BUTTONS_MIN; i <= MUXED_INPUT_BUTTONS_MAX; i++)
                gpio_set_level(muxed_inouts_to_gpio_num[i], !muxed_outputs[i]);
        }
        
        /* Alternate reading inputs and turning on the LEDs. */
        input = !input;
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}
