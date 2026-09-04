#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <furi_hal_vibro.h>
#include <furi_hal_light.h>
#include <furi_hal_power.h>
#include <furi_hal_speaker.h>
#include <gui/elements.h>
#include <nrf24.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define MAX_NRF24 4
#define SETTINGS_DIR "/ext/apps_data/fz_nrf24_jammer"
#define SETTINGS_PATH SETTINGS_DIR "/settings.txt"

typedef enum {
    AppViewMenu,
    AppViewConfig,
    AppViewDialog,
    AppViewAbout,
} AppView;

struct AppModel {
    uint8_t dummy;
};

typedef enum {
    SPI_MODE_DEFAULT, 
    SPI_MODE_EXTRA, 
    SPI_MODE_COUNT
} SpiMode;

typedef enum {
    MODULES_MODE_TOGETHER,
    MODULES_MODE_SEPARATE,
    MODULES_MODE_COUNT
} ModulesMode;

typedef enum {
    SOUND_MODE_OFF,
    SOUND_MODE_ON,
    SOUND_MODE_COUNT
} SoundMode;

typedef enum {
    BT_METHOD_LIST,
    BT_METHOD_RANDOM,
    BT_METHOD_BRUTEFORCE,
    BT_METHOD_COUNT
} BtMethod;

typedef enum {
    DRONE_METHOD_BRUTEFORCE,
    DRONE_METHOD_RANDOM,
    DRONE_METHOD_COUNT
} DroneMethod;

typedef enum {
    WIFI_MODE_ALL,
    WIFI_MODE_SELECT,
    WIFI_MODE_COUNT
} WifiMode;

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* menu_view;
    VariableItemList* config_item_list;
    View* dialog_view;
    View* about_view;
    Gui* gui;

    uint8_t menu_index;
    uint8_t selected_jammer; 
    bool is_running;
    bool is_stop;
    uint8_t anim_frame;

    uint8_t len_modules;
    bool module_connected;

    SpiMode spi_mode;
    ModulesMode modules_mode;
    SoundMode sound_mode;

    BtMethod bt_method;
    DroneMethod drone_method;
    WifiMode wifi_mode;
    uint8_t wifi_channel;

    FuriTimer* anim_timer;
    FuriThread* jam_thread;
} AppState;

static AppState* global_app = NULL;
static nrf24_device_t nrf24_dev[MAX_NRF24];

static const char* spi_names[] = {"Default (4)", "Extra (7)"};
static const char* modules_names[] = {"Together", "Separate"};
static const char* sound_names[] = {"OFF", "ON"};


static void settings_save(AppState* app);

