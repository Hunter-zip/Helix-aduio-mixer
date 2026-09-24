#include "helix/app/AppDetection.h"

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#include "helix/platform/WindowsAppDetector.h"
#endif

namespace helix::app {

// ── AppAssignments ──────────────────────────────────────────────────────────

std::string AppAssignments::normalize(const std::string& executable) {
    // Zostaw samą nazwę pliku, małymi literami.
    std::size_t start = executable.find_last_of("/\\");
    std::string name = (start == std::string::npos) ? executable : executable.substr(start + 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

void AppAssignments::set(const std::string& executable, std::string channelName) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[normalize(executable)] = std::move(channelName);
}

bool AppAssignments::remove(const std::string& executable) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(normalize(executable)) > 0;
}

void AppAssignments::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::string AppAssignments::lookup(const std::string& executable) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(normalize(executable));
    return it == entries_.end() ? std::string() : it->second;
}

std::map<std::string, std::string> AppAssignments::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

void AppAssignments::replaceAll(std::map<std::string, std::string> entries) {
    std::map<std::string, std::string> normalized;
    for (auto& [key, value] : entries) normalized[normalize(key)] = std::move(value);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(normalized);
}

// ── AppDetector ─────────────────────────────────────────────────────────────

void AppDetector::setHandler(Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

void AppDetector::poll() {
    std::vector<AudioApplication> current = enumerate();

    std::vector<AudioApplication> appeared;
    std::vector<AudioApplication> vanished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::uint32_t, AudioApplication> updated;

        for (auto& app : current) {
            const auto it = known_.find(app.processId);
            if (it == known_.end()) appeared.push_back(app);
            updated[app.processId] = std::move(app);
        }
        for (const auto& [pid, app] : known_)
            if (updated.find(pid) == updated.end()) vanished.push_back(app);

        known_ = std::move(updated);
    }

    Handler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = handler_;
    }
    if (!handler) return;

    for (const auto& app : appeared) handler(app, true);
    for (const auto& app : vanished) handler(app, false);
}

std::vector<AudioApplication> AppDetector::known() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioApplication> result;
    result.reserve(known_.size());
    for (const auto& [pid, app] : known_) result.push_back(app);
    return result;
}

// ── ManualAppDetector ───────────────────────────────────────────────────────

std::vector<AudioApplication> ManualAppDetector::enumerate() {
    std::lock_guard<std::mutex> lock(listMutex_);
    return applications_;
}

void ManualAppDetector::simulateStart(AudioApplication app) {
    std::lock_guard<std::mutex> lock(listMutex_);
    const auto it = std::find_if(applications_.begin(), applications_.end(),
                                 [&](const AudioApplication& a) { return a.processId == app.processId; });
    if (it == applications_.end()) applications_.push_back(std::move(app));
    else *it = std::move(app);
}

void ManualAppDetector::simulateStop(std::uint32_t processId) {
    std::lock_guard<std::mutex> lock(listMutex_);
    applications_.erase(std::remove_if(applications_.begin(), applications_.end(),
                                       [&](const AudioApplication& a) { return a.processId == processId; }),
                        applications_.end());
}

std::unique_ptr<AppDetector> createPlatformAppDetector() {
#if defined(_WIN32)
    if (auto detector = createWindowsAppDetector()) return detector;
#endif
    return std::make_unique<ManualAppDetector>();
}

} // namespace helix::app
