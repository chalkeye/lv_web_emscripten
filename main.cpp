#include <lvgl.h>
#include <cmath>
#include <cstdlib>

// ============================================================================
// 1. SHARED UI BRIDGE
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif
    #include "ui/ui.h"
    #include "ui/ui_helpers.h"
#ifdef __cplusplus
}
#endif

// ============================================================================
// 2. PLATFORM-SPECIFIC HEADERS & PORTING
// ============================================================================
#ifdef ARDUINO
    #include <Arduino.h>
    #include <ESP_Panel_Library.h>
    #include <ESP_IOExpander_Library.h>

    #define TP_RST 1
    #define LCD_BL 2
    #define LCD_RST 3
    #define SD_CS 4
    #define USB_SEL 5
    #define I2C_MASTER_NUM 0
    #define I2C_MASTER_SDA_IO 8
    #define I2C_MASTER_SCL_IO 9
    #define BUTTON_ADC_PIN 6 

    #define LVGL_BUF_SIZE (800 * 50)
    #define LVGL_TASK_STACK_SIZE (10 * 1024)
    #define LVGL_TASK_PRIORITY (2)

    ESP_Panel *panel = NULL;
    SemaphoreHandle_t lvgl_mux = NULL;

    void lvgl_port_lock(int timeout_ms) {
        const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
    }
    void lvgl_port_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

#else
    #include <emscripten.h>
    #include <SDL2/SDL.h>
    
    extern "C" {
        #include "lv_drivers/display/monitor.h"
        #include "lv_drivers/indev/mouse.h"
        int monitor_hor_res = 800;
        int monitor_ver_res = 480;
    }

    uint32_t millis() { return lv_tick_get(); }
    void lvgl_port_lock(int timeout_ms) {}
    void lvgl_port_unlock(void) {}
#endif

// ============================================================================
// 3. SHARED GLOBALS
// ============================================================================
uint32_t last_activity_time = 0;
bool is_idle = false; 
bool is_booting = true; 
uint32_t boot_start_time = 0;

enum RoastState { ROAST_NONE, ROAST_HEATING, ROAST_COOLING };
RoastState current_roast_state = ROAST_NONE;
uint32_t roast_start_time = 0;

enum ScreenSaverMode { SS_ON, SS_OFF, SS_GRNSLD };
ScreenSaverMode current_ss_mode = SS_ON;

float ss_logo_x = 0, ss_logo_y = 0, ss_logo_dx = 0, ss_logo_dy = 0;
const float SS_SPEED = 1.5; 

enum MenuTier {
    TIER_ROOT, TIER_BREW, TIER_INTENSITY, TIER_PREFS, TIER_UNITS,
    TIER_LANGUAGE, TIER_DISPLAY, TIER_ROASTING, TIER_CONNECTIVITY, TIER_WIFI, TIER_SCREENSAVER
};
MenuTier current_menu_tier = TIER_ROOT;
char current_brew[32] = "Cold Brew";
char current_intensity[16] = "6.5";
uint16_t last_root_idx = 0;
uint16_t last_prefs_idx = 0;

lv_obj_t* p_panel_prev; lv_obj_t* p_panel_active; lv_obj_t* p_panel_next;
lv_obj_t* p_roller_prev; lv_obj_t* p_roller_active; lv_obj_t* p_roller_next;

enum PhysicalButton { BTN_NONE, BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK };

// ============================================================================
// 4. SHARED UI HELPER FUNCTIONS
// ============================================================================
bool is_leaf_tier(MenuTier tier) {
    switch (tier) {
        case TIER_BREW: case TIER_INTENSITY: case TIER_UNITS: 
        case TIER_LANGUAGE: case TIER_ROASTING: case TIER_WIFI: 
        case TIER_SCREENSAVER: return true; 
        default: return false;
    }
}