static void settings_save(AppState* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    storage_simply_mkdir(storage, SETTINGS_DIR);

    if(file_stream_open(stream, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "spi=%d\n", app->spi_mode);
        stream_write(stream, (uint8_t*)buf, strlen(buf));
        snprintf(buf, sizeof(buf), "modules=%d\n", app->modules_mode);
        stream_write(stream, (uint8_t*)buf, strlen(buf));
        snprintf(buf, sizeof(buf), "sound=%d\n", app->sound_mode);
        stream_write(stream, (uint8_t*)buf, strlen(buf));
        file_stream_close(stream);
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void settings_load(AppState* app) {
  
    app->spi_mode = SPI_MODE_DEFAULT;
    app->modules_mode = MODULES_MODE_TOGETHER;
    app->sound_mode = SOUND_MODE_OFF;


    app->bt_method = BT_METHOD_RANDOM;
    app->drone_method = DRONE_METHOD_RANDOM;
    app->wifi_mode = WIFI_MODE_ALL;
    app->wifi_channel = 1;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);

    if(file_stream_open(stream, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t file_size = stream_size(stream);
        if(file_size > 0 && file_size < 512) {
            uint8_t* raw = malloc(file_size + 1);
            memset(raw, 0, file_size + 1);
            stream_read(stream, raw, file_size);

            char* content = (char*)raw;
            char* line = strtok(content, "\n");
            while(line) {
                char* eq = strchr(line, '=');
                if(eq) {
                    int val = atoi(eq + 1);
                    if(strstr(line, "spi="))
                        app->spi_mode = (val < SPI_MODE_COUNT) ? val : SPI_MODE_DEFAULT;
                    else if(strstr(line, "modules="))
                        app->modules_mode = (val < MODULES_MODE_COUNT) ? val : MODULES_MODE_TOGETHER;
                    else if(strstr(line, "sound="))
                        app->sound_mode = (val < SOUND_MODE_COUNT) ? val : SOUND_MODE_OFF;
                }
                line = strtok(NULL, "\n");
            }
            free(raw);
        }
        file_stream_close(stream);
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}


static inline bool is_separate(AppState* app) {
    return app->modules_mode == MODULES_MODE_SEPARATE;
}

static void nrf24_start_carriers(uint8_t len) {
    for(uint8_t i = 0; i < len; i++) {
        nrf24_startConstCarrier(&nrf24_dev[i], 6, 45);
    }
}

static void nrf24_stop_carriers(uint8_t len) {
    for(uint8_t i = 0; i < len; i++) {
        nrf24_stopConstCarrier(&nrf24_dev[i]);
    }
}

static void ch_random_separate(uint8_t limit, uint8_t len) {
    for(uint8_t i = 0; i < len; i++) {
        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, furi_hal_random_get() % (limit + 1));
    }
}


static void ch_random_together(uint8_t limit, uint8_t len) {
    uint8_t ch = furi_hal_random_get() % (limit + 1);
    for(uint8_t i = 0; i < len; i++) {
        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
    }
}

static void jam_bluetooth(AppState* app) {
    nrf24_start_carriers(app->len_modules);

    while(!app->is_stop) {
        if(is_separate(app)) {
            ch_random_separate(80, app->len_modules);
        } else {
            ch_random_together(80, app->len_modules);
        }
    }

    nrf24_stop_carriers(app->len_modules);
}

static void jam_drone(AppState* app) {
    nrf24_start_carriers(app->len_modules);

    while(!app->is_stop) {
        if(is_separate(app)) {
            ch_random_separate(125, app->len_modules);
        } else {
            ch_random_together(125, app->len_modules);
        }
    }

    nrf24_stop_carriers(app->len_modules);
}

static void jam_wifi(AppState* app) {
    uint8_t mac[] = {0xFF, 0xFF};
    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};

    for(uint8_t i = 0; i < app->len_modules; i++) {
        nrf24_configure(&nrf24_dev[i], 2, mac, mac, 2, 1, true, true);
        nrf24_set_txpower(&nrf24_dev[i], 6);
        nrf24_set_tx_mode(&nrf24_dev[i]);
    }

    while(!app->is_stop) {
        for(uint8_t channel = 0; channel <= 13 && !app->is_stop; channel++) {
            for(uint8_t ch = (channel * 5) + 1; ch <= (channel * 5) + 23 && !app->is_stop; ch++) {
                if(is_separate(app)) {
                    uint8_t i = ch % app->len_modules;
                    nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                    nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                } else {
                    for(uint8_t i = 0; i < app->len_modules; i++) {
                        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                        nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                    }
                }
            }
        }
    }
}

static int32_t jam_thread_callback(void* ctx) {
    AppState* app = (AppState*)ctx;

    switch(app->selected_jammer) {
    case 0:
        jam_bluetooth(app);
        break;
    case 1:
        jam_wifi(app);
        break;
    case 2:
        jam_drone(app);
        break;
    default:
        break;
    }

    app->is_running = false;
    return 0;
}

static void play_vibration(void) {
    furi_hal_vibro_on(true);
    furi_delay_ms(100);
    furi_hal_vibro_on(false);
}

static void play_sound_beep(float freq, uint32_t duration_ms) {
    if(furi_hal_speaker_is_mine() || furi_hal_speaker_acquire(100)) {
        furi_hal_speaker_start(freq, 1.0f);
        furi_delay_ms(duration_ms);
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
}

static void play_sound_startup(void) {
    play_sound_beep(880.0f, 60);
    furi_delay_ms(30);
    play_sound_beep(1320.0f, 80);
}

static void play_sound_start(void) {
    play_sound_beep(1000.0f, 100);
}

static void play_sound_stop(void) {
    play_sound_beep(500.0f, 100);
}

static void play_sound_click(void) {
    play_sound_beep(1200.0f, 15);
}

static void app_feedback_startup(AppState* app) {
    if(!app) return;
    if(app->sound_mode == SOUND_MODE_OFF) {
        play_vibration();
    } else {
        play_sound_startup();
    }
}

static void app_feedback_start(AppState* app) {
    if(!app) return;
    if(app->sound_mode == SOUND_MODE_OFF) {
        play_vibration();
    } else {
        play_sound_start();
    }
}

static void app_feedback_stop(AppState* app) {
    if(!app) return;
    if(app->sound_mode == SOUND_MODE_OFF) {
        play_vibration();
    } else {
        play_sound_stop();
    }
}

static void app_feedback_click(AppState* app) {
    if(!app) return;
    if(app->sound_mode == SOUND_MODE_ON) {
        play_sound_click();
    }
}

static void led_on_green(void) {
    furi_hal_light_set(LightGreen, 255);
}

static void led_off(void) {
    furi_hal_light_set(LightGreen, 0);
}

static void anim_timer_callback(void* context) {
    AppState* app = (AppState*)context;
    if(!app || !app->is_running) return;
    app->anim_frame = (app->anim_frame + 1) % 3;

    if(app->dialog_view) {
        with_view_model(
            app->dialog_view, struct AppModel * model, { UNUSED(model); }, true);
    }
}

static void spi_change_cb(VariableItem* item) {
    AppState* app = global_app;
    if(!app) return;
    uint8_t idx = variable_item_get_current_value_index(item);
    app->spi_mode = idx;
    variable_item_set_current_value_text(item, spi_names[idx]);
    settings_save(app);
}

static void modules_change_cb(VariableItem* item) {
    AppState* app = global_app;
    if(!app) return;
    uint8_t idx = variable_item_get_current_value_index(item);
    app->modules_mode = idx;
    variable_item_set_current_value_text(item, modules_names[idx]);
    settings_save(app);
}

static void sound_change_cb(VariableItem* item) {
    AppState* app = global_app;
    if(!app) return;
    uint8_t idx = variable_item_get_current_value_index(item);
    app->sound_mode = idx;
    variable_item_set_current_value_text(item, sound_names[idx]);
    settings_save(app);
}

static uint32_t config_previous_callback(void* context) {
    UNUSED(context);
    return AppViewMenu;
}

static void about_draw_callback(Canvas* canvas, void* model) {
    UNUSED(model);
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "NRF24 Jammer");

    canvas_draw_line(canvas, 0, 22, 127, 22);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Credit: Xco7ontop");

    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignBottom, "[ Back ] Return");
}

