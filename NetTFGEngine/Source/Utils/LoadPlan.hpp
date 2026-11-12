#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Utils/Debug/Debug.hpp"

// A scene load cut into steps that the render loop runs a slice per frame (ClientWindow::renderLoop), pumping the
// window's events and drawing the loading screen in between. Run as one block, a level load kept the window thread away
// from its message queue for seconds and Windows marked the game "No responde".
//
// Labels are shown to the player as they are: keep them generic ("Loading models"), never asset or file names.
// Weights are relative (only their ratio matters) and drive the progress bar; rough durations are fine.
class LoadPlan {
public:
    // Polled step: returns its own progress in [0, 1]; 1 or more = finished. Anything below 1 ends this frame's slice
    // (it is waiting on something, e.g. a worker thread) and the step is called again next frame.
    using PollFn = std::function<float()>;

    // One-shot step: runs once, in a single frame.
    void Add(std::string label, float weight, std::function<void()> fn) {
        steps_.push_back({ std::move(label), std::max(weight, 0.0f),
            [fn = std::move(fn)]() { fn(); return 1.0f; } });
        totalWeight_ += steps_.back().weight;
    }

    void AddPolled(std::string label, float weight, PollFn fn) {
        steps_.push_back({ std::move(label), std::max(weight, 0.0f), std::move(fn) });
        totalWeight_ += steps_.back().weight;
    }

    bool Empty() const { return steps_.empty(); }
    bool Done() const { return next_ >= steps_.size(); }

    // Runs steps until `budget` is spent (always at least one call, a step can't be cut further) or one is waiting.
    void Run(std::chrono::microseconds budget) {
        using Clock = std::chrono::steady_clock;
        const auto sliceStart = Clock::now();
        if (!started_) {
            started_ = true;
            start_ = sliceStart;
            Debug::Info("Loading") << "Load started: " << steps_.size() << " steps\n";
        }
        ++frames_;

        while (!Done()) {
            Step& step = steps_[next_];
            const auto t0 = Clock::now();
            const float p = step.run();
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            step.workMs += ms;
            ++step.calls;

            if (p < 1.0f) {
                stepProgress_ = std::clamp(p, 0.0f, 1.0f);
                break;   // waiting: give the frame back
            }

            FinishStep(step);
            if (Clock::now() - sliceStart >= budget) break;
        }

        if (Done() && !reported_) {
            reported_ = true;
            const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
            Debug::Info("Loading") << "Load finished in " << totalMs << " ms over " << frames_ << " frames (longest step: '"
                << longestLabel_ << "', " << longestMs_ << " ms)\n";
        }
    }

    // [0, 1], by weight; a polled step counts its partial progress.
    float Progress() const {
        if (Done()) return 1.0f;
        if (totalWeight_ <= 0.0f) return float(next_) / float(steps_.size());
        return std::clamp((doneWeight_ + steps_[next_].weight * stepProgress_) / totalWeight_, 0.0f, 1.0f);
    }

    // Label of the step running now (the last one once done).
    const std::string& CurrentLabel() const {
        static const std::string empty;
        if (steps_.empty()) return empty;
        return steps_[std::min(next_, steps_.size() - 1)].label;
    }

private:
    struct Step {
        std::string label;
        float weight = 1.0f;
        PollFn run;
        double workMs = 0.0;   // time spent inside run(), all calls
        int calls = 0;
    };

    // A single step this long is still one frame without window events: the log says which, to cut it further.
    static constexpr double kSlowStepMs = 1000.0;

    void FinishStep(Step& step) {
        const double sinceStart = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count();
        DebugStream line(step.workMs >= kSlowStepMs ? LogLevel::Warning : LogLevel::Info, "Loading");
        line <<"Step " << (next_ + 1) << "/" << steps_.size() << " '" << step.label << "': " << step.workMs << " ms";
        if (step.calls > 1) line << " over " << step.calls << " frames";
        line << " (t=" << sinceStart << " ms)\n";

        if (step.workMs > longestMs_) {
            longestMs_ = step.workMs;
            longestLabel_ = step.label;
        }
        doneWeight_ += step.weight;
        stepProgress_ = 0.0f;
        ++next_;
    }

    std::vector<Step> steps_;
    size_t next_ = 0;
    float totalWeight_ = 0.0f;
    float doneWeight_ = 0.0f;
    float stepProgress_ = 0.0f;

    bool started_ = false;
    bool reported_ = false;
    std::chrono::steady_clock::time_point start_;
    int frames_ = 0;
    double longestMs_ = 0.0;
    std::string longestLabel_;
};
