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

// Active animations (walk, scratch, play) — used for short bursts of
// activity between idle rests. Scratching (jare) is part of this pool.
static const NekoAnim s_active_anims[] = {
  { { RESOURCE_ID_IMAGE_LEFT1,  RESOURCE_ID_IMAGE_LEFT2  }, 2 },
  { { RESOURCE_ID_IMAGE_RIGHT1, RESOURCE_ID_IMAGE_RIGHT2 }, 2 },
  { { RESOURCE_ID_IMAGE_KAKI1,  RESOURCE_ID_IMAGE_KAKI2  }, 2 },  //low scratch
  { { RESOURCE_ID_IMAGE_LTOGI1, RESOURCE_ID_IMAGE_LTOGI2 }, 2 },
  { { RESOURCE_ID_IMAGE_RTOGI1, RESOURCE_ID_IMAGE_RTOGI2 }, 2 },
  { { RESOURCE_ID_IMAGE_UTOGI1, RESOURCE_ID_IMAGE_UTOGI2 }, 2 },
  { { RESOURCE_ID_IMAGE_DTOGI1, RESOURCE_ID_IMAGE_DTOGI2 }, 2 },
  { { RESOURCE_ID_IMAGE_IDLE,   RESOURCE_ID_IMAGE_JARE2  }, 2 },  //high scratch
};
#define ACTIVE_ANIM_COUNT (sizeof(s_active_anims) / sizeof(s_active_anims[0]))

// Special-purpose animations.
static const NekoAnim s_anim_idle  = { { RESOURCE_ID_IMAGE_IDLE,  RESOURCE_ID_IMAGE_IDLE  }, 1 };
static const NekoAnim s_anim_mati  = { { RESOURCE_ID_IMAGE_MATI3, RESOURCE_ID_IMAGE_MATI3 }, 1 }; //yawn
static const NekoAnim s_anim_sleep = { { RESOURCE_ID_IMAGE_SLEEP1, RESOURCE_ID_IMAGE_SLEEP2 }, 2 };
static const NekoAnim s_anim_awake = { { RESOURCE_ID_IMAGE_AWAKE,  RESOURCE_ID_IMAGE_AWAKE  }, 1 };

// -------- State --------

typedef enum {
  STATE_IDLE,         // resting in idle pose (the default)
  STATE_ACTIVE,       // brief walk/scratch/play animation (incl. jare)
  STATE_YAWNING,      // mati3 (yawn), leads to sleep
  STATE_SLEEPING,     // sleep1 ↔ sleep2, until tap
  STATE_WAKING,       // awake frame, back to idle
} NekoState;

static Window       *s_window;
static BitmapLayer  *s_neko_layer;
static GBitmap      *s_current_bitmap = NULL;
static TextLayer    *s_time_layer;
static TextLayer    *s_date_layer;
static AppTimer     *s_anim_timer = NULL;

static NekoState       s_state = STATE_IDLE;
static const NekoAnim *s_current_anim = &s_anim_idle;
static uint8_t         s_frame_idx = 0;
static uint16_t        s_ticks_in_anim = 0;
static uint16_t        s_anim_duration_ticks = 12;

static char s_time_text[8];
static char s_date_text[24];

// -------- Tunables --------

#define ANIM_PERIOD_MS        150   // frame period for active animations
#define SLEEP_PERIOD_MS       800   // slower cycle for zzz
#define NEKO_WIDTH             64
#define NEKO_HEIGHT            64

// Idle: the cat rests in idle pose for a long stretch.
#define IDLE_MIN_TICKS         20    // ~12 s
#define IDLE_MAX_TICKS         40    // ~24 s
// When leaving idle, YAWN_CHANCE_PCT % of the time it yawns and falls
// asleep; otherwise it picks a short active anim.
#define YAWN_CHANCE_PCT        10
// Active burst length (walk/togi/kaki).
#define ACTIVE_MIN_TICKS       12
#define ACTIVE_MAX_TICKS       20
// Yawning (mati3) duration before falling asleep.
#define YAWN_DURATION_TICKS    10    // ~3 s of yawning
// Awake pose after tap.
#define WAKE_DURATION_TICKS     4    // ~1.2 s

// Per-platform clock sizing. Big screens get the oversized ROBOTO numerics
// (digits + colon only — time format uses %I so no space glyph is needed);
// flint (144x168) drops to BITHAM_42_BOLD. Under TQV the date is hidden
// and the neko + time are re-centred — on big screens the full-size time
// still fits, so the "compact" slot aliases back to the main font.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  #define TIME_FONT_KEY         FONT_KEY_ROBOTO_BOLD_SUBSET_49
  #define DATE_FONT_KEY         FONT_KEY_GOTHIC_28_BOLD
  #define TIME_COMPACT_FONT_KEY TIME_FONT_KEY
  #define TIME_H 60
  #define DATE_H 32
  #define TIME_COMPACT_H TIME_H
