#include "DatabaseManager.hpp"

#include "../../../common/command/Command.hpp"
#include "../../../common/log/Exception.hpp"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/io_uring.h>
#include <ranges>

static constexpr std::string filepath{"dump.aof"};

auto DatabaseManager::create(const std::source_location sourceLocation) -> int {
    const int fileDescriptor{open(filepath.data(), O_CREAT | O_WRONLY | O_APPEND | O_SYNC, S_IRUSR | S_IWUSR)};
    if (fileDescriptor == -1) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }

    return fileDescriptor;
}

DatabaseManager::DatabaseManager(const int fileDescriptor) : FileDescriptor{fileDescriptor} {
    for (unsigned char i{}; i < 16; ++i) this->databases.emplace(i, Database{i, std::span<const std::byte>{}});

    if (std::ifstream file{filepath}; file.is_open()) {
        std::vector<std::byte> buffer{std::filesystem::file_size(filepath)};
        file.read(reinterpret_cast<char *>(buffer.data()), static_cast<long>(buffer.size()));

        if (!buffer.empty()) {
            std::span<const std::byte> data{buffer};

            auto count{*reinterpret_cast<const unsigned long *>(data.data())};
            data = data.subspan(sizeof(decltype(count)));

            while (count > 0) {
                const auto id{*reinterpret_cast<const unsigned long *>(data.data())};
                data = data.subspan(sizeof(decltype(id)));

                const auto size{*reinterpret_cast<const unsigned long *>(data.data())};
                data = data.subspan(sizeof(decltype(size)));

                if (const auto result{this->databases.find(id)}; result != this->databases.cend())
                    result->second = Database{id, data.subspan(0, size)};
                else this->databases.emplace(id, Database{id, data.subspan(0, size)});
                data = data.subspan(size);

                --count;
            }

            while (!data.empty()) {
                const auto size{*reinterpret_cast<const unsigned long *>(data.data())};
                data = data.subspan(sizeof(decltype(size)));

                this->query(data.subspan(0, size));
                data = data.subspan(size);
            }
        }
    }
}

auto DatabaseManager::query(std::span<const std::byte> request) -> std::vector<std::byte> {
    const std::span requestCopy{request};

    const auto command{static_cast<Command>(request.front())};
    request = request.subspan(sizeof(command));

    const auto id{*reinterpret_cast<const unsigned long *>(request.data())};
    request = request.subspan(sizeof(id));

    Database &database{this->databases.at(id)};

    const std::string_view statement{reinterpret_cast<const char *>(request.data()), request.size()};
    std::vector<std::byte> response;
    switch (command) {
        case Command::select:
            response = this->select(id);
            break;
        case Command::del:
            response = database.del(statement);
            this->record(requestCopy);

            break;
        case Command::dump:
            response = database.dump(statement);
            break;
        case Command::exists:
            response = database.exists(statement);
            break;
        case Command::move:
            {
                const std::shared_lock sharedLock{this->lock};

                response = database.move(this->databases, statement);
            }
            this->record(requestCopy);

            break;
        case Command::rename:
            response = database.rename(statement);
            this->record(requestCopy);

            break;
        case Command::renamenx:
            response = database.renamenx(statement);
            this->record(requestCopy);

            break;
        case Command::type:
            response = database.type(statement);
            break;
        case Command::set:
            response = database.set(statement);
            this->record(requestCopy);

            break;
        case Command::get:
            response = database.get(statement);
            break;
        case Command::getRange:
            response = database.getRange(statement);
            break;
        case Command::mget:
            response = database.mget(statement);
            break;
        case Command::setnx:
            response = database.setnx(statement);
            this->record(requestCopy);

            break;
        case Command::setRange:
            response = database.setRange(statement);
            this->record(requestCopy);

            break;
        case Command::strlen:
            response = database.strlen(statement);
            break;
        case Command::mset:
            response = database.mset(statement);
            this->record(requestCopy);

            break;
        case Command::msetnx:
            response = database.msetnx(statement);
            this->record(requestCopy);

            break;
        case Command::incr:
            response = database.incr(statement);
            this->record(requestCopy);

            break;
    }

    return response;
}

auto DatabaseManager::writable() -> bool {
    ++this->seconds;

    if (this->writeBuffer.empty()) {
        if ((this->seconds >= std::chrono::seconds{900} && this->writeCount > 1) ||
            (this->seconds >= std::chrono::seconds{300} && this->writeCount > 10) ||
            (this->seconds >= std::chrono::seconds{60} && this->writeCount > 10000)) {
            this->seconds = std::chrono::seconds::zero();

            {
                const std::lock_guard lockGuard{this->lock};

                this->aofBuffer.clear();
                this->writeCount = 0;
            }

            this->writeBuffer = this->serialize();

            return true;
        }

        if (const std::lock_guard lockGuard{this->lock}; !this->aofBuffer.empty()) {
            if (std::filesystem::file_size(filepath) == 0) {
                this->aofBuffer.insert(this->aofBuffer.cbegin(), sizeof(unsigned long), std::byte{});
                *reinterpret_cast<unsigned long *>(this->aofBuffer.data()) = 0;
            }

            this->writeBuffer = std::move(this->aofBuffer);

            return true;
        }
    }

    return false;
}

auto DatabaseManager::truncatable() const noexcept -> bool {
    return this->seconds == std::chrono::seconds::zero() && !this->writeBuffer.empty();
}

auto DatabaseManager::truncate() const noexcept -> Awaiter {
    Awaiter awaiter;
    awaiter.setSubmission(Submission{this->getFileDescriptor(), IOSQE_FIXED_FILE, 0, Submission::Truncate{}});

    return awaiter;
}

auto DatabaseManager::write() const noexcept -> Awaiter {
    Awaiter awaiter;
    awaiter.setSubmission(Submission{
        this->getFileDescriptor(), IOSQE_FIXED_FILE, 0, Submission::Write{this->writeBuffer, 0}
    });

    return awaiter;
}

auto DatabaseManager::wrote() noexcept -> void { this->writeBuffer.clear(); }

auto DatabaseManager::record(const std::span<const std::byte> request) -> void {
    const std::lock_guard lockGuard{this->lock};

    const unsigned long requestSize{request.size()};
    this->aofBuffer.insert(this->aofBuffer.cend(), sizeof(requestSize), std::byte{});
    *reinterpret_cast<std::remove_const_t<decltype(requestSize)> *>(this->aofBuffer.data() + this->aofBuffer.size() -
                                                                    sizeof(requestSize)) = requestSize;

    this->aofBuffer.insert(this->aofBuffer.cend(), request.cbegin(), request.cend());

    ++this->writeCount;
}

auto DatabaseManager::select(const unsigned long id) -> std::vector<std::byte> {
    const std::lock_guard lockGuard{this->lock};

    this->databases.try_emplace(id, Database{id, std::span<const std::byte>{}});

    return {std::byte{'O'}, std::byte{'K'}};
}

auto DatabaseManager::serialize() -> std::vector<std::byte> {
    const std::shared_lock sharedLock{this->lock};

    const unsigned long size{this->databases.size()};
    std::vector<std::byte> serialization{sizeof(size)};
    *reinterpret_cast<std::remove_const_t<decltype(size)> *>(serialization.data()) = size;

    for (auto &value : this->databases | std::views::values) {
        const std::vector subSerialization{value.serialize()};
        serialization.insert(serialization.cend(), subSerialization.cbegin(), subSerialization.cend());
    }

    return serialization;
}
