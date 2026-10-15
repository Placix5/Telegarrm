#include "auth.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr const char* kPasswordSetting = "auth_password";
// scrypt: 2^15 x 8 x 128 B = 32 MiB de memoria por intento (~0,1 s en la Pi 5)
constexpr std::uint64_t kScryptN = 1u << 15;
constexpr std::uint64_t kScryptR = 8;
constexpr std::uint64_t kScryptP = 1;
constexpr std::uint64_t kScryptMaxMem = 64ull * 1024 * 1024;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
constexpr std::size_t kTokenBytes = 32;
// Una sesión dura 30 días desde su último uso; el uso se apunta como mucho una vez por hora
constexpr std::int64_t kSessionSeconds = 30 * 24 * 3600;
constexpr std::int64_t kTouchEverySeconds = 3600;
// Intentos fallidos: tras 5 de un mismo cliente, 30 s de espera que se doblan con cada fallo (hasta
// 15 min); tras 20 de todos juntos, 1 min que también se dobla
constexpr int kClientFreeAttempts = 5;
constexpr int kGlobalFreeAttempts = 20;
constexpr int kClientLockSeconds = 30;
constexpr int kGlobalLockSeconds = 60;
constexpr int kMaxLockSeconds = 15 * 60;
constexpr const char* kGlobal = "*";
constexpr auto kForgetFailures = std::chrono::hours(1);
// Letras y cifras sin las que se confunden (0/O, 1/I): 32 símbolos, 50 bits con 10
constexpr const char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string toHex(const unsigned char* data, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

std::optional<std::vector<unsigned char>> fromHex(const std::string& text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::vector<unsigned char> out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = value(text[i]);
        const int low = value(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<unsigned char>(high * 16 + low));
    }
    return out;
}

bool scrypt(const std::string& password, const std::vector<unsigned char>& salt, std::uint64_t n, std::uint64_t r,
            std::uint64_t p, std::vector<unsigned char>& out) {
    return EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), n, r, p, kScryptMaxMem,
                          out.data(), out.size()) == 1;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

// "abcde-12345" -> "ABCDE12345": el código se puede escribir sin guion y en minúsculas
std::string normalizeCode(const std::string& code) {
    std::string out;
    for (const char c : code) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool sameText(const std::string& a, const std::string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::optional<std::string> passwordProblem(const std::string& password) {
    if (password.size() < auth::kMinPasswordLength) {
        return "La contraseña debe tener al menos " + std::to_string(auth::kMinPasswordLength) + " caracteres";
    }
    if (password.size() > auth::kMaxPasswordLength) {
        return std::string("La contraseña es demasiado larga");
    }
    return std::nullopt;
}

}  // namespace

namespace auth {

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) {
        throw std::runtime_error("RAND_bytes no ha podido generar datos aleatorios");
    }
    return toHex(data.data(), data.size());
}

std::string sha256Hex(const std::string& text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(text.data(), text.size(), digest, &size, EVP_sha256(), nullptr);
    return toHex(digest, size);
}

std::string hashPassword(const std::string& password) {
    const auto salt = fromHex(randomHex(kSaltBytes));
    std::vector<unsigned char> hash(kHashBytes);
    if (!salt || !scrypt(password, *salt, kScryptN, kScryptR, kScryptP, hash)) {
        throw std::runtime_error("scrypt no ha podido calcular la contraseña");
    }
    return "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" + std::to_string(kScryptP) +
           "$" + toHex(salt->data(), salt->size()) + "$" + toHex(hash.data(), hash.size());
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    const std::vector<std::string> parts = split(stored, '$');
    if (parts.size() != 6 || parts[0] != "scrypt") {
        return false;
    }
    std::uint64_t n = 0;
    std::uint64_t r = 0;
    std::uint64_t p = 0;
    try {
        n = std::stoull(parts[1]);
        r = std::stoull(parts[2]);
        p = std::stoull(parts[3]);
    } catch (const std::exception&) {
        return false;
    }
    const auto salt = fromHex(parts[4]);
    const auto expected = fromHex(parts[5]);
    if (!salt || !expected || expected->empty() || n > (1u << 20) || r > 32 || p > 4) {
        return false;
    }
    std::vector<unsigned char> hash(expected->size());
    return scrypt(password, *salt, n, r, p, hash) && CRYPTO_memcmp(hash.data(), expected->data(), hash.size()) == 0;
}

