/**
 * @file alsaif.c
 * @copyright GNU GPLv2 or later
 *
 * ALSA interface functions.
 * These are needed to get/set/store sound card control elements values.
 *
 * @{ */

#include <alsa/asoundlib.h>
#include <glib.h>

#include "options.h"
#include "logging.h"

#include "alsaif.h"

#define CARDS_COUNT 16
#define CARDS_MASK  0xF

#define ELEMS_COUNT 32
#define ELEMS_MASK  0x1F

typedef struct _alsaif_iomon     alsaif_iomon;
typedef struct _alsaif_card      alsaif_card;
typedef struct _alsaif_elem      alsaif_elem;

struct _alsaif_iomon {
  GIOChannel *iochan;
  guint event_source_id;
};

struct _alsaif_card {
  /* 'next' must be the first field! */
  alsaif_card        *next;
  int                 num;
  void               *hctl;
  char               *id;
  char               *name;
  alsaif_iomon        iomon[4];
  int                 iomon_count;
  alsaif_elem        *elements[ELEMS_COUNT];
};

struct _alsaif_elem
{
  /* 'next' must be the first field! */
  alsaif_elem        *next;
  alsaif_card        *alsaif_card;
  unsigned int        numid;
  void               *hctl;
  char               *ifname;
  char               *name;
  unsigned int        index;
  unsigned int        dev;
  unsigned int        subdev;
  snd_ctl_elem_type_t val_type;
  unsigned int        val_count;
  value_descriptor    descriptor;
};


/* Private structure */
static struct {
  alsaif_event_cb  event_cb;
  alsaif_card      *cards[CARDS_COUNT];
  struct alsaif_options opts;
} priv;


static int alsaif_add_sound_cards(void);
static alsaif_card *alsaif_cards_find(int);
static alsaif_elem *alsaif_card_find_elem(alsaif_card *, int);
static int alsaif_ctl_get_value(alsaif_elem *, long *);
static int alsaif_ctl_set_value(alsaif_elem *, long *);
static alsaif_card *alsaif_card_new(int);
static const char *alsaif_card_to_str(alsaif_card *, char *, int);
static gboolean control_event_cb(GIOChannel *, GIOCondition, gpointer);
static void alsaif_card_add_to_array(alsaif_card *);
static void alsaif_card_add_controls(alsaif_card *);
static void alsaif_card_add_elem(alsaif_card *, alsaif_elem *);
static char *alsaif_elem_to_str(alsaif_elem *, char *, size_t);
static snd_ctl_elem_type_t alsaif_type_cast(snd_ctl_elem_type_t);

static alsaif_elem *
alsaif_card_add_hctl     (alsaif_card *, snd_hctl_elem_t *);

static int
value_descriptor_fill    (snd_hctl_elem_t *, snd_ctl_elem_info_t *,
                          snd_ctl_elem_type_t, value_descriptor *);

static const char *
value_descriptor_to_str  (snd_ctl_elem_type_t, value_descriptor *,
                          char *, size_t);


/**
 * Initialize alsaif options
 * @param options  options parsed from command line
 * @return  always zero
 */
int
alsaif_init(struct options *options)
{
  priv.opts = options->alsaif;
  return 0;
}

/**
 * Set alsaif callback function
 * @param cb  the callback
 */
void
alsaif_set_cb(alsaif_event_cb cb)
{
  priv.event_cb = cb;
}

/**
 * Initialize alsaif with real hardware data
 * @return  always zero
 */
int
alsaif_create()
{
  alsaif_add_sound_cards();
  return 0;
}

/**
 * Get ALSA control element value
 *
 * @param card_num  Sound card number
 * @param numid     Control element numid
 * @param value     Pointer to store value
 *
 * @return  -1 if error, 0 if success
 */
int
alsaif_get_value(int card_num, int numid, long *value)
{
  alsaif_card *card;
  alsaif_elem *elem;

  if (!value)
    return -1;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  elem = alsaif_card_find_elem(card, numid);

  if (elem)
    return alsaif_ctl_get_value(elem, value);

fail:
  log_error("%s(): Can't find control element (card=%d,numid=%d)",
            "alsaif_get_value", card_num, numid);

  return -1;
}

