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
} dir_t;

typedef enum {
    SIDE_LEFT = 0x0,
    SIDE_RIGHT,

    SIDE_COUNT
} side_t;

#define BUTTONS_COUNT 4
#define GAMEPADS_CAPACITY 16
#define KEY_QUEUE_CAPACITY 32
#define USB_REPORT_SIZE 8

#define LOGITECH_VENDORID 0x046d
#define RUMBLEPAD_PRODUCTID 0xc218

static int dpad_mapping[DIR_COUNT] = { GRUB_TERM_NO_KEY };
static int button_mapping[BUTTONS_COUNT] = { GRUB_TERM_NO_KEY };
static int bumper_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };
static int trigger_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };
static int stick_mapping[SIDE_COUNT][DIR_COUNT] = { GRUB_TERM_NO_KEY };
static int stick_press_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };
static int options_mapping[SIDE_COUNT] = { GRUB_TERM_NO_KEY };

struct logitech_rumble_f510_report
{
    grub_uint8_t stick_axes[SIDE_COUNT * 2];
    grub_uint8_t dpad: 4;
    grub_uint8_t buttons: 4;
    grub_uint8_t bumpers: 2;
    grub_uint8_t triggers: 2;
    grub_uint8_t options: 2;
    grub_uint8_t sticks: 2;
    grub_uint8_t mode;
    grub_uint8_t padding;
};

struct grub_usb_gamepad_data
{
    grub_usb_device_t usbdev;
    int configno;
    int interfno;
    struct grub_usb_desc_endp *endp;
    grub_usb_transfer_t transfer;
    grub_uint8_t prev_report[USB_REPORT_SIZE];
    grub_uint8_t report[USB_REPORT_SIZE];
    int key_queue[KEY_QUEUE_CAPACITY];
    int key_queue_begin;
    int key_queue_size;
};

static grub_uint8_t initial_logitech_rumble_f510_report[USB_REPORT_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0xff
};

static struct grub_term_input gamepads[GAMEPADS_CAPACITY];

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

static dir_t
dir_by_coords(grub_uint8_t x0, grub_uint8_t y0)
{
    grub_int32_t x = x0;
    grub_int32_t y = y0;
    x -= 127;
    y -= 127;

    const grub_int32_t t = 3276;

    if (x * x + y * y > t) {
        const grub_int32_t d = 40;
#define POS(a) ((a) > (d))
#define ZERO(a) ((grub_int32_t)grub_abs(a) <= (d))
#define NEG(a) ((a) < -(d))
        if (POS(x) && ZERO(y)) {
            return DIR_RIGHT;
        } else if (POS(x) && NEG(y)) {
            return DIR_UPRIGHT;
        } else if (ZERO(x) && NEG(y)) {
            return DIR_UP;
        } else if (NEG(x) && NEG(y)) {
            return DIR_UPLEFT;
        } else if (NEG(x) && ZERO(y)) {
            return DIR_LEFT;
        } else if (NEG(x) && POS(y)) {
            return DIR_DOWNLEFT;
        } else if (ZERO(x) && POS(y)) {
            return DIR_DOWN;
        } else if (POS(x) && POS(y)) {
            return DIR_DOWNRIGHT;
        }
#undef POS
#undef ZERO
#undef NEG
    }

    return DIR_CENTERED;
}

static void logitech_rumble_f510_generate_keys(struct grub_usb_gamepad_data *data)
{
#define IS_PRESSED(buttons, i) ((buttons) & (1 << (i)))
    struct logitech_rumble_f510_report *prev_report = (struct logitech_rumble_f510_report *)data->prev_report;
    struct logitech_rumble_f510_report *report = (struct logitech_rumble_f510_report *)data->report;

    if (prev_report->dpad != report->dpad) {
        key_queue_push(data, dpad_mapping[report->dpad]);
    }

    for (int i = 0; i < BUTTONS_COUNT; ++i) {
        if (!IS_PRESSED(prev_report->buttons, i)
            && IS_PRESSED(report->buttons, i)) {
            key_queue_push(data, button_mapping[i]);
        }
    }

    for (int side = 0; side < SIDE_COUNT; ++side) {
        if (!IS_PRESSED(prev_report->bumpers, side)
            && IS_PRESSED(report->bumpers, side)) {
            key_queue_push(data, bumper_mapping[side]);
        }

        if (!IS_PRESSED(prev_report->triggers, side)
            && IS_PRESSED(report->triggers, side)) {
            key_queue_push(data, trigger_mapping[side]);
        }

        dir_t prev_dir = dir_by_coords(
            prev_report->stick_axes[side * 2],
            prev_report->stick_axes[side * 2 + 1]);

        dir_t dir = dir_by_coords(
            report->stick_axes[side * 2],
            report->stick_axes[side * 2 + 1]);

        if (prev_dir != dir) {
            key_queue_push(data, stick_mapping[side][dir]);
        }

        if (!IS_PRESSED(prev_report->sticks, side)
            && IS_PRESSED(report->sticks, side)) {
            key_queue_push(data, stick_press_mapping[side]);
        }

        if (!IS_PRESSED(prev_report->options, side)
            && IS_PRESSED(report->options, side)) {
            key_queue_push(data, options_mapping[side]);
        }
    }
#undef IS_PRESSED
}

