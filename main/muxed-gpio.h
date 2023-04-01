#ifndef GPIO_STATES_H
#define GPIO_STATES_H

#include <stdbool.h>

enum muxed_inputs {
    MUXED_INPUT_MIN, /* 0 */
    MUXED_INPUT_BUTTONS_MIN = MUXED_INPUT_MIN,
    /* Buttons. */
    MUXED_INPUT_STAR_L_BUTTON = MUXED_INPUT_BUTTONS_MIN,
    MUXED_INPUT_TRIANGLE_L_BUTTON,
    MUXED_INPUT_SQUARE_L_BUTTON,
    MUXED_INPUT_HEART_L_BUTTON,
    MUXED_INPUT_HEART_R_BUTTON,
    MUXED_INPUT_SQUARE_R_BUTTON,
    MUXED_INPUT_TRIANGLE_R_BUTTON,
    MUXED_INPUT_STAR_R_BUTTON,
    
    MUXED_INPUT_BUTTONS_MAX = MUXED_INPUT_STAR_R_BUTTON, /* 7 */
    
    MUXED_INPUT_N_BUTTONS, /* 8 */
    MUXED_INPUT_CLIPS_MIN = MUXED_INPUT_N_BUTTONS,
    /* Clips. */
    MUXED_INPUT_STAR_L_CLIP = MUXED_INPUT_CLIPS_MIN,
    MUXED_INPUT_TRIANGLE_L_CLIP,
    MUXED_INPUT_SQUARE_L_CLIP,
    MUXED_INPUT_HEART_L_CLIP,
    MUXED_INPUT_HEART_R_CLIP,
    MUXED_INPUT_SQUARE_R_CLIP,
    MUXED_INPUT_TRIANGLE_R_CLIP,
    MUXED_INPUT_STAR_R_CLIP,
    
    MUXED_INPUT_CLIPS_MAX = MUXED_INPUT_STAR_R_CLIP,
   
    MUXED_INPUT_SWITCH_MIN,
    /* Switches. */
    MUXED_INPUT_BEAK_SWITCH = MUXED_INPUT_SWITCH_MIN, /* 16 */
    MUXED_INPUT_LEARN_SWITCH,
    MUXED_INPUT_MUSIC_SWITCH,
    
    MUXED_INPUT_SWITCH_MAX = MUXED_INPUT_MUSIC_SWITCH,
    MUXED_INPUT_MAX = MUXED_INPUT_SWITCH_MAX, /* 18 */
    
    MUXED_INPUT_N /* 19 */
};

typedef void (*muxed_inputs_on_changed_fn)(bool (*)[MUXED_INPUT_N]);

void muxed_gpio_setup(void);
void muxed_gpio_update(void *arg);

extern volatile bool muxed_outputs[8];

#endif /* GPIO_STATES_H */