std::string cookieValue(const std::string& header, const std::string& name) {
    for (std::string part : split(header, ';')) {
        const auto first = part.find_first_not_of(' ');
        part = first == std::string::npos ? "" : part.substr(first);
        if (part.size() > name.size() && part.compare(0, name.size(), name) == 0 && part[name.size()] == '=') {
            return part.substr(name.size() + 1);
        }
    }
    return "";
}

}  // namespace auth

Auth::Auth(DbManager& db, std::string setupCodePath) : db_(db), setupCodePath_(std::move(setupCodePath)) {}

bool Auth::configured() {
    return db_.getSetting(kPasswordSetting).has_value();
}

void Auth::prepareSetup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!setupCode_.empty() || configured()) {
        return;
    }
    std::vector<unsigned char> random(10);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        std::cerr << "[Acceso] No se pudo generar el código para crear la contraseña" << std::endl;
        return;
    }
    std::string code;
    for (std::size_t i = 0; i < random.size(); ++i) {
        code.push_back(kCodeAlphabet[random[i] % 32]);  // 256 es múltiplo de 32: sin sesgo
        if (i == 4) {
            code.push_back('-');
        }
    }
    // Solo lo puede leer el usuario del servicio (0600): quien tenga acceso a la Pi
    const int fd = ::open(setupCodePath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        std::cerr << "[Acceso] No se pudo escribir " << setupCodePath_ << std::endl;
        return;
    }
    ::fchmod(fd, 0600);
    const std::string text = "Código para crear la contraseña de Telegarrm: " + code + "\n";
    const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    if (!written) {
        std::cerr << "[Acceso] No se pudo escribir " << setupCodePath_ << std::endl;
        return;
    }
    setupCode_ = normalizeCode(code);
    std::cout << "[Acceso] Aún no hay contraseña. El código para crearla está en " << setupCodePath_ << std::endl;
}

Auth::Result Auth::setup(const std::string& code, const std::string& password, const std::string& client) {
    prepareSetup();
    std::lock_guard<std::mutex> lock(mutex_);
    if (const int wait = lockedFor(client)) {
        return tooMany(wait);
    }
    Result result;
    if (configured()) {
        result.status = 409;
        result.error = "La contraseña ya está creada";
        return result;
    }
    if (setupCode_.empty() || !sameText(normalizeCode(code), setupCode_)) {
        fail(client);
        result.status = 403;
        result.error = "El código no es correcto. Está en " + setupCodePath_ + ", en la Pi";
        return result;
    }
    if (const auto problem = passwordProblem(password)) {
        result.status = 400;
        result.error = *problem;
        return result;
    }
    if (!db_.setSetting(kPasswordSetting, auth::hashPassword(password))) {
        result.status = 500;
        result.error = "No se pudo guardar la contraseña";
        return result;
    }
    setupCode_.clear();
    std::error_code ec;
    std::filesystem::remove(setupCodePath_, ec);
    failures_.erase(client);
    std::cout << "[Acceso] Contraseña creada" << std::endl;
    result.ok = true;
    result.token = newSession();
    return result;
}

Auth::Result Auth::login(const std::string& password, const std::string& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const int wait = lockedFor(client)) {
        return tooMany(wait);
    }
    Result result;
    const auto stored = db_.getSetting(kPasswordSetting);
    if (!stored) {
        result.status = 409;
        result.error = "Primero hay que crear la contraseña";
        return result;
    }
    if (!auth::verifyPassword(password, *stored)) {
        fail(client);
        std::cerr << "[Acceso] Contraseña incorrecta desde " << client << std::endl;
        result.status = 401;
        result.error = "Contraseña incorrecta";
        return result;
    }
    failures_.erase(client);
    db_.deleteSessions("", false, nowSeconds());  // De paso, las caducadas
    result.ok = true;
    result.token = newSession();
    return result;
}

