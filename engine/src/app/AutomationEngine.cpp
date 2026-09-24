#include "helix/app/AutomationEngine.h"

#include <algorithm>
#include <cctype>

#include "helix/Log.h"

namespace helix::app {

namespace {
constexpr const char* kLog = "Automation";

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}
} // namespace

const char* triggerTypeName(TriggerType type) noexcept {
    switch (type) {
        case TriggerType::Manual:             return "manual";
        case TriggerType::AppStarted:         return "appStarted";
        case TriggerType::AppStopped:         return "appStopped";
        case TriggerType::ProfileActivated:   return "profileActivated";
        case TriggerType::DeviceConnected:    return "deviceConnected";
        case TriggerType::DeviceDisconnected: return "deviceDisconnected";
        case TriggerType::EngineStarted:      return "engineStarted";
        case TriggerType::ChannelMuted:       return "channelMuted";
        case TriggerType::ChannelUnmuted:     return "channelUnmuted";
    }
    return "manual";
}

bool parseTriggerType(const std::string& name, TriggerType& out) noexcept {
    static const std::pair<const char*, TriggerType> kMap[] = {
        {"manual",             TriggerType::Manual},
        {"appStarted",         TriggerType::AppStarted},
        {"appStopped",         TriggerType::AppStopped},
        {"profileActivated",   TriggerType::ProfileActivated},
        {"deviceConnected",    TriggerType::DeviceConnected},
        {"deviceDisconnected", TriggerType::DeviceDisconnected},
        {"engineStarted",      TriggerType::EngineStarted},
        {"channelMuted",       TriggerType::ChannelMuted},
        {"channelUnmuted",     TriggerType::ChannelUnmuted},
    };
    for (const auto& [text, value] : kMap) {
        if (name == text) { out = value; return true; }
    }
    return false;
}

JsonValue AutomationRule::toJson() const {
    JsonValue value = JsonValue::makeObject();
    value.set("id", JsonValue(id));
    value.set("name", JsonValue(name));
    value.set("enabled", JsonValue(enabled));
    value.set("trigger", JsonValue(triggerTypeName(trigger)));
    value.set("condition", condition.isNull() ? JsonValue::makeObject() : condition);

    JsonValue list = JsonValue::makeArray();
    for (const auto& action : actions) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("action", JsonValue(action.action));
        entry.set("args", action.args.isNull() ? JsonValue::makeObject() : action.args);
        list.push(std::move(entry));
    }
    value.set("actions", std::move(list));
    return value;
}

bool AutomationRule::fromJson(const JsonValue& value, AutomationRule& out, std::string& error) {
    if (!value.isObject()) { error = "reguła musi być obiektem"; return false; }

    out.id = value["id"].asString();
    if (out.id.empty()) { error = "reguła wymaga pola \"id\""; return false; }

    out.name    = value["name"].asString(out.id);
    out.enabled = value["enabled"].asBool(true);

    const std::string trigger = value["trigger"].asString("manual");
    if (!parseTriggerType(trigger, out.trigger)) {
        error = "nieznany wyzwalacz: " + trigger;
        return false;
    }

    out.condition = value["condition"];
    out.actions.clear();

    const JsonValue& actions = value["actions"];
    if (!actions.isArray()) { error = "pole \"actions\" musi być tablicą"; return false; }

    for (std::size_t i = 0; i < actions.size(); ++i) {
        const JsonValue& entry = actions[i];
        AutomationAction action;
        action.action = entry["action"].asString();
        if (action.action.empty()) { error = "akcja wymaga pola \"action\""; return false; }
        action.args = entry["args"];
        out.actions.push_back(std::move(action));
    }
    return true;
}

void AutomationEngine::setExecutor(Executor executor) {
    std::lock_guard<std::mutex> lock(mutex_);
    executor_ = std::move(executor);
}

Status AutomationEngine::addRule(AutomationRule rule) {
    if (rule.id.empty()) return Status::error("Reguła wymaga identyfikatora");

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(rules_.begin(), rules_.end(),
                                 [&](const AutomationRule& r) { return r.id == rule.id; });
    if (it != rules_.end()) *it = std::move(rule);
    else rules_.push_back(std::move(rule));
    return Status::success();
}

