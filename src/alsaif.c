#include <alsa/asoundlib.h>
#include <glib.h>

#include "options.h"
#include "logging.h"

#include "alsaif.h"

typedef struct _alsaif_iomon     alsaif_iomon;
typedef struct _alsaif_card      alsaif_card;
typedef struct _alsaif_ctl_elem  alsaif_ctl_elem;

struct _alsaif_iomon {
  GIOChannel *iochan;
  guint event_source_id;
};

struct _alsaif_card {
  alsaif_card        *next;
  int                 num;
  void               *hctl;
  char               *id;
  char               *name;
  alsaif_iomon        iomon[4];
  int                 iomon_count;
  alsaif_ctl_elem    *ctls[32];
};

struct _alsaif_ctl_elem
{
  alsaif_ctl_elem    *next;
  alsaif_card        *alsaif_card;
  unsigned int        numid;
  void               *hctl_elem;
  char               *ifname;
  char               *name;
  unsigned int        index;
  unsigned int        dev;
  unsigned int        subdev;
  snd_ctl_elem_type_t content_type;
  unsigned int        val_count;
  value_descriptor    descriptor;
};


/* Private structure */
static struct {
  alsaif_event_cb  event_cb;
  alsaif_card      *cards[256];
  struct alsaif_options opts;
} priv;


static int alsaif_add_sound_cards(void);
static alsaif_card *alsaif_cards_find(int);
static alsaif_ctl_elem *alsaif_card_find_ctl_elem(alsaif_card *, int);
static int alsaif_ctl_get_value(alsaif_ctl_elem *, long *);
static int alsaif_ctl_set_value(alsaif_ctl_elem *, long *);
static alsaif_card *alsaif_card_new(int);
static const char *alsaif_card_to_str(alsaif_card *, char *, int);
static gboolean control_event_cb(GIOChannel *, GIOCondition, gpointer);
static void alsaif_card_add_to_array(alsaif_card *);
static void alsaif_card_add_ctls(alsaif_card *);
static void alsaif_card_add_ctl_elem(alsaif_card *, alsaif_ctl_elem *);
static char *alsaif_ctl_elem_to_str(alsaif_ctl_elem *, char *, size_t);
static snd_ctl_elem_type_t alsaif_type_cast(snd_ctl_elem_type_t);

static alsaif_ctl_elem *
alsaif_card_add_hctl_elem(alsaif_card *, snd_hctl_elem_t *);

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
  alsaif_ctl_elem *ctl_elem;

  if (!value)
    return -1;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  ctl_elem = alsaif_card_find_ctl_elem(card, numid);

  if (ctl_elem)
    return alsaif_ctl_get_value(ctl_elem, value);

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
  alsaif_ctl_elem *ctl_elem;

  if (!value)
    return -1;

  if (card_num == -1 || numid == -1)
    return 0;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  ctl_elem = alsaif_card_find_ctl_elem(card, numid);

  if (ctl_elem)
    return alsaif_ctl_set_value(ctl_elem, value);

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
  alsaif_ctl_elem *ctl_elem;

  if (!descriptor)
    return SND_CTL_ELEM_TYPE_NONE;

  card = alsaif_cards_find(card_num);

  if (!card)
    goto fail;

  ctl_elem = alsaif_card_find_ctl_elem(card, numid);

  if (!ctl_elem)
    goto fail;

  *descriptor = &ctl_elem->descriptor;

  return ctl_elem->content_type;

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
  snd_ctl_t *snd_ctl_handle;
  snd_ctl_card_info_t *snd_card_info;
  char snd_ctl_handle_name[16];
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

  snprintf(snd_ctl_handle_name, sizeof(snd_ctl_handle_name), "hw:%d", card_num);

  ret = snd_hctl_open(&hctl, snd_ctl_handle_name, 0);

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
               snd_ctl_handle_name, snd_strerror(ret));
    goto fail;
  }

  snd_ctl_handle = snd_hctl_ctl(hctl);

  if (!snd_ctl_handle)
  {
    log_error("Can't obtain ctl handle for '%s'", snd_ctl_handle_name);
    goto fail;
  }

  snd_ctl_card_info_alloca(&snd_card_info);

  ret = snd_ctl_card_info(snd_ctl_handle, snd_card_info);

  if (ret < 0)
  {
    log_error("Can't obtain card info for '%s': %s",
               snd_ctl_handle_name, snd_strerror(ret));
    goto fail;
  }

  id = snd_ctl_card_info_get_id(snd_card_info);
  name = snd_ctl_card_info_get_name(snd_card_info);
  pfds_count = snd_hctl_poll_descriptors(hctl, pfds, PFDS_PER_CARD);

  card = malloc(sizeof(*card));

  if (!card)
  {
    log_error("failed to allocate memory for control '%s': %s",
               snd_ctl_handle_name, strerror(errno));
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

  snd_ctl_subscribe_events(snd_ctl_handle, 1);

  if (priv.opts.log_ctl)
    log_info("Found %s", alsaif_card_to_str(card, card_str, sizeof(card_str)));

  alsaif_card_add_to_array(card);

  if (priv.event_cb)  /* -> alsa_event_cb() */
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_CARD;
    event.data.card.id   = card->id;
    event.data.card.name = card->name;
    event.data.card.num  = card->num;

    priv.event_cb(&event);
  }

  alsaif_card_add_ctls(card);

  if (priv.event_cb)  /* -> alsa_event_cb() */
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_CTLS;
    event.data.card.id   = card->id;
    event.data.card.name = card->name;
    event.data.card.num  = card->num;

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
 * Find and add control elements to alsaif card
 * @param card  the sound card we need controls for
 */
