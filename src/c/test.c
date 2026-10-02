#include <pebble.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// Globals

static DictationSession *dictationSession;

static Window *homeWindow;
static MenuLayer *homeLayer;

static Window *roomsWindow;
static MenuLayer *roomsLayer;

static Window *messagesWindow;
static MenuLayer *messagesLayer;

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

char messages[12][356];
char senders[12][128];
int messagesCounter = 1;

char favourites[20][32];
int favouritesCounter = 0;

int progress = 0;

static bool loadingShown = false;
static bool roomsHasMore = false;
static bool roomsLoaded = false;
static bool hasCache = false;
static bool freshAnimated = false;
static int pending_favourite = -1;
static GRect rooms_anim_from;
static GRect rooms_anim_to;

static void update_loading_text(void);
static void request_more_rooms(void);
static void request_cached_rooms(void);
static void animate_rooms_in(void);
static void show_latest_messages(void);
static void send_favourite(int index, const char *text);
static void start_favourite_dictation(int index);

static PreferredContentSize s_content_size;

static GFont content_font(bool bold) {
  switch (s_content_size) {
    case PreferredContentSizeSmall:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_14);
    case PreferredContentSizeLarge:
    case PreferredContentSizeExtraLarge:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_24);
    case PreferredContentSizeMedium:
    default:
      return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
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
  AppMessageResult res = app_message_outbox_begin(&iter);
  if(res != APP_MSG_OK) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE,  "ROOM_MESSAGES");
  dict_write_cstring(iter, MESSAGE_KEY_ROOM_NAME, room);

  app_message_outbox_send();

}

static void send_message(const char *text) {

  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if(res != APP_MSG_OK) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "SEND_MESSAGE");
  dict_write_cstring(iter, MESSAGE_KEY_TEXT, text);

  app_message_outbox_send();

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
    return;
  }

  if (pending_favourite >= 0) {
    send_favourite(pending_favourite, transcription);
    pending_favourite = -1;
    return;
  }

  if (messagesCounter < 12) {
    strncpy(messages[messagesCounter], transcription, 127);
    messages[messagesCounter][127] = '\0';

    strncpy(senders[messagesCounter], "You", 127);
    senders[messagesCounter][127] = '\0';

    messagesCounter++;
  }

  send_message(transcription);

  menu_layer_reload_data(messagesLayer);
}

static void load_dictation_message() {

  messagesCounter = 1;

  strncpy(messages[0], "Speech to Text", 127);
  messages[0][127] = '\0';

  strncpy(senders[0], "Send Message", 127);
  senders[0][127] = '\0';

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

  bar_load(window);

  layer_add_child(windowLayer, menu_layer_get_layer(homeLayer));
}

static void home_window_unload(Window *window) {
  menu_layer_destroy(homeLayer);
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

  dictation_session_start(dictationSession);
}

static void send_favourite(int index, const char *text) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res != APP_MSG_OK) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "SEND_FAVOURITE");
  dict_write_int32(iter, MESSAGE_KEY_FAVOURITE_INDEX, index);
  dict_write_cstring(iter, MESSAGE_KEY_TEXT, text);

  app_message_outbox_send();
}


// Message Select Handlers

static void messages_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  char *text = messages[cell_index->row];

  if (strcmp(text, "Speech to Text") == 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "STARTING DICTATION");
    if (dictationSession) {
      pending_favourite = -1;
      dictation_session_start(dictationSession);
    }
  } else {
    window_stack_push(viewWindow, true);
    text_layer_set_text(viewBodyTextLayer, text);
    update_scroll_size();
  }

}

static void messages_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  menu_cell_basic_draw(ctx, cell_layer, senders[cell_index->row], messages[cell_index->row], NULL);
}

static uint16_t messages_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return messagesCounter;
}

// Rooms Select Handlers

static void rooms_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= roomsCounter) {
    request_more_rooms();
    return;
  }

  messagesCounter = 0;

  memset(messages, 0, sizeof(messages));
  memset(senders, 0, sizeof(senders));

  PBL_IF_MICROPHONE_ELSE(
    load_dictation_message(),
    APP_LOG(APP_LOG_LEVEL_ERROR, "No microphone available")
  );

  get_room_messages(rooms[cell_index->row]);

  window_stack_push(messagesWindow, true);
}