bool AutomationEngine::removeRule(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                                [&](const AutomationRule& r) { return r.id == id; }),
                 rules_.end());
    return rules_.size() != before;
}

bool AutomationEngine::setRuleEnabled(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& rule : rules_) {
        if (rule.id == id) { rule.enabled = enabled; return true; }
    }
    return false;
}

void AutomationEngine::setRules(std::vector<AutomationRule> rules) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_ = std::move(rules);
}

void AutomationEngine::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.clear();
}

std::vector<AutomationRule> AutomationEngine::rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

bool AutomationEngine::matches(const JsonValue& condition, const JsonValue& context) {
    if (!condition.isObject() || condition.members().empty()) return true;

    for (const auto& [key, expected] : condition.members()) {
        const JsonValue& actual = context[key];
        if (expected.isString() && actual.isString()) {
            if (!equalsIgnoreCase(expected.asString(), actual.asString())) return false;
        } else if (expected.isNumber()) {
            if (expected.asNumber() != actual.asNumber()) return false;
        } else if (expected.isBool()) {
            if (expected.asBool() != actual.asBool()) return false;
        } else if (expected.isNull()) {
            if (!actual.isNull()) return false;
        } else {
            return false;  // złożone warunki nie są (jeszcze) wspierane
        }
    }
    return true;
}

int AutomationEngine::fire(TriggerType trigger, const JsonValue& context) {
    std::vector<AutomationRule> matched;
    Executor executor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        executor = executor_;
        for (const auto& rule : rules_) {
            if (!rule.enabled || rule.trigger != trigger) continue;
            if (!matches(rule.condition, context)) continue;
            matched.push_back(rule);
        }
    }

    if (!executor) return 0;

    int executed = 0;
    for (const auto& rule : matched) {
        for (const auto& action : rule.actions) {
            // Kontekst zdarzenia uzupełnia argumenty akcji — dzięki temu reguła
            // może odwołać się np. do PID-u aplikacji, która właśnie wystartowała.
            JsonValue args = action.args.isObject() ? action.args : JsonValue::makeObject();
            if (context.isObject()) {
                for (const auto& [key, value] : context.members())
                    if (!args.contains(key)) args.set(key, value);
            }

            const Status status = executor(action.action, args);
            if (!status)
                Log::warn(kLog, "Reguła \"" + rule.name + "\": " + action.action + " — " + status.message);
            else
                ++executed;
        }
    }
    return executed;
}

std::vector<AutomationRule> AutomationEngine::defaultRules() {
    auto assignRule = [](const char* id, const char* name, const char* executable, const char* channel) {
        AutomationRule rule;
        rule.id      = id;
        rule.name    = name;
        rule.trigger = TriggerType::AppStarted;
        rule.condition = JsonValue::makeObject();
        rule.condition.set("executable", JsonValue(executable));

        AutomationAction action;
        action.action = "app.assign";
        action.args   = JsonValue::makeObject();
        action.args.set("executable", JsonValue(executable));
        action.args.set("channel", JsonValue(channel));
        rule.actions.push_back(std::move(action));
        return rule;
    };

    std::vector<AutomationRule> rules{
        assignRule("auto-discord", "Discord → Chat",  "Discord.exe", "Chat"),
        assignRule("auto-spotify", "Spotify → Music", "Spotify.exe", "Music"),
        assignRule("auto-chrome",  "Chrome → Media",  "chrome.exe",  "Media"),
    };

    // Profil Streaming włącza magistralę streamową.
    AutomationRule streaming;
    streaming.id      = "streaming-bus";
    streaming.name    = "Profil Streaming → włącz Stream Output";
    streaming.trigger = TriggerType::ProfileActivated;
    streaming.condition = JsonValue::makeObject();
    streaming.condition.set("profile", JsonValue("Streaming"));

    AutomationAction enableStream;
    enableStream.action = "virtual.setEnabled";
    enableStream.args   = JsonValue::makeObject();
    enableStream.args.set("id", JsonValue("stream-output"));
    enableStream.args.set("enabled", JsonValue(true));
    streaming.actions.push_back(std::move(enableStream));

    rules.push_back(std::move(streaming));
    return rules;
}

} // namespace helix::app