Auth::Result Auth::changePassword(const std::string& current, const std::string& password, const std::string& token,
                                  const std::string& client) {
    Result result;
    if (!valid(token)) {
        result.status = 401;
        result.error = "Inicia sesión";
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (const int wait = lockedFor(client)) {
        return tooMany(wait);
    }
    const auto stored = db_.getSetting(kPasswordSetting);
    if (!stored || !auth::verifyPassword(current, *stored)) {
        fail(client);
        result.status = 403;
        result.error = "La contraseña actual no es correcta";
        return result;
    }
    if (const auto problem = passwordProblem(password)) {
        result.status = 400;
        result.error = *problem;
        return result;
    }
    if (!db_.setSetting(kPasswordSetting, auth::hashPassword(password))) {
        result.status = 500;
        result.error = "No se pudo guardar la contraseña";
        return result;
    }
    // Las demás sesiones (otros navegadores) se cierran; quien la cambia sigue dentro con una nueva
    db_.deleteSessions("", true, 0);
    std::cout << "[Acceso] Contraseña cambiada; se cierran las demás sesiones" << std::endl;
    result.ok = true;
    result.token = newSession();
    return result;
}

bool Auth::valid(const std::string& token) {
    if (token.size() != kTokenBytes * 2) {
        return false;
    }
    const std::string hash = auth::sha256Hex(token);
    const auto session = db_.getSession(hash);
    const std::int64_t now = nowSeconds();
    if (!session || session->expiresAt <= now) {
        return false;
    }
    if (now - session->lastUsedAt >= kTouchEverySeconds) {
        db_.touchSession(hash, now, now + kSessionSeconds);
    }
    return true;
}

void Auth::logout(const std::string& token) {
    if (!token.empty()) {
        db_.deleteSessions(auth::sha256Hex(token), false, nowSeconds());
    }
}

bool Auth::reset(DbManager& db) {
    return db.deleteSetting(kPasswordSetting) && db.deleteSessions("", true, 0) >= 0;
}

int Auth::lockedFor(const std::string& client) const {
    const auto now = Clock::now();
    int seconds = 0;
    for (const std::string& key : {client, std::string(kGlobal)}) {
        const auto it = failures_.find(key);
        if (it != failures_.end() && it->second.lockedUntil > now) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(it->second.lockedUntil - now).count() + 1;
            seconds = std::max(seconds, static_cast<int>(left));
        }
    }
    return seconds;
}

void Auth::fail(const std::string& client) {
    const auto lockFor = [](Failures& failures, int freeAttempts, int baseSeconds) {
        const auto now = Clock::now();
        if (now - failures.last > kForgetFailures) {
            failures.count = 0;
        }
        failures.last = now;
        ++failures.count;
        if (failures.count >= freeAttempts) {
            const int doublings = std::min(failures.count - freeAttempts, 10);
            const int seconds = std::min(baseSeconds << doublings, kMaxLockSeconds);
            failures.lockedUntil = Clock::now() + std::chrono::seconds(seconds);
        }
    };
    lockFor(failures_[client], kClientFreeAttempts, kClientLockSeconds);
    lockFor(failures_[kGlobal], kGlobalFreeAttempts, kGlobalLockSeconds);
}

std::string Auth::newSession() {
    const std::string token = auth::randomHex(kTokenBytes);
    const std::int64_t now = nowSeconds();
    db_.addSession(auth::sha256Hex(token), now, now + kSessionSeconds);
    return token;
}

Auth::Result Auth::tooMany(int seconds) const {
    Result result;
    result.status = 429;
    result.retryAfter = seconds;
    result.error = "Demasiados intentos. Espera " + std::to_string(seconds) + " s";
    return result;
}
