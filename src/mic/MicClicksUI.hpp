#pragma once
// Mic Clicks: the "Mic Setup" panel.
//
// Lives outside the huge BufferMenu on purpose: it only talks to MicClicks / MicEngine and
// cocos, and it is safe to open while the engine is running (everything it reads is a
// snapshot, every change goes through applyConfig on the main thread).

#include <Geode/ui/Popup.hpp>

#include "MicEngine.hpp"

namespace micclicks {

class MicSetupPopup : public geode::Popup {
public:
    static MicSetupPopup* create();
    static void open();

    ~MicSetupPopup() override;
    void update(float dt) override;

protected:
    bool init() override;

private:
    void buildRows();
    void refresh();

    void onToggleEnabled(cocos2d::CCObject*);
    void onCalibrate(cocos2d::CCObject*);
    void onCancelCalibration(cocos2d::CCObject*);
    void onClearFingerprint(cocos2d::CCObject*);
    void onCapturePrev(cocos2d::CCObject*);
    void onCaptureNext(cocos2d::CCObject*);
    void onRenderPrev(cocos2d::CCObject*);
    void onRenderNext(cocos2d::CCObject*);
    void onRefreshDevices(cocos2d::CCObject*);
    void onMonitorToggle(cocos2d::CCObject*);
    void onMonitorMode(cocos2d::CCObject*);
    void onDelayDown(cocos2d::CCObject*);
    void onDelayUp(cocos2d::CCObject*);
    void onDelayReset(cocos2d::CCObject*);
    void onAlignMode(cocos2d::CCObject*);

    void stepCapture(int direction);
    void stepRender(int direction);
    void setDelayOffset(float value);

    static inline MicSetupPopup* s_open = nullptr;

    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_captureLabel = nullptr;
    cocos2d::CCLabelBMFont* m_renderLabel = nullptr;
    cocos2d::CCLabelBMFont* m_virtualLabel = nullptr;
    cocos2d::CCLabelBMFont* m_calibration = nullptr;
    cocos2d::CCLabelBMFont* m_delay = nullptr;
    cocos2d::CCLabelBMFont* m_monitor = nullptr;
    cocos2d::CCLabelBMFont* m_mode = nullptr;
    cocos2d::CCLabelBMFont* m_align = nullptr;
    cocos2d::CCLabelBMFont* m_stats = nullptr;
    cocos2d::CCLabelBMFont* m_enable = nullptr;
    cocos2d::CCLabelBMFont* m_calibrate = nullptr;
    cocos2d::CCDrawNode* m_meter = nullptr;
    cocos2d::CCDrawNode* m_progress = nullptr;
    float m_refreshTimer = 0.f;
};

} // namespace micclicks
