#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <string>
#include <initializer_list>
#include <cctype>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    SetupFaceUI();
}

OledDisplay::~OledDisplay() {
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // Replace all newlines with spaces
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OledDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr) {
        return;
    }
    // OLED has one text slot and does not distinguish message roles.
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');
    if (content_str != lv_label_get_text(chat_message_label_)) {
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (content_right_ != nullptr) {
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);

    emotion_label_ = lv_label_create(content_left_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);
    lv_obj_set_style_pad_top(emotion_label_, 8, 0);

    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
}

void OledDisplay::SetupFaceUI() {
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // Monochrome OLED: black background, bright white facial features.
    const int eye_width = (height_ == 32) ? 20 : 27;
    eye_open_height_ = (height_ == 32) ? 19 : 29;
    const int eye_y = (height_ == 32) ? 6 : 17;
    const int eye_gap = (height_ == 32) ? 24 : 20;
    const int eye_x = (width_ - (eye_width * 2 + eye_gap)) / 2;

    eye_left_ = lv_obj_create(screen);
    eye_right_ = lv_obj_create(screen);
    for (lv_obj_t* eye : {eye_left_, eye_right_}) {
        lv_obj_set_size(eye, eye_width, eye_open_height_);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_pos(eye_left_, eye_x, eye_y);
    lv_obj_set_pos(eye_right_, eye_x + eye_width + eye_gap, eye_y);

    // Each heart is built from two round lobes and one rotated square.
    const int heart_size = (height_ == 32) ? 17 : 24;
    const int lobe = (height_ == 32) ? 9 : 13;
    const int diamond = (height_ == 32) ? 11 : 16;
    const int heart_gap = (height_ == 32) ? 24 : 20;
    const int heart_x = (width_ - (heart_size * 2 + heart_gap)) / 2;
    const int heart_y = (height_ == 32) ? 7 : 19;
    for (int i = 0; i < 6; ++i) {
        heart_parts_[i] = lv_obj_create(screen);
        lv_obj_set_style_bg_color(heart_parts_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(heart_parts_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(heart_parts_[i], 0, 0);
        lv_obj_set_style_pad_all(heart_parts_[i], 0, 0);
        lv_obj_clear_flag(heart_parts_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(heart_parts_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int side = 0; side < 2; ++side) {
        const int x = heart_x + side * (heart_size + heart_gap);
        lv_obj_set_size(heart_parts_[side * 3], diamond, diamond);
        lv_obj_set_style_radius(heart_parts_[side * 3], 2, 0);
        lv_obj_set_style_transform_rotation(heart_parts_[side * 3], 450, 0);
        lv_obj_set_pos(heart_parts_[side * 3], x + (heart_size - diamond) / 2,
                       heart_y + heart_size / 2);
        for (int l = 0; l < 2; ++l) {
            lv_obj_t* lobe_obj = heart_parts_[side * 3 + 1 + l];
            lv_obj_set_size(lobe_obj, lobe, lobe);
            lv_obj_set_style_radius(lobe_obj, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(lobe_obj, x + l * (lobe - 1), heart_y);
        }
    }

    // Eyebrows are hidden in the neutral face and shown only for expressions.
    eyebrow_left_ = lv_obj_create(screen);
    eyebrow_right_ = lv_obj_create(screen);
    const int brow_width = (height_ == 32) ? 15 : 21;
    for (lv_obj_t* brow : {eyebrow_left_, eyebrow_right_}) {
        lv_obj_set_size(brow, brow_width, 3);
        lv_obj_set_style_bg_color(brow, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(brow, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(brow, 0, 0);
        lv_obj_set_style_radius(brow, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(brow, 0, 0);
        lv_obj_add_flag(brow, LV_OBJ_FLAG_HIDDEN);
    }

    blink_counter_ = 0;
    blink_timer_ = lv_timer_create(BlinkTimerCallback, 100, this);
}

void OledDisplay::ApplyEyeEmotion(const char* emotion) {
    if (eye_left_ == nullptr || eye_right_ == nullptr) return;

    const std::string mood = (emotion == nullptr) ? "neutral" : emotion;
    const bool happy = mood == "happy" || mood == "laugh" || mood == "love" ||
                       mood == "loving" || mood == "happiness" || mood == "joy";
    heart_mode_ = happy;

    const int base_width = (height_ == 32) ? 20 : 27;
    const int base_height = (height_ == 32) ? 19 : 29;
    const int base_y = (height_ == 32) ? 6 : 17;
    const int gap = (height_ == 32) ? 24 : 20;
    int eye_width = base_width;
    int eye_height = base_height;
    int eye_y = base_y;

    const bool sad = mood == "sad" || mood == "cry" || mood == "crying" ||
                     mood == "sadness" || mood == "sorrow";
    const bool surprised = mood == "surprised" || mood == "surprise" ||
                           mood == "astonished" || mood == "fear" || mood == "scared";
    const bool angry = mood == "angry" || mood == "serious" || mood == "disgust";
    const bool sleepy = mood == "sleepy" || mood == "sleep" || mood == "tired";

    if (sad) {
        eye_height = (height_ == 32) ? 14 : 21;
        eye_y += (height_ == 32) ? 2 : 3;
    } else if (surprised) {
        eye_width = (height_ == 32) ? 22 : 32;
        eye_height = (height_ == 32) ? 22 : 35;
        eye_y = (height_ == 32) ? 5 : 14;
    } else if (angry) {
        eye_height = (height_ == 32) ? 12 : 18;
        eye_width += (height_ == 32) ? 1 : 2;
        eye_y += (height_ == 32) ? 3 : 5;
    } else if (sleepy) {
        eye_height = (height_ == 32) ? 7 : 11;
        eye_y += (height_ == 32) ? 6 : 9;
    } else if (speaking_ && !happy) {
        // Keep the normal round eyes at rest; make them visibly oval while TTS speaks.
        eye_height = (height_ == 32) ? 13 : 19;
        eye_y += (height_ == 32) ? 3 : 5;
    }

    if (heart_mode_) {
        lv_obj_add_flag(eye_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* part : heart_parts_) lv_obj_clear_flag(part, LV_OBJ_FLAG_HIDDEN);
    } else {
        for (lv_obj_t* part : heart_parts_) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(eye_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        const int x = (width_ - (eye_width * 2 + gap)) / 2;
        for (lv_obj_t* eye : {eye_left_, eye_right_}) {
            lv_obj_set_size(eye, eye_width, eye_height);
            lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        }
        lv_obj_set_pos(eye_left_, x, eye_y);
        lv_obj_set_pos(eye_right_, x + eye_width + gap, eye_y);
        eye_open_height_ = eye_height;
    }

    if (sad || angry) {
        lv_obj_clear_flag(eyebrow_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(eyebrow_right_, LV_OBJ_FLAG_HIDDEN);
        const int brow_width = (height_ == 32) ? 15 : 21;
        const int brow_y = (height_ == 32) ? 1 : 10;
        const int brow_gap = (height_ == 32) ? 24 : 20;
        const int brow_x = (width_ - (brow_width * 2 + brow_gap)) / 2;
        // Sad brows rise toward the center; angry brows tilt the opposite way.
        lv_obj_set_style_transform_rotation(eyebrow_left_, sad ? 3400 : 200, 0);
        lv_obj_set_style_transform_rotation(eyebrow_right_, sad ? 200 : 3400, 0);
        lv_obj_set_pos(eyebrow_left_, brow_x, brow_y);
        lv_obj_set_pos(eyebrow_right_, brow_x + brow_width + brow_gap, brow_y);
    } else {
        lv_obj_add_flag(eyebrow_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eyebrow_right_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::BlinkTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->eye_left_ == nullptr || self->eye_right_ == nullptr) return;

    ++self->blink_counter_;
    if (self->blink_counter_ == 45) {
        if (self->heart_mode_) {
            for (lv_obj_t* part : self->heart_parts_) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_height(self->eye_left_, 2);
            lv_obj_set_height(self->eye_right_, 2);
        }
    } else if (self->blink_counter_ >= 47) {
        if (self->heart_mode_) {
            for (lv_obj_t* part : self->heart_parts_) lv_obj_clear_flag(part, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_height(self->eye_left_, self->eye_open_height_);
            lv_obj_set_height(self->eye_right_, self->eye_open_height_);
        }
        self->blink_counter_ = 0;
    }
}

void OledDisplay::SetStatus(const char* status) {
    if (status == nullptr) return;

    const bool speaking_now = (std::strcmp(status, Lang::Strings::SPEAKING) == 0);
    if (speaking_now == speaking_) return;

    DisplayLockGuard lock(this);
    speaking_ = speaking_now;
    ApplyEyeEmotion(current_emotion_.c_str());
}

bool OledDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') return false;

    DisplayLockGuard lock(this);
    current_emotion_ = emotion;
    std::transform(current_emotion_.begin(), current_emotion_.end(),
                   current_emotion_.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    ApplyEyeEmotion(current_emotion_.c_str());
    return true;
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}
