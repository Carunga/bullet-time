#include <pebble.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Longest text we send over AppMessage. Kept small so a 2-key message stays
// under the legacy AppMessage outbox ceiling.
#define MAX_SEND_TEXT 200

// List selection highlight, matching the standard notifications/Messages app.
#define HIGHLIGHT_COLOR PBL_IF_COLOR_ELSE(GColorFolly, GColorBlack)

// Globals

static DictationSession *dictationSession;

static Window *homeWindow;
static MenuLayer *homeLayer;

static Window *roomsWindow;
static MenuLayer *roomsLayer;

static Window *loadingWindow;
static TextLayer *loadingTextLayer;

static Window *viewWindow;
static ScrollLayer *viewScrollLayer;
static TextLayer *viewBodyTextLayer;

static Layer *topBarLayer;
static TextLayer *topBarTextLayer;

static AppTimer *progressTimer;

char rooms[100][32];
int roomsCounter = 0;
int room_times[100];

char favourites[20][32];
int favouritesCounter = 0;

#define CONVERSATION_MAX 6144
static char conversation[CONVERSATION_MAX];
static int conversation_len = 0;

static GRect conversation_bounds;
static bool loading_older = false;
static bool no_more_messages = false;

static char pending_room[32];
static bool pending_room_active = false;

int progress = 0;

static bool loadingShown = false;
static bool roomsHasMore = false;
static bool roomsLoaded = false;
static bool hasCache = false;
static bool freshAnimated = false;
static int pending_favourite = -1;
static GRect rooms_anim_from;
static GRect rooms_anim_to;
static PropertyAnimation *rooms_anim;

static void update_loading_text(void);
static void request_more_rooms(void);
static void request_cached_rooms(void);
static void animate_rooms_in(void);
static void show_latest_messages(void);
static void send_favourite(int index, const char *text);
static void start_favourite_dictation(int index);
static bool outbox_begin(DictionaryIterator **iter);
static void append_conversation(const char *sender, int epoch_sec, const char *text);
static void send_message_to_room(const char *room, const char *text);
static void start_room_dictation(const char *room);
static void start_current_dictation(void);

static PreferredContentSize s_content_size;

// Match the system notification/MenuCell text sizes per the watch's Text Size
// setting (GOTHIC_36 isn't available in this SDK, so XL uses 28).
static GFont content_font(bool bold) {
  switch (s_content_size) {
    case PreferredContentSizeSmall:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
    case PreferredContentSizeLarge:
    case PreferredContentSizeExtraLarge:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_28);
    case PreferredContentSizeMedium:
    default:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_24);
  }
}


// Scroll Layer Handler

static void update_scroll_size() {
  GSize textSize = text_layer_get_content_size(viewBodyTextLayer);

  textSize.h += 20;

  scroll_layer_set_content_size(viewScrollLayer, textSize);
}




// Bar Layer Handlers

static void bar_update(Layer *layer, GContext *ctx) {

  GRect bounds = layer_get_bounds(layer);
  int width = bounds.size.w;

  int section = width / 10;
  int barWidth = section * progress;

  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_line(ctx, GPoint(0, 19), GPoint(barWidth, 19));

}

static void bar_time_update() {
  time_t temp = time(NULL);
  struct tm *tickTime = localtime(&temp);

  static char buffer[8];
  strftime(buffer, sizeof(buffer),clock_is_24h_style() ? "%H:%M" : "%I:%M", tickTime);

  text_layer_set_text(topBarTextLayer, buffer);
}

