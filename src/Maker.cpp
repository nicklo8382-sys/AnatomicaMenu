#include "Maker.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <iterator>
#include <unordered_map>

namespace {
    std::mutex g_mx;
    std::condition_variable g_cv;
    std::deque<std::pair<std::string, std::function<bool()>>> g_jobs;
    std::unordered_map<std::string, Maker::State> g_state;
    bool g_started = false;

    void Worker() {
        for (;;) {
            std::pair<std::string, std::function<bool()>> job;
            {
                std::unique_lock lk(g_mx);
                g_cv.wait(lk, [] { return !g_jobs.empty(); });
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
            }
            bool ok = false;
            try { ok = job.second && job.second(); } catch (...) { ok = false; }
            std::lock_guard lk(g_mx);
            g_state[job.first] = ok ? Maker::State::done : Maker::State::failed;
        }
    }
}

namespace Maker {
    State Of(const std::string& key) {
        std::lock_guard lk(g_mx);
        auto it = g_state.find(key);
        return it == g_state.end() ? State::none : it->second;
    }
    void MarkDone(const std::string& key) { std::lock_guard lk(g_mx); g_state[key] = State::done; }
    void ForgetDone() {
        std::lock_guard lk(g_mx);
        for (auto it = g_state.begin(); it != g_state.end();) it = it->second == State::done ? g_state.erase(it) : std::next(it);
    }
    void Queue(const std::string& key, std::function<bool()> job) {
        std::lock_guard lk(g_mx);
        auto& st = g_state[key];
        if (st == State::busy || st == State::done) return;
        st = State::busy;
        g_jobs.emplace_back(key, std::move(job));
        if (!g_started) { g_started = true; std::thread(Worker).detach(); }
        g_cv.notify_one();
    }
}
