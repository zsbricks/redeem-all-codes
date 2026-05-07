#include <Geode/Geode.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/FLAlertLayer.hpp>
#include <Geode/binding/GameLevelManager.hpp>

#include <matjson.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    bool g_redeemRunning = false;
    bool g_fetchRunning = false;

    struct DatabaseParseResult {
        std::vector<std::string> codes;
        std::string version;
    };

    std::string trim(std::string value) {
        auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool isReasonableCode(std::string const& code) {
        if (code.empty() || code.size() > 64) return false;
        return std::all_of(code.begin(), code.end(), [](unsigned char ch) {
            return ch >= 32 && ch <= 126;
        });
    }

    void pushUnique(std::vector<std::string>& out, std::set<std::string>& seen, std::string code) {
        code = trim(code);
        if (!isReasonableCode(code)) return;
        if (seen.insert(code).second) {
            out.push_back(std::move(code));
        }
    }

    std::vector<std::string> builtInCodes() {
        std::vector<std::string> out;
        std::set<std::string> seen;

        // Built-in fallback list. Keep codes.json on GitHub as the real live database.
        // Some are condition-based, seasonal, puzzle-based, or already-redeemed; the game will reject those normally.
        static constexpr std::string_view baseCodes[] = {
            // The Vault
            "lenny", "blockbite", "spooky", "neverending", "mule", "ahead", "gandalfpotter",
            "sparky", "robotop", "finalboss", "thechickenisready", "givemehelper", "backontrack",

            // Special Vault number sequence; sent one by one on purpose.
            "8", "16", "30", "32", "46", "84",

            // Vault of Secrets
            "octocube", "brainpower", "brain power", "seven", "gimmiethecolor", "the challenge",
            "glubfub", "cod3breaker", "d4sHg30mE7ry", "geometrydash.com",

            // Chamber of Time
            "volcano", "river", "silence", "darkness", "hunger",

            // Wraith Vault / 2.2+ public reward codes
            "spacegauntlet", "iaminpain", "gd2025", "checksteam", "key", "wellmet",
            "fireinthehole", "wateronthehill", "touchgrass", "bussin", "67", "robtopisnice",
            "skibidi", "thickofit", "gullible", "kingsamgd", "v0rtrox", "backondash",
            "citadel", "skylinept2", "brainpowah47", "randomgauntlet", "dualitygauntlet",
            "gdawards", "geometry", "ruins", "retrospective", "duckstep", "cheatcodes",
            "buttonmasher", "putyahandsup", "ravenousbeasts", "boogie", "backstreetboy",
            "noelelectra", "ncsalbum"
        };

        for (auto code : baseCodes) {
            pushUnique(out, seen, std::string(code));
        }
        return out;
    }

    std::filesystem::path cachePath() {
        return Mod::get()->getSaveDir() / "codes-cache.json";
    }

    std::optional<std::string> readTextFile(std::filesystem::path const& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    bool writeTextFile(std::filesystem::path const& path, std::string const& text) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << text;
        return file.good();
    }

    void collectCodesFromJson(matjson::Value const& value, std::vector<std::string>& out, std::set<std::string>& seen) {
        if (value.isString()) {
            if (auto str = value.asString()) {
                pushUnique(out, seen, str.unwrap());
            }
            return;
        }

        if (value.isArray()) {
            for (auto const& item : value) {
                collectCodesFromJson(item, out, seen);
            }
            return;
        }

        if (!value.isObject()) return;

        // Supported object shapes:
        // { "code": "lenny" }
        // { "codes": ["lenny", {"code":"blockbite"}] }
        // { "categories": { "vault": [ ... ], "wraith": [ ... ] } }
        auto const& directCode = value["code"];
        if (directCode.isString()) {
            if (auto str = directCode.asString()) {
                pushUnique(out, seen, str.unwrap());
            }
        }

        static constexpr std::string_view arrayKeys[] = {
            "codes", "items", "entries", "values"
        };
        for (auto key : arrayKeys) {
            auto const& node = value[key];
            if (!node.isNull()) {
                collectCodesFromJson(node, out, seen);
            }
        }

        auto const& categories = value["categories"];
        if (categories.isArray()) {
            collectCodesFromJson(categories, out, seen);
        }
        else if (categories.isObject()) {
            for (auto const& category : categories) {
                collectCodesFromJson(category, out, seen);
            }
        }
    }

    DatabaseParseResult parseDatabase(std::string const& raw) {
        DatabaseParseResult result;
        std::set<std::string> seen;

        auto parsed = matjson::parse(raw);
        if (!parsed) {
            log::warn("Could not parse code database JSON: {}", parsed.unwrapErr());
            return result;
        }

        auto root = parsed.unwrap();
        if (root.isObject()) {
            if (auto version = root["version"].asString()) {
                result.version = version.unwrap();
            }
        }

        collectCodesFromJson(root, result.codes, seen);
        return result;
    }

    std::vector<std::string> appendVariableCodes(std::vector<std::string> codes) {
        std::set<std::string> seen(codes.begin(), codes.end());

        auto playerName = Mod::get()->getSettingValue<std::string>("player-name");
        pushUnique(codes, seen, playerName);

        auto stars = Mod::get()->getSettingValue<int64_t>("star-count");
        if (stars > 0) {
            pushUnique(codes, seen, std::to_string(stars));
        }

        return codes;
    }

    std::vector<std::string> loadCachedOrBuiltInCodes(std::string* sourceLabel = nullptr) {
        if (auto cachedRaw = readTextFile(cachePath())) {
            auto parsed = parseDatabase(*cachedRaw);
            if (!parsed.codes.empty()) {
                if (sourceLabel) {
                    *sourceLabel = parsed.version.empty()
                        ? "cached online database"
                        : fmt::format("cached online database ({})", parsed.version);
                }
                return parsed.codes;
            }
        }

        if (sourceLabel) *sourceLabel = "built-in fallback list";
        return builtInCodes();
    }

    class RedeemQueueLayer : public CCLayer {
    protected:
        std::vector<std::string> m_codes;
        std::string m_sourceLabel;
        size_t m_index = 0;
        float m_delaySeconds = 1.75f;

        bool init(std::vector<std::string> codes, std::string sourceLabel, float delaySeconds) {
            if (!CCLayer::init()) return false;
            m_codes = std::move(codes);
            m_sourceLabel = std::move(sourceLabel);
            m_delaySeconds = delaySeconds;
            this->setID("redeem-all-codes-queue");
            g_redeemRunning = true;

            if (m_codes.empty()) {
                FLAlertLayer::create("Redeem Codes", "No codes are in the queue.", "OK")->show();
                g_redeemRunning = false;
                return true;
            }

            FLAlertLayer::create(
                "Redeem Codes",
                fmt::format(
                    "Started redeem queue for <cg>{}</c> codes from <cy>{}</c>. Do not spam the button.",
                    m_codes.size(), m_sourceLabel
                ),
                "OK"
            )->show();

            this->redeemNext(0.0f);
            this->schedule(schedule_selector(RedeemQueueLayer::redeemNext), m_delaySeconds);
            return true;
        }

        void redeemNext(float) {
            if (m_index >= m_codes.size()) {
                this->unschedule(schedule_selector(RedeemQueueLayer::redeemNext));
                g_redeemRunning = false;
                FLAlertLayer::create(
                    "Redeem Codes",
                    "Finished sending all known public codes. Some may fail if they are already redeemed, limited, condition-based, or require a specific vault/progression state.",
                    "OK"
                )->show();
                this->removeFromParentAndCleanup(true);
                return;
            }

            auto code = m_codes[m_index++];
            log::info("Redeeming code {}/{}: {}", m_index, m_codes.size(), code);

            if (auto glm = GameLevelManager::sharedState()) {
                auto sent = glm->getGJSecretReward(gd::string(code));
                if (!sent) {
                    log::warn("getGJSecretReward returned false for '{}'", code);
                }
            }
            else {
                this->unschedule(schedule_selector(RedeemQueueLayer::redeemNext));
                g_redeemRunning = false;
                FLAlertLayer::create("Redeem Codes", "Could not access GameLevelManager.", "OK")->show();
                this->removeFromParentAndCleanup(true);
            }
        }

    public:
        static RedeemQueueLayer* create(std::vector<std::string> codes, std::string sourceLabel, float delaySeconds) {
            auto ret = new RedeemQueueLayer();
            if (ret && ret->init(std::move(codes), std::move(sourceLabel), delaySeconds)) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }
    };

    void startRedeemQueue(std::vector<std::string> codes, std::string sourceLabel) {
        if (g_redeemRunning) {
            FLAlertLayer::create("Redeem Codes", "A redeem queue is already running.", "OK")->show();
            return;
        }

        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            FLAlertLayer::create("Redeem Codes", "No running scene found.", "OK")->show();
            return;
        }

        auto delayMs = Mod::get()->getSettingValue<int64_t>("delay-ms");
        delayMs = std::clamp<int64_t>(delayMs, 1000, 10000);
        auto layer = RedeemQueueLayer::create(appendVariableCodes(std::move(codes)), std::move(sourceLabel), static_cast<float>(delayMs) / 1000.0f);
        scene->addChild(layer, 9999);
    }

    void startRedeemFromLocalDatabase() {
        std::string sourceLabel;
        auto codes = loadCachedOrBuiltInCodes(&sourceLabel);
        startRedeemQueue(std::move(codes), std::move(sourceLabel));
    }

    void handleFetchedDatabase(web::WebResponse response, bool redeemAfterUpdate) {
        g_fetchRunning = false;

        if (!response.ok()) {
            log::warn("Code database HTTP request failed with status {}", response.code());
            FLAlertLayer::create(
                "Code Database",
                fmt::format("Could not update online database. HTTP status: <cr>{}</c>. Using cache/fallback instead.", response.code()),
                "OK"
            )->show();
            if (redeemAfterUpdate) startRedeemFromLocalDatabase();
            return;
        }

        auto bodyResult = response.string();
        if (!bodyResult) {
            FLAlertLayer::create("Code Database", "Could not read the online database response body. Using cache/fallback instead.", "OK")->show();
            if (redeemAfterUpdate) startRedeemFromLocalDatabase();
            return;
        }

        auto body = bodyResult.unwrap();
        auto parsed = parseDatabase(body);
        if (parsed.codes.empty()) {
            FLAlertLayer::create("Code Database", "Online database was downloaded, but no valid codes were found. Using cache/fallback instead.", "OK")->show();
            if (redeemAfterUpdate) startRedeemFromLocalDatabase();
            return;
        }

        auto saved = writeTextFile(cachePath(), body);
        auto sourceLabel = parsed.version.empty()
            ? "online database"
            : fmt::format("online database ({})", parsed.version);

        if (!redeemAfterUpdate) {
            FLAlertLayer::create(
                "Code Database",
                fmt::format(
                    "Updated <cg>{}</c> codes from the online database.{}",
                    parsed.codes.size(), saved ? "" : " Cache save failed, but the download worked."
                ),
                "OK"
            )->show();
            return;
        }

        startRedeemQueue(std::move(parsed.codes), std::move(sourceLabel));
    }

    void fetchOnlineDatabase(bool redeemAfterUpdate) {
        if (g_fetchRunning) {
            FLAlertLayer::create("Code Database", "A database update is already running.", "OK")->show();
            return;
        }

        auto url = trim(Mod::get()->getSettingValue<std::string>("database-url"));
        if (url.empty()) {
            FLAlertLayer::create("Code Database", "Database URL is empty. Using cache/fallback instead.", "OK")->show();
            if (redeemAfterUpdate) startRedeemFromLocalDatabase();
            return;
        }

        g_fetchRunning = true;
        log::info("Fetching code database from {}", url);

        auto request = web::WebRequest();
        request.userAgent("zbricks.redeem-all-codes/1.1.0 Geode");
        request.timeout(std::chrono::seconds(15));
        request.followRedirects(true);

        async::spawn(
            request.get(url, Mod::get()),
            [redeemAfterUpdate](web::WebResponse response) {
                handleFetchedDatabase(std::move(response), redeemAfterUpdate);
            }
        ).setName("Redeem All Codes: Fetch Code Database");
    }

    void redeemButtonPressed() {
        auto useOnline = Mod::get()->getSettingValue<bool>("use-online-database");
        auto checkBeforeRedeem = Mod::get()->getSettingValue<bool>("check-before-redeem");

        if (useOnline && checkBeforeRedeem) {
            fetchOnlineDatabase(true);
        }
        else {
            startRedeemFromLocalDatabase();
        }
    }
}

