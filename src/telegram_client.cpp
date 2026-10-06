#include "telegram_client.hpp"

#include <chrono>
#include <iostream>

namespace {
constexpr auto kTickInterval = std::chrono::seconds(5);
}

TelegramClient::~TelegramClient() {
    stop();
}

void TelegramClient::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) {
        return;  // Ya está en marcha
    }
    running_ = true;
    worker_ = std::thread(&TelegramClient::run, this);
}

void TelegramClient::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void TelegramClient::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        std::cout << "Hilo TDLib simulado corriendo..." << std::endl;
        // Espera 5 s, pero despierta en cuanto stop() lo solicite
        cv_.wait_for(lock, kTickInterval, [this] { return !running_; });
    }
}