static bool about_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    AppState* app = global_app;
    if(!event || !app) return false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyBack || event->key == InputKeyOk) {
            app_feedback_click(app);
            if(app->view_dispatcher) {
                view_dispatcher_switch_to_view(app->view_dispatcher, AppViewMenu);
            }
            return true;
        }
    }
    return false;
}

static void dialog_draw_callback(Canvas* canvas, void* model) {
    UNUSED(model);
    AppState* app = global_app;
    if(!app) return;

    canvas_clear(canvas);
   
    const char* title;
    if(app->selected_jammer == 0) {
        title = "Bluetooth Jammer";
    } else if(app->selected_jammer == 1) {
        title = "WiFi Jammer";
    } else {
        title = "Drone Jammer";
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, title);

    if(app->is_running) {
        char text[16];
        if(app->anim_frame == 0)
            strcpy(text, "Jamming...");
        else if(app->anim_frame == 1)
            strcpy(text, "Jamming..");
        else
            strcpy(text, "Jamming.");

        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, text);

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "[ OK ] Stop");
    } else {
        canvas_set_font(canvas, FontSecondary);
        if(app->module_connected) {
            char status[32];
            snprintf(
                status,
                sizeof(status),
                "Ready (%d module%s)",
                app->len_modules,
                app->len_modules > 1 ? "s" : "");
            canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, status);

            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "[ OK ] Start");
        } else {
            canvas_draw_str_aligned(
                canvas, 64, 30, AlignCenter, AlignCenter, "No module connected");
        }
    }
}