class ActionButtonSetting : public SettingV3 {
protected:
    std::string m_buttonText = "Run";
    std::string m_action = "redeem";

public:
    static Result<std::shared_ptr<SettingV3>> parse(std::string const& key, std::string const& modID, matjson::Value const& json) {
        auto res = std::make_shared<ActionButtonSetting>();
        auto root = checkJson(json, "ActionButtonSetting");
        res->init(key, modID, root);
        res->parseNameAndDescription(root);
        res->parseEnableIf(root);
        root.has("button-text").into(res->m_buttonText);
        root.has("action").into(res->m_action);
        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(res));
    }

    std::string const& getButtonText() const {
        return m_buttonText;
    }

    std::string const& getAction() const {
        return m_action;
    }

    bool load(matjson::Value const&) override { return true; }
    bool save(matjson::Value&) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}

    SettingNodeV3* createNode(float width) override;
};

class ActionButtonSettingNode : public SettingNodeV3 {
protected:
    ButtonSprite* m_buttonSprite = nullptr;
    CCMenuItemSpriteExtra* m_button = nullptr;

    bool init(std::shared_ptr<ActionButtonSetting> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;

        m_buttonSprite = ButtonSprite::create(setting->getButtonText().c_str(), "goldFont.fnt", "GJ_button_01.png", 0.8f);
        m_buttonSprite->setScale(0.55f);
        m_button = CCMenuItemSpriteExtra::create(m_buttonSprite, this, menu_selector(ActionButtonSettingNode::onButton));

        this->getButtonMenu()->addChildAtPosition(m_button, Anchor::Center);
        this->getButtonMenu()->setContentWidth(82);
        this->getButtonMenu()->updateLayout();
        this->updateState(nullptr);
        return true;
    }

