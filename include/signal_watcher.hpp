#pragma once

#include <atomic>
#include <functional>
#include <thread>

// Convierte SIGINT (Ctrl+C) y SIGTERM (systemd, kill) en una parada ordenada.
// Debe construirse al principio de main(), antes de crear cualquier otro hilo:
// bloquea esas señales para que todos los hilos hereden la máscara y solo las
// reciba el hilo vigilante, fuera de un manejador de señales asíncrono.
// En Windows no hace nada: Ctrl+C sigue terminando el proceso directamente.
class SignalWatcher {
public:
    SignalWatcher();
    ~SignalWatcher();

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    // Lanza el hilo vigilante; ejecuta onSignal (una sola vez) al recibir una señal
    void start(std::function<void()> onSignal);
    // Detiene el hilo vigilante. Llamar antes de destruir lo que use onSignal.
    void stop();

private:
    std::thread worker_;
    std::atomic<bool> stopping_{false};
};
