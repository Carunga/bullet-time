#include <pebble.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// Globals

static DictationSession *dictationSession;

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

int progress = 0;


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

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Loading Bar Window: %s");

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

  if (status == DictationSessionStatusSuccess) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Heard: %s", transcription);

    strncpy(messages[messagesCounter], transcription, 127);
    messages[messagesCounter][127] = '\0';

    strncpy(senders[messagesCounter], "You", 127);
    senders[messagesCounter][127] = '\0';

    messagesCounter++;

    send_message(transcription);

    menu_layer_reload_data(messagesLayer);
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Dictation failed");
    dictation_session_destroy(dictationSession);
    dictationSession = NULL;
  }
}

static void load_dictation_message() {

  messagesCounter = 1;

  strncpy(messages[0], "Speech to Text", 127);
  messages[0][127] = '\0';

  strncpy(senders[0], "Send Message", 127);
  senders[0][127] = '\0';

}




// Message Select Handlers

static void messages_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  char *text = messages[cell_index->row];

  APP_LOG(APP_LOG_LEVEL_INFO, "Clicking message : %s", text);

  if (strcmp(text, "Speech to Text") == 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "STARTING DICTATION");
    dictation_session_start(dictationSession);
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
  APP_LOG(APP_LOG_LEVEL_INFO, "Opening room: %s", rooms[cell_index->row]);

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
  menu_cell_basic_draw(ctx, cell_layer, rooms[cell_index->row], NULL, NULL);
}

static uint16_t rooms_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return roomsCounter;
}




// Inbox Handlers

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_TYPE);
  if (!type_tuple) return;

  const char *type = type_tuple->value->cstring;

  if (strcmp(type, "ROOMS") == 0) {

    if (roomsCounter >= 32) return;
    if (loadingWindow) {
      window_stack_remove(loadingWindow, true);
    }

    Tuple *room_tuple = dict_find(iterator, MESSAGE_KEY_ROOM_NAME);
    if (!room_tuple) return;

    const char *room = room_tuple->value->cstring;

    strncpy(rooms[roomsCounter], room, 31);
    rooms[roomsCounter][31] = '\0';
    roomsCounter++;

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Room ID: %s", room);

    if (roomsLayer) {
      menu_layer_reload_data(roomsLayer);
    }

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

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Message Received: %s", text);

    if (messagesLayer) {
      menu_layer_reload_data(messagesLayer);
    }
  } else if (strcmp(type, "CLEAR_ROOMS") == 0) {
    roomsCounter = 0;
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
  text_layer_set_font(loadingTextLayer, fonts_get_system_font(FONT_KEY_GOTHIC_28));
  text_layer_set_text(loadingTextLayer, "Loading...");
  text_layer_set_overflow_mode(loadingTextLayer, GTextOverflowModeWordWrap);

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
  text_layer_set_font(viewBodyTextLayer, fonts_get_system_font(FONT_KEY_GOTHIC_18));

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

  window_stack_push(roomsWindow, true);
  window_stack_push(loadingWindow, false);
}

static void deinit() {
  window_destroy(roomsWindow);
  window_destroy(messagesWindow);
  window_destroy(loadingWindow);

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