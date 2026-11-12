#pragma once
// Session kind `custom`: a user-registered service plugin. Lets an app call a non-neural engine (e.g. Cypha)
// through the same inference entry points as neural models, without that engine depending on inference: the engine
// side only implements `service` (plain std types); the app registers it under a name; a blueprint with
// `session custom` + manifest key "plugin" (or an explicit name) resolves it.
#include <forge/core/error.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace forge::inference::plugin {

using tensors = std::map<std::string, std::vector<float>>;
struct service {
    virtual ~service() = default;
    virtual std::string name() const = 0;
    virtual tensors run(const tensors& in) = 0;                 // one call = one request
    virtual std::string describe() const { return name(); }
};
using factory = std::function<std::unique_ptr<service>(const std::string& config)>;

class registry {
public:
    static registry& global() { static registry r; return r; }
    void add(const std::string& kind, factory f) { std::lock_guard l(m_); f_[kind] = std::move(f); }
    bool remove(const std::string& kind) { std::lock_guard l(m_); return f_.erase(kind) != 0; }
    bool has(const std::string& kind) const { std::lock_guard l(m_); return f_.count(kind) != 0; }
    std::vector<std::string> kinds() const { std::lock_guard l(m_); std::vector<std::string> k; for (auto& [n, _] : f_) k.push_back(n); return k; }
    std::unique_ptr<service> make(const std::string& kind, const std::string& config = {}) const {
        factory f;
        { std::lock_guard l(m_); auto it = f_.find(kind); if (it == f_.end()) raise(errc::not_found, "custom session plugin '{}' is not registered", kind); f = it->second; }
        auto s = f(config);
        if (!s) raise(errc::unsupported, "custom session plugin '{}' factory returned null", kind);
        return s;
    }
private:
    mutable std::mutex m_; std::map<std::string, factory> f_;
};

// Static registration helper: `static forge::inference::plugin::registrar r{"cypha", [](auto&){...}};`
struct registrar { registrar(const std::string& kind, factory f) { registry::global().add(kind, std::move(f)); } };

// A session over a plugin: same infer() shape as inference::session's one_shot path.
class session {
public:
    explicit session(const std::string& kind, const std::string& config = {}) : s_(registry::global().make(kind, config)) {}
    tensors infer(const tensors& in) { std::lock_guard l(m_); return s_->run(in); }
    service& get() { return *s_; }
private:
    std::unique_ptr<service> s_; std::mutex m_;
};

} // namespace forge::inference::plugin