/**
 * Set ALSA control element value
 *
 * @param card_num  Sound card number
 * @param numid     Control element numid
 * @param value     Pointer to the value
 *
 * @return  -1 if error, 0 if success
 */
int
alsaif_set_value(int card_num, int numid, long *value)
{
  alsaif_card *card;
  alsaif_elem *elem;

  if (!value)
    return -1;

  if (card_num == -1 || numid == -1)
    return 0;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  elem = alsaif_card_find_elem(card, numid);

  if (elem)
    return alsaif_ctl_set_value(elem, value);

fail:
  log_error("%s(): Can't find control element (card=%d,numid=%d)",
            "alsaif_set_value", card_num, numid);

  return -1;
}

/**
 * Get control element value descriptor
 *
 * @param card_num    Sound card number
 * @param numid       Control element numid
 * @param descriptor  Pointer to store the reference
 *
 * @return  Control element value type
 */
snd_ctl_elem_type_t
alsaif_get_value_descriptor(int card_num,
                            int numid,
                            value_descriptor **descriptor)
{
  alsaif_card *card;
  alsaif_elem *elem;

  if (!descriptor)
    return SND_CTL_ELEM_TYPE_NONE;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  elem = alsaif_card_find_elem(card, numid);

  if (!elem)
    goto fail;

  *descriptor = &elem->descriptor;

  return elem->val_type;

fail:
  log_error("%s(): Can't find control element (card=%d,numid=%d)",
            "alsaif_get_value_descriptor", card_num, numid);

  return SND_CTL_ELEM_TYPE_NONE;
}

/**
 * Add all sound cards data to alsaif.
 * @return  Zero if success, otherwise a negative error code
 */
static int
alsaif_add_sound_cards()
{
  int result;
  int card_num = -1;

  for (;;)
  {
    result = snd_card_next(&card_num);

    if (result || card_num < 0)
      break;

    alsaif_card_new(card_num);
  }

  return result;
}

/**
 * Add new sound card to alsaif.
 * @param card_num  Sound card number
 * @return  Created instance of alsaif_card
 */