static void bar_load(Window *window) {

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Bar window loaded");

  Layer *windowLayer = window_get_root_layer(window);
  GRect windowBounds = layer_get_bounds(windowLayer);
  int width = windowBounds.size.w;

  GRect bounds = GRect(0, 0, width, 20);

  topBarLayer = layer_create(bounds);
  topBarTextLayer = text_layer_create(bounds);

  text_layer_set_text_alignment(topBarTextLayer, GTextAlignmentCenter);
  text_layer_set_font(topBarTextLayer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_color(topBarTextLayer, GColorBlack);
  text_layer_set_background_color(topBarTextLayer, GColorClear);
  text_layer_set_text(topBarTextLayer, "00:00");

  layer_set_update_proc(topBarLayer, bar_update);

  layer_add_child(windowLayer, topBarLayer);
  layer_add_child(topBarLayer, text_layer_get_layer(topBarTextLayer));

  layer_mark_dirty(topBarLayer);
  bar_time_update();

}

static void bar_unload() {
  if (progressTimer) {
    app_timer_cancel(progressTimer);
    progressTimer = NULL;
  }

  if (topBarLayer) {
    layer_destroy(topBarLayer);
    topBarLayer = NULL;
  }

  if (topBarTextLayer) {
    text_layer_destroy(topBarTextLayer);
    topBarTextLayer = NULL;
  }
}

GRect reserve_bar_space(Layer*windowLayer) {
  GRect windowBounds = layer_get_bounds(windowLayer);
  return GRect(0, 20, windowBounds.size.w, windowBounds.size.h - 20);
}




// Time Handlers

static void tick_handler(struct tm *tickTime, TimeUnits unitsChanged) {

  bar_time_update();

}

static void progress_timer_callback(void *context) {

  if (!topBarLayer) return;

  if (progress >= 10) {
    if (progressTimer) {
      app_timer_cancel(progressTimer);
      progressTimer = NULL;
    }
  } else {
    progress++;

    layer_mark_dirty(topBarLayer);

    progressTimer = app_timer_register(500, progress_timer_callback, NULL); 
  }
}




// Message Functions

static void get_room_messages(const char *room) {

  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE,  "ROOM_MESSAGES");
  dict_write_cstring(iter, MESSAGE_KEY_ROOM_NAME, room);

  app_message_outbox_send();

}

// The AppMessage outbox can end up stuck in OUT_WRITING (e.g. after a message
// that was too large), after which every begin returns APP_MSG_INVALID_STATE.
// Release it and retry so the app heals itself.
static bool outbox_begin(DictionaryIterator **iter) {
  AppMessageResult res = app_message_outbox_begin(iter);

  if (res == APP_MSG_INVALID_STATE) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Releasing stuck outbox");
    app_message_outbox_send();
    res = app_message_outbox_begin(iter);
  }

  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox begin failed: %d", (int) res);
    return false;
  }

  return true;
}

static void send_message(const char *text) {

  static char buffer[MAX_SEND_TEXT + 1];
  strncpy(buffer, text, MAX_SEND_TEXT);
  buffer[MAX_SEND_TEXT] = '\0';

  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "SEND_MESSAGE");
  dict_write_cstring(iter, MESSAGE_KEY_TEXT, buffer);

  app_message_outbox_send();

}

static void send_message_to_room(const char *room, const char *text) {

  static char buffer[MAX_SEND_TEXT + 1];
  strncpy(buffer, text, MAX_SEND_TEXT);
  buffer[MAX_SEND_TEXT] = '\0';

  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "SEND_MESSAGE");
  dict_write_cstring(iter, MESSAGE_KEY_ROOM_NAME, room);
  dict_write_cstring(iter, MESSAGE_KEY_TEXT, buffer);

  app_message_outbox_send();

}


// Conversation buffer

static void format_message_time(int epoch_sec, char *out, size_t outlen) {
  time_t when = (time_t) epoch_sec;
  struct tm lt = *localtime(&when);

  time_t now = time(NULL);
  struct tm nt = *localtime(&now);

  if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) {
    strftime(out, outlen, "%H:%M", &lt);
  } else {
    strftime(out, outlen, "%d/%m %H:%M", &lt);
  }
}

static void reset_conversation(void) {
  conversation[0] = '\0';
  conversation_len = 0;
  no_more_messages = false;
  loading_older = false;
}

static void append_conversation(const char *sender, int epoch_sec, const char *text) {
  char timebuf[24];
  format_message_time(epoch_sec, timebuf, sizeof(timebuf));

  int remaining = CONVERSATION_MAX - conversation_len - 1;
  if (remaining <= 0) return;

  int written = snprintf(conversation + conversation_len, remaining, "%s - %s\n%s\n\n",
                         sender, timebuf, text);
  if (written < 0) return;

  if (written >= remaining) {
    conversation_len = CONVERSATION_MAX - 1;
  } else {
    conversation_len += written;
  }
}




