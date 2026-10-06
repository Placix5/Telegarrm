#include "process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;

namespace process {
namespace {

constexpr std::size_t kMaxOutput = 64 * 1024;
constexpr int kPollMilliseconds = 500;

// Cierra un descriptor al salir del ámbito
struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

}  // namespace

Result run(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onOutput,
           const std::function<bool()>& shouldStop) {
    Result result;
    if (argv.empty()) {
        result.output = "No hay programa que ejecutar";
        return result;
    }

    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
        result.output = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    Fd readEnd{pipeFds[0]};
    Fd writeEnd{pipeFds[1]};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, writeEnd.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, writeEnd.fd, STDERR_FILENO);
    // Ningún otro descriptor del servicio (sockets, base de datos...) pasa al hijo
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

    // El servicio bloquea SIGINT/SIGTERM en todos sus hilos (SignalWatcher): el hijo debe tenerlas
    // desbloqueadas y con su acción por defecto, o no se le podría parar
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t empty;
    sigemptyset(&empty);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> args;
    for (const std::string& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);

    pid_t pid = 0;
    const int spawnError = ::posix_spawn(&pid, argv[0].c_str(), &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawnError != 0) {
        result.output = "No se pudo ejecutar " + argv[0] + ": " + std::strerror(spawnError);
        return result;
    }
    result.started = true;
    ::close(writeEnd.fd);  // Solo lo usa el hijo: así read() devuelve 0 cuando termina
    writeEnd.fd = -1;

    char buffer[4096];
    for (;;) {
        pollfd pfd{readEnd.fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, kPollMilliseconds);
        if (ready > 0) {
            const ssize_t count = ::read(readEnd.fd, buffer, sizeof(buffer));
            if (count <= 0) {
                break;  // El hijo ha cerrado su salida (ha terminado)
            }
            const std::string chunk(buffer, static_cast<std::size_t>(count));
            result.output += chunk;
            if (result.output.size() > kMaxOutput) {
                result.output.erase(0, result.output.size() - kMaxOutput);
            }
            if (onOutput) {
                onOutput(chunk);
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
        if (!result.stopped && shouldStop && shouldStop()) {
            result.stopped = true;
            ::kill(pid, SIGTERM);
        }
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

}  // namespace process
