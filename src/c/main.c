#include <pebble.h>

// Persistent storage keys
#define PERSIST_CONFIG_COUNT_KEY 100
#define PERSIST_CONFIG_BASE_KEY 101
#define PERSIST_CONFIG_MAX_CHUNKS 16
#define PERSIST_CHUNK_SIZE 256
#define PERSIST_NAME_BASE_KEY 200

#define REQUEST_COUNT 3
#define REQUEST_BADGE_WIDTH 20
#define REQUEST_BADGE_INSET 5
#define IDLE_TIMEOUT_MS (3 * 60 * 1000)

static Window *s_window;
static AppTimer *s_idle_timer;
static Layer *s_chrome_layer;
static TextLayer *s_title_layer;
static TextLayer *s_request_layers[REQUEST_COUNT];
static TextLayer *s_status_layer;

static char s_request_names[REQUEST_COUNT][64];
static char s_status_text[64];

static void idle_timeout(void *context) {
  s_idle_timer = NULL;
  window_stack_remove(s_window, true);
}

static void restart_idle_timer(void) {
  if (s_idle_timer && app_timer_reschedule(s_idle_timer, IDLE_TIMEOUT_MS)) {
    return;
  }
  s_idle_timer = app_timer_register(IDLE_TIMEOUT_MS, idle_timeout, NULL);
}

static void show_status(GColor background) {
  window_set_background_color(s_window, PBL_IF_COLOR_ELSE(background, GColorBlack));
  text_layer_set_text(s_status_layer, s_status_text);
  layer_mark_dirty(s_chrome_layer);
}

static void save_config_to_persist(const char *json) {
  int len = strlen(json) + 1; // include null terminator
  int chunks = (len + PERSIST_CHUNK_SIZE - 1) / PERSIST_CHUNK_SIZE;
  if (chunks > PERSIST_CONFIG_MAX_CHUNKS) chunks = PERSIST_CONFIG_MAX_CHUNKS;
  persist_write_int(PERSIST_CONFIG_COUNT_KEY, chunks);
  for (int i = 0; i < chunks; i++) {
    int offset = i * PERSIST_CHUNK_SIZE;
    int chunk_len = len - offset;
    if (chunk_len > PERSIST_CHUNK_SIZE) chunk_len = PERSIST_CHUNK_SIZE;
    persist_write_data(PERSIST_CONFIG_BASE_KEY + i, json + offset, chunk_len);
  }
}

static bool load_config_from_persist(char *buf, int buf_size) {
  if (!persist_exists(PERSIST_CONFIG_COUNT_KEY)) return false;
  int chunks = persist_read_int(PERSIST_CONFIG_COUNT_KEY);
  if (chunks <= 0 || chunks > PERSIST_CONFIG_MAX_CHUNKS) return false;
  int total = 0;
  for (int i = 0; i < chunks; i++) {
    int remaining = buf_size - total - 1;
    if (remaining <= 0) break;
    int read = persist_read_data(PERSIST_CONFIG_BASE_KEY + i, buf + total, remaining);
    if (read < 0) break;
    total += read;
  }
  buf[total] = '\0';
  return total > 0;
}


