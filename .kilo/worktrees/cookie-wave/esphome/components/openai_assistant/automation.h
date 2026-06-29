#ifndef OPENAI_ASSISTANT_AUTOMATION_H
#define OPENAI_ASSISTANT_AUTOMATION_H

#include "esphome/core/automation.h"
#include "openai_assistant.h"

namespace esphome {
namespace openai_assistant {

template<typename... Ts>
class StartAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
public:
    // This macro automatically generates the set_wake_word() method used in Python
    TEMPLATABLE_VALUE(std::string, wake_word)

    void play(Ts... x) override {
        // Evaluate the lambda
        std::string detected_word = this->wake_word_.value(x...);
        this->parent_->on_wake_word_triggered(detected_word);    }
};

template<typename... Ts>
class StopAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
public:
    void play(Ts... x) override {
        // If you add a handle_stop() function later, call it here.
        // For now, we just leave it blank so 'openai_assistant.stop' doesn't crash.
    }
};

template<typename... Ts> 
class IsRunningCondition : public Condition<Ts...>, public Parented<OpenAIAssistant> {
public:
    bool check(Ts... x) override {
        // Returns true if the assistant is doing anything other than waiting (IDLE)
        return this->parent_->get_state() != State::IDLE;
    }
};

// Add this to automation.h
template<typename... Ts>
class StartContinuousAction : public Action<Ts...>, public Parented<OpenAIAssistant> {
public:
    void play(Ts... x) override {
        // You need to ensure your OpenAIAssistant class has this method
        this->parent_->handle_start_continuous();
    }
};

} // namespace openai_assistant
} // namespace esphome

#endif // OPENAI_ASSISTANT_AUTOMATION_H
