#include <grub/dl.h>
#include <grub/term.h>
#include <grub/usb.h>
#include <grub/command.h>

GRUB_MOD_LICENSE ("GPLv3");

typedef enum {
    DIR_UP = 0x0,
    DIR_UPRIGHT,
    DIR_RIGHT,
    DIR_DOWNRIGHT,
    DIR_DOWN,
    DIR_DOWNLEFT,
    DIR_LEFT,
    DIR_UPLEFT,
    DIR_CENTERED,

    DIR_COUNT
} logitech_rumble_f510_dir_t;

typedef enum {
    SIDE_LEFT = 0x0,
    SIDE_RIGHT,

    SIDE_COUNT
} logitech_rumble_f510_side_t;

#define BUTTONS_COUNT 4
#define STICKS_THRESHOLD_PCT 25
#define STICK_X 0
#define STICK_Y 1

// TODO(#22): dpad_mapping and button mapping should be probably part of grub_usb_gamepad_data
static int dpad_mapping[DIR_COUNT] = { GRUB_TERM_NO_KEY };
// TODO(#23): there is no way to configure button_mappings from the GRUB config
static int button_mapping[BUTTONS_COUNT] = { GRUB_TERM_NO_KEY };
static int bumper_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };
static int trigger_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };
static int stick_mapping[SIDE_COUNT][DIR_COUNT] = { GRUB_TERM_NO_KEY };

// TODO(#18): usb_gamepad has no respect to endianness
struct logitech_rumble_f510_state
{
    grub_uint8_t sticks[SIDE_COUNT * 2];
    grub_uint8_t dpad: 4;
    grub_uint8_t buttons: 4;
    grub_uint8_t bumpers: 2;
    grub_uint8_t triggers: 2;
    // TODO(#26): back/start are not mappable
    grub_uint8_t back: 1;
    grub_uint8_t start: 1;
    // TODO(#27): stick presses are not mappable
    grub_uint8_t ls: 1;
    grub_uint8_t rs: 1;
    grub_uint8_t mode;
    grub_uint8_t padding;
};

static inline
void print_logitech_state(struct logitech_rumble_f510_state *state)
{
    grub_dprintf("usb_gamepad",
        "x1: %u, "
        "y1: %u, "
        "x2: %u, "
        "y2: %u, "
        "dpad: %u, "
        "buttons: %u, "
        "bumpers: %u, "
        "triggers: %u, "
        "back: %u, "
        "start: %u, "
        "ls: %u, "
        "rs: %u, "
        "mode: %u\n",
        state->sticks[0],
        state->sticks[1],
        state->sticks[2],
        state->sticks[3],
        state->dpad, state->buttons,
        state->bumpers,
        state->triggers,
        state->back, state->start,
        state->ls, state->rs,
        state->mode);
}

static grub_uint8_t initial_state[8] = {
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0xff
};

#define KEY_QUEUE_CAPACITY 32

struct grub_usb_gamepad_data
{
    grub_usb_device_t usbdev;
    int configno;
    int interfno;
    struct grub_usb_desc_endp *endp;
    grub_usb_transfer_t transfer;
    struct logitech_rumble_f510_state prev_state;
    struct logitech_rumble_f510_state state;
    int key_queue[KEY_QUEUE_CAPACITY];
    int key_queue_begin;
    int key_queue_size;
};

static inline
void key_queue_push(struct grub_usb_gamepad_data *data, int key)
{
    data->key_queue[(data->key_queue_begin + data->key_queue_size) % KEY_QUEUE_CAPACITY] = key;

    if (data->key_queue_size < KEY_QUEUE_CAPACITY) {
        data->key_queue_size++;
    } else {
        data->key_queue_begin = (data->key_queue_begin + 1) % KEY_QUEUE_CAPACITY;
    }
}

static inline
int key_queue_pop(struct grub_usb_gamepad_data *data)
{
    if (data->key_queue_size <= 0) {
        return GRUB_TERM_NO_KEY;
    }

    int key = data->key_queue[data->key_queue_begin];
    data->key_queue_begin = (data->key_queue_begin + 1) % KEY_QUEUE_CAPACITY;
    data->key_queue_size--;

    return key;
}

