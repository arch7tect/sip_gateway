#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sip_gateway {

class TtsPipeline {
public:
    using SynthFn = std::function<std::optional<std::filesystem::path>(
        const std::string& text,
        const std::shared_ptr<std::atomic<bool>>& canceled)>;
    using ReadyFn = std::function<void(const std::filesystem::path& path,
                                       const std::string& text)>;
    using ReadySignalFn = std::function<void()>;

    TtsPipeline(int max_inflight,
                SynthFn synth_fn,
                ReadyFn ready_fn,
                ReadySignalFn ready_signal_fn);

    void enqueue(const std::string& text, double delay_sec);
    void cancel();
    bool has_queue() const;
    void try_play(bool can_play);

private:
    struct TtsTask {
        std::string text;
        std::shared_future<std::optional<std::filesystem::path>> future;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    struct PendingTtsTask {
        std::string text;
        std::shared_ptr<std::packaged_task<std::optional<std::filesystem::path>()>> task;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    void maybe_start_synthesis();
    void on_synthesis_finished();

    int max_inflight_;
    SynthFn synth_fn_;
    ReadyFn ready_fn_;
    ReadySignalFn ready_signal_fn_;

    mutable std::mutex mutex_;
    std::deque<TtsTask> queue_;
    std::deque<PendingTtsTask> pending_;
    size_t inflight_ = 0;
};

}
