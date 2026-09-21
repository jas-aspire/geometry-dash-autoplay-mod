/*
 * jas.autoplayer v2.0
 *
 * Modes  — Browser | Map Pack List | Gauntlet | Main Levels (1-22)
 * Delay  — set in Geode mod settings (0.5 – 20s slider)
 * Stop   — F5 anywhere, or tap the AUTO button again
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/GauntletLayer.hpp>
#include <Geode/modify/LevelSelectLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

// ─────────────────────── Mode & State ────────────────────────────────────────

enum class AutoMode { None, Browser, MapPackList, Gauntlet, MainLevels };

// Read delay from Geode mod settings
static float getDelay() {
    return Mod::get()->getSettingValue<double>("delay");
}

struct AutoRunState {
    bool      active        = false;
    bool      justCompleted = false;
    bool      waitingForPage = false;
    int       levelIndex    = 0;
    int       failStreak    = 0;
    AutoMode  mode          = AutoMode::None;
    std::vector<int>   queue;
    LevelBrowserLayer* browser  = nullptr;
    GauntletLayer*     gauntlet = nullptr;

    void reset();  // defined after AutoDownloader
};
static AutoRunState g_run;
static bool g_ownsDelegate = false;

// ─────────────────────── Forward declarations ─────────────────────────────────
static void downloadNext();
static void launchLevel(GJGameLevel* lvl);

// ─────────────────────── Completion check ────────────────────────────────────
// For ONLINE levels: getSavedLevel() has local completion data
static bool isCompleted(int levelID) {
    auto* saved = GameLevelManager::sharedState()->getSavedLevel(levelID);
    return saved && saved->m_normalPercent.value() >= 100;
}

// For MAIN levels (IDs 1-22): they live in m_mainLevels, NOT the online save dict
// getSavedLevel(int) won't find them — use getMainLevel() directly
static bool isMainLevelCompleted(int levelID) {
    auto* lvl = GameLevelManager::sharedState()->getMainLevel(levelID, true);
    return lvl && lvl->m_normalPercent.value() >= 100;
}

// ─────────────────────── Delay timer helper ───────────────────────────────────
// scheduleOnce in this SDK only accepts SEL_SCHEDULE (no lambda overload).
// APDelayTimer stores a std::function and fires it after the given delay.

class APDelayTimer : public CCNode {
public:
    std::function<void()> m_fn;

    static APDelayTimer* create() {
        auto* n = new APDelayTimer();
        if (n->init()) { n->autorelease(); return n; }
        delete n; return nullptr;
    }

    // Attach to 'parent', fire 'fn' after 'delay' seconds then self-destruct
    static void run(CCNode* parent, float delay, std::function<void()> fn) {
        if (!parent) { fn(); return; }
        auto* t = APDelayTimer::create();
        if (!t) { fn(); return; }
        t->m_fn = std::move(fn);
        parent->addChild(t);
        t->scheduleOnce(schedule_selector(APDelayTimer::fire), delay);
    }

    void fire(float) {
        if (m_fn) m_fn();
        this->removeFromParent();
    }
};

// ─────────────────────── Queue builders ──────────────────────────────────────

static void buildBrowserQueue() {
    g_run.queue.clear(); g_run.levelIndex = 0; g_run.failStreak = 0;
    if (!g_run.browser) return;
    auto* arr = g_run.browser->m_levels;
    if (!arr) return;
    for (unsigned i = 0; i < arr->count(); ++i) {
        auto* lvl = typeinfo_cast<GJGameLevel*>(arr->objectAtIndex(i));
        if (lvl && !isCompleted(lvl->m_levelID))
            g_run.queue.push_back(lvl->m_levelID);
    }
    log::info("[AutoPlayer] Browser queue: {}/{}", g_run.queue.size(), arr->count());
}

static void buildMapPackQueue() {
    g_run.queue.clear(); g_run.levelIndex = 0; g_run.failStreak = 0;
    if (!g_run.browser) return;
    auto* arr = g_run.browser->m_levels;
    if (!arr) return;
    for (unsigned i = 0; i < arr->count(); ++i) {
        auto* pack = typeinfo_cast<GJMapPack*>(arr->objectAtIndex(i));
        if (!pack || pack->hasCompletedMapPack() || !pack->m_levels) continue;
        for (unsigned j = 0; j < pack->m_levels->count(); ++j) {
            auto* num = typeinfo_cast<CCInteger*>(pack->m_levels->objectAtIndex(j));
            if (num && !isCompleted(num->getValue()))
                g_run.queue.push_back(num->getValue());
        }
    }
    log::info("[AutoPlayer] MapPack queue: {} levels", g_run.queue.size());
}

static void buildGauntletQueue() {
    g_run.queue.clear(); g_run.levelIndex = 0; g_run.failStreak = 0;
    if (!g_run.gauntlet) return;
    auto* arr = g_run.gauntlet->m_levels;
    if (!arr) return;
    for (unsigned i = 0; i < arr->count(); ++i) {
        auto* lvl = typeinfo_cast<GJGameLevel*>(arr->objectAtIndex(i));
        if (lvl && !isCompleted(lvl->m_levelID))
            g_run.queue.push_back(lvl->m_levelID);
    }
    log::info("[AutoPlayer] Gauntlet queue: {}/{}", g_run.queue.size(), arr->count());
}

static void buildMainLevelQueue() {
    g_run.queue.clear(); g_run.levelIndex = 0; g_run.failStreak = 0;
    // Use isMainLevelCompleted — main levels are NOT in the online save dict
    for (int id = 1; id <= 22; ++id) {
        if (!isMainLevelCompleted(id)) g_run.queue.push_back(id);
    }
    log::info("[AutoPlayer] Main queue: {}/22", g_run.queue.size());
}

// ─────────────────── AutoDownloader singleton ─────────────────────────────────

class AutoDownloader : public CCObject, public LevelDownloadDelegate {
public:
    static AutoDownloader* s_inst;

    static AutoDownloader* get() {
        if (!s_inst) { s_inst = new AutoDownloader(); s_inst->retain(); }
        return s_inst;
    }

    void downloadID(int levelID) {
        auto* glm = GameLevelManager::sharedState();
        glm->m_levelDownloadDelegate = this;
        g_ownsDelegate = true;
        glm->downloadLevel(levelID, false, 0);
    }

    void levelDownloadFinished(GJGameLevel* level) override {
        auto* glm = GameLevelManager::sharedState();
        if (glm->m_levelDownloadDelegate == this) {
            glm->m_levelDownloadDelegate = nullptr;
            g_ownsDelegate = false;
        }
        if (!g_run.active) return;
        g_run.failStreak = 0;
        log::info("[AutoPlayer] Download OK — '{}'", level->m_levelName);
        launchLevel(level);
    }

    void levelDownloadFailed(int response) override {
        auto* glm = GameLevelManager::sharedState();
        if (glm->m_levelDownloadDelegate == this) {
            glm->m_levelDownloadDelegate = nullptr;
            g_ownsDelegate = false;
        }
        if (!g_run.active) return;
        g_run.failStreak++;
        log::warn("[AutoPlayer] Download failed ({}) streak={}", response, g_run.failStreak);
        if (g_run.failStreak >= 5) {
            g_run.reset();
            Loader::get()->queueInMainThread([]() {
                FLAlertLayer::create("AutoPlayer",
                    "<cr>Stopped</c> — too many download failures.\n"
                    "GD may be <cy>rate-limiting</c> you.", "OK")->show();
            });
            return;
        }
        Loader::get()->queueInMainThread([]() {
            if (g_run.active) downloadNext();
        });
    }
};
AutoDownloader* AutoDownloader::s_inst = nullptr;

// ─────────────────────── AutoRunState::reset ─────────────────────────────────
void AutoRunState::reset() {
    active = false; justCompleted = false;
    waitingForPage = false; levelIndex = 0; failStreak = 0;
    mode = AutoMode::None;
    queue.clear();
    browser = nullptr; gauntlet = nullptr;
    if (g_ownsDelegate) {
        GameLevelManager::sharedState()->m_levelDownloadDelegate = nullptr;
        g_ownsDelegate = false;
    }
}

// ─────────────────────── Core logic ──────────────────────────────────────────

static void downloadNext() {
    if (!g_run.active) return;

    if (g_run.levelIndex >= static_cast<int>(g_run.queue.size())) {
        if (g_run.mode == AutoMode::MainLevels || g_run.mode == AutoMode::Gauntlet) {
            const char* msg = g_run.mode == AutoMode::MainLevels
                ? "<cg>All main levels complete!</c>" : "<cg>Gauntlet done!</c>";
            g_run.reset();
            FLAlertLayer::create("AutoPlayer", msg, "OK")->show();
            return;
        }
        if (!g_run.browser) { g_run.reset(); return; }
        auto* so = g_run.browser->m_searchObject;
        if (!so) { g_run.reset(); return; }
        log::info("[AutoPlayer] Page done — loading next.");
        g_run.waitingForPage = true;
        g_run.browser->loadPage(so->getPageObject(so->m_page + 1));
        return;
    }

    int id = g_run.queue[g_run.levelIndex++];

    if (g_run.mode == AutoMode::MainLevels) {
        auto* lvl = GameLevelManager::sharedState()->getMainLevel(id, false);
        if (!lvl) { downloadNext(); return; }
        log::info("[AutoPlayer] Main level {}", id);
        launchLevel(lvl);
        return;
    }

    log::info("[AutoPlayer] Downloading ID {}...", id);
    AutoDownloader::get()->downloadID(id);
}

static void launchLevel(GJGameLevel* lvl) {
    CCDirector::sharedDirector()->pushScene(
        CCTransitionFade::create(0.5f, PlayLayer::scene(lvl, false, false))
    );
}

// ─────────────────────── F5 global stop ──────────────────────────────────────

struct KeyboardHook : Modify<KeyboardHook, CCKeyboardDispatcher> {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool repeat, double ts) {
        if (key == cocos2d::enumKeyCodes::KEY_F5 && down && !repeat && g_run.active) {
            g_run.reset();
            FLAlertLayer::create("AutoPlayer", "Auto-play <cr>stopped</c> (F5).", "OK")->show();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

// ─────────────────────── LevelBrowserLayer hook ──────────────────────────────

struct AutoPlayerBrowserHook : Modify<AutoPlayerBrowserHook, LevelBrowserLayer> {

    bool init(GJSearchObject* p0) {
        if (!LevelBrowserLayer::init(p0)) return false;
        injectButton();
        return true;
    }

    // Check at CLICK TIME (m_levels is populated by then)
    bool isMapPackList() {
        // SearchType::MapPack  (9)  = map pack LIST page → m_levels has GJMapPack objects
        // SearchType::MapPackOnClick (10) = inside a pack → m_levels has GJGameLevel objects
        if (!m_searchObject) return false;
        return m_searchObject->m_searchType == SearchType::MapPack;
    }

    void onEnterTransitionDidFinish() {
        LevelBrowserLayer::onEnterTransitionDidFinish();
        if (g_run.active && g_run.browser == this && g_run.justCompleted) {
            g_run.justCompleted = false;
            APDelayTimer::run(this, getDelay(), []() {
                if (g_run.active) downloadNext();
            });
        }
    }

    void setupLevelBrowser(cocos2d::CCArray* items) {
        LevelBrowserLayer::setupLevelBrowser(items);
        if (!g_run.active || g_run.browser != this || !g_run.waitingForPage) return;
        g_run.waitingForPage = false;

        if (g_run.mode == AutoMode::MapPackList) buildMapPackQueue();
        else                                     buildBrowserQueue();

        if (g_run.queue.empty()) {
            g_run.reset();
            FLAlertLayer::create("AutoPlayer",
                "No more <cy>uncompleted</c> levels.\nAuto-play <cg>finished</c>!", "OK")->show();
            return;
        }
        downloadNext();
    }

    void onExit() {
        if (g_run.browser == this) g_run.reset();
        LevelBrowserLayer::onExit();
    }
    void onNextPage(cocos2d::CCObject* s) {
        if (g_run.active && g_run.browser == this) g_run.reset();
        LevelBrowserLayer::onNextPage(s);
    }
    void onPrevPage(cocos2d::CCObject* s) {
        if (g_run.active && g_run.browser == this) g_run.reset();
        LevelBrowserLayer::onPrevPage(s);
    }

    void injectButton() {
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto* spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");

        CCMenuItemSpriteExtra* btn;
        if (spr) {
            spr->setScale(0.65f);
            auto* lbl = CCLabelBMFont::create("AUTO", "goldFont.fnt");
            lbl->setScale(0.45f);
            lbl->setAnchorPoint({0.5f, 0.5f});
            lbl->setPosition({spr->getContentSize().width * 0.5f, -12.f});
            spr->addChild(lbl);
            btn = CCMenuItemSpriteExtra::create(spr, nullptr, this,
                      menu_selector(AutoPlayerBrowserHook::onAutoPlay));
        } else {
            auto* s2 = ButtonSprite::create("AUTO", 80, true, "bigFont.fnt",
                                            "GJ_button_01.png", 25.f, 0.5f);
            btn = CCMenuItemSpriteExtra::create(s2, nullptr, this,
                      menu_selector(AutoPlayerBrowserHook::onAutoPlay));
        }
        btn->setID("autoplay-btn"_spr);
        auto* menu = CCMenu::create();
        menu->setID("autoplay-menu"_spr);
        menu->addChild(btn);
        menu->setPosition({winSize.width - 38.f, winSize.height - 50.f});
        menu->setZOrder(10);
        this->addChild(menu);
    }

    void onAutoPlay(cocos2d::CCObject*) {
        if (g_run.active && g_run.browser == this) {
            g_run.reset();
            FLAlertLayer::create("AutoPlayer",
                "Auto-play <cr>stopped</c>.\n<cy>F5</c> also stops anytime.", "OK")->show();
            return;
        }
        AutoMode mode = isMapPackList() ? AutoMode::MapPackList : AutoMode::Browser;
        g_run.reset();
        g_run.mode = mode; g_run.browser = this;

        if (mode == AutoMode::MapPackList) buildMapPackQueue();
        else                              buildBrowserQueue();

        if (g_run.queue.empty()) {
            FLAlertLayer::create("AutoPlayer",
                "No <cy>uncompleted</c> levels on this page!", "OK")->show();
            g_run.reset(); return;
        }
        g_run.active = true;
        int sz = (int)g_run.queue.size();
        FLAlertLayer::create("AutoPlayer",
            fmt::format("<cg>{}</c> levels queued!\n"
                        "Delay: <cy>{:.1f}s</c> (change in mod settings)\n"
                        "<cy>F5</c> or tap button to stop.", sz, getDelay()).c_str(),
            "GO!")->show();
        APDelayTimer::run(this, 1.2f, []() {
            if (g_run.active) downloadNext();
        });
    }
};

// ─────────────────────── GauntletLayer hook ──────────────────────────────────

struct AutoPlayerGauntletHook : Modify<AutoPlayerGauntletHook, GauntletLayer> {

    bool init(GauntletType type) {
        if (!GauntletLayer::init(type)) return false;
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto* spr = ButtonSprite::create("AUTO", 80, true, "bigFont.fnt",
                                         "GJ_button_02.png", 28.f, 0.6f);
        auto* btn = CCMenuItemSpriteExtra::create(spr, nullptr, this,
                        menu_selector(AutoPlayerGauntletHook::onAutoPlay));
        auto* menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({winSize.width - 38.f, winSize.height - 50.f});
        menu->setZOrder(10);
        this->addChild(menu);
        return true;
    }

    // setupGauntlet is unique to GauntletLayer — safe to hook unlike loadLevelsFinished
    // which shares a vtable slot with LevelBrowserLayer and causes cross-class crashes.
    void setupGauntlet(cocos2d::CCArray* levels) {
        GauntletLayer::setupGauntlet(levels);
        if (g_run.active && g_run.gauntlet == this && g_run.waitingForPage) {
            g_run.waitingForPage = false;
            buildGauntletQueue();
            if (!g_run.queue.empty()) downloadNext();
        }
    }

    void onEnterTransitionDidFinish() {
        GauntletLayer::onEnterTransitionDidFinish();
        if (g_run.active && g_run.gauntlet == this && g_run.justCompleted) {
            g_run.justCompleted = false;
            APDelayTimer::run(this, getDelay(), []() {
                if (g_run.active) downloadNext();
            });
        }
    }

    void onExit() {
        if (g_run.gauntlet == this) g_run.reset();
        GauntletLayer::onExit();
    }

    void onAutoPlay(cocos2d::CCObject*) {
        if (g_run.active && g_run.gauntlet == this) {
            g_run.reset();
            FLAlertLayer::create("AutoPlayer", "Auto-play <cr>stopped</c>.", "OK")->show();
            return;
        }
        g_run.reset();
        g_run.mode = AutoMode::Gauntlet;
        g_run.gauntlet = this;
        buildGauntletQueue();
        if (g_run.queue.empty()) {
            FLAlertLayer::create("AutoPlayer",
                "All gauntlet levels already <cg>complete</c>!", "OK")->show();
            g_run.reset(); return;
        }
        g_run.active = true;
        FLAlertLayer::create("AutoPlayer",
            fmt::format("<cg>{}</c> gauntlet levels queued!", g_run.queue.size()).c_str(),
            "GO!")->show();
        APDelayTimer::run(this, 1.2f, []() {
            if (g_run.active) downloadNext();
        });
    }
};

// ─────────────────────── LevelSelectLayer hook (main levels) ─────────────────

struct AutoPlayerMainHook : Modify<AutoPlayerMainHook, LevelSelectLayer> {

    bool init(int page) {
        if (!LevelSelectLayer::init(page)) return false;
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto* spr = ButtonSprite::create("AUTO\nMAIN", 80, true, "bigFont.fnt",
                                          "GJ_button_02.png", 28.f, 0.45f);
        auto* btn = CCMenuItemSpriteExtra::create(spr, nullptr, this,
                        menu_selector(AutoPlayerMainHook::onAutoMain));
        auto* menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({winSize.width - 42.f, winSize.height - 50.f});
        menu->setZOrder(10);
        this->addChild(menu);
        return true;
    }

    void onEnterTransitionDidFinish() {
        LevelSelectLayer::onEnterTransitionDidFinish();
        if (g_run.active && g_run.mode == AutoMode::MainLevels && g_run.justCompleted) {
            g_run.justCompleted = false;
            APDelayTimer::run(this, getDelay(), []() {
                if (g_run.active) downloadNext();
            });
        }
    }

    void onExit() {
        if (g_run.mode == AutoMode::MainLevels) g_run.reset();
        LevelSelectLayer::onExit();
    }

    void onAutoMain(cocos2d::CCObject*) {
        if (g_run.active && g_run.mode == AutoMode::MainLevels) {
            g_run.reset();
            FLAlertLayer::create("AutoPlayer", "Auto-play <cr>stopped</c>.", "OK")->show();
            return;
        }
        g_run.reset();
        g_run.mode = AutoMode::MainLevels;
        buildMainLevelQueue();
        if (g_run.queue.empty()) {
            FLAlertLayer::create("AutoPlayer",
                "All main levels are <cg>complete</c>! GG!", "OK")->show();
            g_run.reset(); return;
        }
        g_run.active = true;
        FLAlertLayer::create("AutoPlayer",
            fmt::format("<cg>{}</c>/22 main levels to play!", g_run.queue.size()).c_str(),
            "GO!")->show();
        APDelayTimer::run(this, 1.2f, []() {
            if (g_run.active) downloadNext();
        });
    }
};

// ─────────────────────── PlayLayer hook ──────────────────────────────────────

struct AutoPlayerPlayHook : Modify<AutoPlayerPlayHook, PlayLayer> {

    void levelComplete() {
        PlayLayer::levelComplete();
        if (!g_run.active) return;
        log::info("[AutoPlayer] Level complete — popping.");
        g_run.justCompleted = true;
        Loader::get()->queueInMainThread([]() {
            CCDirector::sharedDirector()->popScene();
        });
    }

    void onQuit() {
        if (g_run.active && !g_run.justCompleted) {
            log::info("[AutoPlayer] Manual quit — cancelling.");
            g_run.reset();
        }
        PlayLayer::onQuit();
    }
};
