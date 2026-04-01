#include <lvgl.h>
#include <cmath>
#include <cstdlib>

// ============================================================================
// 1. SHARED UI BRIDGE (Must be at the top)
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

    #define LVGL_TICK_PERIOD_MS     (2)
    #define LVGL_TASK_MAX_DELAY_MS  (5)
    #define LVGL_TASK_MIN_DELAY_MS  (1)
    #define LVGL_TASK_STACK_SIZE    (10 * 1024)
    #define LVGL_TASK_PRIORITY      (2)
    #define LVGL_BUF_SIZE           (800 * 50)

    ESP_Panel *panel = NULL;
    SemaphoreHandle_t lvgl_mux = NULL;

    void lvgl_port_lock(int timeout_ms) {
        const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
    }
    void lvgl_port_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

#else
    #include <emscripten.h>
    extern "C" {
        #include "lv_drivers/display/monitor.h"
        #include "lv_drivers/indev/mouse.h"
    }
    uint32_t millis() { return lv_tick_get(); }
    void lvgl_port_lock(int timeout_ms) {}
    void lvgl_port_unlock(void) {}
#endif

// ============================================================================
// 3. SHARED GLOBALS (Must be declared before run_system_loop)
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
// 4. SHARED UI LOGIC FUNCTIONS
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

void reset_menu_state() {
    current_menu_tier = TIER_ROOT;
    p_panel_prev = ui_MenuPrev; p_panel_active = ui_MenuActive; p_panel_next = ui_MenuNext;
    p_roller_prev = ui_MenuPrevRoller; p_roller_active = ui_MenuActiveRoller; p_roller_next = ui_MenuNextRoller;
    apply_menu_formatting();
    load_menu_tier_to_roller(TIER_ROOT, p_roller_active);
    update_next_preview();
}

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
#endif
    if ((millis() - last_debounce_time) > 50) {
        if (current_reading != last_steady_state) {
            last_steady_state = current_reading;
            return current_reading; 
        }
    }
    return BTN_NONE;
}

void process_button_logic(PhysicalButton btn) {
    if (btn == BTN_NONE) return;
    if (btn == BTN_ENTER && lv_scr_act() == ui_RoastMain) {
        lv_scr_load(ui_Roasting); current_roast_state = ROAST_HEATING; roast_start_time = millis();
    }
    else if (btn == BTN_BACK && lv_scr_act() == ui_RoastMain) {
        lv_scr_load(ui_Menu1); reset_menu_state();
    }
}

// ============================================================================
// 5. SYSTEM LOOP (SHARED)
// ============================================================================
void run_system_loop() {
    uint32_t current_time = millis();

    if (is_booting) {
        if (current_time - boot_start_time > 5000) {
            is_booting = false;
            lvgl_port_lock(-1);
            lv_scr_load(ui_RoastMain);
            lvgl_port_unlock();
            last_activity_time = current_time; 
        }
    } 
    else {
        PhysicalButton active_btn = get_physical_button_press();
        if (active_btn != BTN_NONE) {
            lvgl_port_lock(-1);
            process_button_logic(active_btn);
            lvgl_port_unlock();
            last_activity_time = current_time;
        }
    }

    if (lv_scr_act() == ui_Roasting && current_roast_state != ROAST_NONE) {
        lvgl_port_lock(-1);
        // ... (Roasting Logic)
        lvgl_port_unlock();
    }
}

// ============================================================================
// 6. ENTRY POINTS (Hardware vs. Web)
// ============================================================================
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
        Serial.begin(115200);
        panel = new ESP_Panel();
        lv_init();
        
        static lv_disp_draw_buf_t draw_buf;
        uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
        lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);

        // ... (Your existing Arduino Display Setup) ...
        
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

    void loop() { run_system_loop(); delay(10); }

#else
    void emscripten_loop_wrapper() {
        run_system_loop();
        lv_timer_handler();
    }

    int main(void) {
        lv_init();
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

        emscripten_set_main_loop(emscripten_loop_wrapper, 0, 1);
        return 0;
    }
#endif