static alsaif_card *
alsaif_card_new(int card_num)
{
#define PFDS_PER_CARD 4
  alsaif_card *card = NULL;
  snd_hctl_t *hctl = NULL;
  snd_ctl_t *ctl;
  snd_ctl_card_info_t *info;
  char ctl_name[16];
  const char *id;
  const char *name;
  int pfds_count;
  struct pollfd pfds[PFDS_PER_CARD];
  GIOChannel *iochan;
  guint event_source_id;
  char card_str[256];
  alsaif_iomon *iomon;
  alsaif_event event;
  int ret, i;

  if (card_num < 0)
    return NULL;

  snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card_num);

  ret = snd_hctl_open(&hctl, ctl_name, 0);

  if (ret < 0)
  {
    log_error("Can't open '%d' ALSA hctl: %s",
               card_num, snd_strerror(ret));
    goto fail;
  }

  ret = snd_hctl_load(hctl);

  if (ret < 0)
  {
    log_error("Control '%s' local error: %s",
               ctl_name, snd_strerror(ret));
    goto fail;
  }

  ctl = snd_hctl_ctl(hctl);

  if (!ctl)
  {
    log_error("Can't obtain ctl handle for '%s'", ctl_name);
    goto fail;
  }

  snd_ctl_card_info_alloca(&info);

  ret = snd_ctl_card_info(ctl, info);

  if (ret < 0)
  {
    log_error("Can't obtain card info for '%s': %s",
               ctl_name, snd_strerror(ret));
    goto fail;
  }

  id = snd_ctl_card_info_get_id(info);
  name = snd_ctl_card_info_get_name(info);
  pfds_count = snd_hctl_poll_descriptors(hctl, pfds, PFDS_PER_CARD);

  card = malloc(sizeof(*card));

  if (!card)
  {
    log_error("failed to allocate memory for control '%s': %s",
               ctl_name, strerror(errno));
    goto fail;
  }

  memset(card, 0, sizeof(*card));

  card->next = NULL;
  card->num  = card_num;
  card->hctl = hctl;
  card->id   = strdup(id);
  card->name = strdup(name);

  for (i = 0;  i < pfds_count;  i++)
  {
    if (pfds[i].events & POLLIN)
    {
      iochan = g_io_channel_unix_new(pfds[i].fd);

      if (!iochan)
      {
        log_error("Can't add ctl to main loop");
        goto fail;
      }

      event_source_id = g_io_add_watch(iochan, G_IO_IN | G_IO_ERR,
                                       control_event_cb, card);

      iomon = &card->iomon[card->iomon_count++];
      iomon->iochan = iochan;
      iomon->event_source_id = event_source_id;
    }
  }

  snd_ctl_subscribe_events(ctl, 1);

  if (priv.opts.log_ctl)
    log_info("Found %s", alsaif_card_to_str(card, card_str, sizeof(card_str)));

  alsaif_card_add_to_array(card);

  if (priv.event_cb)  /* -> alsa_event_cb() */
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_SOUNDCARD_ADDED;
    event.card.id   = card->id;
    event.card.name = card->name;
    event.card.num  = card->num;

    priv.event_cb(&event);
  }

  alsaif_card_add_controls(card);

  if (priv.event_cb)  /* -> alsa_event_cb() */
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_CONTROLS_ADDED;
    event.card.id   = card->id;
    event.card.name = card->name;
    event.card.num  = card->num;

    priv.event_cb(&event);
  }

  return card;

fail:
  free(card);

  if (hctl)
  {
    snd_hctl_free(hctl);
    snd_hctl_close(hctl);
  }

  return NULL;
}

/**
 * Find and add control elements to alsaif sound card interface.
 * @param card  the sound card
 */
static void
alsaif_card_add_controls(alsaif_card *card)
{
  snd_ctl_elem_info_t *info;
  snd_hctl_elem_t *hctl;
  alsaif_elem *elem;
  char elem_str[256];
  int ret, i;

  snd_ctl_elem_info_alloca(&info);

  for (hctl = snd_hctl_first_elem(card->hctl);  hctl;
       hctl = snd_hctl_elem_next(hctl))
  {
    snd_ctl_elem_info_clear(info);
    ret = snd_hctl_elem_info(hctl, info);

    if (ret < 0)
    {
      log_error("Can't obtain elem info for 'hw:%d': %s",
                 card->num, snd_strerror(ret));
      continue;
    }

    if (snd_ctl_elem_info_is_inactive(info))
      continue;

    elem = alsaif_card_add_hctl(card, hctl);
  }

  if (priv.opts.log_ctl)
  {
    i = alsaif_card_find_elem(card, 0) ? 0 : 1;
    for (;  (elem = alsaif_card_find_elem(card, i));  i++)
      log_info("  %s", alsaif_elem_to_str(elem, elem_str, sizeof(elem_str)));
  }
}

/**
 * Callback for ALSA changes to sound cards state.
 *
 * This function just logs ALSA controls changes:
 * - ctl add
 * - ctl remove
 * - value/info modified
 *
 * @param source     the GIOChannel event source
 * @param condition  the condition which has been satisfied
 * @param data       alsaif_card instance (user data)
 *
 * @return  FALSE if the event source should be removed
 */