    void updateState(CCNode* invoker) override {
        SettingNodeV3::updateState(invoker);
        auto shouldEnable = this->getSetting()->shouldEnable();
        m_button->setEnabled(shouldEnable);
        m_buttonSprite->setCascadeColorEnabled(true);
        m_buttonSprite->setCascadeOpacityEnabled(true);
        m_buttonSprite->setOpacity(shouldEnable ? 255 : 155);
        m_buttonSprite->setColor(shouldEnable ? ccWHITE : ccGRAY);
    }

    void onButton(CCObject*) {
        auto action = this->getSetting()->getAction();
        if (action == "update-database") {
            fetchOnlineDatabase(false);
        }
        else {
            redeemButtonPressed();
        }
    }

    void onCommit() override {}
    void onResetToDefault() override {}

public:
    static ActionButtonSettingNode* create(std::shared_ptr<ActionButtonSetting> setting, float width) {
        auto ret = new ActionButtonSettingNode();
        if (ret && ret->init(setting, width)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }

    std::shared_ptr<ActionButtonSetting> getSetting() const {
        return std::static_pointer_cast<ActionButtonSetting>(SettingNodeV3::getSetting());
    }
};

SettingNodeV3* ActionButtonSetting::createNode(float width) {
    return ActionButtonSettingNode::create(std::static_pointer_cast<ActionButtonSetting>(shared_from_this()), width);
}

$on_mod(Loaded) {
    (void)Mod::get()->registerCustomSettingType("action-button", &ActionButtonSetting::parse);
}
