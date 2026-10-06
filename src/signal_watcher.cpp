#include "signal_watcher.hpp"

#include <iostream>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>

namespace {

sigset_t stopSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}

}  // namespace
#endif

SignalWatcher::SignalWatcher() {
#ifndef _WIN32
    const sigset_t set = stopSignals();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
}

SignalWatcher::~SignalWatcher() {
    stop();
}

void SignalWatcher::start(std::function<void()> onSignal) {
#ifndef _WIN32
    if (worker_.joinable()) {
        return;  // Ya está en marcha
    }
    stopping_ = false;
    worker_ = std::thread([this, onSignal = std::move(onSignal)] {
        const sigset_t set = stopSignals();
        int sig = 0;
        if (sigwait(&set, &sig) != 0 || stopping_) {
            return;  // Despertado por stop(), no por una señal externa
        }
        std::cout << "Señal " << (sig == SIGINT ? "SIGINT" : "SIGTERM")
                  << " recibida, deteniendo Telegarrm..." << std::endl;
        onSignal();
    });
#else
    (void)onSignal;
#endif
}

void SignalWatcher::stop() {
#ifndef _WIN32
    if (!worker_.joinable()) {
        return;
    }
    stopping_ = true;
    // Si todavía no ha llegado ninguna señal, despierta a sigwait() para poder unir el hilo
    pthread_kill(worker_.native_handle(), SIGTERM);
    worker_.join();
#endif
}
