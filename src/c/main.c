#include <pebble.h>

// ============================================================
// Neko watchface
// ------------------------------------------------------------
// An animated 90s-style cat screenmate sits atop the digital
// clock, cycling between walking, scratching, and idle poses.
// After a while it yawns, falls asleep, and waits for a tap to
// wake up. Timeline Quick View hides the cat and promotes the
// clock into the unobstructed area.
// ============================================================

// -------- Animation resources --------

typedef struct {
  uint32_t frames[2];
  uint8_t  frame_count;
} NekoAnim;

static const NekoAnim s_normal_anims[] = {
  { { RESOURCE_ID_IMAGE_JARE2,    RESOURCE_ID_IMAGE_JARE2    }, 1 },
  { { RESOURCE_ID_IMAGE_LEFT1,    RESOURCE_ID_IMAGE_LEFT2    }, 2 },
  { { RESOURCE_ID_IMAGE_RIGHT1,   RESOURCE_ID_IMAGE_RIGHT2   }, 2 },
  { { RESOURCE_ID_IMAGE_UP1,      RESOURCE_ID_IMAGE_UP2      }, 2 },
  { { RESOURCE_ID_IMAGE_DOWN1,    RESOURCE_ID_IMAGE_DOWN2    }, 2 },
  { { RESOURCE_ID_IMAGE_UPLEFT1,  RESOURCE_ID_IMAGE_UPLEFT2  }, 2 },
  { { RESOURCE_ID_IMAGE_UPRIGHT1, RESOURCE_ID_IMAGE_UPRIGHT2 }, 2 },
  { { RESOURCE_ID_IMAGE_DWLEFT1,  RESOURCE_ID_IMAGE_DWLEFT2  }, 2 },
  { { RESOURCE_ID_IMAGE_DWRIGHT1, RESOURCE_ID_IMAGE_DWRIGHT2 }, 2 },
  { { RESOURCE_ID_IMAGE_KAKI1,    RESOURCE_ID_IMAGE_KAKI2    }, 2 },
  { { RESOURCE_ID_IMAGE_LTOGI1,   RESOURCE_ID_IMAGE_LTOGI2   }, 2 },
  { { RESOURCE_ID_IMAGE_RTOGI1,   RESOURCE_ID_IMAGE_RTOGI2   }, 2 },
  { { RESOURCE_ID_IMAGE_UTOGI1,   RESOURCE_ID_IMAGE_UTOGI2   }, 2 },
  { { RESOURCE_ID_IMAGE_DTOGI1,   RESOURCE_ID_IMAGE_DTOGI2   }, 2 },
};
#define NORMAL_ANIM_COUNT (sizeof(s_normal_anims) / sizeof(s_normal_anims[0]))

static const NekoAnim s_anim_mati  = { { RESOURCE_ID_IMAGE_MATI2,  RESOURCE_ID_IMAGE_MATI3  }, 2 };
static const NekoAnim s_anim_sleep = { { RESOURCE_ID_IMAGE_SLEEP1, RESOURCE_ID_IMAGE_SLEEP2 }, 2 };
static const NekoAnim s_anim_awake = { { RESOURCE_ID_IMAGE_AWAKE,  RESOURCE_ID_IMAGE_AWAKE  }, 1 };

// -------- State --------

typedef enum {
  STATE_NORMAL,
  STATE_YAWNING,
  STATE_SLEEPING,
  STATE_WAKING,
} NekoState;

static Window       *s_window;
static BitmapLayer  *s_neko_layer;
static GBitmap      *s_current_bitmap = NULL;
static TextLayer    *s_time_layer;
static TextLayer    *s_date_layer;
static AppTimer     *s_anim_timer = NULL;

static NekoState       s_state = STATE_NORMAL;
static const NekoAnim *s_current_anim = &s_normal_anims[0];
static uint8_t         s_frame_idx = 0;
static uint16_t        s_ticks_in_anim = 0;
static uint16_t        s_anim_duration_ticks = 12;

static char s_time_text[8];
static char s_date_text[24];

// -------- Tunables --------

