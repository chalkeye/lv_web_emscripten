#ifdef ARDUINO
    // ============================================================================
    // PHYSICAL HARDWARE INCLUDES & DEFINES (ESP32-S3)
    // ============================================================================
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

    #define LVGL_TICK_PERIOD_MS     (2)
    #define LVGL_TASK_MAX_DELAY_MS  (5)
    #define LVGL_TASK_MIN_DELAY_MS  (1)
    #define LVGL_TASK_STACK_SIZE    (10 * 1024)
    #define LVGL_TASK_PRIORITY      (2)
    #define LVGL_BUF_SIZE           (ESP_PANEL_LCD_H_RES * 50)

    ESP_Panel *panel = NULL;
    SemaphoreHandle_t lvgl_mux = NULL;                  
#else
    // ============================================================================
    // WEB SIMULATOR INCLUDES & DEFINES (Emscripten)
    // ============================================================================
    #include <emscripten.h>
    #include "lv_drivers/display/monitor.h"
    #include "lv_drivers/indev/mouse.h"
    #include "lv_drivers/indev/keyboard.h"

    // Mock the Arduino millis() function using LVGL's internal tick counter
    uint32_t millis() { return lv_tick_get(); }
#endif

#include <lvgl.h>
#include <cmath>
#include "ui/ui.h"

// ============================================================================
// HARDWARE-SPECIFIC LVGL FLUSH FUNCTIONS
// ============================================================================
#ifdef ARDUINO
    #if ESP_PANEL_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB
    void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
        panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
        lv_disp_flush_ready(disp);
    }
    #else
    void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
        panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    }
    bool notify_lvgl_flush_ready(void *user_ctx) {
        lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
        lv_disp_flush_ready(disp_driver);
        return false;
    }
    #endif 

    #if ESP_PANEL_USE_LCD_TOUCH
    void lvgl_port_tp_read(lv_indev_drv_t * indev, lv_indev_data_t * data) {
        panel->getLcdTouch()->readData();
        bool touched = panel->getLcdTouch()->getTouchState();
        if(!touched) {
            data->state = LV_INDEV_STATE_REL;
        } else {
            TouchPoint point = panel->getLcdTouch()->getPoint();
            data->state = LV_INDEV_STATE_PR;
            data->point.x = point.x;
            data->point.y = point.y;
        }
    }
    #endif

    void lvgl_port_lock(int timeout_ms) {
        const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
    }

    void lvgl_port_unlock(void) {
        xSemaphoreGiveRecursive(lvgl_mux);
    }

    void lvgl_port_task(void *arg) {
        uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        while (1) {
            lvgl_port_lock(-1);
            task_delay_ms = lv_timer_handler();
            lvgl_port_unlock();
            if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
            else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
            vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
        }
    }
#else
    // Mock the thread locks for the web wrapper so the main logic stays clean
    void lvgl_port_lock(int timeout_ms) {}
    void lvgl_port_unlock(void) {}
#endif

// ============================================================================
// KAFFELOGIC UI LOGIC & STATE MACHINE (100% SHARED)
// ============================================================================
const uint32_t ANIM_SCROLL_SPEED = 500;  
const uint32_t ANIM_SLIDE_SPEED = 600;   

uint32_t last_activity_time = 0;
bool is_idle = false; 
bool is_booting = true; 
uint32_t boot_start_time = 0;

enum RoastState { ROAST_NONE, ROAST_HEATING, ROAST_COOLING };
RoastState current_roast_state = ROAST_NONE;
uint32_t roast_start_time = 0;

enum ScreenSaverMode { SS_ON, SS_OFF, SS_GRNSLD };
ScreenSaverMode current_ss_mode = SS_ON;

float ss_logo_x = 0;
float ss_logo_y = 0;
float ss_logo_dx = 0;
float ss_logo_dy = 0;
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