// Dictation functions

static void dictation_callback(
  DictationSession *session,
  DictationSessionStatus status,
  char *transcription,
  void *context) {

  if (status != DictationSessionStatusSuccess) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Dictation cancelled");
    pending_favourite = -1;
    pending_room_active = false;
    return;
  }

  if (pending_favourite >= 0) {
    send_favourite(pending_favourite, transcription);
    pending_favourite = -1;
    pending_room_active = false;
    return;
  }

  if (pending_room_active) {
    send_message_to_room(pending_room, transcription);
    pending_room_active = false;
    return;
  }

  // Continuous conversation: send to the open room and show it locally.
  append_conversation("You", (int) time(NULL), transcription);
  send_message(transcription);

  if (viewBodyTextLayer) {
    text_layer_set_text(viewBodyTextLayer, conversation);
    update_scroll_size();
  }
}




// Home Menu

static uint16_t home_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return 1 + favouritesCounter;
}

static void home_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Latest messages", NULL, NULL);
    return;
  }

  menu_cell_basic_draw(ctx, cell_layer, favourites[cell_index->row - 1], NULL, NULL);
}

static void home_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row == 0) {
    show_latest_messages();
    return;
  }

  start_favourite_dictation(cell_index->row - 1);
}

static void home_window_load(Window *window) {
  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  homeLayer = menu_layer_create(bounds);

  menu_layer_set_click_config_onto_window(homeLayer, window);

  menu_layer_set_callbacks(homeLayer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = home_get_num_rows_callback,
    .draw_row = home_draw_row_callback,
    .select_click = home_select_callback
  });

  menu_layer_set_normal_colors(homeLayer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(homeLayer, HIGHLIGHT_COLOR, gcolor_legible_over(HIGHLIGHT_COLOR));

  bar_load(window);

  layer_add_child(windowLayer, menu_layer_get_layer(homeLayer));
}

static void home_window_unload(Window *window) {
  menu_layer_destroy(homeLayer);
  homeLayer = NULL;
  bar_unload();
}


// Navigation / favourites

static void show_latest_messages(void) {
  window_stack_push(roomsWindow, true);

  if (!roomsLoaded) {
    loadingShown = true;
    window_stack_push(loadingWindow, false);
  }
}

static void start_favourite_dictation(int index) {
  if (!dictationSession) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "No dictation session");
    return;
  }

  pending_favourite = index;
  pending_room_active = false;

  dictation_session_start(dictationSession);
}

static void start_room_dictation(const char *room) {
  if (!dictationSession) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "No dictation session");
    return;
  }

  strncpy(pending_room, room, 31);
  pending_room[31] = '\0';
  pending_room_active = true;
  pending_favourite = -1;

  dictation_session_start(dictationSession);
}

static void start_current_dictation(void) {
  if (!dictationSession) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "No dictation session");
    return;
  }

  pending_favourite = -1;
  pending_room_active = false;

  dictation_session_start(dictationSession);
}

static void send_favourite(int index, const char *text) {
  static char buffer[MAX_SEND_TEXT + 1];
  strncpy(buffer, text, MAX_SEND_TEXT);
  buffer[MAX_SEND_TEXT] = '\0';

  // Encode the favourite index in the type so this stays a 2-key message,
  // the same size as the (working) room send.
  static char type[16];
  snprintf(type, sizeof(type), "SEND_FAV%d", index);

  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, type);
  dict_write_cstring(iter, MESSAGE_KEY_TEXT, buffer);

  app_message_outbox_send();
}


// Rooms Select Handlers

static void rooms_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= roomsCounter) {
    request_more_rooms();
    return;
  }

  reset_conversation();
  get_room_messages(rooms[cell_index->row]);
  window_stack_push(viewWindow, true);
}

static void rooms_select_long_click_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= roomsCounter) return;

  start_room_dictation(rooms[cell_index->row]);
}

