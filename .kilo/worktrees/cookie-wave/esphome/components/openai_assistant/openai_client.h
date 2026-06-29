#ifndef OPENAI_CLIENT_H
#define OPENAI_CLIENT_H

//#include "esphome/components/common/common.h"
#include "esphome/components/network/network_component.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace esphome {
namespace openai_client {

class OpenAIClient {
public:
    OpenAIClient();

    bool connect(const std::string& api_key, const std::string& model, const std::string& endpoint);
    void disconnect();

    void receive_frame();
    void send_text_event(const std::string& event_type, const std::string& content);
    void send_audio_chunk(const uint8_t* data, size_t size);

    // Callbacks
    std::function<void(const std::vector<uint8_t>&)> on_audio_delta;
    std::function<void(const std::string&)> on_text_delta;
    std::function<void(const std::string&)> on_error;

    bool is_connected() const { return this->connected_; }

private:
    std::shared_ptr<network::NetworkComponent> network_;
    std::string api_key_;
    std::string model_;
    std::string endpoint_;
    bool connected_{false};
    int socket_fd_{-1};
    std::string ws_buffer_;

    void send_ws_frame(const std::string& payload, bool is_binary);
    void parse_ws_frame(const uint8_t* data, size_t size);
};

} // namespace openai_client
} // namespace esphome

#endif // OPENAI_CLIENT_H