static inline
int is_pressed(grub_uint8_t buttons, int i)
{
    return buttons & (1 << i);
}

static logitech_rumble_f510_dir_t
dir_by_coords(grub_uint8_t x __attribute__ ((unused)),
              grub_uint8_t y __attribute__ ((unused)))
{
    // TODO: dir_by_coords is not implemented
    return DIR_CENTERED;
}

static void generate_keys(struct grub_usb_gamepad_data *data)
{
    if (data->prev_state.dpad != data->state.dpad) {
        key_queue_push(data, dpad_mapping[data->state.dpad]);
    }

    for (int i = 0; i < BUTTONS_COUNT; ++i) {
        if (!is_pressed(data->prev_state.buttons, i)
            && is_pressed(data->state.buttons, i)) {
            key_queue_push(data, button_mapping[i]);
        }
    }

    for (int side = 0; side < SIDE_COUNT; ++side) {
        if (!is_pressed(data->prev_state.bumpers, side)
            && is_pressed(data->state.bumpers, side)) {
            key_queue_push(data, bumper_mapping[side]);
        }

        if (!is_pressed(data->prev_state.triggers, side)
            && is_pressed(data->state.triggers, side)) {
            key_queue_push(data, trigger_mapping[side]);
        }

        logitech_rumble_f510_dir_t prev_dir = dir_by_coords(
            data->prev_state.sticks[side * 2 + STICK_X],
            data->prev_state.sticks[side * 2 + STICK_Y]);

        logitech_rumble_f510_dir_t dir = dir_by_coords(
            data->state.sticks[side * 2 + STICK_X],
            data->state.sticks[side * 2 + STICK_Y]);

        if (prev_dir != dir) {
            key_queue_push(data, stick_mapping[side][dir]);
        }
    }
}

static int
usb_gamepad_getkey (struct grub_term_input *term)
{
    struct grub_usb_gamepad_data *termdata = term->data;
    grub_size_t actual;

    grub_usb_err_t err = grub_usb_check_transfer (termdata->transfer, &actual);

    if (err != GRUB_USB_ERR_WAIT) {
        print_logitech_state(&termdata->state);
        generate_keys(termdata);
        termdata->prev_state = termdata->state;

        termdata->transfer = grub_usb_bulk_read_background (
            termdata->usbdev,
            termdata->endp,
            sizeof (termdata->state),
            (char *) &termdata->state);

        if (!termdata->transfer)
        {
            grub_print_error ();
        }
    }

    return key_queue_pop(termdata);
}

static int
usb_gamepad_getkeystatus (struct grub_term_input *term __attribute__ ((unused)))
{
    return 0;
}

static struct grub_term_input usb_gamepad_input_term =
  {
    .name = "usb_gamepad",
    .getkey = usb_gamepad_getkey,
    .getkeystatus = usb_gamepad_getkeystatus
  };

static int
grub_usb_gamepad_attach(grub_usb_device_t usbdev, int configno, int interfno)
{
    // TODO(#14): grub_usb_gamepad_attach leaks memory every time you connect a new USB device
    grub_dprintf("usb_gamepad", "Usb_Gamepad configno: %d, interfno: %d\n", configno, interfno);
    struct grub_usb_gamepad_data *data = grub_malloc(sizeof(struct grub_usb_gamepad_data));
    struct grub_usb_desc_endp *endp = NULL;
    if (!data) {
        grub_print_error();
        return 0;
    }

    grub_dprintf("usb_gamepad", "Endpoints: %d\n",
                 usbdev->config[configno].interf[interfno].descif->endpointcnt);

    int j = 0;
    for (j = 0;
         j < usbdev->config[configno].interf[interfno].descif->endpointcnt;
         j++)
    {
        endp = &usbdev->config[configno].interf[interfno].descendp[j];

        if ((endp->endp_addr & 128) && grub_usb_get_ep_type(endp)
            == GRUB_USB_EP_INTERRUPT)
            break;
    }

    if (j == usbdev->config[configno].interf[interfno].descif->endpointcnt)
        return 0;

    grub_dprintf ("usb_gamepad", "HID Usb_Gamepad found! Endpoint: %d\n", j);

    data->usbdev = usbdev;
    data->configno = configno;
    data->interfno = interfno;
    data->endp = endp;
    data->key_queue_begin = 0;
    data->key_queue_size = 0;
    usb_gamepad_input_term.data = data;

    data->prev_state = *((struct logitech_rumble_f510_state*) initial_state);

    data->transfer = grub_usb_bulk_read_background (
        usbdev,
        data->endp,
        sizeof (data->state),
        (char *) &data->state);

    if (!data->transfer)
    {
        grub_print_error ();
        return 0;
    }

    grub_term_register_input("usb_gamepad", &usb_gamepad_input_term);

    return 0;
}