static void rooms_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= roomsCounter) {
    menu_cell_basic_draw(ctx, cell_layer, "Load more", NULL, NULL);
    return;
  }

  int epoch = room_times[cell_index->row];
  if (epoch > 0) {
    static char timebuf[24];
    format_message_time(epoch, timebuf, sizeof(timebuf));
    menu_cell_basic_draw(ctx, cell_layer, rooms[cell_index->row], timebuf, NULL);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, rooms[cell_index->row], NULL, NULL);
  }
}

static uint16_t rooms_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return roomsCounter + (roomsHasMore ? 1 : 0);
}




// Inbox Handlers

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_TYPE);
  if (!type_tuple) return;

  const char *type = type_tuple->value->cstring;

  if (strcmp(type, "ROOMS") == 0) {

    if (roomsCounter >= 100) return;

    Tuple *room_tuple = dict_find(iterator, MESSAGE_KEY_ROOM_NAME);
    if (!room_tuple) return;

    Tuple *time_tuple = dict_find(iterator, MESSAGE_KEY_TIME);

    const char *room = room_tuple->value->cstring;

    strncpy(rooms[roomsCounter], room, 31);
    rooms[roomsCounter][31] = '\0';
    room_times[roomsCounter] = time_tuple ? time_tuple->value->int32 : 0;
    roomsCounter++;

    if (roomsLayer) {
      menu_layer_reload_data(roomsLayer);
    }

  } else if (strcmp(type, "ROOMS_DONE") == 0) {
    Tuple *more_tuple = dict_find(iterator, MESSAGE_KEY_HAS_MORE);
    roomsHasMore = more_tuple && more_tuple->value->int32 != 0;

    Tuple *cache_tuple = dict_find(iterator, MESSAGE_KEY_FROM_CACHE);
    bool fromCache = cache_tuple && cache_tuple->value->int32 != 0;

    roomsLoaded = true;

    if (loadingShown) {
      loadingShown = false;
      if (loadingWindow) {
        window_stack_remove(loadingWindow, true);
      }
    }

    if (!fromCache && !freshAnimated && roomsWindow &&
        window_stack_get_top_window() == roomsWindow) {
      freshAnimated = true;
      animate_rooms_in();
    }

    if (roomsLayer) {
      menu_layer_reload_data(roomsLayer);
    }
  } else if (strcmp(type, "CLEAR_FAVOURITES") == 0) {
    favouritesCounter = 0;
    memset(favourites, 0, sizeof(favourites));

    if (homeLayer) {
      menu_layer_reload_data(homeLayer);
    }
  } else if (strcmp(type, "FAVOURITE") == 0) {
    if (favouritesCounter >= 20) return;

    Tuple *favourite_tuple = dict_find(iterator, MESSAGE_KEY_ROOM_NAME);
    if (!favourite_tuple) return;

    const char *favourite = favourite_tuple->value->cstring;

    strncpy(favourites[favouritesCounter], favourite, 31);
    favourites[favouritesCounter][31] = '\0';
    favouritesCounter++;

    if (homeLayer) {
      menu_layer_reload_data(homeLayer);
    }
  } else if (strcmp(type, "FAVOURITES_DONE") == 0) {
    if (homeLayer) {
      menu_layer_reload_data(homeLayer);
    }
  } else if (strcmp(type, "CACHE_STATE") == 0) {
    Tuple *cache_tuple = dict_find(iterator, MESSAGE_KEY_HAS_CACHE);
    hasCache = cache_tuple && cache_tuple->value->int32 != 0;

    update_loading_text();
  } else if (strcmp(type, "MESSAGE") == 0) {
    Tuple *sender_tuple = dict_find(iterator, MESSAGE_KEY_SENDER);
    Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_TEXT);
    if (!sender_tuple || !text_tuple) return;

    Tuple *time_tuple = dict_find(iterator, MESSAGE_KEY_TIME);
    int epoch = time_tuple ? time_tuple->value->int32 : (int) time(NULL);

    append_conversation(sender_tuple->value->cstring, epoch, text_tuple->value->cstring);
    loading_older = false;

    if (viewBodyTextLayer) {
      text_layer_set_text(viewBodyTextLayer, conversation);
      update_scroll_size();
    }
  } else if (strcmp(type, "NO_MORE") == 0) {
    no_more_messages = true;
    loading_older = false;
  } else if (strcmp(type, "CLEAR_ROOMS") == 0) {
    roomsCounter = 0;
    roomsHasMore = false;
    freshAnimated = false;
    memset(rooms, 0, sizeof(rooms));
    memset(room_times, 0, sizeof(room_times));

    if (roomsLayer) {
      menu_layer_reload_data(roomsLayer);
    }
  } else if (strcmp(type, "NOT_CONF") == 0) {
    if (loadingTextLayer) {
      text_layer_set_text(loadingTextLayer, "Open Settings And Sign In With SSO");
    }
  }
  
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}




