#include "openai_client.h"
#include <iostream>
#include <vector>
#include <string>
#include "base64_helpers.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include "esphome/components/network/network_component.h"

namespace esphome {
namespace openai_client {

OpenAIClient::OpenAIClient() : connected_(false), socket_fd_(-1) {}

bool OpenAIClient::connect(const std::string& api_key, const std::string& model, const std::string& endpoint) {
    if (!esphome::network::is_connected()) { //check if the network is connected before doing anything else
        ESP_LOGE("openai_client", "Network not connected");
        return false;
    }
    this->api_key_ = api_key;
    this->model_ = model;
    this->endpoint_ = endpoint;

    if (this->endpoint_.empty()) {
        this->endpoint_ = "wss://api.openai.com/v1/realtime";
    }
    if (this->endpoint_.find("wss://") == 0) {
        this->endpoint_.replace(0, 6, "ws://");
    } else if (this->endpoint_.find("ws://") != 0) {
        this->endpoint_ = "ws://" + this->endpoint_;
    }

    std::string url = this->endpoint_ + "?model=" + this->model_;

    std::string host;
    size_t ws_pos = url.find("://");
    if (ws_pos != std::string::npos) {
        size_t start = ws_pos + 3;
        size_t end = url.find_first_of("/?", start);
        if (end == std::string::npos) {
            host = url.substr(start);
        } else {
            host = url.substr(start, end - start);
        }
    } else {
        host = url;
    }

    std::string hostname;
    int port = 80;
    size_t colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        hostname = host.substr(0, colon_pos);
        port = std::stoi(host.substr(colon_pos + 1));
    } else {
        hostname = host;
    }

    struct hostent *server = gethostbyname(hostname.c_str());
    if (server == nullptr) {
        return false;
    }

    this->socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->socket_fd_ < 0) {
        return false;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    if (::connect(this->socket_fd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(this->socket_fd_);
        this->socket_fd_ = -1;
        return false;
    }

    int flags = fcntl(this->socket_fd_, F_GETFL, 0);
    fcntl(this->socket_fd_, F_SETFL, flags | O_NONBLOCK);

    std::string handshake = 
        "GET " + url + " HTTP/1.1\r\n"
        "Host: " + hostname + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBjb250ZW50IDEyOC4/IGlmIHlvdSBpcyB0aGUgZ2V0IG9mIHRoZSB3b3NlZCBjb250ZW50Pw==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer " + this->api_key_ + "\r\n\r\n";

    send(this->socket_fd_, handshake.c_str(), handshake.size(), 0);

    char buffer[1024];
    int bytes_received = recv(this->socket_fd_, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        if (std::string(buffer).find("101 Switching Protocols") != std::string::npos) {
            this->connected_ = true;
            return true;
        }
    }

    close(this->socket_fd_);
    this->socket_fd_ = -1;
    this->connected_ = false;
    return false;
}

void OpenAIClient::disconnect() {
    if (this->socket_fd_ != -1) {
        close(this->socket_fd_);
        this->socket_fd_ = -1;
    }
    this->connected_ = false;
}

void OpenAIClient::send_audio_chunk(const uint8_t* data, size_t size) {
    if (!this->connected_) return;

    std::string base64_audio = esphome::utils::base64_encode(data, size);
    std::string json = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + base64_audio + "\"}";

    this->send_ws_frame(json, false);
}

void OpenAIClient::send_text_event(const std::string& event_type, const std::string& content) {
    if (!this->connected_) return;

    std::string json = "{\"type\":\"" + event_type + "\",\"content\":\"" + content + "\"}";
    this->send_ws_frame(json, false);
}

void OpenAIClient::receive_frame() {
    if (!this->connected_) return;

    uint8_t buffer[2048];
    // MSG_DONTWAIT ensures it returns immediately if no data is available
    int bytes_read = recv(this->socket_fd_, buffer, sizeof(buffer), MSG_DONTWAIT); 
    
    if (bytes_read > 0) {
        this->parse_ws_frame(buffer, bytes_read);
    } else if (bytes_read == -1) {
        // Handle normal non-blocking return (no data right now)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; 
        } else {
            // A real network error occurred
            this->disconnect();
            if (this->on_error) this->on_error("Socket read error");
        }
    } else if (bytes_read == 0) {
        // Server closed connection
        this->disconnect();
    }
}

void OpenAIClient::send_ws_frame(const std::string& payload, bool is_binary) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | (is_binary ? 0x80 : 0x01));

    uint16_t len = static_cast<uint16_t>(payload.size());
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (8 * (7 - i))) & 0xFF));
        }
    }

    frame.insert(frame.end(), payload.begin(), payload.end());

    send(this->socket_fd_, frame.data(), frame.size(), 0);
}

void OpenAIClient::parse_ws_frame(const uint8_t* data, size_t size) {
    if (size < 2) return;

    uint8_t first_byte = data[0];
    bool is_binary = (first_byte & 0x02);
    uint8_t opcode = first_byte & 0x0F;

    if (opcode == 0x01) { // Text frame
        std::string content(reinterpret_cast<const char*>(data + 2), size - 2);
        if (content.find("audio") != std::string::npos) {
            this->on_audio_delta({0x00}); 
        } else if (content.find("text") != std::string::npos) {
            this->on_text_delta(content);
        }
    } else if (opcode == 0x02) { // Binary frame
        // Handle binary data
    }
}

} // namespace openai_client
} // namespace esphome
