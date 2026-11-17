#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <memory>
#include <vector>
#include <string>
#include "display.h"
#include "expression_emote.h"
#include "lvgl_image.h"

namespace emote {

// Flush progress, for a board that wants to notice a display which has stopped flushing.
// GetMicrosecondsSinceLastFlush returns -1 before the first flush, which is not the same
// answer as a large number.
uint32_t GetFlushCount();
int64_t GetMicrosecondsSinceLastFlush();

class EmoteDisplay : public Display {
public:
    using Display::SetPreviewImage;

    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                 int height);
    virtual ~EmoteDisplay();

    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);
    // 拍照预览：把 LVGL 图片对象盖在表情上，显示 5 秒后恢复表情
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    bool StopAnimDialog();
    virtual bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) override;
    virtual bool MountAssets(const char* partition_label) override;
    virtual void UnmountAssets() override;
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) override;
    virtual void LoadAssets() override;

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    emote_handle_t emote_handle_ = nullptr;
    gfx_obj_t* preview_obj_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_;
    gfx_image_dsc_t preview_dsc_ = {};
    std::vector<uint8_t> preview_buf_;
    esp_timer_handle_t preview_timer_ = nullptr;
    static void PreviewTimerCallback(void* arg);
    void ShowPreview(std::unique_ptr<LvglImage> image);
    void HidePreview();
};

}  // namespace emote
