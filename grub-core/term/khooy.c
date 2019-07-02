#include <grub/dl.h>
#include <grub/term.h>

GRUB_MOD_LICENSE ("GPLv3");

static int
khooy_getkey (struct grub_term_input *term __attribute__ ((unused)))
{
    return 0x43;
}

static int
khooy_getkeystatus (struct grub_term_input *term __attribute__ ((unused)))
{
    return 1;
}

static struct grub_term_input khooy_input_term =
  {
    .name = "khooy",
    .getkey = khooy_getkey,
    .getkeystatus = khooy_getkeystatus
  };

GRUB_MOD_INIT(khooy)
{
    grub_dprintf("khooy", "Khooy module loaded\n");
    grub_term_register_input("khooy", &khooy_input_term);
}

GRUB_MOD_FINI(khooy)
{
    grub_dprintf("khooy", "Khooy fini-ed\n");
}