static void
alsaif_card_add_ctls(alsaif_card *card)
{
  snd_ctl_elem_info_t *ctl_elem_info;
  snd_hctl_elem_t *hctl_elem;
  alsaif_ctl_elem *ctl_elem;
  char ctl_elem_str[256];
  int ret;

  snd_ctl_elem_info_alloca(&ctl_elem_info);

  for (hctl_elem = snd_hctl_first_elem(card->hctl);  hctl_elem;
       hctl_elem = snd_hctl_elem_next(hctl_elem))
  {
    snd_ctl_elem_info_clear(ctl_elem_info);
    ret = snd_hctl_elem_info(hctl_elem, ctl_elem_info);

    if (ret < 0)
    {
      log_error("Can't obtain elem info for 'hw:%d': %s",
                 card->num, snd_strerror(ret));
      continue;
    }

    if (snd_ctl_elem_info_is_inactive(ctl_elem_info))
      continue;

    ctl_elem = alsaif_card_add_hctl_elem(card, hctl_elem);

    if (ctl_elem && priv.opts.log_ctl)
    {
      log_info("  %s", alsaif_ctl_elem_to_str(
            ctl_elem, ctl_elem_str, sizeof(ctl_elem_str)));
    }
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
  alsaif_ctl_elem *ctl_elem;
  char ctl_elem_str[256];
  char ctl_elem_value_str[256];
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
  ctl_elem = alsaif_card_find_ctl_elem(card, numid);

  if (event_mask == SND_CTL_EVENT_MASK_REMOVE)
  {
    if (ctl_elem && priv.opts.log_ctl)
    {
      log_info("%s removed",
          alsaif_ctl_elem_to_str(ctl_elem, ctl_elem_str, sizeof(ctl_elem_str)));
    }

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_ADD)
  {
    if (!ctl_elem && priv.opts.log_ctl)
      log_info("Element added");

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_VALUE)
  {
    if (!ctl_elem)
      return TRUE;

    ctl_elem_value_str[0] = 0;

    if (alsaif_ctl_get_value(ctl_elem, &value) >= 0)
    {
      switch (ctl_elem->content_type)
      {
        case SND_CTL_ELEM_TYPE_INTEGER:
          snprintf(ctl_elem_value_str, sizeof(ctl_elem_value_str),
                  "[%ld]", value);
          break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
          if ((unsigned int)value >= ctl_elem->descriptor.enum_t.count)
          {
            snprintf(ctl_elem_value_str, sizeof(ctl_elem_value_str),
                    "[<invalid>]");
          }
          else
          {
            snprintf(ctl_elem_value_str, sizeof(ctl_elem_value_str),
                    "[%s]", ctl_elem->descriptor.enum_t.names[value]);
          }
          break;
        case SND_CTL_ELEM_TYPE_BOOLEAN:
          snprintf(ctl_elem_value_str, sizeof(ctl_elem_value_str),
                  "[%s]", value ? "on" : "off");
          break;
        default:
          snprintf(ctl_elem_value_str, sizeof(ctl_elem_value_str),
                  "[<unsupported>]");
      }
    }

    if (priv.opts.log_val)
    {
      log_info("Element value changed %s %s",
          alsaif_ctl_elem_to_str(ctl_elem, ctl_elem_str, sizeof(ctl_elem_str)),
          ctl_elem_value_str);
    }

    return TRUE;
  }

  if (event_mask & SND_CTL_EVENT_MASK_INFO && priv.opts.log_info)
  {
    /* Element info has been changed */
    log_info("Element info %s",
        alsaif_ctl_elem_to_str(ctl_elem, ctl_elem_str, sizeof(ctl_elem_str)));
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
 * @param card       alsaif_card instance
 * @param hctl_elem  ALSA hctl element
 *
 * @return  Reference to created alsaif_ctl_elem instance or NULL on error
 */
static alsaif_ctl_elem *
alsaif_card_add_hctl_elem(alsaif_card *card, snd_hctl_elem_t *hctl_elem)
{
  alsaif_ctl_elem *ctl_elem = NULL;
  snd_ctl_elem_info_t *ctl_elem_info = NULL;
  snd_ctl_elem_id_t *ctl_elem_id = NULL;
  snd_ctl_elem_iface_t ctl_elem_iface;
  const char *ctl_elem_name;
  const char *ctl_elem_ifname;
  snd_ctl_elem_type_t ctl_elem_content_type;
  alsaif_event event;

  if (!card || !hctl_elem)
    return NULL;

  ctl_elem = malloc(sizeof(*ctl_elem));

  if (!ctl_elem)
    return NULL;

  snd_ctl_elem_id_alloca(&ctl_elem_id);
  snd_hctl_elem_get_id(hctl_elem, ctl_elem_id);

  snd_ctl_elem_info_alloca(&ctl_elem_info);
  snd_hctl_elem_info(hctl_elem, ctl_elem_info);

  ctl_elem_iface = snd_ctl_elem_id_get_interface(ctl_elem_id);
  ctl_elem_ifname = snd_ctl_elem_iface_name(ctl_elem_iface);
  ctl_elem_name = snd_ctl_elem_id_get_name(ctl_elem_id);
  ctl_elem_content_type = snd_ctl_elem_info_get_type(ctl_elem_info);

  memset(ctl_elem, 0, sizeof(*ctl_elem));

  ctl_elem->next = NULL;
  ctl_elem->alsaif_card = card;
  ctl_elem->numid = snd_ctl_elem_id_get_numid(ctl_elem_id);
  ctl_elem->hctl_elem = hctl_elem;
  ctl_elem->ifname = strdup(ctl_elem_ifname);
  ctl_elem->name = strdup(ctl_elem_name);
  ctl_elem->index = snd_ctl_elem_id_get_index(ctl_elem_id);
  ctl_elem->dev = snd_ctl_elem_id_get_device(ctl_elem_id);
  ctl_elem->subdev = snd_ctl_elem_id_get_subdevice(ctl_elem_id);
  ctl_elem->content_type = alsaif_type_cast(ctl_elem_content_type);
  ctl_elem->val_count = snd_ctl_elem_info_get_count(ctl_elem_info);

  value_descriptor_fill(hctl_elem, ctl_elem_info,
      ctl_elem->content_type, &ctl_elem->descriptor);

  alsaif_card_add_ctl_elem(card, ctl_elem);

  if (priv.event_cb)
  {
    memset(&event, 0, sizeof(event));

    event.type = EVENT_ELEM;
    event.data.elem.ifname   = ctl_elem->ifname;
    event.data.elem.name     = ctl_elem->name;
    event.data.elem.index    = ctl_elem->index;
    event.data.elem.dev      = ctl_elem->dev;
    event.data.elem.subdev   = ctl_elem->subdev;
    event.data.elem.card_num = card->num;
    event.data.elem.numid    = ctl_elem->numid;

    priv.event_cb(&event);  /* alsa_event_cb() */
  }

  return ctl_elem;
}

/**
 * Get current value for ALSA control element
 *
 * @param ctl_elem  alsaif_ctl_elem instance
 * @param value     Pointer to returned value
 *
 * @return  -1 on error, 0 on success
 */
static int
alsaif_ctl_get_value(alsaif_ctl_elem *ctl_elem, long *value)
{
  snd_ctl_elem_value_t *ctl_elem_value;
  int ret;

  snd_ctl_elem_value_alloca(&ctl_elem_value);
  ret = snd_hctl_elem_read(ctl_elem->hctl_elem, ctl_elem_value);

  if (ret < 0)
  {
    log_error("Failed to read value for %s: %s",
               ctl_elem->name, snd_strerror(ret));
    return -1;
  }

  switch (ctl_elem->content_type)
  {
    case SND_CTL_ELEM_TYPE_INTEGER:
      *value = snd_ctl_elem_value_get_integer(ctl_elem_value, ctl_elem->index);
      break;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
      *value = snd_ctl_elem_value_get_enumerated(ctl_elem_value, ctl_elem->index);
      break;
    case SND_CTL_ELEM_TYPE_BOOLEAN:
      *value = snd_ctl_elem_value_get_boolean(ctl_elem_value, ctl_elem->index);
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
 * @param ctl_elem  alsaif_ctl_elem instance
 * @param value     value pointer
 *
 * @return  -1 on error, 0 on success
 */
static int
alsaif_ctl_set_value(alsaif_ctl_elem *ctl_elem, long *value)
{
  snd_ctl_elem_value_t *ctl_elem_value;
  unsigned int i;
  int ret;

  snd_ctl_elem_value_alloca(&ctl_elem_value);

  for (i = 0;  ctl_elem->val_count > i;  i++)
  {
    switch (ctl_elem->content_type)
    {
      case SND_CTL_ELEM_TYPE_INTEGER:
        snd_ctl_elem_value_set_integer(
                           ctl_elem_value, ctl_elem->index + i, *value);
        break;
      case SND_CTL_ELEM_TYPE_ENUMERATED:
        snd_ctl_elem_value_set_enumerated(
                           ctl_elem_value, ctl_elem->index + i, *value);
        break;
      case SND_CTL_ELEM_TYPE_BOOLEAN:
        snd_ctl_elem_value_set_boolean(
                           ctl_elem_value, ctl_elem->index + i, *value);
        break;
      default:
        return -1;
    }

    ret = snd_hctl_elem_write(ctl_elem->hctl_elem, ctl_elem_value);

    if (ret < 0)
    {
      log_error("Failed to write value for %s: %s",
                 ctl_elem->name, snd_strerror(ret));
      return -1;
    }
  }

  return 0;
}

/*
 * Put control element information to string. Needed for logging and debug.
 *
 * @param ctl_elem  alsaif_ctl_elem instance
 * @param str       pointer to buffer to store the string
 * @param size      size of the buffer
 *
 * @return  The reference to the string or "<buffer overflow>" if buffer size is
 *          not enough.
 */
static char *
alsaif_ctl_elem_to_str(alsaif_ctl_elem *ctl_elem, char *str, size_t size)
{
  char *p_limit;
  char *p_end;
  int size_needed;
  char descriptor_str[256];

  size_needed = snprintf(str, size, "numid=%u,iface=%s,name='%s'",
                         ctl_elem->numid, ctl_elem->ifname, ctl_elem->name);

  p_end = &str[size_needed];
  p_limit = &str[size];

  if (p_end > p_limit)
    goto fail;

  if (ctl_elem->index)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",index=%u", ctl_elem->index);
    if (p_end > p_limit)
      goto fail;
  }

  if (ctl_elem->dev)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",device=%u", ctl_elem->dev);
    if (p_end > p_limit)
      goto fail;
  }

  if (ctl_elem->subdev)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",subdevice=%u",
                      ctl_elem->subdev);

    if (p_end > p_limit)
      goto fail;
  }

  if (ctl_elem->val_count > 1)
  {
    p_end += snprintf(p_end, p_limit - p_end, ",values=%u",
                      ctl_elem->val_count);

    if (p_end > p_limit)
      goto fail;
  }

  value_descriptor_to_str(ctl_elem->content_type, &ctl_elem->descriptor,
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
 * @param hctl_elem      ALSA hctl element
 * @param ctl_elem_info  control element info structure pointer
 * @param content_type   content type for the value we need descriptor for
 * @param descriptor     Pointer to returned value_descriptor
 *
 * @return  0 if success, number of last error with minus sign otherwise
 */
static int
value_descriptor_fill(snd_hctl_elem_t *hctl_elem,
                      snd_ctl_elem_info_t *ctl_elem_info,
                      snd_ctl_elem_type_t content_type,
                      value_descriptor *descriptor)
{
  const char *enum_item_name;
  int ret, i;

  if (!hctl_elem || !ctl_elem_info || !descriptor)
    return -EINVAL;  /* Invalid argument */

  memset(descriptor, 0, sizeof(*descriptor));

  if (content_type == SND_CTL_ELEM_TYPE_INTEGER)
  {
    descriptor->int_t.min = snd_ctl_elem_info_get_min(ctl_elem_info);
    descriptor->int_t.max = snd_ctl_elem_info_get_max(ctl_elem_info);
    descriptor->int_t.step = snd_ctl_elem_info_get_step(ctl_elem_info);
    return 0;
  }

  if (content_type == SND_CTL_ELEM_TYPE_ENUMERATED)
  {
    descriptor->enum_t.count = snd_ctl_elem_info_get_items(ctl_elem_info);
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
      snd_ctl_elem_info_set_item(ctl_elem_info, i);
      ret = snd_hctl_elem_info(hctl_elem, ctl_elem_info);

      if (ret < 0)
      {
        log_error("Element info error: %s", snd_strerror(ret));
        return ret;
      }

      enum_item_name = snd_ctl_elem_info_get_item_name(ctl_elem_info);
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
  i = (alsaif_card *)&priv.cards[card->num & 0xFF];

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

  for (card = priv.cards[num & 0xFF];  card;  card = card->next)
  {
    if (card->num == num)
      return card;
  }

  return NULL;
}

/**
 * Add control element to alsaif card
 *
 * @param card      alsaif_card instance
 * @param ctl_elem  alsaif_ctl_elem instance
 */
static void
alsaif_card_add_ctl_elem(alsaif_card *card, alsaif_ctl_elem *ctl_elem)
{
  alsaif_ctl_elem *i;

  if (!card || !ctl_elem)
    return;

  i = (alsaif_ctl_elem *)&card->ctls[ctl_elem->numid & 0x1F];

  while(i->next)
    i = i->next;

  i->next = ctl_elem;
  ctl_elem->next = NULL;
}

/**
 * Find control element in alsaif_card instance
 *
 * @param card   alsaif_card instance
 * @param numid  control element numeric identifier
 *
 * @return       alsaif_ctl_elem instance or NULL if not found
 */
static alsaif_ctl_elem *
alsaif_card_find_ctl_elem(alsaif_card *card, int numid)
{
  alsaif_ctl_elem *ctl_elem;

  if (!card)
    return NULL;

  for (ctl_elem = card->ctls[numid & 0x1F]; ctl_elem; ctl_elem = ctl_elem->next)
  {
    if (ctl_elem->numid == numid)
      return ctl_elem;
  }

  return NULL;
}