// Loading Window Helpers

static void update_loading_text(void) {
  if (!loadingTextLayer) return;

  if (hasCache) {
    text_layer_set_text(loadingTextLayer, "Loading...\nSelect For Cached Messages");
  } else {
    text_layer_set_text(loadingTextLayer, "Loading...");
  }
}

static void request_more_rooms(void) {
  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "LOAD_MORE");

  app_message_outbox_send();
}

static void request_cached_rooms(void) {
  DictionaryIterator *iter;
  if (!outbox_begin(&iter)) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "SHOW_CACHE");

  app_message_outbox_send();
}

static void loading_select_click(ClickRecognizerRef recognizer, void *context) {
  request_cached_rooms();
}

static void loading_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, loading_select_click);
}


// Rooms Window Helpers

static void animate_rooms_in(void) {
  if (!roomsLayer) return;

  Layer *layer = menu_layer_get_layer(roomsLayer);

  rooms_anim_to = reserve_bar_space(window_get_root_layer(roomsWindow));
  rooms_anim_from = rooms_anim_to;
  rooms_anim_from.origin.y = rooms_anim_to.origin.y + rooms_anim_to.size.h;

  rooms_anim = property_animation_create_layer_frame(layer, &rooms_anim_from, &rooms_anim_to);
  animation_set_duration((Animation *)rooms_anim, 300);
  animation_set_curve((Animation *)rooms_anim, AnimationCurveEaseOut);
  animation_schedule((Animation *)rooms_anim);
}


// Rooms Window Handlers

static void rooms_window_load(Window *window) {

  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  roomsLayer = menu_layer_create(bounds);

  menu_layer_set_click_config_onto_window(roomsLayer, window);

  menu_layer_set_callbacks(roomsLayer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = rooms_get_num_rows_callback,
    .draw_row = rooms_draw_row_callback,
    .select_click = rooms_select_callback,
    .select_long_click = rooms_select_long_click_callback
  });

  menu_layer_set_normal_colors(roomsLayer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(roomsLayer, HIGHLIGHT_COLOR, gcolor_legible_over(HIGHLIGHT_COLOR));

  bar_load(window);

  layer_add_child(windowLayer, menu_layer_get_layer(roomsLayer));

  if (roomsLoaded && !freshAnimated) {
    freshAnimated = true;
    animate_rooms_in();
  }
}

static void rooms_window_unload(Window *window) {
  if (rooms_anim) {
    animation_unschedule((Animation *)rooms_anim);
    rooms_anim = NULL;
  }

  menu_layer_destroy(roomsLayer);
  roomsLayer = NULL;
  bar_unload();
}


// Loading Window Handlers

static void loading_window_load(Window *window) {

  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  loadingTextLayer = text_layer_create(bounds);

  text_layer_set_background_color(loadingTextLayer, GColorWhite);
  text_layer_set_text_alignment(loadingTextLayer, GTextAlignmentCenter);
  text_layer_set_text_color(loadingTextLayer, GColorBlack);
  text_layer_set_font(loadingTextLayer, content_font(true));
  text_layer_set_overflow_mode(loadingTextLayer, GTextOverflowModeWordWrap);

  update_loading_text();

  window_set_click_config_provider(window, loading_click_config_provider);

  bar_load(window);

  layer_add_child(windowLayer, text_layer_get_layer(loadingTextLayer));

  progressTimer = app_timer_register(500, progress_timer_callback, NULL);

}