static gboolean
control_event_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  alsaif_card *card = (alsaif_card *)data;
  snd_ctl_t *snd_ctl;
  snd_ctl_event_t *snd_ctl_event;
  unsigned int numid;
  unsigned int event_mask;
  alsaif_elem *elem;
  char elem_str[256];
  char elem_value_str[256];
  long value;
  int ret;

  snd_ctl = snd_hctl_ctl(card->hctl);
  snd_ctl_event_alloca(&snd_ctl_event);
  ret = snd_ctl_read(snd_ctl, snd_ctl_event);

  if (ret < 0)
  {
    log_error("%s(): failed to read events on card %d: %s",
              "control_event_cb", card->num, snd_strerror(ret));

    /* Event source should be removed */
    return FALSE;
  }

  if (snd_ctl_event_get_type(snd_ctl_event) != SND_CTL_EVENT_ELEM)
    return TRUE;

  numid = snd_ctl_event_elem_get_numid(snd_ctl_event);
  event_mask = snd_ctl_event_elem_get_mask(snd_ctl_event);
  elem = alsaif_card_find_elem(card, numid);

  if (event_mask == SND_CTL_EVENT_MASK_REMOVE)
  {
    if (elem && priv.opts.log_ctl)
    {
      log_info("%s removed",
          alsaif_elem_to_str(elem, elem_str, sizeof(elem_str)));
    }

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_ADD)
  {
    if (!elem && priv.opts.log_ctl)
      log_info("Element added");

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_VALUE)
  {
    if (!elem)
      return TRUE;

    elem_value_str[0] = 0;

    if (alsaif_ctl_get_value(elem, &value) >= 0)
    {
      switch (elem->val_type)
      {
        case SND_CTL_ELEM_TYPE_INTEGER:
          snprintf(elem_value_str, sizeof(elem_value_str),
                  "[%ld]", value);
          break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
          if ((unsigned int)value >= elem->descriptor.enum_t.count)
          {
            snprintf(elem_value_str, sizeof(elem_value_str),
                    "[<invalid>]");
          }
          else
          {
            snprintf(elem_value_str, sizeof(elem_value_str),
                    "[%s]", elem->descriptor.enum_t.names[value]);
          }
          break;
        case SND_CTL_ELEM_TYPE_BOOLEAN:
          snprintf(elem_value_str, sizeof(elem_value_str),
                  "[%s]", value ? "on" : "off");
          break;
        default:
          snprintf(elem_value_str, sizeof(elem_value_str),
                  "[<unsupported>]");
      }
    }

    if (priv.opts.log_val)
    {
      log_info("Element value changed %s %s",
          alsaif_elem_to_str(elem, elem_str, sizeof(elem_str)), elem_value_str);
    }

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_INFO && priv.opts.log_info)
  {
    /* Element info has been changed */
    log_info("Element info %s",
        alsaif_elem_to_str(elem, elem_str, sizeof(elem_str)));
  }

  return TRUE;
}

/**
 * Put soundcard id information to string.
 * This is needed for logging.
 *
 * @param card  alsaif_card instance
 * @param str   pointer to the string buffer
 * @param size  buffer size
 *
 * @return  Reference to the string or "<buffer overflow>" if there's no enough
 *          space in the buffer
 */
static const char *
alsaif_card_to_str(alsaif_card *card, char *str, int size)
{
  int size_needed = snprintf(str, size, "card=%d,id=%s,name='%s'",
                             card->num, card->id, card->name);

  return size_needed <= size ? str : "<buffer overflow>";
}

/**
 * Add ALSA hctl element to alsaif_card controls
 *
 * @param card  alsaif_card instance
 * @param hctl  ALSA hctl element
 *
 * @return  Reference to created alsaif_elem instance or NULL on error
 */