#define ANIM_PERIOD_MS        300
#define NEKO_WIDTH             96
#define NEKO_HEIGHT            96
#define YAWN_DURATION_TICKS    10    // ~3 s of yawning before sleep
#define WAKE_DURATION_TICKS     4    // ~1.2 s of "awake" frame after tap
#define MIN_ANIM_TICKS          8    // minimum length of a normal animation
#define MAX_ANIM_TICKS         20    // maximum length of a normal animation
#define YAWN_PROB_DENOM        12    // 1-in-N chance to yawn after each normal anim

// Per-platform clock sizing: BITHAM_42 is too tall for 144x168 screens.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  #define TIME_FONT_KEY FONT_KEY_BITHAM_42_BOLD
  #define DATE_FONT_KEY FONT_KEY_GOTHIC_24_BOLD
  #define TIME_H 50
  #define DATE_H 28
#else
  #define TIME_FONT_KEY FONT_KEY_BITHAM_30_BLACK
  #define DATE_FONT_KEY FONT_KEY_GOTHIC_18_BOLD
  #define TIME_H 34
  #define DATE_H 22
#endif

// -------- Forward declarations --------

static void schedule_anim_timer(void);
static void update_bitmap(void);
static void layout_ui(void);

// -------- Helpers --------

static uint32_t rand_range(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return lo;
  return lo + ((uint32_t)rand() % (hi - lo + 1));
}

static void swap_bitmap(uint32_t resource_id) {
  GBitmap *old = s_current_bitmap;
  s_current_bitmap = gbitmap_create_with_resource(resource_id);
  bitmap_layer_set_bitmap(s_neko_layer, s_current_bitmap);
  if (old) gbitmap_destroy(old);
}

static void update_bitmap(void) {
  uint8_t idx = s_frame_idx % s_current_anim->frame_count;
  swap_bitmap(s_current_anim->frames[idx]);
}

static void start_normal(void) {
  s_state = STATE_NORMAL;
  s_current_anim = &s_normal_anims[rand() % NORMAL_ANIM_COUNT];
  s_frame_idx = 0;
  s_ticks_in_anim = 0;
  s_anim_duration_ticks = rand_range(MIN_ANIM_TICKS, MAX_ANIM_TICKS);
}

static void start_yawn(void) {
  s_state = STATE_YAWNING;
  s_current_anim = &s_anim_mati;
  s_frame_idx = 0;
  s_ticks_in_anim = 0;
}

static void start_sleep(void) {
  s_state = STATE_SLEEPING;
  s_current_anim = &s_anim_sleep;
  s_frame_idx = 0;
  s_ticks_in_anim = 0;
}

static void start_wake(void) {
  s_state = STATE_WAKING;
  s_current_anim = &s_anim_awake;
  s_frame_idx = 0;
  s_ticks_in_anim = 0;
}

// -------- Animation tick --------

static void anim_timer_cb(void *data) {
  s_anim_timer = NULL;
  s_ticks_in_anim++;
  s_frame_idx++;

  switch (s_state) {
    case STATE_NORMAL:
      if (s_ticks_in_anim >= s_anim_duration_ticks) {
        if ((rand() % YAWN_PROB_DENOM) == 0) {
          start_yawn();
        } else {
          start_normal();
        }
      }
      break;
    case STATE_YAWNING:
      if (s_ticks_in_anim >= YAWN_DURATION_TICKS) {
        start_sleep();
      }
      break;
    case STATE_SLEEPING:
      // Stays asleep until a tap.
      break;
    case STATE_WAKING:
      if (s_ticks_in_anim >= WAKE_DURATION_TICKS) {
        start_normal();
      }
      break;
  }

  update_bitmap();
  schedule_anim_timer();
}

static void schedule_anim_timer(void) {
  if (s_anim_timer) return;
  s_anim_timer = app_timer_register(ANIM_PERIOD_MS, anim_timer_cb, NULL);
}

// -------- Tap (wake from sleep) --------

static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_state == STATE_SLEEPING || s_state == STATE_YAWNING) {
    start_wake();
    update_bitmap();
  }
}

// -------- Clock --------

