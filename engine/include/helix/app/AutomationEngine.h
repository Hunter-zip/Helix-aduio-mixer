// Reguły automatyzacji (spec §16).
//
// Model wyzwalacz → warunek → akcje jest celowo ogólny: dołożenie nowego
// wyzwalacza lub akcji nie wymaga zmian w silniku ani w GUI.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/app/Json.h"

namespace helix::app {

enum class TriggerType : std::uint8_t {
    Manual,
    AppStarted,
    AppStopped,
    ProfileActivated,
    DeviceConnected,
    DeviceDisconnected,
    EngineStarted,
    ChannelMuted,
    ChannelUnmuted
};

[[nodiscard]] const char* triggerTypeName(TriggerType type) noexcept;
[[nodiscard]] bool parseTriggerType(const std::string& name, TriggerType& out) noexcept;

/// Pojedyncza akcja: nazwa komendy sterującej + argumenty.
struct AutomationAction {
    std::string action;
    JsonValue   args;
};

struct AutomationRule {
    std::string id;
    std::string name;
    bool        enabled = true;
    TriggerType trigger = TriggerType::Manual;
    /// Warunek: wszystkie pola muszą pasować do kontekstu zdarzenia
    /// (porównanie napisów bez uwzględniania wielkości liter).
    JsonValue   condition;
    std::vector<AutomationAction> actions;

    [[nodiscard]] JsonValue toJson() const;
    [[nodiscard]] static bool fromJson(const JsonValue& value, AutomationRule& out, std::string& error);
};

class AutomationEngine {
public:
    /// Wykonawca akcji — w praktyce dyspozytor komend EngineControllera.
    using Executor = std::function<Status(const std::string& action, const JsonValue& args)>;

    void setExecutor(Executor executor);

    Status addRule(AutomationRule rule);
    bool   removeRule(const std::string& id);
    bool   setRuleEnabled(const std::string& id, bool enabled);
    void   setRules(std::vector<AutomationRule> rules);
    void   clear();

    [[nodiscard]] std::vector<AutomationRule> rules() const;

    /// Uruchamia reguły pasujące do zdarzenia. Zwraca liczbę wykonanych akcji.
    int fire(TriggerType trigger, const JsonValue& context);

    /// Domyślne reguły ze specyfikacji §16.
    [[nodiscard]] static std::vector<AutomationRule> defaultRules();

    /// Czy warunek pasuje do kontekstu.
    [[nodiscard]] static bool matches(const JsonValue& condition, const JsonValue& context);

private:
    mutable std::mutex          mutex_;
    std::vector<AutomationRule> rules_;
    Executor                    executor_;
};

} // namespace helix::app