bool is_leaf_tier(MenuTier tier) {
    switch (tier) {
        case TIER_BREW: case TIER_INTENSITY: case TIER_UNITS: 
        case TIER_LANGUAGE: case TIER_ROASTING: case TIER_WIFI: 
        case TIER_SCREENSAVER: return true; 
        case TIER_ROOT: case TIER_PREFS: case TIER_CONNECTIVITY: 
        case TIER_DISPLAY: return false;
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
    lv_roller_set_selected(roller, 0, LV_ANIM_OFF);
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
        uint16_t idx = (override_idx != -1) ? override_idx : lv_roller_get_selected(p_roller_active);
        if (idx == 0) lv_roller_set_options(p_roller_next, current_brew, LV_ROLLER_MODE_NORMAL);
        else if (idx == 1) lv_roller_set_options(p_roller_next, current_intensity, LV_ROLLER_MODE_NORMAL);
        else if (idx == 2) lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL); 
    }
    else if (current_menu_tier == TIER_PREFS) {
        uint16_t idx = (override_idx != -1) ? override_idx : lv_roller_get_selected(p_roller_active);
        if (idx == 0) lv_roller_set_options(p_roller_next, "Celsius", LV_ROLLER_MODE_NORMAL); 
        else if (idx == 1) lv_roller_set_options(p_roller_next, "English", LV_ROLLER_MODE_NORMAL);
        else lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL);
    }
    else if (current_menu_tier == TIER_DISPLAY) {
        uint16_t idx = (override_idx != -1) ? override_idx : lv_roller_get_selected(p_roller_active);
        if (idx == 3) {
            if (current_ss_mode == SS_ON) lv_roller_set_options(p_roller_next, "On", LV_ROLLER_MODE_NORMAL);
            else if (current_ss_mode == SS_OFF) lv_roller_set_options(p_roller_next, "Off", LV_ROLLER_MODE_NORMAL);
            else lv_roller_set_options(p_roller_next, "GRNSLD", LV_ROLLER_MODE_NORMAL);
        } else {
            lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL);
        }
    }
    else { lv_roller_set_options(p_roller_next, " ", LV_ROLLER_MODE_NORMAL); }
}