static alsaif_elem *
alsaif_card_add_hctl(alsaif_card *card, snd_hctl_elem_t *hctl)
{
  alsaif_elem *elem = NULL;
  snd_ctl_elem_info_t *info = NULL;
  snd_ctl_elem_id_t *elem_id = NULL;
  snd_ctl_elem_iface_t elem_iface;
  const char *elem_name;
  const char *elem_ifname;
  snd_ctl_elem_type_t elem_val_type;
  alsaif_event event;

  if (!card || !hctl)
    return NULL;

  elem = malloc(sizeof(*elem));

  if (!elem)
    return NULL;

  snd_ctl_elem_id_alloca(&elem_id);
  snd_hctl_elem_get_id(hctl, elem_id);

  snd_ctl_elem_info_alloca(&info);
  snd_hctl_elem_info(hctl, info);

  elem_iface = snd_ctl_elem_id_get_interface(elem_id);
  elem_ifname = snd_ctl_elem_iface_name(elem_iface);
  elem_name = snd_ctl_elem_id_get_name(elem_id);
  elem_val_type = snd_ctl_elem_info_get_type(info);

  memset(elem, 0, sizeof(*elem));

  elem->next = NULL;
  elem->alsaif_card = card;
  elem->numid = snd_ctl_elem_id_get_numid(elem_id);
  elem->hctl = hctl;
  elem->ifname = strdup(elem_ifname);
  elem->name = strdup(elem_name);
  elem->index = snd_ctl_elem_id_get_index(elem_id);
  elem->dev = snd_ctl_elem_id_get_device(elem_id);
  elem->subdev = snd_ctl_elem_id_get_subdevice(elem_id);
  elem->val_type = alsaif_type_cast(elem_val_type);
  elem->val_count = snd_ctl_elem_info_get_count(info);

  value_descriptor_fill(hctl, info, elem->val_type, &elem->descriptor);

  alsaif_card_add_elem(card, elem);

  if (priv.event_cb)
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_CTL_ELEM_ADDED;
    event.elem.ifname   = elem->ifname;
    event.elem.name     = elem->name;
    event.elem.index    = elem->index;
    event.elem.dev      = elem->dev;
    event.elem.subdev   = elem->subdev;
    event.elem.card_num = card->num;
    event.elem.numid    = elem->numid;

    priv.event_cb(&event);  /* alsa_event_cb() */
  }

  return elem;
}

/**
 * Get current value for ALSA control element
 *
 * @param elem   alsaif_elem instance
 * @param value  Pointer to returned value
 *
 * @return  -1 on error, 0 on success
 */
static int
alsaif_ctl_get_value(alsaif_elem *elem, long *value)
{
  snd_ctl_elem_value_t *elem_value;
  int ret;

  snd_ctl_elem_value_alloca(&elem_value);
  ret = snd_hctl_elem_read(elem->hctl, elem_value);

  if (ret < 0)
  {
    log_error("Failed to read value for %s: %s", elem->name, snd_strerror(ret));
    return -1;
  }

  switch (elem->val_type)
  {
    case SND_CTL_ELEM_TYPE_INTEGER:
      *value = snd_ctl_elem_value_get_integer(elem_value, elem->index);
      break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
      *value = snd_ctl_elem_value_get_enumerated(elem_value, elem->index);
      break;
    case SND_CTL_ELEM_TYPE_BOOLEAN:
      *value = snd_ctl_elem_value_get_boolean(elem_value, elem->index);
      break;
    default:
      *value = 0;
      return -1;
  }

  return 0;
}

/**
 * Set ALSA control element value. If there's more than one value for control
 * element, all values are set to the same.
 *
 * @param elem   alsaif_elem instance
 * @param value  value pointer
 *
 * @return  -1 on error, 0 on success
 */
static int
alsaif_ctl_set_value(alsaif_elem *elem, long *value)
{
  snd_ctl_elem_value_t *elem_value;
  unsigned int i;
  int ret;

  snd_ctl_elem_value_alloca(&elem_value);

  for (i = 0;  elem->val_count > i;  i++)
  {
    switch (elem->val_type)
    {
      case SND_CTL_ELEM_TYPE_INTEGER:
        snd_ctl_elem_value_set_integer(elem_value, elem->index + i, *value);
        break;
      case SND_CTL_ELEM_TYPE_ENUMERATED:
        snd_ctl_elem_value_set_enumerated(elem_value, elem->index + i, *value);
        break;
      case SND_CTL_ELEM_TYPE_BOOLEAN:
        snd_ctl_elem_value_set_boolean(elem_value, elem->index + i, *value);
        break;
      default:
        return -1;
    }

    ret = snd_hctl_elem_write(elem->hctl, elem_value);

    if (ret < 0)
    {
      log_error("Failed to write value for %s: %s",
                 elem->name, snd_strerror(ret));
      return -1;
    }
  }

  return 0;
}

