#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

// Esqueleto del cliente de Telegram. Por ahora solo simula el hilo donde correrá
// el bucle de TDLib, para validar que convive con el servidor HTTP.
// start() y stop() deben llamarse desde el mismo hilo (el principal).
class TelegramClient {
public:
    TelegramClient() = default;
    ~TelegramClient();

    TelegramClient(const TelegramClient&) = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;

    // Lanza el hilo de trabajo y vuelve inmediatamente
    void start();
    // Pide al hilo que termine y espera a que lo haga
    void stop();

private:
    void run();

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
};
