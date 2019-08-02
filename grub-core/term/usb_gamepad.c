#include <grub/dl.h>
#include <grub/term.h>
#include <grub/usb.h>
#include <grub/command.h>

GRUB_MOD_LICENSE ("GPLv3");

typedef enum {
    DPAD_UP = 0x0,
    DPAD_UPRIGHT,
    DPAD_RIGHT,
    DPAD_DOWNRIGHT,
    DPAD_DOWN,
    DPAD_DOWNLEFT,
    DPAD_LEFT,
    DPAD_UPLEFT,
    DPAD_CENTERED,

    DPAD_COUNT
} logitech_rumble_f510_dpad_t;

#define BUTTONS_COUNT 4

// TODO(#22): dpad_mapping and button mapping should be probably part of grub_usb_gamepad_data
static int dpad_mapping[DPAD_COUNT] = { GRUB_TERM_NO_KEY };
// TODO(#23): there is no way to configure button_mappings from the GRUB config
static int button_mapping[BUTTONS_COUNT] = {
    GRUB_TERM_NO_KEY,
    '\n',
    GRUB_TERM_NO_KEY,
    GRUB_TERM_NO_KEY
};

static const char *dpad_names[DPAD_COUNT] = {
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

// TODO(#18): usb_gamepad has no respect to endianness
struct logitech_rumble_f510_state
{
    // TODO(#24): stick axis are not mappable
    grub_uint8_t x1;
    grub_uint8_t y1;
    grub_uint8_t x2;
    grub_uint8_t y2;
    grub_uint8_t dpad: 4;
    grub_uint8_t buttons: 4;
    // TODO: bumpers and triggers are not mappable
    grub_uint8_t lb: 1;
    grub_uint8_t rb: 1;
    grub_uint8_t lt: 1;
    grub_uint8_t rt: 1;
    // TODO: back/start are not mappable
    grub_uint8_t back: 1;
    grub_uint8_t start: 1;
    // TODO: stick presses are not mappable
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
        "lb: %u, "
        "rb: %u, "
        "lt: %u, "
        "rt: %u, "
        "back: %u, "
        "start: %u, "
        "ls: %u, "
        "rs: %u, "
        "mode: %u\n",
        state->x1, state->y1, state->x2, state->y2,
        state->dpad, state->buttons,
        state->lb, state->rb,
        state->lt, state->rt,
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


static int dpad_dir_by_name(const char *name)
{
    for (int i = 0; i < DPAD_COUNT; ++i) {
        if (grub_strcmp(name, dpad_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// TODO: key mnemonics are not sufficient
static const char *key_names[] = {
    "key_up",
    "key_down"
};

static int key_mapping[] = {
    GRUB_TERM_KEY_UP,
    GRUB_TERM_KEY_DOWN
};

static int keycode_by_name(const char *name)
{
    const int n = sizeof(key_names) / sizeof(key_names[0]);
    for (int i = 0; i < n; ++i) {
        if (grub_strcmp(name, key_names[i]) == 0) {
            return key_mapping[i];
        }
    }

    return -1;
}

static grub_err_t
grub_cmd_gamepad_dpad(grub_command_t cmd __attribute__((unused)),
                      int argc, char **args)
{
    if (argc < 2) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("Expected at least two arguments"));
    }

    const char *dpad_dir_name = args[0];
    int dpad_dir = dpad_dir_by_name(dpad_dir_name);

    if (dpad_dir < 0) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("%s is not a correct dpad direction name"),
            dpad_dir_name);
    }

    const char *key_name = args[1];
    int keycode = keycode_by_name(key_name);
    if (keycode < 0) {
        return grub_error(
            GRUB_ERR_BAD_ARGUMENT,
            N_("%s is not a correct key mnemonic"),
            key_name);
    }

    dpad_mapping[dpad_dir] = keycode;

    grub_dprintf(
        "usb_keyboard",
        "Dpad direction %s was mapped to keycode %d\n",
        dpad_dir_name, keycode);

    return GRUB_ERR_NONE;
}

static grub_command_t cmd_gamepad_dpad;

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
        N_("<dpad-direction> <keycode>"),
        N_("Map gamepad dpad direction to a keycode"));
    grub_usb_register_attach_hook_class(&attach_hook);
}

GRUB_MOD_FINI(usb_gamepad)
{
    grub_unregister_command (cmd_gamepad_dpad);
    grub_dprintf("usb_gamepad", "Usb_Gamepad fini-ed\n");
    // TODO(#20): usb_gamepad does not uninitialize usb stuff on FINI
}
