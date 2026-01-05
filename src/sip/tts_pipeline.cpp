#include "sip_gateway/sip/tts_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "sip_gateway/utils/async.hpp"

namespace sip_gateway {

TtsPipeline::TtsPipeline(int max_inflight,
                         SynthFn synth_fn,
                         ReadyFn ready_fn,
                         ReadySignalFn ready_signal_fn)
    : max_inflight_(max_inflight),
      synth_fn_(std::move(synth_fn)),
      ready_fn_(std::move(ready_fn)),
      ready_signal_fn_(std::move(ready_signal_fn)) {}

void TtsPipeline::enqueue(const std::string& text, double delay_sec) {
    if (delay_sec > 0.0) {
        utils::run_async([this, text, delay_sec]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(delay_sec));
            enqueue(text, 0.0);
        });
        return;
    }

    auto canceled = std::make_shared<std::atomic<bool>>(false);
    auto task_ptr = std::make_shared<
        std::packaged_task<std::optional<std::filesystem::path>()>>(
        [this, text, canceled]() -> std::optional<std::filesystem::path> {
            return synth_fn_(text, canceled);
        });
    auto future = task_ptr->get_future().share();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({text, future, canceled});
        pending_.push_back({text, task_ptr, canceled});
    }

    maybe_start_synthesis();
    if (ready_signal_fn_) {
        ready_signal_fn_();
    }
}

void TtsPipeline::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& task : queue_) {
        if (task.canceled) {
            task.canceled->store(true);
        }
    }
    for (auto& task : pending_) {
        if (task.canceled) {
            task.canceled->store(true);
        }
    }
    queue_.clear();
    pending_.clear();
}

bool TtsPipeline::has_queue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !queue_.empty();
}

void TtsPipeline::try_play(bool can_play) {
    if (!can_play) {
        return;
    }
    while (true) {
        TtsTask task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return;
            }
            auto& front = queue_.front();
            if (front.future.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
                return;
            }
            task = front;
            queue_.pop_front();
        }

        if (task.canceled && task.canceled->load()) {
            continue;
        }

        std::optional<std::filesystem::path> audio_path;
        try {
            audio_path = task.future.get();
        } catch (...) {
            continue;
        }
        if (!audio_path || audio_path->empty()) {
            continue;
        }
        if (ready_fn_) {
            ready_fn_(*audio_path, task.text);
        }
    }
}

void TtsPipeline::maybe_start_synthesis() {
    std::vector<PendingTtsTask> to_start;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto max_inflight = static_cast<size_t>(
            std::max(1, max_inflight_));
        while (inflight_ < max_inflight && !pending_.empty()) {
            PendingTtsTask task = std::move(pending_.front());
            pending_.pop_front();
            if (task.canceled && task.canceled->load()) {
                continue;
            }
            ++inflight_;
            to_start.push_back(std::move(task));
        }
    }

    for (auto& task : to_start) {
        utils::run_async([this, task_ptr = task.task]() {
            (*task_ptr)();
            on_synthesis_finished();
        });
    }
}

void TtsPipeline::on_synthesis_finished() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_ > 0) {
            --inflight_;
        }
    }
    if (ready_signal_fn_) {
        ready_signal_fn_();
    }
    maybe_start_synthesis();
}

}