static int
usb_gamepad_getkey (struct grub_term_input *term)
{
    struct grub_usb_gamepad_data *termdata = term->data;
    grub_size_t actual;

    grub_usb_err_t err = grub_usb_check_transfer (termdata->transfer, &actual);

    if (err != GRUB_USB_ERR_WAIT) {
        logitech_rumble_f510_generate_keys(termdata);
        grub_memcpy(termdata->prev_report, termdata->report, USB_REPORT_SIZE);

        termdata->transfer = grub_usb_bulk_read_background (
            termdata->usbdev,
            termdata->endp,
            sizeof (termdata->report),
            (char *) &termdata->report);

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

static void
grub_usb_gamepad_detach (grub_usb_device_t usbdev,
                         int config __attribute__ ((unused)),
                         int interface __attribute__ ((unused)))
{
    grub_dprintf("usb_gamepad", "Detaching usb_gamepad...\n");

    for (grub_size_t i = 0; i < ARRAY_SIZE(gamepads); ++i) {
        if (!gamepads[i].data) {
            continue;
        }

        struct grub_usb_gamepad_data *data = gamepads[i].data;

        if (data->usbdev != usbdev) {
            continue;
        }

        if (data->transfer) {
            grub_usb_cancel_transfer(data->transfer);
        }

        grub_term_unregister_input(&gamepads[i]);
        grub_free((char *) gamepads[i].name);
        gamepads[i].name = NULL;
        grub_free(gamepads[i].data);
        gamepads[i].data = NULL;
    }
}


static int
grub_usb_gamepad_attach(grub_usb_device_t usbdev, int configno, int interfno)
{
    if ((usbdev->descdev.vendorid != LOGITECH_VENDORID)
        || (usbdev->descdev.prodid != RUMBLEPAD_PRODUCTID)) {
        grub_dprintf("usb_gamepad",
                     "Ignoring vendor %x, product %x. "
                     "Only vendor %x and product %x are supported\n",
                     usbdev->descdev.vendorid,
                     usbdev->descdev.prodid,
                     LOGITECH_VENDORID,
                     RUMBLEPAD_PRODUCTID);
        return 0;
    }

    grub_dprintf("usb_gamepad", "usb_gamepad configno: %d, interfno: %d\n", configno, interfno);

    unsigned curnum = 0;
    for (curnum = 0; curnum < ARRAY_SIZE(gamepads); ++curnum)
        if (gamepads[curnum].data == 0)
            break;

    if (curnum >= ARRAY_SIZE(gamepads)) {
        grub_dprintf("usb_gamepad",
                     "Reached limit of attached gamepads. The limit is %d.\n",
                     GAMEPADS_CAPACITY);
        return 0;
    }

    grub_dprintf("usb_gamepad", "Endpoints: %d\n",
                 usbdev->config[configno].interf[interfno].descif->endpointcnt);

    struct grub_usb_desc_endp *endp = NULL;
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

    if (j == usbdev->config[configno].interf[interfno].descif->endpointcnt) {
        grub_dprintf("usb_gamepad", "No fitting endpoints found.\n");
        return 0;
    }

    grub_dprintf ("usb_gamepad", "HID usb_gamepad found! Endpoint: %d\n", j);

    struct grub_usb_gamepad_data *data = grub_malloc(sizeof(struct grub_usb_gamepad_data));
    if (!data) {
        grub_print_error();
        return 0;
    }

    gamepads[curnum].name = grub_xasprintf("usb_gamepad%d", curnum);
    gamepads[curnum].getkey = usb_gamepad_getkey;
    gamepads[curnum].getkeystatus = usb_gamepad_getkeystatus;
    gamepads[curnum].data = data;
    gamepads[curnum].next = 0;

    usbdev->config[configno].interf[interfno].detach_hook = grub_usb_gamepad_detach;
    data->usbdev = usbdev;
    data->configno = configno;
    data->interfno = interfno;
    data->endp = endp;
    data->key_queue_begin = 0;
    data->key_queue_size = 0;
    grub_memcpy(data->prev_report, initial_logitech_rumble_f510_report, USB_REPORT_SIZE);
    data->transfer = grub_usb_bulk_read_background (
        usbdev,
        data->endp,
        sizeof (data->report),
        (char *) &data->report);

    if (!data->transfer) {
        grub_print_error ();
        return 0;
    }

    grub_term_register_input_active("usb_gamepad", &gamepads[curnum]);

    return 0;
}

struct dir_name_t
{
    const char *name;
    dir_t value;
};

static struct dir_name_t dir_names[] = {
    {"U",  DIR_UP},
    {"UR", DIR_UPRIGHT},
    {"RU", DIR_UPRIGHT},
    {"R",  DIR_RIGHT},
    {"DR", DIR_DOWNRIGHT},
    {"RD", DIR_DOWNRIGHT},
    {"D",  DIR_DOWN},
    {"DL", DIR_DOWNLEFT},
    {"LD", DIR_DOWNLEFT},
    {"L",  DIR_LEFT},
    {"UL", DIR_UPLEFT},
    {"LU", DIR_UPLEFT},
    {"C" , DIR_CENTERED}
};

static grub_err_t
parse_dir_by_name(const char *name,
                  dir_t *dir)
{
    for (grub_size_t i = 0; i < ARRAY_SIZE(dir_names); ++i) {
        if (grub_strcmp(name, dir_names[i].name) == 0) {
            *dir = dir_names[i].value;
            return GRUB_ERR_NONE;
        }
    }

    return grub_error(
        GRUB_ERR_BAD_ARGUMENT,
        N_("%s is not a valid direction name"),
        name);
}

typedef struct {
    const char *name;
    int keycode;
} key_mapping_t;

static key_mapping_t key_mapping[] = {
    {"up",       GRUB_TERM_KEY_UP},
    {"down",     GRUB_TERM_KEY_DOWN},
    {"left",     GRUB_TERM_KEY_LEFT},
    {"right",    GRUB_TERM_KEY_RIGHT},
    {"home",     GRUB_TERM_KEY_HOME},
    {"end",      GRUB_TERM_KEY_END},
    {"dc",       GRUB_TERM_KEY_DC},
    {"ppage",    GRUB_TERM_KEY_PPAGE},
    {"npage",    GRUB_TERM_KEY_NPAGE},
    {"f1",       GRUB_TERM_KEY_F1},
    {"f2",       GRUB_TERM_KEY_F2},
    {"f3",       GRUB_TERM_KEY_F3},
    {"f4",       GRUB_TERM_KEY_F4},
    {"f5",       GRUB_TERM_KEY_F5},
    {"f6",       GRUB_TERM_KEY_F6},
    {"f7",       GRUB_TERM_KEY_F7},
    {"f8",       GRUB_TERM_KEY_F8},
    {"f9",       GRUB_TERM_KEY_F9},
    {"f10",      GRUB_TERM_KEY_F10},
    {"f11",      GRUB_TERM_KEY_F11},
    {"f12",      GRUB_TERM_KEY_F12},
    {"insert",   GRUB_TERM_KEY_INSERT},
    {"center",   GRUB_TERM_KEY_CENTER},
    {"esc",      GRUB_TERM_ESC},
    {"tab",      GRUB_TERM_TAB},
    {"bspace",   GRUB_TERM_BACKSPACE},
    {"space",    32},
};

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
        const int n = sizeof(key_mapping) / sizeof(key_mapping[0]);
        for (int i = 0; i < n; ++i) {
            if (grub_strcmp(input, key_mapping[i].name) == 0) {
                *keycode = key_mapping[i].keycode;
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

#define ASSERT_ARGC(argc, N)                            \
    do {                                                \
        if (argc < N) {                                 \
            return grub_error(                          \
                GRUB_ERR_BAD_ARGUMENT,                  \
                N_("Expected at least %d arguments"),   \
                N);                                     \
        }                                               \
    } while(0)

static grub_err_t
grub_cmd_gamepad_btn(grub_command_t cmd __attribute__((unused)),
                     int argc, char **args)
{
    ASSERT_ARGC(argc, 3);

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
    ASSERT_ARGC(argc, 3);

    dir_t dpad_dir = 0;
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

static grub_err_t
grub_cmd_gamepad_sided(grub_command_t cmd, int argc, char **args)
{
    side_t side =
        cmd->name[8] == 'l'
        ? SIDE_LEFT
        : SIDE_RIGHT;

    dir_t dir = DIR_CENTERED;
    int keycode = 0;
    grub_err_t err = GRUB_ERR_NONE;

    switch (cmd->name[9]) {
    case 'b': {
        ASSERT_ARGC(argc, 2);

        err = parse_keycode_name(args[0], args[1], &keycode);
        if (err) {
            return err;
        }

        bumper_mapping[side] = keycode;
    } break;

    case 't': {
        ASSERT_ARGC(argc, 2);

        err = parse_keycode_name(args[0], args[1], &keycode);
        if (err) {
            return err;
        }

        trigger_mapping[side] = keycode;
    } break;

    case 's': {
        ASSERT_ARGC(argc, 3);

        if (grub_strcmp(args[0], "P") == 0) {
            err = parse_keycode_name(args[1], args[2], &keycode);
            if (err) {
                return err;
            }

            stick_press_mapping[side] = keycode;
        } else {
            err = parse_dir_by_name(args[0], &dir);
            if (err) {
                return err;
            }

            err = parse_keycode_name(args[1], args[2], &keycode);
            if (err) {
                return err;
            }

            stick_mapping[side][dir] = keycode;
        }
    } break;
    }

    return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_gamepad_options(grub_command_t cmd, int argc, char **args)
{
    ASSERT_ARGC(argc, 2);

    int keycode = 0;
    grub_err_t err = parse_keycode_name(args[0], args[1], &keycode);
    if (err) {
        return err;
    }

    if (cmd->name[8] == 'b') {
        options_mapping[SIDE_LEFT] = keycode;
    } else if (cmd->name[8] == 's') {
        options_mapping[SIDE_RIGHT] = keycode;
    }

    return GRUB_ERR_NONE;
}

static struct grub_usb_attach_desc attach_hook =
{
    .class = GRUB_USB_CLASS_HID,
    .hook = grub_usb_gamepad_attach
};

struct command_proto {
    const char *name;
    grub_command_func_t func;
    const char *summary;
    const char *description;
};

static struct command_proto cmds_proto[] = {
    {"gamepad_dpad",grub_cmd_gamepad_dpad,N_("<dpad-direction> <key>"),N_("Map gamepad dpad direction to a key")},
    {"gamepad_btn",grub_cmd_gamepad_btn,N_("<button-number> <key>"),N_("Map gamepad button to a key")},
    {"gamepad_lb",grub_cmd_gamepad_sided,N_("<key>"),N_("Map gamepad Left Bumper to a key")},
    {"gamepad_rb",grub_cmd_gamepad_sided,N_("<key>"),N_("Map gamepad Right Bumper to a key")},
    {"gamepad_lt",grub_cmd_gamepad_sided,N_("<key>"),N_("Map gamepad Left Trigger to a key")},
    {"gamepad_rt",grub_cmd_gamepad_sided,N_("<key>"),N_("Map gamepad Right Trigger to a key")},
    {"gamepad_ls",grub_cmd_gamepad_sided,N_("<direction|P> <key>"),N_("Map gamepad Left Stick Action to a key")},
    {"gamepad_rs",grub_cmd_gamepad_sided,N_("<direction|P> <key>"),N_("Map gamepad Right Stick Action to a key")},
    {"gamepad_back",grub_cmd_gamepad_options,N_("<key>"),N_("Map gamepad Back button to a key")},
    {"gamepad_start",grub_cmd_gamepad_options,N_("<key>"),N_("Map gamepad Start button to a key")}
};

static grub_command_t cmds[ARRAY_SIZE(cmds_proto)];

GRUB_MOD_INIT(usb_gamepad)
{
    grub_dprintf("usb_gamepad", "Usb_Gamepad module loaded\n");

    for (grub_size_t i = 0; i < ARRAY_SIZE(cmds); ++i) {
        cmds[i] = grub_register_command(
            cmds_proto[i].name,
            cmds_proto[i].func,
            cmds_proto[i].summary,
            cmds_proto[i].description);
    }

    grub_usb_register_attach_hook_class(&attach_hook);
}

GRUB_MOD_FINI(usb_gamepad)
{
    for (grub_size_t i = 0; i < ARRAY_SIZE(cmds); ++i) {
        grub_unregister_command(cmds[i]);
    }

    for (grub_size_t i = 0; i < ARRAY_SIZE(gamepads); ++i) {
        if (!gamepads[i].data) {
            continue;
        }

        struct grub_usb_gamepad_data *data = gamepads[i].data;

        if (data->transfer) {
            grub_usb_cancel_transfer(data->transfer);
        }

        grub_term_unregister_input(&gamepads[i]);
        grub_free((char *) gamepads[i].name);
        gamepads[i].name = NULL;
        grub_free(gamepads[i].data);
        gamepads[i].data = NULL;
    }

    grub_usb_unregister_attach_hook_class (&attach_hook);

    grub_dprintf("usb_gamepad", "usb_gamepad fini-ed\n");
}
