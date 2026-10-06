#include "catalog.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <tuple>
#include <utility>

#include "media_parser.hpp"

namespace {

using Message = DbManager::Message;

bool isBlank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Valor más repetido (el primero en aparecer si hay empate); vacío si no hay ninguno
std::string mostCommon(const std::vector<std::string>& values) {
    std::string best;
    std::size_t bestCount = 0;
    for (const std::string& value : values) {
        if (value.empty()) {
            continue;
        }
        const auto count = static_cast<std::size_t>(std::count(values.begin(), values.end(), value));
        if (count > bestCount) {
            best = value;
            bestCount = count;
        }
    }
    return best;
}

// Bloque de mensajes entre dos fichas
struct Block {
    std::int64_t anchor = 0;
    std::int64_t poster = 0;
    std::string caption;
    std::vector<const Message*> files;
};

Catalog::Item buildItem(std::int64_t chatId, const std::string& channelTitle, const Block& block) {
    Catalog::Item item;
    item.chatId = chatId;
    item.anchorMessageId = block.anchor;
    item.posterMessageId = block.poster;
    item.description = block.caption;
    item.channelTitle = channelTitle;

    const media::Ficha ficha = block.caption.empty() ? media::Ficha{} : media::parseFicha(block.caption);

    std::vector<std::string> seriesNames;
    std::vector<std::string> qualities;
    for (const Message* message : block.files) {
        Catalog::File file;
        file.messageId = message->messageId;
        file.fileName = message->fileName.value_or("");
        file.size = message->fileSize.value_or(0);
        file.archive = media::isArchive(file.fileName);
        file.quality = media::detectQuality(file.fileName + " " + message->text);

        // El nombre del fichero manda; si no tiene marcador, se prueba con el pie
        auto episode = media::parseEpisode(file.fileName);
        if (!episode) {
            episode = media::parseEpisode(message->text);
        }
        if (episode) {
            file.season = episode->season;
            file.episode = episode->episode;
            file.episodeEnd = episode->episodeEnd;
            file.episodeTitle = episode->episodeTitle;
            seriesNames.push_back(episode->seriesName);
        }
        qualities.push_back(file.quality);
        item.totalSize += file.size;
        item.files.push_back(std::move(file));
    }

    const bool isSeries = std::any_of(item.files.begin(), item.files.end(),
                                      [](const Catalog::File& file) { return file.episode > 0; });
    item.kind = isSeries ? "series" : "movie";

    // Título: ficha > nombre en los ficheros > (película) nombre del fichero > título del canal
    item.title = ficha.title;
    if (item.title.empty()) {
        item.title = mostCommon(seriesNames);
    }
    if (item.title.empty() && !isSeries && !item.files.empty()) {
        item.title = media::cleanTitle(item.files.front().fileName);
    }
    if (item.title.empty()) {
        item.title = media::cleanTitle(channelTitle);
    }
    if (item.title.empty()) {
        item.title = channelTitle;
    }

    item.year = ficha.year;
    if (!item.year && !isSeries && !item.files.empty()) {
        item.year = media::detectYear(item.files.front().fileName);
    }
    item.quality = !ficha.quality.empty() ? ficha.quality : mostCommon(qualities);
    if (item.quality.empty()) {
        item.quality = media::detectQuality(channelTitle);
    }
    item.genres = ficha.genres;
    item.languages = ficha.languages;
    for (const std::string& language : media::detectLanguages(channelTitle)) {
        if (std::find(item.languages.begin(), item.languages.end(), language) == item.languages.end()) {
            item.languages.push_back(language);
        }
    }

    if (isSeries) {
        // Un "título de episodio" repetido en más de la mitad de los episodios, o igual al de la
        // serie, es en realidad el nombre de la serie ("1x01 - Ultimate Spiderman.mkv")
        std::vector<std::string> titles;
        for (const Catalog::File& file : item.files) {
            if (file.episode > 0) {
                titles.push_back(media::titleKey(file.episodeTitle));
            }
        }
        const std::string itemKey = media::titleKey(item.title);
        for (Catalog::File& file : item.files) {
            const std::string key = media::titleKey(file.episodeTitle);
            const auto repeated = static_cast<std::size_t>(std::count(titles.begin(), titles.end(), key));
            if (key == itemKey || (titles.size() > 1 && repeated * 2 > titles.size())) {
                file.episodeTitle.clear();
            }
        }

        std::stable_sort(item.files.begin(), item.files.end(), [](const Catalog::File& a, const Catalog::File& b) {
            // Ficheros sin marcador al final; a igualdad, por orden de publicación
            const bool aEpisode = a.episode > 0;
            const bool bEpisode = b.episode > 0;
            return std::make_tuple(!aEpisode, a.season, a.episode, a.messageId) <
                   std::make_tuple(!bEpisode, b.season, b.episode, b.messageId);
        });

        std::set<int> seasons;
        std::set<std::pair<int, int>> episodes;
        for (const Catalog::File& file : item.files) {
            if (file.episode > 0) {
                seasons.insert(file.season);
                episodes.emplace(file.season, file.episode);
            }
        }
        item.seasonCount = static_cast<int>(seasons.size());
        item.episodeCount = static_cast<int>(episodes.size());
    }
    return item;
}

}  // namespace