static void send_button_press(int index) {
  restart_idle_timer();

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK) return;

  dict_write_int32(iter, MESSAGE_KEY_ButtonIndex, index);
  app_message_outbox_send();

  snprintf(s_status_text, sizeof(s_status_text), "Sending...");
  show_status(GColorOxfordBlue);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_button_press(0);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_button_press(1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_button_press(2);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static void display_request_name(int index) {
  TextLayer *layer = s_request_layers[index];
  const char *name = s_request_names[index];
  text_layer_set_text(layer, name[0] ? name : "Not set");
  text_layer_set_text_color(layer, name[0] ? GColorBlack
    : PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
}

static void update_request_name(int index, const char *name) {
  snprintf(s_request_names[index], sizeof(s_request_names[index]), "%s", name);
  display_request_name(index);
}

static void load_persisted_names(void) {
  for (int i = 0; i < REQUEST_COUNT; i++) {
    if (persist_exists(PERSIST_NAME_BASE_KEY + i)) {
      if (persist_read_string(PERSIST_NAME_BASE_KEY + i, s_request_names[i],
                             sizeof(s_request_names[i])) < 0) {
        s_request_names[i][0] = '\0';
      }
    }
  }
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  // Handle request names from phone — also persist them on the watch
  Tuple *name0 = dict_find(iter, MESSAGE_KEY_RequestName0);
  if (name0) {
    update_request_name(0, name0->value->cstring);
    persist_write_string(PERSIST_NAME_BASE_KEY + 0, name0->value->cstring);
  }

  Tuple *name1 = dict_find(iter, MESSAGE_KEY_RequestName1);
  if (name1) {
    update_request_name(1, name1->value->cstring);
    persist_write_string(PERSIST_NAME_BASE_KEY + 1, name1->value->cstring);
  }

  Tuple *name2 = dict_find(iter, MESSAGE_KEY_RequestName2);
  if (name2) {
    update_request_name(2, name2->value->cstring);
    persist_write_string(PERSIST_NAME_BASE_KEY + 2, name2->value->cstring);
  }

  // Handle config backup from phone
  Tuple *config_data = dict_find(iter, MESSAGE_KEY_ConfigData);
  if (config_data) {
    save_config_to_persist(config_data->value->cstring);
  }

  // Handle backup restore request from phone
  Tuple *request_backup = dict_find(iter, MESSAGE_KEY_RequestBackup);
  if (request_backup && request_backup->value->int32) {
    static char config_buf[PERSIST_CHUNK_SIZE * PERSIST_CONFIG_MAX_CHUNKS];
    if (load_config_from_persist(config_buf, sizeof(config_buf))) {
      DictionaryIterator *out;
      if (app_message_outbox_begin(&out) == APP_MSG_OK) {
        dict_write_cstring(out, MESSAGE_KEY_ConfigData, config_buf);
        app_message_outbox_send();
      }
    }
  }

  // Handle response status
  Tuple *success = dict_find(iter, MESSAGE_KEY_ResponseSuccess);
  Tuple *status = dict_find(iter, MESSAGE_KEY_ResponseStatus);
  if (success && status) {
    if (success->value->int32) {
      snprintf(s_status_text, sizeof(s_status_text), "OK (%d)", (int)status->value->int32);
      vibes_short_pulse();
      show_status(GColorDarkGreen);
    } else {
      snprintf(s_status_text, sizeof(s_status_text), "Failed (%d)", (int)status->value->int32);
      vibes_double_pulse();
      show_status(GColorDarkCandyAppleRed);
    }
  }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  snprintf(s_status_text, sizeof(s_status_text), "Msg dropped");
  show_status(GColorDarkCandyAppleRed);
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  snprintf(s_status_text, sizeof(s_status_text), "Send failed");
  show_status(GColorDarkCandyAppleRed);
}

static GRect request_frame(GRect bounds, int index) {
  bool tall = bounds.size.h > 180;
  int x = PBL_IF_ROUND_ELSE(21, tall ? 14 : 8);
  int y = PBL_IF_ROUND_ELSE(40, tall ? 44 : 34);
  int height = tall ? 40 : 30;
  int gap = tall ? 8 : 4;
  return GRect(x, y + index * (height + gap), bounds.size.w - 2 * x, height);
}

static GRect status_frame(GRect bounds) {
  int width = PBL_IF_ROUND_ELSE(98, 112);
  int bottom_inset = PBL_IF_ROUND_ELSE(35, 30);
  return GRect((bounds.size.w - width) / 2, bounds.size.h - bottom_inset, width, 22);
}

static void draw_arrow(GContext *ctx, GPoint center, GPoint direction) {
  int dx = direction.x;
  int dy = direction.y;
  GPoint tip = GPoint(center.x + 5 * dx, center.y + 5 * dy);
  GPoint tail = GPoint(center.x - 5 * dx, center.y - 5 * dy);
  // The arrowhead wings sit on either side of the shaft, four pixels back from the tip.
  GPoint left = GPoint(tip.x - 4 * dx + 4 * dy, tip.y - 4 * dy - 4 * dx);
  GPoint right = GPoint(tip.x - 4 * dx - 4 * dy, tip.y - 4 * dy + 4 * dx);

  graphics_draw_line(ctx, tail, tip);
  graphics_draw_line(ctx, tip, left);
  graphics_draw_line(ctx, tip, right);
}

static void draw_chrome(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const GPoint directions[REQUEST_COUNT] = {{0, -1}, {1, 0}, {0, 1}};
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 3);

  for (int i = 0; i < REQUEST_COUNT; i++) {
    GRect row = request_frame(bounds, i);
    GRect badge = GRect(row.origin.x + row.size.w - REQUEST_BADGE_WIDTH - REQUEST_BADGE_INSET,
                        row.origin.y + (row.size.h - 18) / 2, REQUEST_BADGE_WIDTH, 18);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, row, 6, GCornersAll);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, badge, 3, GCornersAll);
    draw_arrow(ctx, grect_center_point(&badge), directions[i]);
  }

  if (s_status_text[0]) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, status_frame(bounds), 11, GCornersAll);
  }
}

static TextLayer *create_text_layer(Layer *parent, GRect frame, GFont font,
                                    GColor color, GTextAlignment align) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, color);
  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, align);
  text_layer_set_overflow_mode(layer, GTextOverflowModeTrailingEllipsis);
  layer_add_child(parent, text_layer_get_layer(layer));
  return layer;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  bool tall = bounds.size.h > 180;
  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont request_font = fonts_get_system_font(tall ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD);
  GFont status_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  s_chrome_layer = layer_create(bounds);
  layer_set_update_proc(s_chrome_layer, draw_chrome);
  layer_add_child(root, s_chrome_layer);

  int title_y = PBL_IF_ROUND_ELSE(11, tall ? 12 : 5);
  s_title_layer = create_text_layer(root, GRect(0, title_y, bounds.size.w, 23),
                                    title_font, GColorWhite, GTextAlignmentCenter);
  text_layer_set_text(s_title_layer, "fetch");

  int name_height = tall ? 33 : 28;
  for (int i = 0; i < REQUEST_COUNT; i++) {
    GRect row = request_frame(bounds, i);
    GRect label = GRect(row.origin.x + 8, row.origin.y + (row.size.h - name_height) / 2,
                        row.size.w - 8 - REQUEST_BADGE_WIDTH - 2 * REQUEST_BADGE_INSET, name_height);
    s_request_layers[i] = create_text_layer(root, label, request_font, GColorBlack, GTextAlignmentLeft);
    display_request_name(i);
  }

  s_status_layer = create_text_layer(root, status_frame(bounds), status_font, GColorBlack, GTextAlignmentCenter);
  text_layer_set_text(s_status_layer, s_status_text);
  restart_idle_timer();
}

static void window_unload(Window *window) {
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }

  text_layer_destroy(s_title_layer);
  for (int i = 0; i < REQUEST_COUNT; i++) {
    text_layer_destroy(s_request_layers[i]);
  }
  text_layer_destroy(s_status_layer);
  layer_destroy(s_chrome_layer);
}

static void init(void) {
  // Register AppMessage handlers
  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(4096, 4096);

  // Load persisted request names so they show immediately
  load_persisted_names();

  // Create window
  s_window = window_create();
  window_set_background_color(s_window, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack));
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