#else
  #define TIME_FONT_KEY         FONT_KEY_BITHAM_42_BOLD
  #define DATE_FONT_KEY         FONT_KEY_GOTHIC_24_BOLD
  #define TIME_COMPACT_FONT_KEY FONT_KEY_GOTHIC_28_BOLD
  #define TIME_H 46
  #define DATE_H 26
  #define TIME_COMPACT_H 32
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

static void enter_state(NekoState state, const NekoAnim *anim) {
  s_state = state;
  s_current_anim = anim;
  s_frame_idx = 0;
  s_ticks_in_anim = 0;
  s_anim_duration_ticks = 0;
}

static void start_idle(void) {
  enter_state(STATE_IDLE, &s_anim_idle);
  s_anim_duration_ticks = rand_range(IDLE_MIN_TICKS, IDLE_MAX_TICKS);
}

static void start_active(void) {
  enter_state(STATE_ACTIVE, &s_active_anims[rand() % ACTIVE_ANIM_COUNT]);
  s_anim_duration_ticks = rand_range(ACTIVE_MIN_TICKS, ACTIVE_MAX_TICKS);
}

// -------- Animation tick --------

static void anim_timer_cb(void *data) {
  s_anim_timer = NULL;
  s_ticks_in_anim++;
  s_frame_idx++;

  switch (s_state) {
    case STATE_IDLE:
      // Resting in idle pose. After a long pause, either do a short active
      // burst or yawn and fall asleep.
      if (s_ticks_in_anim >= s_anim_duration_ticks) {
        if ((int)(rand() % 100) < YAWN_CHANCE_PCT) {
          enter_state(STATE_YAWNING, &s_anim_mati);
        } else {
          start_active();
        }
      }
      break;
    case STATE_ACTIVE:
      // Short walk/scratch/play, then back to idle.
      if (s_ticks_in_anim >= s_anim_duration_ticks) {
        start_idle();
      }
      break;
    case STATE_YAWNING:
      // Yawn (mati3), then fall asleep.
      if (s_ticks_in_anim >= YAWN_DURATION_TICKS) {
        enter_state(STATE_SLEEPING, &s_anim_sleep);
      }
      break;
    case STATE_SLEEPING:
      // Stays asleep until a tap.
      break;
    case STATE_WAKING:
      if (s_ticks_in_anim >= WAKE_DURATION_TICKS) {
        start_idle();
      }
      break;
  }

  update_bitmap();
  schedule_anim_timer();
}

static void schedule_anim_timer(void) {
  if (s_anim_timer) return;
  int period = (s_state == STATE_SLEEPING) ? SLEEP_PERIOD_MS : ANIM_PERIOD_MS;
  s_anim_timer = app_timer_register(period, anim_timer_cb, NULL);
}

// -------- Tap (wake from sleep) --------

static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_state == STATE_SLEEPING || s_state == STATE_YAWNING) {
    enter_state(STATE_WAKING, &s_anim_awake);
    update_bitmap();
  }
}

// -------- Clock --------

static void update_time_text(struct tm *t) {
  // %I (zero-padded) rather than %l, so the glyph set stays digits + ':'
  // — the ROBOTO_BOLD_SUBSET_49 font used on big screens has no space glyph.
  if (clock_is_24h_style()) {
    strftime(s_time_text, sizeof(s_time_text), "%H:%M", t);
  } else {
    strftime(s_time_text, sizeof(s_time_text), "%I:%M", t);
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
    // Timeline Quick View is peeking in. Keep the neko on screen, hide
    // the date, and render the time with a compact font so the pair
    // fits inside the unobstructed area.
    layer_set_hidden(neko_l, false);
    layer_set_hidden(date_l, true);
    text_layer_set_font(s_time_layer, fonts_get_system_font(TIME_COMPACT_FONT_KEY));
    const int gap = 2;
    int block_h = NEKO_HEIGHT + gap + TIME_COMPACT_H;
    int top = (avail.size.h - block_h) / 2;
    if (top < 0) top = 0;
    int neko_x = (full.size.w - NEKO_WIDTH) / 2;
    int t_y = top + NEKO_HEIGHT + gap;
    layer_set_frame(neko_l, GRect(neko_x, top, NEKO_WIDTH, NEKO_HEIGHT));
    layer_set_frame(time_l, GRect(0, t_y, full.size.w, TIME_COMPACT_H));
  } else {
    layer_set_hidden(neko_l, false);
    layer_set_hidden(date_l, false);
    text_layer_set_font(s_time_layer, fonts_get_system_font(TIME_FONT_KEY));
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
  start_idle();
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
