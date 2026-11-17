#include "emote_display.h"

// Standard C++ headers
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <tuple>
#include <unordered_map>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "expression_emote.h"
#include "gfx.h"

namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t* const edata, void* user_ctx) {
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush bookkeeping, so that a display which has stopped flushing can be noticed.
//
// Nothing in the tree watches the render task: no task is subscribed to the task watchdog and
// CONFIG_ESP_TASK_WDT_PANIC is off, and a task blocked on an event group yields the CPU, so the
// idle-task watchdog sees nothing wrong. These two values are the minimum a board needs to tell
// a live display from a wedged one.
static std::atomic<uint32_t> s_flush_count{0};
static std::atomic<int64_t> s_last_flush_us{0};

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data,
                            emote_handle_t handle) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);

    // Every exit from here must end in a flush-finished report, one way or the other.
    //
    // The renderer clears the flush-done bit, calls this, then waits on that bit with
    // portMAX_DELAY (esp_emote_gfx, gfx_render.c) - and the only thing that sets it is
    // emote_notify_flush_finished, which is reached only from the panel's on_color_trans_done
    // callback. So a block that never reaches the bus is not a dropped frame: the render task
    // waits for it forever, holding the graphics lock, and every caller of gfx_emote_lock
    // queues up behind it. The device keeps answering pings and WebSocket pongs the whole
    // time, because those are other tasks - so it looks alive and the screen is simply frozen
    // on one frame. Reporting completion here is safe: the bit is set before the renderer
    // reaches its wait, and a pre-set bit makes that wait return at once.
    if (panel == nullptr) {
        emote_notify_flush_finished(handle);
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed (%s) for (%d,%d)-(%d,%d); reporting flush done so the "
                      "render task is not left waiting for a transfer that never started",
                 esp_err_to_name(err), x_start, y_start, x_end, y_end);
        emote_notify_flush_finished(handle);
        return;
    }

    s_flush_count.fetch_add(1, std::memory_order_relaxed);
    s_last_flush_us.store(esp_timer_get_time(), std::memory_order_relaxed);
}

uint32_t GetFlushCount() {
    return s_flush_count.load(std::memory_order_relaxed);
}

int64_t GetMicrosecondsSinceLastFlush() {
    int64_t last = s_last_flush_us.load(std::memory_order_relaxed);
    if (last == 0) {
        return -1;  // nothing has been flushed yet; not the same as "a long time ago"
    }
    return esp_timer_get_time() - last;
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width,
                                      const int height) {
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags =
            {
                .swap = true,
#if CONFIG_IDF_TARGET_ESP32S31
                // 内部 RAM 紧张：单缓冲 + DMA 内存即可，双缓冲会多占 11KB 内部堆
                .double_buffer = false,
#else
                .double_buffer = true,
#endif
#if CONFIG_IDF_TARGET_ESP32S31
                // S31 喵伴：Wi-Fi/BT/摄像头起来后内部堆碎片化，PSRAM 帧缓冲每次
                // flush 都要临时分配 SPI bounce buffer，会 ESP_ERR_NO_MEM 导致花屏。
                // 用内部 DMA 内存做帧缓冲，避免每个事务的临时分配。
                .buff_dma = true,
#else
                .buff_dma = false,
#endif
            },
        .gfx_emote =
            {
                .h_res = width,
                .v_res = height,
                .fps = 30,
            },
        .buffers =
            {
                .buf_pixels = static_cast<size_t>(width * 16),
            },
        .task =
            {
                .task_priority = 5,
                .task_stack = 6 * 1024,
                .task_affinity = 0,
                .task_stack_in_ext = false,
            },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel,
                           const esp_lcd_panel_io_handle_t panel_io, const int width,
                           const int height) {
    width_ = width;
    height_ = height;
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay() {
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

bool EmoteDisplay::SetEmotion(const char* const emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emote_handle_ == nullptr || emotion == nullptr || strlen(emotion) == 0) {
        return false;
    }
    // The result was already being produced and discarded. An emotion the mounted asset pack
    // does not carry fails inside emote_set_emoji_animation, and callers had no way to find
    // out: an MCP tool, a server-driven emotion or a board's own avatar mapping all saw the
    // same silence as a success.
    esp_err_t err = emote_set_anim_emoji(emote_handle_, emotion);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emotion \"%s\" not set: %s", emotion, esp_err_to_name(err));
        return false;
    }
    return true;
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content) {
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ && content && strlen(content) > 0) {
        if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
            size_t len = strlen(content);
            char* new_content = new char[len + 1];
            strcpy(new_content, content);
            std::replace(new_content, new_content + len, static_cast<char>(0x0A),
                         static_cast<char>(0x20));
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
            delete[] new_content;
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
        }
    }
}

void EmoteDisplay::SetStatus(const char* const status) {
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all) {
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on) {
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::PreviewTimerCallback(void* arg) {
    static_cast<EmoteDisplay*>(arg)->HidePreview();
}

void EmoteDisplay::HidePreview() {
    if (preview_obj_ != nullptr) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(preview_obj_, false);
        emote_unlock(emote_handle_);
    }
    preview_image_.reset();
    preview_buf_.clear();
    preview_buf_.shrink_to_fit();
    emote_set_obj_visible(emote_handle_, EMT_DEF_ELEM_EYE_ANIM, true);
    emote_set_obj_visible(emote_handle_, EMT_DEF_ELEM_STATUS_ICON, true);
    emote_set_anim_visible(emote_handle_, true);
    ESP_LOGI(TAG, "camera preview hidden");
}