/*
 * Put control element information to string. Needed for logging and debug.
 *
 * @param elem  alsaif_elem instance
 * @param str   pointer to buffer to store the string
 * @param size  size of the buffer
 *
 * @return      The reference to the string or "<buffer overflow>" if buffer size
 *              is not enough.
 */
static char *
alsaif_elem_to_str(alsaif_elem *elem, char *str, size_t size)
{
  char *p_limit;
  char *p_end;
  int size_needed;
  char descriptor_str[256];

  size_needed = snprintf(str, size, "numid=%u,iface=%s,name='%s'",
                         elem->numid, elem->ifname, elem->name);

  p_end = &str[size_needed];
  p_limit = &str[size];

  if (p_end > p_limit)
    goto fail;

  if (elem->index)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",index=%u", elem->index);
    if (p_end > p_limit)
      goto fail;
  }

  if (elem->dev)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",device=%u", elem->dev);
    if (p_end > p_limit)
      goto fail;
  }

  if (elem->subdev)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",subdevice=%u", elem->subdev);

    if (p_end > p_limit)
      goto fail;
  }

  if (elem->val_count > 1)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",values=%u",
                      elem->val_count);

    if (p_end > p_limit)
      goto fail;
  }

  value_descriptor_to_str(elem->val_type, &elem->descriptor,
                          descriptor_str, sizeof(descriptor_str));

  p_end += snprintf(p_end, p_limit - p_end, "\n   [%s]", descriptor_str);

  if (p_end <= p_limit)
    return str;

fail:
  if (size > 0)
    str[0] = 0;

  return "<buffer overflow>";
}

/**
 * Limit snd_ctl_elem_type_t to only types we know how to handle.
 * @param type  snd_ctl_elem_type_t we need to proceed
 * @return      a type we are able to handle or SND_CTL_ELEM_TYPE_NONE
 */
static snd_ctl_elem_type_t
alsaif_type_cast(snd_ctl_elem_type_t type)
{
  if (type == SND_CTL_ELEM_TYPE_INTEGER ||
      type == SND_CTL_ELEM_TYPE_ENUMERATED ||
      type == SND_CTL_ELEM_TYPE_BOOLEAN)
  {
    return type;
  }

  return SND_CTL_ELEM_TYPE_NONE;
}

/**
 * Get value descriptor information and put to value_descriptor instance
 *
 * @param hctl        ALSA hctl element
 * @param info        Control element info structure pointer
 * @param val_type    Type of the value we need descriptor for
 * @param descriptor  Pointer to returned value_descriptor
 *
 * @return  0 if success, number of last error with minus sign otherwise
 */
static int
value_descriptor_fill(snd_hctl_elem_t *hctl,
                      snd_ctl_elem_info_t *info,
                      snd_ctl_elem_type_t val_type,
                      value_descriptor *descriptor)
{
  const char *enum_item_name;
  int ret, i;

  if (!hctl || !info || !descriptor)
    return -EINVAL;  /* Invalid argument */

  memset(descriptor, 0, sizeof(*descriptor));

  if (val_type == SND_CTL_ELEM_TYPE_INTEGER)
  {
    descriptor->int_t.min = snd_ctl_elem_info_get_min(info);
    descriptor->int_t.max = snd_ctl_elem_info_get_max(info);
    descriptor->int_t.step = snd_ctl_elem_info_get_step(info);
    return 0;
  }

  if (val_type == SND_CTL_ELEM_TYPE_ENUMERATED)
  {
    descriptor->enum_t.count = snd_ctl_elem_info_get_items(info);
    descriptor->enum_t.names = calloc(descriptor->enum_t.count, sizeof(char *));

    if (!descriptor->enum_t.names)
    {
      log_error(
        "Can't allocate memory for enumerated value descriptor with %d items: %s",
        descriptor->enum_t.count, strerror(errno));
      return -errno;
    }

    for (i = 0;  i < descriptor->enum_t.count;  i++)
    {
      snd_ctl_elem_info_set_item(info, i);
      ret = snd_hctl_elem_info(hctl, info);

      if (ret < 0)
      {
        log_error("Element info error: %s", snd_strerror(ret));
        return ret;
      }

      enum_item_name = snd_ctl_elem_info_get_item_name(info);
      if (!enum_item_name)
      {
        log_error("Element item error");
        return -EIO;  /* I/O error */
      }

      descriptor->enum_t.names[i] = strdup(enum_item_name);
    }
  }

  return 0;
}