static bool dialog_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    AppState* app = global_app;
    if(!event || !app) return false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyOk) {
            if(!app->is_running) {
        
                if(app->module_connected) {
                    app->is_stop = false;
                    app->is_running = true;
                    app->anim_frame = 0;
                    app_feedback_start(app);
                    led_on_green();
                    if(app->anim_timer) {
                        furi_timer_start(app->anim_timer, 350);
                    }
                    furi_thread_start(app->jam_thread);
                }
            } else {
        
                app->is_stop = true;
                furi_thread_join(app->jam_thread);
                app->is_running = false;
                app_feedback_stop(app);
                led_off();
                if(app->anim_timer) {
                    furi_timer_stop(app->anim_timer);
                }
            }
            with_view_model(
                app->dialog_view, struct AppModel * model, { UNUSED(model); }, true);
            return true;
        }

        if(event->key == InputKeyBack) {
       
            if(app->is_running) {
                app->is_stop = true;
                furi_thread_join(app->jam_thread);
                app->is_running = false;
                app_feedback_stop(app);
            }
            led_off();
            if(app->anim_timer) {
                furi_timer_stop(app->anim_timer);
            }
            if(app->view_dispatcher) {
                view_dispatcher_switch_to_view(app->view_dispatcher, AppViewMenu);
            }
            return true;
        }
    }
    return false;
}

static void custom_menu_draw_callback(Canvas* canvas, void* model) {
    UNUSED(model);
    AppState* app = global_app;
    if(!app) return;

    canvas_clear(canvas);


    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 2, 10, AlignLeft, AlignBottom, "Nrf24 - Jammer");

    canvas_set_font(canvas, FontSecondary);
    if(app->module_connected) {
        char status[16];
        snprintf(status, sizeof(status), "Online (%d)", app->len_modules);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, status);
    } else {
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "No module");
    }

    canvas_draw_line(canvas, 0, 12, 127, 12);
    const char* items[5] = {"Bluetooth Jammer", "WiFi Jammer", "Drone Jammer", "Config", "About"};
    uint8_t total_items = 5;
    uint8_t visible_items = 4;
    uint8_t item_height = 12;
    uint8_t start_y = 14;

    uint8_t offset = 0;
    if(app->menu_index >= visible_items) {
        offset = app->menu_index - visible_items + 1;
    }

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < visible_items; i++) {
        uint8_t item_idx = i + offset;
        if(item_idx >= total_items) break;

        uint8_t y = start_y + i * item_height;

        if(app->menu_index == item_idx) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 122, item_height);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }

        canvas_draw_str_aligned(
            canvas,
            5,
            y + (item_height / 2) + 1,
            AlignLeft,
            AlignCenter,
            items[item_idx]);
    }

    canvas_set_color(canvas, ColorBlack);
    uint8_t scrollbar_x = 126;
    uint8_t scrollbar_y = start_y + 1;
    uint8_t scrollbar_h = 64 - scrollbar_y;

    for(uint8_t i = 0; i < scrollbar_h; i += 2) {
        canvas_draw_dot(canvas, scrollbar_x, scrollbar_y + i);
    }

    if(total_items > 0) {
        uint8_t indicator_h = (scrollbar_h / total_items);
        if(indicator_h < 3) indicator_h = 3;
        uint8_t indicator_y = scrollbar_y;
        if(total_items > 1) {
            indicator_y += ((scrollbar_h - indicator_h) * app->menu_index / (total_items - 1));
        }
        canvas_draw_box(canvas, scrollbar_x - 1, indicator_y, 3, indicator_h);
    }
}