void set_roller_to_string(lv_obj_t* roller, const char* target) {
    const char* options = lv_roller_get_options(roller);
    int index = 0; const char* p = options; char buffer[64];
    while (p && *p) {
        const char* next = strchr(p, '\n');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len < sizeof(buffer)) {
            strncpy(buffer, p, len); buffer[len] = '\0';
            if (len > 0 && buffer[len-1] == '\r') buffer[len-1] = '\0';
            if (strcmp(buffer, target) == 0) { lv_roller_set_selected(roller, index, LV_ANIM_OFF); return; }
        }
        index++; if (!next) break; p = next + 1;
    }
}

void load_menu_tier_to_roller(MenuTier tier, lv_obj_t* target_roller) {
    switch (tier) {
    case TIER_ROOT: lv_roller_set_options(target_roller, "Brew Method\nIntensity\nPreferences", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, last_root_idx, LV_ANIM_OFF); break;
    case TIER_BREW: lv_roller_set_options(target_roller, "Filter\nEspresso\nMy Profiles\nCold Brew\nCupping", LV_ROLLER_MODE_NORMAL); set_roller_to_string(target_roller, current_brew); break;
    case TIER_INTENSITY: lv_roller_set_options(target_roller, "1.0\n1.5\n2.0\n2.5\n3.0\n3.5\n4.0\n4.5\n5.0\n5.5\n6.0\n6.5\n7.0\n7.5\n8.0\n8.5\n9.0\n9.5\n10.0", LV_ROLLER_MODE_NORMAL); set_roller_to_string(target_roller, current_intensity); break;
    case TIER_PREFS: lv_roller_set_options(target_roller, "Units\nLanguage\nDisplay\nRoasting\nConnectivity", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, last_prefs_idx, LV_ANIM_OFF); break;
    case TIER_UNITS: lv_roller_set_options(target_roller, "Celsius\nFahrenheit", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_LANGUAGE: lv_roller_set_options(target_roller, "English\nChinese\nJapanese\nKorean\nSpanish", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_DISPLAY: lv_roller_set_options(target_roller, "Temperature\nGraph progress\nBrightness\nScreen Saver", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_ROASTING: lv_roller_set_options(target_roller, "Load size\nFirst crack\nDynamic weightloss", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_CONNECTIVITY: lv_roller_set_options(target_roller, "Bluetooth\nConnect devices\nWifi settings", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_WIFI: lv_roller_set_options(target_roller, "Current connection\nNetwork list\nForget network\nManually connect", LV_ROLLER_MODE_NORMAL); lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF); break;
    case TIER_SCREENSAVER: 
        lv_roller_set_options(target_roller, "On\nOff\nGRNSLD", LV_ROLLER_MODE_NORMAL); 
        if (current_ss_mode == SS_ON) lv_roller_set_selected(target_roller, 0, LV_ANIM_OFF);
        else if (current_ss_mode == SS_OFF) lv_roller_set_selected(target_roller, 1, LV_ANIM_OFF);
        else lv_roller_set_selected(target_roller, 2, LV_ANIM_OFF);
        break;
    }
}

void update_next_preview(int override_idx = -1) {
    if (current_menu_tier == TIER_ROOT) {
        uint16_t idx = (override_idx != -1) ? (uint16_t)override_idx : lv_roller_get_selected(p_roller_active);
        if (idx == 0) lv_roller_set_options(p_roller_next, current_brew, LV_ROLLER_MODE_NORMAL);
        else if (idx == 1) lv_roller_set_options(p_roller_next, current_intensity, LV_ROLLER_MODE_NORMAL);
        else lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL); 
    }
    else { lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL); }
}

void apply_menu_formatting() {
    lv_obj_set_style_bg_color(p_panel_active, lv_color_hex(0x000000), 0);
    lv_label_set_text(ui_BrewStyleVariable, current_brew);
    lv_label_set_text(ui_IntensityVariable1, current_intensity);
}

// ============================================================================
// 5. MISSING MENU TRANSITION FUNCTIONS (The "Muscles")
// ============================================================================
void reset_menu_state() {
    current_menu_tier = TIER_ROOT;
    p_panel_prev = ui_MenuPrev; p_panel_active = ui_MenuActive; p_panel_next = ui_MenuNext;
    p_roller_prev = ui_MenuPrevRoller; p_roller_active = ui_MenuActiveRoller; p_roller_next = ui_MenuNextRoller;
    
    lv_obj_set_x(p_panel_prev, -400); 
    lv_obj_set_x(p_panel_active, 0); 
    lv_obj_set_x(p_panel_next, 400);
    
    apply_menu_formatting();
    load_menu_tier_to_roller(TIER_ROOT, p_roller_active);
    update_next_preview();
}

void slide_menu_forward(MenuTier target_tier) {
    load_menu_tier_to_roller(target_tier, p_roller_next);
    
    lv_anim_t a_out; lv_anim_init(&a_out); lv_anim_set_var(&a_out, p_panel_active); lv_anim_set_values(&a_out, 0, -400); 
    lv_anim_set_time(&a_out, 500); lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_start(&a_out);
    
    lv_anim_t a_in; lv_anim_init(&a_in); lv_anim_set_var(&a_in, p_panel_next); lv_anim_set_values(&a_in, 400, 0); 
    lv_anim_set_time(&a_in, 500); lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_start(&a_in);
    
    current_menu_tier = target_tier;
    lv_obj_t* t_p = p_panel_prev; p_panel_prev = p_panel_active; p_panel_active = p_panel_next; p_panel_next = t_p;
    lv_obj_t* t_r = p_roller_prev; p_roller_prev = p_roller_active; p_roller_active = p_roller_next; p_roller_next = t_r;
    lv_obj_set_x(p_panel_next, 400); update_next_preview(); apply_menu_formatting();
}

void slide_menu_backward(MenuTier target_tier) {
    load_menu_tier_to_roller(target_tier, p_roller_prev);
    
    lv_anim_t a_out; lv_anim_init(&a_out); lv_anim_set_var(&a_out, p_panel_active); lv_anim_set_values(&a_out, 0, 400); 
    lv_anim_set_time(&a_out, 500); lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_start(&a_out);
    
    lv_anim_t a_in; lv_anim_init(&a_in); lv_anim_set_var(&a_in, p_panel_prev); lv_anim_set_values(&a_in, -400, 0); 
    lv_anim_set_time(&a_in, 500); lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_start(&a_in);
    
    current_menu_tier = target_tier;
    lv_obj_t* t_p = p_panel_next; p_panel_next = p_panel_active; p_panel_active = p_panel_prev; p_panel_prev = t_p;
    lv_obj_t* t_r = p_roller_next; p_roller_next = p_roller_active; p_roller_active = p_roller_prev; p_roller_prev = t_r;
    lv_obj_set_x(p_panel_prev, -400); update_next_preview(); apply_menu_formatting();
}

void expand_menu_in_place(MenuTier target_tier) { current_menu_tier = target_tier; load_menu_tier_to_roller(target_tier, p_roller_next); }
void collapse_menu_in_place(MenuTier parent_tier) { current_menu_tier = parent_tier; update_next_preview(); }

// ============================================================================
// 6. BUTTON DETECTION & LOGIC
// ============================================================================
PhysicalButton get_physical_button_press() {
    static uint32_t last_debounce_time = 0;
    static PhysicalButton last_steady_state = BTN_NONE;
    PhysicalButton current_reading = BTN_NONE;

#ifdef ARDUINO
    int adc_value = analogRead(BUTTON_ADC_PIN);
    if (adc_value > 3800) current_reading = BTN_ENTER;
    else if (adc_value > 1850 && adc_value < 2250) current_reading = BTN_BACK;
    else if (adc_value > 1200 && adc_value < 1550) current_reading = BTN_DOWN;
    else if (adc_value > 850 && adc_value < 1150) current_reading = BTN_UP;
#else
    const uint8_t *state = SDL_GetKeyboardState(NULL);
    if (state[SDL_SCANCODE_1])      current_reading = BTN_UP;
    else if (state[SDL_SCANCODE_2]) current_reading = BTN_DOWN;
    else if (state[SDL_SCANCODE_3]) current_reading = BTN_BACK;
    else if (state[SDL_SCANCODE_4]) current_reading = BTN_ENTER;
#endif

    if ((millis() - last_debounce_time) > 50) {
        if (current_reading != last_steady_state) {
            last_steady_state = current_reading;
            last_debounce_time = millis();
            return current_reading; 
        }
    }
    return BTN_NONE;
}

void process_button_logic(PhysicalButton btn) {
    if (btn == BTN_NONE) return;
    lv_obj_t* focused_roller = is_leaf_tier(current_menu_tier) ? p_roller_next : p_roller_active;

    if (btn == BTN_UP) {
        if (lv_scr_act() == ui_RoastMain) { lv_event_send(ui_Button1, LV_EVENT_CLICKED, NULL); reset_menu_state(); } 
        else if (lv_scr_act() == ui_Menu1) {
            uint16_t cur = lv_roller_get_selected(focused_roller);
            if (cur > 0) { 
                lv_roller_set_selected(focused_roller, cur - 1, LV_ANIM_ON); 
                if (!is_leaf_tier(current_menu_tier)) update_next_preview(cur - 1); 
            }
        }
    }
    else if (btn == BTN_DOWN) {
        if (lv_scr_act() == ui_Menu1) {
            uint16_t cur = lv_roller_get_selected(focused_roller); 
            if (cur < lv_roller_get_option_cnt(focused_roller) - 1) { 
                lv_roller_set_selected(focused_roller, cur + 1, LV_ANIM_ON); 
                if (!is_leaf_tier(current_menu_tier)) update_next_preview(cur + 1); 
            }
        }
    }
    else if (btn == BTN_BACK) {
        if (lv_scr_act() == ui_Roasting) { current_roast_state = ROAST_NONE; lv_scr_load(ui_RoastMain); }
        else if (lv_scr_act() == ui_Menu1) {
            if (current_menu_tier == TIER_ROOT) lv_scr_load(ui_RoastMain);
            else if (current_menu_tier == TIER_PREFS) slide_menu_backward(TIER_ROOT);
            else if (current_menu_tier == TIER_DISPLAY) slide_menu_backward(TIER_PREFS);
            else if (current_menu_tier == TIER_CONNECTIVITY) slide_menu_backward(TIER_PREFS);
            else if (is_leaf_tier(current_menu_tier)) {
                if (current_menu_tier == TIER_BREW || current_menu_tier == TIER_INTENSITY) collapse_menu_in_place(TIER_ROOT);
                else if (current_menu_tier == TIER_WIFI) collapse_menu_in_place(TIER_CONNECTIVITY);
                else if (current_menu_tier == TIER_SCREENSAVER) collapse_menu_in_place(TIER_DISPLAY);
                else collapse_menu_in_place(TIER_PREFS);
            }
        }
    }
    else if (btn == BTN_ENTER) {
        if (lv_scr_act() == ui_RoastMain) { 
            lv_scr_load(ui_Roasting); current_roast_state = ROAST_HEATING; roast_start_time = millis();
            lv_label_set_text(ui_RoasterStatus, "Roasting..."); 
        }
        else if (lv_scr_act() == ui_Menu1) {
            uint16_t idx = lv_roller_get_selected(focused_roller);
            if (current_menu_tier == TIER_ROOT) {
                last_root_idx = idx;
                if (idx == 0) expand_menu_in_place(TIER_BREW); 
                else if (idx == 1) expand_menu_in_place(TIER_INTENSITY); 
                else if (idx == 2) slide_menu_forward(TIER_PREFS);
            }
            else if (current_menu_tier == TIER_PREFS) {
                last_prefs_idx = idx;
                if (idx == 0) expand_menu_in_place(TIER_UNITS); 
                else if (idx == 1) expand_menu_in_place(TIER_LANGUAGE); 
                else if (idx == 2) slide_menu_forward(TIER_DISPLAY); 
                else if (idx == 3) expand_menu_in_place(TIER_ROASTING); 
                else if (idx == 4) slide_menu_forward(TIER_CONNECTIVITY);
            }
            else if (current_menu_tier == TIER_DISPLAY) {
                if (idx == 3) expand_menu_in_place(TIER_SCREENSAVER); 
                else slide_menu_backward(TIER_PREFS); 
            }
            else if (is_leaf_tier(current_menu_tier)) {
                char sel_str[32]; lv_roller_get_selected_str(focused_roller, sel_str, sizeof(sel_str));
                if (current_menu_tier == TIER_BREW) { 
                    strncpy(current_brew, sel_str, 31); 
                    lv_label_set_text(ui_BrewStyleVariable, current_brew); 
                    collapse_menu_in_place(TIER_ROOT); 
                }
                else if (current_menu_tier == TIER_INTENSITY) { 
                    strncpy(current_intensity, sel_str, 15); 
                    lv_label_set_text(ui_IntensityVariable1, current_intensity); 
                    collapse_menu_in_place(TIER_ROOT); 
                }
                else collapse_menu_in_place(TIER_PREFS);
            }
        }
    }
}

// ============================================================================
// 7. SYSTEM LOOP & ENTRY POINTS
// ============================================================================
void run_system_loop() {
    uint32_t current_time = millis();
    if (is_booting) {
        if (current_time - boot_start_time > 5000) {
            is_booting = false;
            lvgl_port_lock(-1);
            lv_scr_load(ui_RoastMain);
            lvgl_port_unlock();
        }
    } 
    else {
        PhysicalButton active_btn = get_physical_button_press();
        if (active_btn != BTN_NONE) {
            lvgl_port_lock(-1);
            process_button_logic(active_btn);
            lvgl_port_unlock();
        }
    }
}

#ifdef ARDUINO
    void lvgl_port_task(void *arg) {
        while (1) {
            lvgl_port_lock(-1);
            lv_timer_handler();
            lvgl_port_unlock();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    void setup() {
        panel = new ESP_Panel();
        lv_init();
        static lv_disp_draw_buf_t draw_buf;
        uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
        lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);
        // ... Display Setup ...
        lvgl_mux = xSemaphoreCreateRecursiveMutex();
        xTaskCreate(lvgl_port_task, "lvgl", 10240, NULL, 2, NULL);
        lvgl_port_lock(-1);
        ui_init(); reset_menu_state(); lv_scr_load(ui_SplashScreen);
        is_booting = true; boot_start_time = millis();
        lvgl_port_unlock();
    }
    void loop() { run_system_loop(); delay(10); }
#else
    void emscripten_loop_wrapper() { run_system_loop(); lv_timer_handler(); }
    int main(void) {
        lv_init(); monitor_init();
        static lv_disp_draw_buf_t disp_buf; static lv_color_t buf[800 * 100];
        lv_disp_draw_buf_init(&disp_buf, buf, NULL, 800 * 100);
        static lv_disp_drv_t disp_drv; lv_disp_drv_init(&disp_drv);
        disp_drv.draw_buf = &disp_buf; disp_drv.flush_cb = monitor_flush;
        disp_drv.hor_res = 800; disp_drv.ver_res = 480; lv_disp_drv_register(&disp_drv);
        mouse_init();
        ui_init(); reset_menu_state(); lv_scr_load(ui_SplashScreen);
        is_booting = true; boot_start_time = millis();
        emscripten_set_main_loop(emscripten_loop_wrapper, 0, 1);
        return 0;
    }
#endif