/**
 * Put value descriptor information to a string.
 *
 * @param type        value content type descriptor describes
 * @param descriptor  instance of value_descriptor
 * @param str         string buffer to output the result
 * @param size        string buffer size
 *
 * @return  The string reference
 */
static const char *
value_descriptor_to_str(snd_ctl_elem_type_t type,
                        value_descriptor *descriptor,
                        char *str,
                        size_t size)
{
  char *p_end, *p_limit;
  const char *prefix;
  int i;

  switch (type)
  {
    case SND_CTL_ELEM_TYPE_INTEGER:
      snprintf(str, size, "range %ld - %ld, step %ld",
          descriptor->int_t.min,
          descriptor->int_t.max,
          descriptor->int_t.step);
      break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
      p_limit = &str[size];
      p_end = str;
      prefix = "";

      for (i = 0;  i < descriptor->enum_t.count;  i++)
      {
        p_end += snprintf(p_end, p_limit - p_end, "%s'%s'",
                          prefix, descriptor->enum_t.names[i]);

        if (p_end > p_limit)
        {
          snprintf(str, size, "<buffer overflow>");
          break;
        }

        prefix = ", ";
      }
      break;
    case SND_CTL_ELEM_TYPE_BOOLEAN:
      snprintf(str, size, "'on', 'off'");
      break;
    default:
      if (size > 0)
        str[0] = 0;
  }

  return str;
}

/**
 * Add alsaif_card instance to alsaif array of cards
 * @param card  alsaif_card instance
 */
static void
alsaif_card_add_to_array(alsaif_card *card)
{
  alsaif_card *i;

  if (!card)
    return;

  /* The hacky way to add the first instance to the list */
  i = (alsaif_card *)&priv.cards[card->num & CARDS_MASK];

  while(i->next)
    i = i->next;

  i->next = card;
  card->next = NULL;
}

/**
 * Find alsaif_card in alsaif array
 * @param num  card number
 * @return     alsaif_card instance or NULL if not found
 */
static alsaif_card *
alsaif_cards_find(int num)
{
  alsaif_card *card;

  if (num < 0)
    return NULL;

  for (card = priv.cards[num & CARDS_MASK];  card;  card = card->next)
  {
    if (card->num == num)
      return card;
  }

  return NULL;
}

/**
 * Add control element to alsaif card
 *
 * @param card  alsaif_card instance
 * @param elem  alsaif_elem instance
 */
static void
alsaif_card_add_elem(alsaif_card *card, alsaif_elem *elem)
{
  alsaif_elem *i;

  if (!card || !elem)
    return;

  i = (alsaif_elem *)&card->elements[elem->numid & ELEMS_MASK];

  while (i->next)
    i = i->next;

  i->next = elem;
  elem->next = NULL;
}

/**
 * Find control element in alsaif_card instance
 *
 * @param card   alsaif_card instance
 * @param numid  control element numeric identifier
 *
 * @return       alsaif_elem instance or NULL if not found
 */
static alsaif_elem *
alsaif_card_find_elem(alsaif_card *card, int numid)
{
  alsaif_elem *elem;

  if (!card)
    return NULL;

  for (elem = card->elements[numid & ELEMS_MASK]; elem; elem = elem->next)
  {
    if (elem->numid == numid)
      return elem;
  }

  return NULL;
}

/** @} */