static void loading_window_unload(Window *window) {
  text_layer_destroy(loadingTextLayer);
  loadingTextLayer = NULL;
  bar_unload();
  progress = 10;
  if (progressTimer) {
    app_timer_cancel(progressTimer);
    progressTimer = NULL;
  }
}

// View Window (continuous conversation) Handlers

static void conversation_select_click(ClickRecognizerRef recognizer, void *context) {
  start_current_dictation();
}

static void conversation_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, conversation_select_click);
}

static void conversation_scroll_handler(ScrollLayer *scroll_layer, void *context) {
  if (no_more_messages || loading_older) return;

  GSize content = scroll_layer_get_content_size(scroll_layer);
  GPoint offset = scroll_layer_get_content_offset(scroll_layer);

  int max_offset = content.h - conversation_bounds.size.h;
  if (max_offset <= 0) return;

  if (offset.y >= max_offset - 4) {
    DictionaryIterator *iter;
    if (!outbox_begin(&iter)) return;

    loading_older = true;
    dict_write_cstring(iter, MESSAGE_KEY_TYPE, "LOAD_OLDER");
    app_message_outbox_send();
  }
}

static void view_window_load(Window *window) {

  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  conversation_bounds = bounds;

  viewScrollLayer = scroll_layer_create(bounds);

  scroll_layer_set_callbacks(viewScrollLayer, (ScrollLayerCallbacks) {
    .click_config_provider = conversation_click_config_provider,
    .content_offset_changed_handler = conversation_scroll_handler
  });

  scroll_layer_set_click_config_onto_window(viewScrollLayer, window);

  viewBodyTextLayer = text_layer_create(GRect(5, 20, bounds.size.w - 10, 20000));

  text_layer_set_background_color(viewBodyTextLayer, GColorWhite);
  text_layer_set_text_alignment(viewBodyTextLayer, GTextAlignmentLeft);
  text_layer_set_overflow_mode(viewBodyTextLayer, GTextOverflowModeWordWrap);
  text_layer_set_font(viewBodyTextLayer, content_font(false));
  text_layer_set_text(viewBodyTextLayer, conversation);

  bar_load(window);

  scroll_layer_add_child(viewScrollLayer, text_layer_get_layer(viewBodyTextLayer));
  layer_add_child(windowLayer, scroll_layer_get_layer(viewScrollLayer));

  update_scroll_size();

}

static void view_window_unload(Window *window) {
  scroll_layer_destroy(viewScrollLayer);
  viewScrollLayer = NULL;
  text_layer_destroy(viewBodyTextLayer);
  viewBodyTextLayer = NULL;
}




// Basic Handlers

static void init() {
  s_content_size = preferred_content_size();

  homeWindow = window_create();

  window_set_window_handlers(homeWindow, (WindowHandlers) {
    .load = home_window_load,
    .unload = home_window_unload
  });

  roomsWindow = window_create();

  window_set_window_handlers(roomsWindow, (WindowHandlers) {
    .load = rooms_window_load,
    .unload = rooms_window_unload
  });

  loadingWindow = window_create();

  window_set_window_handlers(loadingWindow, (WindowHandlers) {
    .load = loading_window_load,
    .unload = loading_window_unload
  });

  viewWindow = window_create();

  window_set_window_handlers(viewWindow, (WindowHandlers) {
    .load = view_window_load,
    .unload = view_window_unload
  });

  dictationSession = dictation_session_create(
      512,
      dictation_callback,
      NULL
  );

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  dictation_session_enable_confirmation(dictationSession, true);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  const int inbox_size = 1024;
  const int outbox_size = 1024;
  AppMessageResult open_result = app_message_open(inbox_size, outbox_size);
  APP_LOG(APP_LOG_LEVEL_INFO, "app_message_open: %d, max outbox: %d",
          (int) open_result, (int) app_message_outbox_size_maximum());

  window_stack_push(homeWindow, true);
}

static void deinit() {
  window_destroy(homeWindow);
  window_destroy(roomsWindow);
  window_destroy(loadingWindow);
  window_destroy(viewWindow);

  tick_timer_service_unsubscribe();

  if (dictationSession) {
    dictation_session_destroy(dictationSession);
    dictationSession = NULL;
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}