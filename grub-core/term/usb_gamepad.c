#include <grub/dl.h>
#include <grub/term.h>
#include <grub/usb.h>
#include <grub/command.h>

GRUB_MOD_LICENSE ("GPLv3");

#define USB_HID_GET_REPORT	0x01
#define USB_HID_GET_IDLE	0x02
#define USB_HID_GET_PROTOCOL	0x03
#define USB_HID_SET_REPORT	0x09
#define USB_HID_SET_IDLE	0x0A
#define USB_HID_SET_PROTOCOL	0x0B

#define DPAD_UP 0x0
#define DPAD_UPRIGHT 0x1
#define DPAD_RIGHT 0x2
#define DPAD_DOWNRIGHT 0x3
#define DPAD_DOWN 0x4
#define DPAD_DOWNLEFT 0x5
#define DPAD_LEFT 0x6
#define DPAD_UPLEFT 0x7
#define DPAD_CENTERED 0x8

static int dpad_mapping[9] = { GRUB_TERM_NO_KEY };
// static int button_mapping[4] = { GRUB_TERM_NO_KEY };

static const char *dpad_names[9] = {
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
    grub_uint8_t x1;
    grub_uint8_t y1;
    grub_uint8_t x2;
    grub_uint8_t y2;
    grub_uint8_t dpad: 4;
    grub_uint8_t buttons: 4;
    grub_uint8_t lb: 1;
    grub_uint8_t rb: 1;
    grub_uint8_t lt: 1;
    grub_uint8_t rt: 1;
    grub_uint8_t back: 1;
    grub_uint8_t start: 1;
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

// 80 7f 80 7f 08 00 04 ff
// 00 00 00 00 08 00 04 ff

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
    int key_queue_size;
};

// static void generate_keys(struct grub_usb_gamepad_data *data)
// {
//     if (termdata->prev_state.dpad != termdata->state.dpad) {
//         // TODO: generate_keys does not handle key_queue overflow
//         key_queue[key_queue_size++] = dpad_mapping[termdata->state.dpad];
//     }
// }

static int
usb_gamepad_getkey (struct grub_term_input *term)
{
    struct grub_usb_gamepad_data *termdata = term->data;
    grub_size_t actual;

    grub_usb_err_t err = grub_usb_check_transfer (termdata->transfer, &actual);
    if (err == GRUB_USB_ERR_WAIT)
    {
        return GRUB_TERM_NO_KEY;
    }

    termdata->prev_state = termdata->state;

    print_logitech_state(&termdata->state);

    grub_dprintf("usb_gamepad", "Key down: %d\n", GRUB_TERM_KEY_DOWN);

    int key = GRUB_TERM_NO_KEY;

    key = dpad_mapping[termdata->state.dpad];

    // TODO(#19): one usb report can represent several key strokes
    //   And usb_gamepad_getkey does not support that.
    if (termdata->state.buttons & (1 << 1)) {
        key = '\n';
    }

    termdata->transfer = grub_usb_bulk_read_background (
        termdata->usbdev,
        termdata->endp,
        sizeof (termdata->state),
        (char *) &termdata->state);

    if (!termdata->transfer)
    {
        grub_print_error ();
        return key;
    }

    return key;
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
    for (int i = 0; i < 9; ++i) {
        if (grub_strcmp(name, dpad_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

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