static bool custom_menu_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    AppState* app = global_app;
    if(!event || !app) return false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            if(app->menu_index > 0)
                app->menu_index--;
            else
                app->menu_index = 4;
            app_feedback_click(app);
            with_view_model(
                app->menu_view, struct AppModel * model, { UNUSED(model); }, true);
            return true;
        }
        if(event->key == InputKeyDown) {
            if(app->menu_index < 4)
                app->menu_index++;
            else
                app->menu_index = 0;
            app_feedback_click(app);
            with_view_model(
                app->menu_view, struct AppModel * model, { UNUSED(model); }, true);
            return true;
        }
        if(event->key == InputKeyOk) {
            app_feedback_click(app);
            if(app->menu_index <= 2) {
        
                app->selected_jammer = app->menu_index;
                app->is_running = false;
                app->anim_frame = 0;
                led_off();
                if(app->anim_timer) {
                    furi_timer_stop(app->anim_timer);
                }
                with_view_model(
                    app->dialog_view, struct AppModel * model, { UNUSED(model); }, true);
                view_dispatcher_switch_to_view(app->view_dispatcher, AppViewDialog);
            } else if(app->menu_index == 3) {
         
                view_dispatcher_switch_to_view(app->view_dispatcher, AppViewConfig);
            } else {
             
                view_dispatcher_switch_to_view(app->view_dispatcher, AppViewAbout);
            }
            return true;
        }
        if(event->key == InputKeyBack) {
            if(app->view_dispatcher) {
                view_dispatcher_stop(app->view_dispatcher);
            }
            return true;
        }
    }
    return false;
}