void apply_menu_formatting() {
    lv_obj_set_style_bg_color(p_panel_prev, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p_panel_prev, 255, 0);
    lv_obj_set_style_bg_color(p_panel_active, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p_panel_active, 255, 0);
    lv_obj_set_style_bg_color(p_panel_next, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p_panel_next, 255, 0);
   
    lv_obj_set_width(p_panel_prev, 400); lv_obj_set_width(p_panel_active, 400); lv_obj_set_width(p_panel_next, 400);
    lv_obj_set_width(p_roller_prev, 400); lv_obj_set_width(p_roller_active, 400); lv_obj_set_width(p_roller_next, 400);

    lv_obj_set_style_text_align(p_roller_active, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_style_text_align(p_roller_active, LV_TEXT_ALIGN_RIGHT, LV_PART_SELECTED);
    lv_obj_set_style_pad_right(p_roller_active, 20, 0); lv_obj_set_style_pad_right(p_roller_active, 20, LV_PART_SELECTED);

    lv_obj_set_style_text_align(p_roller_next, LV_TEXT_ALIGN_LEFT, 0); lv_obj_set_style_text_align(p_roller_next, LV_TEXT_ALIGN_LEFT, LV_PART_SELECTED);
    lv_obj_set_style_pad_left(p_roller_next, 20, 0); lv_obj_set_style_pad_left(p_roller_next, 20, LV_PART_SELECTED);

    lv_obj_set_style_text_align(p_roller_prev, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_style_text_align(p_roller_prev, LV_TEXT_ALIGN_RIGHT, LV_PART_SELECTED);
    lv_obj_set_style_pad_right(p_roller_prev, 20, 0); lv_obj_set_style_pad_right(p_roller_prev, 20, LV_PART_SELECTED);

    lv_obj_set_style_anim_time(p_roller_prev, ANIM_SCROLL_SPEED, 0); 
    lv_obj_set_style_anim_time(p_roller_active, ANIM_SCROLL_SPEED, 0); 
    lv_obj_set_style_anim_time(p_roller_next, ANIM_SCROLL_SPEED, 0);
    
    lv_label_set_text(ui_BrewStyleVariable, current_brew);
    lv_label_set_text(ui_IntensityVariable1, current_intensity);
}

void reset_menu_state() {
    current_menu_tier = TIER_ROOT;
    last_root_idx = 0;
    
    p_panel_prev = ui_MenuPrev; p_panel_active = ui_MenuActive; p_panel_next = ui_MenuNext;
    p_roller_prev = ui_MenuPrevRoller; p_roller_active = ui_MenuActiveRoller; p_roller_next = ui_MenuNextRoller;
    
    lv_obj_set_x(p_panel_prev, -400); lv_obj_set_y(p_panel_prev, 0);
    lv_obj_set_x(p_panel_active, 0);  lv_obj_set_y(p_panel_active, 0);
    lv_obj_set_x(p_panel_next, 400);  lv_obj_set_y(p_panel_next, 0);
    
    apply_menu_formatting();
    load_menu_tier_to_roller(TIER_ROOT, p_roller_active);
    update_next_preview();
}

void slide_menu_forward(MenuTier target_tier) {
    load_menu_tier_to_roller(target_tier, p_roller_next);
    lv_anim_t a_out; lv_anim_init(&a_out); lv_anim_set_var(&a_out, p_panel_active); lv_anim_set_values(&a_out, 0, -400); 
    lv_anim_set_time(&a_out, ANIM_SLIDE_SPEED); lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_set_path_cb(&a_out, lv_anim_path_ease_out); lv_anim_start(&a_out);
    lv_anim_t a_in; lv_anim_init(&a_in); lv_anim_set_var(&a_in, p_panel_next); lv_anim_set_values(&a_in, 400, 0); 
    lv_anim_set_time(&a_in, ANIM_SLIDE_SPEED); lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out); lv_anim_start(&a_in);
    
    current_menu_tier = target_tier;
    lv_obj_t* temp_panel = p_panel_prev; p_panel_prev = p_panel_active; p_panel_active = p_panel_next; p_panel_next = temp_panel;
    lv_obj_t* temp_roller = p_roller_prev; p_roller_prev = p_roller_active; p_roller_active = p_roller_next; p_roller_next = temp_roller;
    lv_obj_set_x(p_panel_next, 400); update_next_preview(); apply_menu_formatting();
}

void slide_menu_backward(MenuTier target_tier) {
    load_menu_tier_to_roller(target_tier, p_roller_prev);
    lv_anim_t a_out; lv_anim_init(&a_out); lv_anim_set_var(&a_out, p_panel_active); lv_anim_set_values(&a_out, 0, 400); 
    lv_anim_set_time(&a_out, ANIM_SLIDE_SPEED); lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_set_path_cb(&a_out, lv_anim_path_ease_out); lv_anim_start(&a_out);
    lv_anim_t a_in; lv_anim_init(&a_in); lv_anim_set_var(&a_in, p_panel_prev); lv_anim_set_values(&a_in, -400, 0); 
    lv_anim_set_time(&a_in, ANIM_SLIDE_SPEED); lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x); lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out); lv_anim_start(&a_in);
    
    current_menu_tier = target_tier;
    lv_obj_t* temp_panel = p_panel_next; p_panel_next = p_panel_active; p_panel_active = p_panel_prev; p_panel_prev = temp_panel;
    lv_obj_t* temp_roller = p_roller_next; p_roller_next = p_roller_active; p_roller_active = p_roller_prev; p_roller_prev = temp_roller;
    lv_obj_set_x(p_panel_prev, -400); update_next_preview(); apply_menu_formatting();
}

void expand_menu_in_place(MenuTier target_tier) { current_menu_tier = target_tier; load_menu_tier_to_roller(target_tier, p_roller_next); }
void collapse_menu_in_place(MenuTier parent_tier) { current_menu_tier = parent_tier; update_next_preview(); }

enum PhysicalButton { BTN_NONE, BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK };

PhysicalButton get_physical_button_press() {
    static uint32_t last_debounce_time = 0;
    static PhysicalButton last_steady_state = BTN_NONE;
    static PhysicalButton last_reading = BTN_NONE;
    PhysicalButton current_reading = BTN_NONE;

#ifdef ARDUINO
    // ESP32 Hardware ADC reading
    int adc_value = analogRead(BUTTON_ADC_PIN);
    if (adc_value > 3800) { current_reading = BTN_ENTER; } 
    else if (adc_value > 1850 && adc_value < 2250) { current_reading = BTN_BACK; } 
    else if (adc_value > 1200 && adc_value < 1550) { current_reading = BTN_DOWN; } 
    else if (adc_value > 850 && adc_value < 1150) { current_reading = BTN_UP; }
#endif

    if (current_reading != last_reading) { last_debounce_time = millis(); }
    if ((millis() - last_debounce_time) > 50) {
        if (current_reading != last_steady_state) {
            last_steady_state = current_reading;
            last_reading = current_reading;
            return current_reading; 
        }
    }
    last_reading = current_reading;
    return BTN_NONE;
}

void process_button_logic(PhysicalButton btn) {
    if (btn == BTN_NONE) return;
    lv_obj_t* focused_roller = is_leaf_tier(current_menu_tier) ? p_roller_next : p_roller_active;

    if (btn == BTN_UP) {
        if (lv_scr_act() == ui_RoastMain) { lv_event_send(ui_Button1, LV_EVENT_CLICKED, NULL); reset_menu_state(); } 
        else if (lv_scr_act() == ui_Menu1) {
            lv_event_send(ui_Button9, LV_EVENT_CLICKED, NULL); uint16_t current_sel = lv_roller_get_selected(focused_roller);
            if (current_sel > 0) { lv_roller_set_selected(focused_roller, current_sel - 1, LV_ANIM_ON); if (!is_leaf_tier(current_menu_tier)) update_next_preview(current_sel - 1); }
        }
    }
    else if (btn == BTN_DOWN) {
        if (lv_scr_act() == ui_Menu1) {
            lv_event_send(ui_Button5, LV_EVENT_CLICKED, NULL); uint16_t current_sel = lv_roller_get_selected(focused_roller); uint16_t total_options = lv_roller_get_option_cnt(focused_roller);
            if (current_sel < total_options - 1) { lv_roller_set_selected(focused_roller, current_sel + 1, LV_ANIM_ON); if (!is_leaf_tier(current_menu_tier)) update_next_preview(current_sel + 1); }
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
        if (lv_scr_act() == ui_Roasting) { current_roast_state = ROAST_NONE; lv_scr_load(ui_RoastMain); }
        else if (lv_scr_act() == ui_RoastMain) { 
            lv_scr_load(ui_Roasting); current_roast_state = ROAST_HEATING; roast_start_time = millis();
            lv_label_set_text(ui_RoasterStatus, "Roasting..."); lv_label_set_text(ui_TimeRemaining, "10:00"); lv_arc_set_value(ui_Arc1, 0); 
            lv_label_set_text(ui_BrewStyleVariable1, current_brew); lv_label_set_text(ui_IntensityVariable2, current_intensity);
        }
        else if (lv_scr_act() == ui_Menu1) {
            lv_event_send(ui_Button7, LV_EVENT_CLICKED, NULL); uint16_t idx = lv_roller_get_selected(focused_roller);
            if (current_menu_tier == TIER_ROOT) {
                last_root_idx = idx;
                if (idx == 0) expand_menu_in_place(TIER_BREW); else if (idx == 1) expand_menu_in_place(TIER_INTENSITY); else if (idx == 2) { last_prefs_idx = 0; slide_menu_forward(TIER_PREFS); }
            }
            else if (current_menu_tier == TIER_PREFS) {
                last_prefs_idx = idx;
                if (idx == 0) expand_menu_in_place(TIER_UNITS); else if (idx == 1) expand_menu_in_place(TIER_LANGUAGE); else if (idx == 2) slide_menu_forward(TIER_DISPLAY); else if (idx == 3) expand_menu_in_place(TIER_ROASTING); else if (idx == 4) slide_menu_forward(TIER_CONNECTIVITY);
            }
            else if (current_menu_tier == TIER_DISPLAY) {
                if (idx == 3) expand_menu_in_place(TIER_SCREENSAVER); else slide_menu_backward(TIER_PREFS); 
            }
            else if (current_menu_tier == TIER_CONNECTIVITY) { if (idx == 2) expand_menu_in_place(TIER_WIFI); }
            else if (is_leaf_tier(current_menu_tier)) {
                char sel_str[32]; lv_roller_get_selected_str(focused_roller, sel_str, sizeof(sel_str));
                if (current_menu_tier == TIER_BREW) { strncpy(current_brew, sel_str, sizeof(current_brew) - 1); current_brew[sizeof(current_brew) - 1] = '\0'; lv_label_set_text(ui_BrewStyleVariable, current_brew); collapse_menu_in_place(TIER_ROOT); }
                else if (current_menu_tier == TIER_INTENSITY) { strncpy(current_intensity, sel_str, sizeof(current_intensity) - 1); current_intensity[sizeof(current_intensity) - 1] = '\0'; lv_label_set_text(ui_IntensityVariable1, current_intensity); collapse_menu_in_place(TIER_ROOT); }
                else if (current_menu_tier == TIER_SCREENSAVER) {
                    if (strcmp(sel_str, "On") == 0) current_ss_mode = SS_ON;
                    else if (strcmp(sel_str, "Off") == 0) current_ss_mode = SS_OFF;
                    else current_ss_mode = SS_GRNSLD;
                    collapse_menu_in_place(TIER_DISPLAY);
                }
                else if (current_menu_tier == TIER_WIFI) { collapse_menu_in_place(TIER_CONNECTIVITY); }
                else { collapse_menu_in_place(TIER_PREFS); }
            }
        }
    }
}

// ============================================================================
// SYSTEM LOOP (Called by Arduino loop() or Emscripten Callback)
// ============================================================================
void run_system_loop() {
    uint32_t current_time = millis();

    // 1. Handle Boot Sequence
    if (is_booting) {
        if (current_time - boot_start_time > 5000) {
            is_booting = false;
            lvgl_port_lock(-1);
            lv_scr_load(ui_RoastMain);
            lvgl_port_unlock();
            last_activity_time = current_time; 
        }
    } 
    // 2. Handle Screensaver Transition
    else if (!is_idle && current_ss_mode != SS_OFF) {
        if (lv_scr_act() == ui_RoastMain) {
            if (current_time - last_activity_time > 30000) {
                is_idle = true;
                lvgl_port_lock(-1);
                lv_scr_load(ui_SplashScreen); 

                lv_obj_t* splash_logo = lv_obj_get_child(ui_SplashScreen, 0);
                if (current_ss_mode == SS_GRNSLD && splash_logo != NULL) {
                    lv_obj_set_align(splash_logo, LV_ALIGN_TOP_LEFT);
                    lv_coord_t sw = 800; // Hardcoded fallback for logic safety
                    lv_coord_t sh = 480;
                    lv_coord_t lw = lv_obj_get_width(splash_logo);
                    lv_coord_t lh = lv_obj_get_height(splash_logo);
                    if (lw == 0) lw = 150; if (lh == 0) lh = 150;

                    ss_logo_x = (sw - lw) / 2.0f;
                    ss_logo_y = (sh - lh) / 2.0f;
                    lv_obj_set_pos(splash_logo, (lv_coord_t)ss_logo_x, (lv_coord_t)ss_logo_y);

                    // Pick random start directions (Using standard rand() for cross-compatibility)
                    ss_logo_dx = ((rand() % 2) == 0 ? 1.0f : -1.0f) * SS_SPEED;
                    ss_logo_dy = ((rand() % 2) == 0 ? 1.0f : -1.0f) * SS_SPEED;
                } else if (splash_logo != NULL) {
                    lv_obj_set_align(splash_logo, LV_ALIGN_CENTER);
                    lv_obj_set_pos(splash_logo, 0, 0);
                }
                lvgl_port_unlock();
            }
        } else {
            last_activity_time = current_time; 
        }
    }

    // 3. Handle GRNSLD Animation Physics
    if (is_idle && current_ss_mode == SS_GRNSLD) {
        static uint32_t last_ss_frame = 0;
        if (current_time - last_ss_frame >= 20) {
            last_ss_frame = current_time;
            lvgl_port_lock(-1);
            lv_obj_t* splash_logo = lv_obj_get_child(ui_SplashScreen, 0);
            if (splash_logo != NULL) {
                lv_coord_t sw = 800;
                lv_coord_t sh = 480;
                lv_coord_t lw = lv_obj_get_width(splash_logo);
                lv_coord_t lh = lv_obj_get_height(splash_logo);
                if (lw == 0) lw = 150; if (lh == 0) lh = 150;

                ss_logo_x += ss_logo_dx;
                ss_logo_y += ss_logo_dy;

                if (ss_logo_x <= 0) { ss_logo_x = 0; ss_logo_dx = std::fabs(ss_logo_dx); } 
                else if (ss_logo_x >= (sw - lw)) { ss_logo_x = sw - lw; ss_logo_dx = -std::fabs(ss_logo_dx); }

                if (ss_logo_y <= 0) { ss_logo_y = 0; ss_logo_dy = std::fabs(ss_logo_dy); } 
                else if (ss_logo_y >= (sh - lh)) { ss_logo_y = sh - lh; ss_logo_dy = -std::fabs(ss_logo_dy); }

                lv_obj_set_pos(splash_logo, (lv_coord_t)ss_logo_x, (lv_coord_t)ss_logo_y);
            }
            lvgl_port_unlock();
        }
    }

    PhysicalButton active_btn = get_physical_button_press();

    if (active_btn != BTN_NONE) {
        last_activity_time = millis(); 
        if (is_booting) {
            is_booting = false;
            lvgl_port_lock(-1);
            lv_scr_load(ui_RoastMain);
            lvgl_port_unlock();
        } 
        else if (is_idle) {
            is_idle = false;
            lvgl_port_lock(-1);
            lv_obj_t* splash_logo = lv_obj_get_child(ui_SplashScreen, 0);
            if (splash_logo != NULL) { lv_obj_set_align(splash_logo, LV_ALIGN_CENTER); lv_obj_set_pos(splash_logo, 0, 0); }
            lv_scr_load(ui_RoastMain);
            lvgl_port_unlock();
        } 
        else {
            lvgl_port_lock(-1);                  
            process_button_logic(active_btn);    
            lvgl_port_unlock();                  
        }
    }

    current_time = millis();

    if (lv_scr_act() == ui_Roasting && current_roast_state != ROAST_NONE) {
        uint32_t elapsed = current_time - roast_start_time;
        lvgl_port_lock(-1);
        char time_str[8];
        int progress = 0;

        if (current_roast_state == ROAST_HEATING) {
            uint32_t roasting_duration_ms = 5000;
            if (elapsed >= roasting_duration_ms) {
                current_roast_state = ROAST_COOLING; roast_start_time = current_time; 
                lv_label_set_text(ui_RoasterStatus, "Cooling..."); lv_arc_set_value(ui_Arc1, 0); 
            } else {
                int simulated_total_sec = 600; 
                int simulated_elapsed_sec = (elapsed * simulated_total_sec) / roasting_duration_ms;
                int remaining_sec = simulated_total_sec - simulated_elapsed_sec;
                if (remaining_sec < 0) remaining_sec = 0;
                snprintf(time_str, sizeof(time_str), "%d:%02d", remaining_sec / 60, remaining_sec % 60);
                lv_label_set_text(ui_TimeRemaining, time_str);
                progress = (elapsed * 100) / roasting_duration_ms;
                lv_arc_set_value(ui_Arc1, progress > 100 ? 100 : progress);
            }
        } 
        else if (current_roast_state == ROAST_COOLING) {
            uint32_t cooling_duration_ms = 5000; 
            if (elapsed >= cooling_duration_ms) {
                current_roast_state = ROAST_NONE; lv_scr_load(ui_RoastMain); last_activity_time = current_time; 
            } else {
                int simulated_total_sec = 300; 
                int simulated_elapsed_sec = (elapsed * simulated_total_sec) / cooling_duration_ms;
                int remaining_sec = simulated_total_sec - simulated_elapsed_sec;
                if (remaining_sec < 0) remaining_sec = 0;
                snprintf(time_str, sizeof(time_str), "%d:%02d", remaining_sec / 60, remaining_sec % 60);
                lv_label_set_text(ui_TimeRemaining, time_str);
                progress = (elapsed * 100) / cooling_duration_ms;
                lv_arc_set_value(ui_Arc1, progress > 100 ? 100 : progress);
            }
        }
        lvgl_port_unlock();
    }
}

// ============================================================================
// PLATFORM-SPECIFIC MAIN INITIALIZATIONS
// ============================================================================
#ifdef ARDUINO
void setup() {
    Serial.begin(115200);

    panel = new ESP_Panel();
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    assert(buf);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = ESP_PANEL_LCD_H_RES;
    disp_drv.ver_res = ESP_PANEL_LCD_V_RES;
    disp_drv.flush_cb = lvgl_port_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.sw_rotate = 1;
    disp_drv.rotated = LV_DISP_ROT_180;
    lv_disp_drv_register(&disp_drv);

    #if ESP_PANEL_USE_LCD_TOUCH
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_port_tp_read;
    lv_indev_drv_register(&indev_drv);
    #endif

    panel->init();
    #if ESP_PANEL_LCD_BUS_TYPE != ESP_PANEL_BUS_TYPE_RGB
    panel->getLcd()->setCallback(notify_lvgl_flush_ready, &disp_drv);
    #endif

    ESP_IOExpander *expander = new ESP_IOExpander_CH422G(I2C_MASTER_NUM, ESP_IO_EXPANDER_I2C_CH422G_ADDRESS_000);
    expander->init();
    expander->begin();
    expander->multiPinMode(TP_RST | LCD_BL | LCD_RST | SD_CS | USB_SEL, OUTPUT);
    expander->multiDigitalWrite(TP_RST | LCD_RST | SD_CS, HIGH); 
    expander->digitalWrite(LCD_BL, LOW); 
    expander->digitalWrite(USB_SEL, LOW);
    
    panel->addIOExpander(expander);
    panel->begin();

    delay(2000);
    expander->digitalWrite(LCD_BL, HIGH);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    lvgl_port_lock(-1);
    ui_init(); 
    reset_menu_state(); 
    lv_scr_load(ui_SplashScreen);
    is_booting = true;
    boot_start_time = millis();
    lvgl_port_unlock();
}

void loop() {
    run_system_loop();
    delay(10);
}
#else
// --- EMSCRIPTEN (WEB) MAIN ---
void emscripten_loop_wrapper() {
    run_system_loop();
    lv_timer_handler(); // Tell LVGL to draw the frame
}

int main(void) {
    lv_init();
    
    // Boot up the web monitor (resolutions)
    monitor_init();
    static lv_disp_draw_buf_t disp_buf;
    static lv_color_t buf[800 * 100];
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, 800 * 100);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = monitor_flush;
    disp_drv.hor_res = 800;
    disp_drv.ver_res = 480;
    lv_disp_drv_register(&disp_drv);

    // Boot up the web mouse (for clicking menus)
    mouse_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_read;
    lv_indev_drv_register(&indev_drv);

    ui_init(); 
    reset_menu_state();
    lv_scr_load(ui_SplashScreen);
    is_booting = true;
    boot_start_time = millis();

    // Tell the web browser to run our loop indefinitely
    emscripten_set_main_loop(emscripten_loop_wrapper, 0, 1);
    return 0;
}
#endif