static void update_time_text(struct tm *t) {
  if (clock_is_24h_style()) {
    strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  } else {
    strftime(s_time_text, sizeof(s_time_text), "%l:%M", t);
  }
  strftime(s_date_text, sizeof(s_date_text), "%a %b %e", t);
  text_layer_set_text(s_time_layer, s_time_text);
  text_layer_set_text(s_date_layer, s_date_text);
}

static void tick_handler(struct tm *tick_time, TimeUnits units) {
  update_time_text(tick_time);
}

// -------- Layout (handles Timeline Quick View) --------

static void layout_ui(void) {
  Layer *window_layer = window_get_root_layer(s_window);
  GRect full  = layer_get_bounds(window_layer);
  GRect avail = layer_get_unobstructed_bounds(window_layer);
  bool obstructed = !grect_equal(&full, &avail);

  Layer *neko_l = bitmap_layer_get_layer(s_neko_layer);
  Layer *time_l = text_layer_get_layer(s_time_layer);
  Layer *date_l = text_layer_get_layer(s_date_layer);

  if (obstructed) {
    // Timeline Quick View is peeking in. Hide the neko, push the
    // clock to the top of the remaining visible area.
    layer_set_hidden(neko_l, true);
    int t_y = 4;
    int d_y = t_y + TIME_H + 4;
    layer_set_frame(time_l, GRect(0, t_y, avail.size.w, TIME_H));
    layer_set_frame(date_l, GRect(0, d_y, avail.size.w, DATE_H));
  } else {
    layer_set_hidden(neko_l, false);
    // Dynamic layout — works for emery (200x228 rect), gabbro
    // (260x260 round), and flint (144x168 rect): neko sits above
    // centre, clock block below. Shift scales with screen height.
    int neko_x = (full.size.w - NEKO_WIDTH) / 2;
    int neko_y = (full.size.h - NEKO_HEIGHT) / 2 - full.size.h / 5;
    if (neko_y < 2) neko_y = 2;
    int t_y = neko_y + NEKO_HEIGHT + 6;
    int d_y = t_y + TIME_H + 4;
    layer_set_frame(neko_l, GRect(neko_x, neko_y, NEKO_WIDTH, NEKO_HEIGHT));
    layer_set_frame(time_l, GRect(0, t_y, full.size.w, TIME_H));
    layer_set_frame(date_l, GRect(0, d_y, full.size.w, DATE_H));
  }
}

// -------- Unobstructed area callbacks --------

static void prv_unobstructed_did_change(void *context) {
  layout_ui();
}

// -------- Window --------

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  window_set_background_color(window, GColorWhite);

  s_neko_layer = bitmap_layer_create(GRect(0, 0, NEKO_WIDTH, NEKO_HEIGHT));
  bitmap_layer_set_background_color(s_neko_layer, GColorClear);
  bitmap_layer_set_compositing_mode(s_neko_layer, GCompOpSet);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_neko_layer));

  s_time_layer = text_layer_create(GRect(0, 0, 10, TIME_H));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorBlack);
  text_layer_set_font(s_time_layer, fonts_get_system_font(TIME_FONT_KEY));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_date_layer = text_layer_create(GRect(0, 0, 10, DATE_H));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorBlack);
  text_layer_set_font(s_date_layer, fonts_get_system_font(DATE_FONT_KEY));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_date_layer));

  // Initial animation + clock state
  start_normal();
  update_bitmap();

  time_t now = time(NULL);
  update_time_text(localtime(&now));

  // Apply layout (covers both normal and already-obstructed cases)
  layout_ui();

  UnobstructedAreaHandlers handlers = {
    .did_change = prv_unobstructed_did_change,
  };
  unobstructed_area_service_subscribe(handlers, NULL);

  schedule_anim_timer();
}

static void window_unload(Window *window) {
  unobstructed_area_service_unsubscribe();
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
    s_anim_timer = NULL;
  }
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
    s_current_bitmap = NULL;
  }
  bitmap_layer_destroy(s_neko_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
}

// -------- init / deinit --------

static void init(void) {
  srand((unsigned int)time(NULL));

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