static const char *dir_names[DIR_COUNT] = {
    "up",
    "upright",
    "right",
    "downright",
    "down",
    "downleft",
    "left",
    "upleft",
    "centered"
};

static grub_err_t
parse_dir_by_name(const char *name,
                  logitech_rumble_f510_dir_t *dir)
{
    for (int i = 0; i < DIR_COUNT; ++i) {
        if (grub_strcmp(name, dir_names[i]) == 0) {
            *dir = i;
            return GRUB_ERR_NONE;
        }
    }

    return grub_error(
        GRUB_ERR_BAD_ARGUMENT,
        N_("%s is not a valid direction name"),
        name);
}

// TODO(#28): key mnemonics are not sufficient
static const char *key_names[] = {
    "key_up",
    "key_down"
};

static int key_mapping[] = {
    GRUB_TERM_KEY_UP,
    GRUB_TERM_KEY_DOWN
};

// TODO(#33): it would be good to have some unit tests for parse_keycode_name
static
grub_err_t parse_keycode_name(const char *type,
                              const char *input,
                              int *keycode)
{
    if (grub_strcmp(type, "code") == 0) {
        *keycode = grub_strtol(input, 0, 10);

        if (grub_errno) {
            return grub_error(grub_errno, N_("`%s` is not a number"), input);
        }
    } else if (grub_strcmp(type, "char") == 0) {
        if (grub_strlen(input) <= 0) {
            return grub_error(
                GRUB_ERR_BAD_ARGUMENT,
                N_("Cannot accept an empty string as character for mapping"));
        }

        *keycode = input[0];
    } else if (grub_strcmp(type, "name") == 0) {
        const int n = sizeof(key_names) / sizeof(key_names[0]);
        for (int i = 0; i < n; ++i) {
            if (grub_strcmp(input, key_names[i]) == 0) {
                *keycode = key_mapping[i];
                return GRUB_ERR_NONE;
            }
        }

        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("`%s` is not a correct key name"),
            input);
    } else {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("`%s` is not a correct keycode mapping type"),
            type);
    }

    return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gamepad_buttons(grub_command_t cmd __attribute__((unused)),
                         int argc, char **args)
{
#define N 3
    if (argc < N) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Expected at least %d arguments"),
            N);
    }
#undef N

    long button_number = grub_strtol(args[0], 0, 10);
    if (grub_errno) {
        return grub_error(
            grub_errno,
            N_("Expected button number. `%s` is not a number."),
            args[0]);
    }

    if (!(0 <= button_number && button_number < 4)) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Button number should be within the range of 0-3."));
    }

    int keycode = 0;
    grub_err_t err = parse_keycode_name(args[1], args[2], &keycode);
    if (err) {
        return err;
    }

    button_mapping[button_number] = keycode;

    return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gamepad_dpad(grub_command_t cmd __attribute__((unused)),
                      int argc, char **args)
{
#define N 3
    if (argc < N) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Expected at least %d arguments"),
            N);
    }
#undef N

    logitech_rumble_f510_dir_t dpad_dir = 0;
    grub_err_t err = parse_dir_by_name(args[0], &dpad_dir);
    if (err) {
        return err;
    }

    int keycode = 0;
    err = parse_keycode_name(args[1], args[2], &keycode);
    if (err) {
        return err;
    }

    dpad_mapping[dpad_dir] = keycode;

    return GRUB_ERR_NONE;
}

