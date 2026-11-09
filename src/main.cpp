#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

// ---------- helpers ----------

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool B(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
double F(char const* k) { return Mod::get()->getSettingValue<double>(k); }
int64_t I(char const* k) { return Mod::get()->getSettingValue<int64_t>(k); }
std::string S(char const* k) { return Mod::get()->getSettingValue<std::string>(k); }

double rnd(double a, double b) {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<double>(a, b)(rng);
}

struct Click {
    double down;
    double up; // < 0 while still held
};

// ---------- assist state (global, shared by all hooks) ----------

struct Assist {
    // real input tracking
    std::deque<Click> clicks;
    bool realHeld = false;
    double realDownAt = 0.0;
    double lastRealEvent = 0.0;

    // auto-click state
    bool active = false;
    bool autoDown = false;
    bool heldP1 = false;
    bool heldP2 = false;
    bool nextToP2 = false;
    double nextToggle = 0.0;
    double cps = 10.0;
    double drift = 0.0;
    double lockY = 0.0;
    int waveDir = 0; // -1 down, 0 straight, 1 up
    bool injecting = false;

    void prune(double now) {
        double w = static_cast<double>(I("window-ms"));
        while (!clicks.empty() && clicks.front().down < now - w) clicks.pop_front();
    }

    void onRealPress(double now) {
        realHeld = true;
        realDownAt = now;
        lastRealEvent = now;
        clicks.push_back({now, -1.0});
        prune(now);
    }

    void onRealRelease(double now) {
        realHeld = false;
        lastRealEvent = now;
        if (!clicks.empty() && clicks.back().up < 0.0) clicks.back().up = now;
    }

    // measures the rhythm of recent real presses
    bool measure(double& outCps, double& outCv, double& lastRatio) {
        size_t n = clicks.size();
        if (n < static_cast<size_t>(I("min-clicks"))) return false;
        std::vector<double> iv;
        for (size_t i = 1; i < n; i++) iv.push_back(clicks[i].down - clicks[i - 1].down);
        if (iv.size() < 2) return false;
        double mean = 0.0;
        for (double x : iv) mean += x;
        mean /= static_cast<double>(iv.size());
        if (mean <= 1.0) return false;
        double var = 0.0;
        for (double x : iv) var += (x - mean) * (x - mean);
        var /= static_cast<double>(iv.size());
        outCps = 1000.0 / mean;
        outCv = std::sqrt(var) / mean;
        lastRatio = iv.back() / mean;
        return true;
    }

    // average hold ratio of the user's real clicks -> wave direction
    int detectWaveDir() {
        double sum = 0.0;
        int cnt = 0;
        for (size_t i = 0; i + 1 < clicks.size(); i++) {
            if (clicks[i].up < 0.0) continue;
            double period = clicks[i + 1].down - clicks[i].down;
            if (period <= 1.0) continue;
            sum += (clicks[i].up - clicks[i].down) / period;
            cnt++;
        }
        if (cnt == 0) return 0;
        double avg = sum / cnt;
        double dz = static_cast<double>(I("wave-deadzone")) / 100.0;
        if (avg > 0.5 + dz) return 1;
        if (avg < 0.5 - dz) return -1;
        return 0;
    }

    // ---------- injection ----------

    void press(GJBaseGameLayer* g, bool forceBoth = false) {
        bool dual = g->m_gameState.m_isDualMode && g->m_player2;
        bool toP2 = false;
        bool both = false;
        if (dual) {
            if (!forceBoth && B("alternate-players")) {
                toP2 = nextToP2;
                nextToP2 = !nextToP2;
            } else {
                both = true;
            }
        }
        injecting = true;
        if (both || !toP2) {
            g->handleButton(true, 1, true);
            heldP1 = true;
        }
        if (both || toP2) {
            g->handleButton(true, 1, false);
            heldP2 = true;
        }
        injecting = false;
    }

    void release(GJBaseGameLayer* g) {
        injecting = true;
        if (heldP1) {
            g->handleButton(false, 1, true);
            heldP1 = false;
        }
        if (heldP2) {
            g->handleButton(false, 1, false);
            heldP2 = false;
        }
        injecting = false;
    }

    // ---------- activation / deactivation ----------

    double capCps(double c) { return std::clamp(c, 1.0, F("max-cps")); }

    double duty(GJBaseGameLayer* g) {
        auto p = g->m_player1;
        if (p->m_isDart) { // wave
            int dir = waveDir;
            std::string m = S("wave-direction");
            if (m == "Straight") dir = 0;
            else if (m == "Up") dir = 1;
            else if (m == "Down") dir = -1;
            double st = static_cast<double>(I("wave-steepness")) / 100.0;
            return dir > 0 ? st : (dir < 0 ? 1.0 - st : 0.5);
        }
        if (p->m_isShip && B("ship-straight")) {
            // simple PD controller around the locked height (gains to tune)
            double err = lockY - static_cast<double>(p->getPositionY());
            double v = static_cast<double>(p->m_yVelocity);
            double d = 0.5 + F("ship-correction") * (0.010 * err - 0.040 * v);
            return std::clamp(d, 0.15, 0.85);
        }
        return static_cast<double>(I("duty-cycle")) / 100.0;
    }

    void tryActivate(GJBaseGameLayer* g, double now) {
        if (active || !g->m_player1 || g->m_player1->m_isDead) return;
        prune(now);
        double c = 0, cv = 0, last = 1;
        if (!measure(c, cv, last)) return;
        if (c < F("cps-threshold")) return;
        if (B("smart-detect") && (cv > F("consistency") || last > 1.6)) return;

        bool dual = g->m_gameState.m_isDualMode && g->m_player2;
        active = true;
        autoDown = true; // the real press that triggered us already went through
        heldP1 = true;
        heldP2 = dual && !B("alternate-players");
        nextToP2 = true;
        drift = 0.0;
        lockY = static_cast<double>(g->m_player1->getPositionY());
        waveDir = detectWaveDir();
        cps = capCps(S("mode") == "Fixed" ? F("fixed-cps") : c);
        nextToggle = now + (1000.0 / cps) * duty(g);
    }

    void deactivate(GJBaseGameLayer* g) {
        if (!active) return;
        active = false;
        // hand control back so the game state matches the player's real finger
        if (autoDown && !realHeld) {
            release(g);
        } else if (!autoDown && realHeld) {
            press(g, true);
        } else {
            heldP1 = heldP2 = false;
        }
        autoDown = false;
        drift = 0.0;
        clicks.clear();
    }

    void reset() {
        active = false;
        autoDown = false;
        heldP1 = heldP2 = false;
        realHeld = false;
        drift = 0.0;
        clicks.clear();
    }

    // ---------- per-frame step (runs before the game's update) ----------

    void step(GJBaseGameLayer* g) {
        if (!active) return;
        double now = nowMs();
        auto p = g->m_player1;

        if (!B("enabled") || !p || p->m_isDead) {
            deactivate(g);
            return;
        }
        if (realHeld && now - realDownAt > static_cast<double>(I("hold-release-ms"))) {
            deactivate(g);
            return;
        }
        if (!realHeld && now - lastRealEvent > static_cast<double>(I("idle-timeout-ms"))) {
            deactivate(g);
            return;
        }

        // follow the user's speed (smoothed) in Match mode
        if (S("mode") == "Match") {
            prune(now);
            double c = 0, cv = 0, last = 1;
            if (measure(c, cv, last)) cps += (capCps(c) - cps) * 0.15;
        }
        if (p->m_isDart && S("wave-direction") == "Match Me") waveDir = detectWaveDir();

        if (now < nextToggle) return;

        bool ship = p->m_isShip;
        bool human = B("humanize") && (!ship || B("humanize-ship"));
        double c = capCps(cps + (human ? drift : 0.0));
        double period = 1000.0 / c;
        double d = duty(g);
        double holdT = period * d;
        double relT = period * (1.0 - d);
        double tv = human ? F("humanize-timing") / 100.0 : 0.0;
        auto jit = [&](double v) { return tv > 0.0 ? v * (1.0 + rnd(-tv, tv)) : v; };

        if (human) {
            double D = F("humanize-drift");
            drift = std::clamp(drift + rnd(-0.25, 0.25), -D, D);
        }
        if (now - nextToggle > period) nextToggle = now; // we lagged, resync

        if (autoDown) {
            release(g);
            autoDown = false;
            nextToggle += jit(relT);
        } else {
            bool miss = human && !ship && rnd(0.0, 100.0) < F("humanize-miss");
            if (miss) {
                nextToggle += period;
            } else {
                press(g);
                autoDown = true;
                nextToggle += jit(holdT);
            }
        }
    }
};

Assist& assist() {
    static Assist a;
    return a;
}

// ---------- on-screen indicator ----------

void setIndicator(bool on) {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_uiLayer) return;
    bool show = on && B("show-indicator");
    auto node = pl->m_uiLayer->getChildByID("spam-assist-label"_spr);
    if (!node && show) {
        auto label = CCLabelBMFont::create("Spam Assist", "bigFont.fnt");
        label->setID("spam-assist-label"_spr);
        label->setScale(0.35f);
        label->setOpacity(160);
        label->setAnchorPoint({0.f, 1.f});
        auto ws = CCDirector::get()->getWinSize();
        label->setPosition({6.f, ws.height - 6.f});
        pl->m_uiLayer->addChild(label, 100);
        node = label;
    }
    if (node) node->setVisible(show);
}

} // namespace

// ---------- hooks ----------

class $modify(SpamAssistLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& A = assist();
        if (A.injecting || button != 1 || !B("enabled")) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        double now = nowMs();
        if (isPlayer1) {
            if (down) A.onRealPress(now);
            else A.onRealRelease(now);
        }

        // while the assist is driving, real input is swallowed
        if (A.active) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (isPlayer1 && down) A.tryActivate(this, now);
    }

    void update(float dt) {
        auto& A = assist();
        A.step(this);
        GJBaseGameLayer::update(dt);
        setIndicator(A.active);
    }
};

class $modify(SpamAssistPlay, PlayLayer) {
    void resetLevel() {
        assist().reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        assist().reset();
        PlayLayer::onQuit();
    }
};

class $modify(SpamAssistPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn02_001.png");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SpamAssistPause::onAssistSettings));
        btn->setID("spam-assist-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto m = CCMenu::create();
            m->addChild(btn);
            m->setPosition({30.f, 30.f});
            this->addChild(m);
        }
    }

    void onAssistSettings(CCObject*) {
        openSettingsPopup(Mod::get(), false);
    }
};