static void rooms_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row >= roomsCounter) {
    menu_cell_basic_draw(ctx, cell_layer, "Load more", NULL, NULL);
    return;
  }

  menu_cell_basic_draw(ctx, cell_layer, rooms[cell_index->row], NULL, NULL);
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

    const char *room = room_tuple->value->cstring;

    strncpy(rooms[roomsCounter], room, 31);
    rooms[roomsCounter][31] = '\0';
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
    if (messagesCounter >= 12) return;

    Tuple *sender_tuple = dict_find(iterator, MESSAGE_KEY_SENDER);
    if (!sender_tuple) return;

    const char *sender = sender_tuple->value->cstring;

    Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_TEXT);
    if (!text_tuple) return;

    const char *text = text_tuple->value->cstring;

    strncpy(messages[messagesCounter], text, 355);
    messages[messagesCounter][355] = '\0';

    strncpy(senders[messagesCounter], sender, 127);
    senders[messagesCounter][127] = '\0';
    messagesCounter++;

    if (messagesLayer) {
      menu_layer_reload_data(messagesLayer);
    }
  } else if (strcmp(type, "CLEAR_ROOMS") == 0) {
    roomsCounter = 0;
    roomsHasMore = false;
    freshAnimated = false;
    memset(rooms, 0, sizeof(rooms));

    if (roomsLayer) {
      menu_layer_reload_data(roomsLayer);
    }
  } else if (strcmp(type, "NOT_CONF") == 0) {
    text_layer_set_text(loadingTextLayer, "Open Settings And Sign In With SSO");
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
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res != APP_MSG_OK) return;

  dict_write_cstring(iter, MESSAGE_KEY_TYPE, "LOAD_MORE");

  app_message_outbox_send();
}

static void request_cached_rooms(void) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res != APP_MSG_OK) return;

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

  PropertyAnimation *animation =
      property_animation_create_layer_frame(layer, &rooms_anim_from, &rooms_anim_to);
  animation_set_duration((Animation *)animation, 300);
  animation_set_curve((Animation *)animation, AnimationCurveEaseOut);
  animation_schedule((Animation *)animation);
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
    .select_click = rooms_select_callback
  });

  bar_load(window);

  layer_add_child(windowLayer, menu_layer_get_layer(roomsLayer));

  if (roomsLoaded && !freshAnimated) {
    freshAnimated = true;
    animate_rooms_in();
  }
}

static void rooms_window_unload(Window *window) {
  menu_layer_destroy(roomsLayer);
  bar_unload();
}


// Messages Window Handlers

static void messages_window_load(Window *window) {

  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  messagesLayer = menu_layer_create(bounds);

  menu_layer_set_click_config_onto_window(messagesLayer, window);

  menu_layer_set_callbacks(messagesLayer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = messages_get_num_rows_callback,
    .draw_row = messages_draw_row_callback,
    .select_click = messages_select_callback
  });

  bar_load(window);

  layer_add_child(windowLayer, menu_layer_get_layer(messagesLayer));

}

static void messages_window_unload(Window *window) {
  menu_layer_destroy(messagesLayer);
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
  bar_unload();
  progress = 10;
  if (progressTimer) {
    app_timer_cancel(progressTimer);
    progressTimer = NULL;
  }
}

// View Window Handlers

static void view_window_load(Window *window) {

  Layer *windowLayer = window_get_root_layer(window);
  GRect bounds = reserve_bar_space(windowLayer);

  viewScrollLayer = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(viewScrollLayer, window);

  viewBodyTextLayer = text_layer_create(GRect(5, 20, bounds.size.w - 10, 20000));
  
  text_layer_set_background_color(viewBodyTextLayer, GColorWhite);
  text_layer_set_text_alignment(viewBodyTextLayer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(viewBodyTextLayer, GTextOverflowModeWordWrap);
  text_layer_set_font(viewBodyTextLayer, content_font(false));

  bar_load(window);

  scroll_layer_add_child(viewScrollLayer, text_layer_get_layer(viewBodyTextLayer));
  layer_add_child(windowLayer, scroll_layer_get_layer(viewScrollLayer));

  update_scroll_size();

}

static void view_window_unload(Window *window) {
  scroll_layer_destroy(viewScrollLayer);
  text_layer_destroy(viewBodyTextLayer);
}




// Basic Handlers

static void init() {
  s_content_size = preferred_content_size();

  homeWindow = window_create();

  window_set_window_handlers(homeWindow, (WindowHandlers) {
    .load = home_window_load,
    .unload = home_window_unload
  });

  messagesWindow = window_create();

  window_set_window_handlers(messagesWindow, (WindowHandlers) {
    .load = messages_window_load,
    .unload = messages_window_unload
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
      sizeof(messages[0]),
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
  const int outbox_size = 128;
  app_message_open(inbox_size, outbox_size);

  window_stack_push(homeWindow, true);
}

static void deinit() {
  window_destroy(homeWindow);
  window_destroy(roomsWindow);
  window_destroy(messagesWindow);
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