static
grub_err_t parse_gamepad_side(const char *side_name,
                              logitech_rumble_f510_side_t *side)
{
    if (grub_strcmp(side_name, "left") == 0) {
        *side = SIDE_LEFT;
        return GRUB_ERR_NONE;
    } else if (grub_strcmp(side_name, "right") == 0) {
        *side = SIDE_RIGHT;
        return GRUB_ERR_NONE;
    }

    return grub_error(
        GRUB_ERR_BAD_ARGUMENT,
        N_("%s is not a correct name of a side (expected: left or right)"),
        side_name);
}

static grub_err_t
grub_cmd_gamepad_sided(grub_command_t cmd __attribute__((unused)),
                       int argc,
                       char **args)
{
#define N 3
    if (argc < N) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Expected at least %d arguments"),
            N);
    }
#undef N

    logitech_rumble_f510_side_t side = 0;
    grub_err_t err = parse_gamepad_side(args[0], &side);
    if (err) {
        return err;
    }

    int keycode = 0;
    err = parse_keycode_name(args[1], args[2], &keycode);
    if (err) {
        return err;
    }

    switch (cmd->name[8]) {
    case 'b': {
        bumper_mapping[side] = keycode;
    } break;

    case 't': {
        trigger_mapping[side] = keycode;
    } break;
    }

    return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gamepad_stick(grub_command_t cmd __attribute__((unused)),
                       int argc,
                       char **args)
{
#define N 4
    if (argc < N) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Expected at least %d arguments"),
            N);
    }
#undef N

    logitech_rumble_f510_side_t side;
    grub_err_t err = parse_gamepad_side(args[0], &side);
    if (err) {
        return err;
    }

    logitech_rumble_f510_dir_t dir;
    err = parse_dir_by_name(args[1], &dir);
    if (err) {
        return err;
    }

    int keycode = 0;
    err = parse_keycode_name(args[2], args[3], &keycode);
    if (err) {
        return err;
    }

    stick_mapping[side][dir] = keycode;

    return GRUB_ERR_NONE;
}

// TODO(#31): grub command handlers should be just an array
static grub_command_t cmd_gamepad_dpad;
static grub_command_t cmd_gamepad_buttons;
static grub_command_t cmd_gamepad_bumper;
static grub_command_t cmd_gamepad_trigger;
static grub_command_t cmd_gamepad_stick;

static struct grub_usb_attach_desc attach_hook =
{
    .class = GRUB_USB_CLASS_HID,
    .hook = grub_usb_gamepad_attach
};

GRUB_MOD_INIT(usb_gamepad)
{
    grub_dprintf("usb_gamepad", "Usb_Gamepad module loaded\n");

    cmd_gamepad_dpad = grub_register_command(
        "gamepad_dpad",
        grub_cmd_gamepad_dpad,
        N_("<dpad-direction> <key>"),
        N_("Map gamepad dpad direction to a key"));

    cmd_gamepad_buttons = grub_register_command(
        "gamepad_buttons",
        grub_cmd_gamepad_buttons,
        N_("<button-number> <key>"),
        N_("Map gamepad button to a key"));

    cmd_gamepad_bumper = grub_register_command(
        "gamepad_bumper",
        grub_cmd_gamepad_sided,
        N_("<button-side> <key>"),
        N_("Map gamepad bumper to a key"));

    cmd_gamepad_trigger = grub_register_command(
        "gamepad_trigger",
        grub_cmd_gamepad_sided,
        N_("<button-side> <key>"),
        N_("Map gamepad trigger to a key"));

    cmd_gamepad_stick = grub_register_command(
        "gamepad_stick",
        grub_cmd_gamepad_stick,
        N_("<side> <stick-direction> <key>"),
        N_("Map gamepad stick direction to a key"));

    grub_usb_register_attach_hook_class(&attach_hook);
}

GRUB_MOD_FINI(usb_gamepad)
{
    grub_unregister_command (cmd_gamepad_dpad);
    grub_unregister_command (cmd_gamepad_buttons);
    grub_unregister_command (cmd_gamepad_bumper);
    grub_unregister_command (cmd_gamepad_trigger);
    grub_unregister_command (cmd_gamepad_stick);
    grub_dprintf("usb_gamepad", "Usb_Gamepad fini-ed\n");
    // TODO(#20): usb_gamepad does not uninitialize usb stuff on FINI
}