void EmoteDisplay::ShowPreview(std::unique_ptr<LvglImage> image) {
    const lv_img_dsc_t* src = image->image_dsc();
    if (src == nullptr || src->data == nullptr) {
        return;
    }
    const int sw = src->header.w;
    const int sh = src->header.h;
    const int sstride = src->header.stride ? src->header.stride : sw * 2;
    if (sw <= 0 || sh <= 0) {
        return;
    }

    // 屏幕是 360x360 的圆屏：取画面中心的正方形，避免显示被拉伸
    const int cw = std::min(sw, width_);
    const int ch = std::min(sh, height_);
    if (cw <= 0 || ch <= 0) {
        ESP_LOGW(TAG, "camera preview: invalid crop %dx%d (screen %dx%d, image %dx%d)", cw, ch,
                 width_, height_, sw, sh);
        return;
    }
    const int x0 = (sw - cw) / 2;
    const int y0 = (sh - ch) / 2;

    preview_buf_.resize(static_cast<size_t>(cw) * ch * 2);
    const uint8_t* src_data = src->data;
    // gfx 直接把图源按“原生 framebuffer 顺序”拷贝到 emote 帧缓冲，而本屏是 16bit
    // 字节交换（emote 配置 swap=true）、摄像头给的是 RGB565 little-endian，
    // 所以这里逐像素交换高低字节，否则颜色会错。
    for (int y = 0; y < ch; ++y) {
        const uint8_t* src_row = src_data + static_cast<size_t>(y0 + y) * sstride + x0 * 2;
        uint8_t* dst_row = &preview_buf_[static_cast<size_t>(y) * cw * 2];
        for (int x = 0; x < cw; ++x) {
            dst_row[x * 2] = src_row[x * 2 + 1];
            dst_row[x * 2 + 1] = src_row[x * 2];
        }
    }

    preview_dsc_ = {};
    preview_dsc_.header.magic = 0x19;  // C_ARRAY_HEADER_MAGIC
    preview_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565;
    preview_dsc_.header.w = cw;
    preview_dsc_.header.h = ch;
    preview_dsc_.header.stride = cw * 2;
    preview_dsc_.data_size = preview_buf_.size();
    preview_dsc_.data = preview_buf_.data();

    if (preview_obj_ == nullptr) {
        preview_obj_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "camera_preview");
        if (preview_obj_ == nullptr) {
            ESP_LOGW(TAG, "camera preview: cannot create image object");
            preview_buf_.clear();
            return;
        }
    }

    esp_err_t src_ret = ESP_FAIL;
    emote_lock(emote_handle_);
    src_ret = gfx_img_set_src(preview_obj_, &preview_dsc_);
    gfx_obj_set_visible(preview_obj_, true);
    gfx_obj_align(preview_obj_, GFX_ALIGN_CENTER, 0, 0);
    // 只隐藏眼睛/文字等元素，不动整个 anim 层（自定义图片对象挂在同一层时会被一起隐藏）
    emote_set_obj_visible(emote_handle_, EMT_DEF_ELEM_EYE_ANIM, false);
    emote_set_obj_visible(emote_handle_, EMT_DEF_ELEM_STATUS_ICON, false);
    emote_unlock(emote_handle_);
    if (src_ret != ESP_OK) {
        ESP_LOGW(TAG, "camera preview: gfx_img_set_src failed: %s", esp_err_to_name(src_ret));
    }

    preview_image_ = std::move(image);

    if (preview_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = PreviewTimerCallback;
        args.arg = this;
        args.name = "cam_preview";
        ESP_ERROR_CHECK(esp_timer_create(&args, &preview_timer_));
    }
    esp_timer_stop(preview_timer_);
    esp_timer_start_once(preview_timer_, 5 * 1000 * 1000);
    ESP_LOGI(TAG, "camera preview shown (%dx%d crop from %dx%d), 5s", cw, ch, sw, sh);
}

void EmoteDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        HidePreview();
        return;
    }
    ShowPreview(std::move(image));
}

void EmoteDisplay::SetPreviewImage(const void* image) {
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

bool EmoteDisplay::MountAssets(const char* partition_label) {
    if (emote_handle_ == nullptr || partition_label == nullptr) {
        return false;
    }
    const emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source =
            {
                .partition_label = partition_label,
            },
        .flags =
            {
                .mmap_enable = true,
            },
    };
    return emote_mount_assets(emote_handle_, &data) == ESP_OK;
}

void EmoteDisplay::UnmountAssets() {
    if (emote_handle_ != nullptr) {
        emote_unmount_assets(emote_handle_);
    }
}

bool EmoteDisplay::GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
    if (emote_handle_ == nullptr) {
        return false;
    }
    return emote_get_asset_data_by_name(emote_handle_, name.c_str(), &data, &size) == ESP_OK;
}

void EmoteDisplay::LoadAssets() {
    if (emote_handle_ != nullptr) {
        emote_load_assets(emote_handle_);
    }
}

void EmoteDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool EmoteDisplay::Lock(const int timeout_ms) {
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock() {}

bool EmoteDisplay::StopAnimDialog() {
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        // esp_err_t, where ESP_OK is 0: returned as bool it reports false on success and true on
        // failure. GetAssetData and SetEmotion compare against ESP_OK for the same reason.
        return emote_stop_anim_dialog(emote_handle_) == ESP_OK;
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) {
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms) == ESP_OK;
    }
    return false;
}

void EmoteDisplay::RefreshAll() {
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

}  // namespace emote