Catalog::Catalog(DbManager& db) : db_(db) {}

std::vector<Catalog::Item> Catalog::buildItems(std::int64_t chatId, const std::string& channelTitle,
                                               const std::vector<Message>& messages) {
    std::vector<Block> blocks(1);  // Bloque inicial para los archivos publicados antes de la primera ficha
    for (const Message& message : messages) {
        if (message.contentType == "messagePhoto") {
            if (!isBlank(message.text)) {
                // Ficha: abre un bloque nuevo (o adopta el inicial si aún está vacío)
                if (blocks.back().anchor != 0 || !blocks.back().files.empty()) {
                    blocks.emplace_back();
                }
                Block& block = blocks.back();
                block.anchor = message.messageId;
                block.poster = message.messageId;
                block.caption = message.text;
            } else if (blocks.back().poster == 0) {
                // Foto sin pie (ej. portada de temporada): no abre bloque; como mucho, sirve de portada
                blocks.back().poster = message.messageId;
            }
            continue;
        }
        if (message.fileSize && media::isMediaFile(message.fileName.value_or(""), message.mimeType.value_or(""))) {
            blocks.back().files.push_back(&message);
        }
        // Textos sueltos ("FIN DE SERIE", créditos, enlaces) no cambian de bloque
    }

    std::vector<Item> items;
    for (const Block& block : blocks) {
        if (!block.files.empty()) {
            items.push_back(buildItem(chatId, channelTitle, block));
        }
    }
    return items;
}

void Catalog::rebuildChannel(std::int64_t chatId) {
    std::lock_guard<std::mutex> rebuildLock(rebuildMutex_);
    const auto channel = db_.getChannel(chatId);
    if (!channel) {
        std::lock_guard<std::mutex> lock(mutex_);
        byChannel_.erase(chatId);
        return;
    }
    std::vector<ItemPtr> items;
    for (Item& item : buildItems(chatId, channel->title, db_.channelMessages(chatId))) {
        items.push_back(std::make_shared<const Item>(std::move(item)));
    }

    const std::size_t count = items.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        byChannel_[chatId] = std::move(items);
    }
    std::cout << "[Catálogo] " << channel->title << ": " << count << " elementos" << std::endl;
}

void Catalog::rebuildAll() {
    for (const DbManager::Channel& channel : db_.listChannels()) {
        rebuildChannel(channel.id);
    }
}

void Catalog::removeChannel(std::int64_t chatId) {
    std::lock_guard<std::mutex> rebuildLock(rebuildMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    byChannel_.erase(chatId);
}

std::vector<Catalog::ItemPtr> Catalog::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ItemPtr> all;
    for (const auto& entry : byChannel_) {
        all.insert(all.end(), entry.second.begin(), entry.second.end());
    }
    return all;
}

Catalog::ItemPtr Catalog::find(std::int64_t chatId, std::int64_t anchorMessageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto channel = byChannel_.find(chatId);
    if (channel == byChannel_.end()) {
        return nullptr;
    }
    for (const ItemPtr& item : channel->second) {
        if (item->anchorMessageId == anchorMessageId) {
            return item;
        }
    }
    return nullptr;
}