static void nrf24_hw_init(AppState* app) {
    if(app->spi_mode == SPI_MODE_EXTRA) {
        nrf24_dev[0].spi_handle = (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_external;
        nrf24_dev[0].initialized = false;
        nrf24_dev[0].ce_pin = &gpio_ext_pb2;
        nrf24_dev[0].cs_pin = &gpio_ext_pc3;
        nrf24_init(&nrf24_dev[0]);
        for(uint8_t i = 1; i < MAX_NRF24; i++) {
            nrf24_dev[i].initialized = false;
        }
    } else {
        for(uint8_t i = 0; i < MAX_NRF24; i++) {
            nrf24_dev[i].spi_handle =
                (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_external;
            nrf24_dev[i].initialized = false;
            if(i == 0) {
                nrf24_dev[i].ce_pin = &gpio_ext_pb2;
                nrf24_dev[i].cs_pin = &gpio_ext_pa4;
            } else if(i == 1) {
                nrf24_dev[i].ce_pin = &gpio_swclk;
                nrf24_dev[i].cs_pin = &gpio_ext_pc3;
            } else if(i == 2) {
                nrf24_dev[i].ce_pin = &gpio_ext_pc1;
                nrf24_dev[i].cs_pin = &gpio_swdio;
            } else if(i == 3) {
                nrf24_dev[i].ce_pin = &gpio_ibutton;
                nrf24_dev[i].cs_pin = &gpio_ext_pc0;
            }
            nrf24_init(&nrf24_dev[i]);
        }
    }

    app->len_modules = 0;
    uint8_t max_check = (app->spi_mode == SPI_MODE_EXTRA) ? 1 : MAX_NRF24;
    for(uint8_t i = 0; i < max_check; i++) {
        if(nrf24_check_connected(&nrf24_dev[i])) {
            app->len_modules++;
        }
    }
    app->module_connected = (app->len_modules > 0);
}

int32_t nrf24_jammer_app(void* p) {
    UNUSED(p);

    AppState* app = malloc(sizeof(AppState));
    if(!app) return -1;
    memset(app, 0, sizeof(AppState));
    global_app = app;

    settings_load(app);
    app_feedback_startup(app);

    app->selected_jammer = 0;
    app->is_running = false;
    app->is_stop = true;
    app->anim_frame = 0;
    app->menu_index = 0;

    if(!furi_hal_power_is_otg_enabled()) {
        furi_hal_power_enable_otg();
    }

    furi_delay_ms(100);
    nrf24_hw_init(app);
    app->jam_thread = furi_thread_alloc_ex("nrf24_jam", 2048, jam_thread_callback, app);
    app->anim_timer = furi_timer_alloc(anim_timer_callback, FuriTimerTypePeriodic, app);

    app->view_dispatcher = view_dispatcher_alloc();
    if(!app->view_dispatcher) {
        furi_thread_free(app->jam_thread);
        furi_timer_free(app->anim_timer);
        for(uint8_t i = 0; i < MAX_NRF24; i++) nrf24_deinit(&nrf24_dev[i]);
        furi_hal_power_disable_otg();
        free(app);
        global_app = NULL;
        return -1;
    }

    app->menu_view = view_alloc();
    if(app->menu_view) {
        view_allocate_model(app->menu_view, ViewModelTypeLocking, sizeof(struct AppModel));
        view_set_draw_callback(app->menu_view, custom_menu_draw_callback);
        view_set_input_callback(app->menu_view, custom_menu_input_callback);
    }

    app->dialog_view = view_alloc();
    if(app->dialog_view) {
        view_allocate_model(app->dialog_view, ViewModelTypeLocking, sizeof(struct AppModel));
        view_set_draw_callback(app->dialog_view, dialog_draw_callback);
        view_set_input_callback(app->dialog_view, dialog_input_callback);
    }

    app->about_view = view_alloc();
    if(app->about_view) {
        view_allocate_model(app->about_view, ViewModelTypeLocking, sizeof(struct AppModel));
        view_set_draw_callback(app->about_view, about_draw_callback);
        view_set_input_callback(app->about_view, about_input_callback);
    }

    app->config_item_list = variable_item_list_alloc();
    View* config_view = variable_item_list_get_view(app->config_item_list);
    if(config_view) {
        view_set_previous_callback(config_view, config_previous_callback);
    }

    VariableItem* item;

    item = variable_item_list_add(
        app->config_item_list, "SPI Pin", SPI_MODE_COUNT, spi_change_cb, app);
    if(item) {
        variable_item_set_current_value_index(item, app->spi_mode);
        variable_item_set_current_value_text(item, spi_names[app->spi_mode]);
    }

    item = variable_item_list_add(
        app->config_item_list, "Modules", MODULES_MODE_COUNT, modules_change_cb, app);
    if(item) {
        variable_item_set_current_value_index(item, app->modules_mode);
        variable_item_set_current_value_text(item, modules_names[app->modules_mode]);
    }

    item = variable_item_list_add(
        app->config_item_list, "Sound", SOUND_MODE_COUNT, sound_change_cb, app);
    if(item) {
        variable_item_set_current_value_index(item, app->sound_mode);
        variable_item_set_current_value_text(item, sound_names[app->sound_mode]);
    }

    app->gui = furi_record_open(RECORD_GUI);

    if(app->view_dispatcher && app->gui) {
        view_dispatcher_attach_to_gui(
            app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
        view_dispatcher_add_view(app->view_dispatcher, AppViewMenu, app->menu_view);
        view_dispatcher_add_view(app->view_dispatcher, AppViewConfig, config_view);
        view_dispatcher_add_view(app->view_dispatcher, AppViewDialog, app->dialog_view);
        view_dispatcher_add_view(app->view_dispatcher, AppViewAbout, app->about_view);

        view_dispatcher_switch_to_view(app->view_dispatcher, AppViewMenu);
        view_dispatcher_run(app->view_dispatcher);
    }

    settings_save(app);

    if(app->is_running) {
        app->is_stop = true;
        furi_thread_join(app->jam_thread);
    }

    led_off();
    if(app->anim_timer) {
        furi_timer_stop(app->anim_timer);
        furi_timer_free(app->anim_timer);
    }

    if(app->view_dispatcher) {
        view_dispatcher_remove_view(app->view_dispatcher, AppViewMenu);
        view_dispatcher_remove_view(app->view_dispatcher, AppViewConfig);
        view_dispatcher_remove_view(app->view_dispatcher, AppViewDialog);
        view_dispatcher_remove_view(app->view_dispatcher, AppViewAbout);
        view_dispatcher_free(app->view_dispatcher);
    }
    if(app->menu_view) view_free(app->menu_view);
    if(app->dialog_view) view_free(app->dialog_view);
    if(app->about_view) view_free(app->about_view);
    if(app->config_item_list) variable_item_list_free(app->config_item_list);
    if(app->gui) furi_record_close(RECORD_GUI);

    furi_thread_free(app->jam_thread);

    for(uint8_t i = 0; i < MAX_NRF24; i++) {
        nrf24_deinit(&nrf24_dev[i]);
    }
    furi_hal_power_disable_otg();

    free(app);
    global_app = NULL;
    return 0;
}