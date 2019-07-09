#include <grub/dl.h>
#include <grub/term.h>
#include <grub/usb.h>

GRUB_MOD_LICENSE ("GPLv3");

#define USB_HID_GET_REPORT	0x01
#define USB_HID_GET_IDLE	0x02
#define USB_HID_GET_PROTOCOL	0x03
#define USB_HID_SET_REPORT	0x09
#define USB_HID_SET_IDLE	0x0A
#define USB_HID_SET_PROTOCOL	0x0B

struct grub_usb_khooy_data
{
    grub_usb_device_t usbdev;
    int configno;
    int interfno;
    struct grub_usb_desc_endp *endp;
    grub_usb_transfer_t transfer;
    grub_uint8_t report[8];
};

static int
khooy_getkey (struct grub_term_input *term)
{
    struct grub_usb_khooy_data *termdata = term->data;
    grub_size_t actual;

    grub_usb_err_t err = grub_usb_check_transfer (termdata->transfer, &actual);
    if (err == GRUB_USB_ERR_WAIT)
    {
        return GRUB_TERM_NO_KEY;
    }

    grub_dprintf("khooy",
                 "Received report: %x %x %x %x %x %x %x %x\n",
                 termdata->report[0],
                 termdata->report[1],
                 termdata->report[2],
                 termdata->report[3],
                 termdata->report[4],
                 termdata->report[5],
                 termdata->report[6],
                 termdata->report[7]);


    termdata->transfer = grub_usb_bulk_read_background (
        termdata->usbdev,
        termdata->endp,
        sizeof (termdata->report),
        (char *) termdata->report);

    if (!termdata->transfer)
    {
        grub_print_error ();
        return GRUB_TERM_NO_KEY;
    }

    return GRUB_TERM_NO_KEY;
}

static int
khooy_getkeystatus (struct grub_term_input *term __attribute__ ((unused)))
{
    return 0;
}

static struct grub_term_input khooy_input_term =
  {
    .name = "khooy",
    .getkey = khooy_getkey,
    .getkeystatus = khooy_getkeystatus
  };

static int
grub_usb_khooy_attach(grub_usb_device_t usbdev, int configno, int interfno)
{
    // TODO: grub_usb_khooy_attach leaks memory every time you connect a new USB device
    grub_dprintf("khooy", "Khooy configno: %d, interfno: %d\n", configno, interfno);
    struct grub_usb_khooy_data *data = grub_malloc(sizeof(struct grub_usb_khooy_data));
    struct grub_usb_desc_endp *endp = NULL;
    if (!data) {
        grub_print_error();
        return 0;
    }

    grub_dprintf("khooy", "Endpoints: %d\n",
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

    grub_dprintf ("khooy", "HID Khooy found! Endpoint: %d\n", j);

    data->usbdev = usbdev;
    data->configno = configno;
    data->interfno = interfno;
    data->endp = endp;
    khooy_input_term.data = data;

    data->transfer = grub_usb_bulk_read_background (
        usbdev,
        data->endp,
        sizeof (data->report),
        (char *) data->report);

    if (!data->transfer)
    {
        grub_print_error ();
        return 0;
    }

    grub_term_register_input("khooy", &khooy_input_term);

    return 0;
}

static struct grub_usb_attach_desc attach_hook =
{
    .class = GRUB_USB_CLASS_HID,
    .hook = grub_usb_khooy_attach
};

GRUB_MOD_INIT(khooy)
{
    grub_dprintf("khooy", "Khooy module loaded\n");
    grub_usb_register_attach_hook_class(&attach_hook);
}

GRUB_MOD_FINI(khooy)
{
    grub_dprintf("khooy", "Khooy fini-ed\n");
}
