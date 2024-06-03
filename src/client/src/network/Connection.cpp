#include "Connection.hpp"

#include "../../../common/log/Exception.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <utility>

Connection::Connection() :
    fileDescriptor{[] {
        const int fileDescriptor{socket()};

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9090);
        translateIpAddress(address.sin_addr);

        connect(fileDescriptor, address);

        return fileDescriptor;
    }()} {}

Connection::Connection(Connection &&other) noexcept : fileDescriptor{std::exchange(other.fileDescriptor, -1)} {}

auto Connection::operator=(Connection &&other) noexcept -> Connection & {
    if (this == &other) return *this;

    this->close();

    this->fileDescriptor = std::exchange(other.fileDescriptor, -1);

    return *this;
}

Connection::~Connection() { this->close(); }

auto Connection::getPeerName(std::source_location sourceLocation) const -> std::pair<std::string, std::string> {
    sockaddr_in address{};
    socklen_t addressLength{sizeof(address)};
    if (getpeername(this->fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }

    return {deTranslateIpAddress(address.sin_addr), std::to_string(ntohs(address.sin_port))};
}

auto Connection::send(std::span<const std::byte> data, std::source_location sourceLocation) const -> void {
    const long reuslt{::send(this->fileDescriptor, data.data(), data.size(), 0)};
    if (reuslt <= 0) {
        std::string error{reuslt == 0 ? "connection closed" : std::strerror(errno)};
        throw Exception{
            Log{Log::Level::fatal, std::move(error), sourceLocation}
        };
    }
}

auto Connection::receive(std::source_location sourceLocation) const -> std::vector<std::byte> {
    std::vector<std::byte> buffer;

    while (true) {
        std::vector<std::byte> subBuffer{1024};
        const long result{recv(this->fileDescriptor, subBuffer.data(), subBuffer.size(), MSG_DONTWAIT)};
        if (result > 0) {
            subBuffer.resize(result);
            buffer.insert(buffer.cend(), subBuffer.cbegin(), subBuffer.cend());
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (buffer.empty()) continue;

                break;
            }

            std::string error{result == 0 ? "connection closed" : std::strerror(errno)};
            throw Exception{
                Log{Log::Level::fatal, std::move(error), sourceLocation}
            };
        }
    }

    return buffer;
}

auto Connection::socket(std::source_location sourceLocation) -> int {
    const int fileDescriptor{::socket(AF_INET, SOCK_STREAM, 0)};
    if (fileDescriptor == -1) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }

    return fileDescriptor;
}

auto Connection::translateIpAddress(in_addr &address, std::source_location sourceLocation) -> void {
    if (inet_pton(AF_INET, "127.0.0.1", &address) != 1) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }
}

auto Connection::deTranslateIpAddress(const in_addr &address, std::source_location sourceLocation) -> std::string {
    std::string buffer(INET_ADDRSTRLEN, 0);
    if (inet_ntop(AF_INET, &address, buffer.data(), buffer.size()) == nullptr) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }

    return buffer;
}

auto Connection::connect(int fileDescriptor, const sockaddr_in &address, std::source_location sourceLocation) -> void {
    if (::connect(fileDescriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }
}

auto Connection::close(std::source_location sourceLocation) const -> void {
    if (this->fileDescriptor != -1 && ::close(this->fileDescriptor) != 0) {
        throw Exception{
            Log{Log::Level::fatal, std::strerror(errno), sourceLocation}
        };
    }
}
