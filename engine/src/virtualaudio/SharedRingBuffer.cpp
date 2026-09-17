#include "helix/virtualaudio/SharedRingBuffer.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace helix::virtualaudio {

namespace {

std::size_t nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 2;
    while (result < value) result <<= 1;
    return result;
}

std::string platformName(const std::string& name) {
#if defined(_WIN32)
    return "Local\\helix." + name;
#else
    return "/helix." + name;
#endif
}

} // namespace

SharedRingBuffer::~SharedRingBuffer() { close(); }

SharedRingBuffer::SharedRingBuffer(SharedRingBuffer&& other) noexcept { *this = std::move(other); }

SharedRingBuffer& SharedRingBuffer::operator=(SharedRingBuffer&& other) noexcept {
    if (this == &other) return *this;
    close();
    name_          = std::move(other.name_);
    header_        = other.header_;
    mapping_       = other.mapping_;
    mappingBytes_  = other.mappingBytes_;
    owner_         = other.owner_;
#if defined(_WIN32)
    fileMapping_   = other.fileMapping_;
    other.fileMapping_ = nullptr;
#else
    fileDescriptor_ = other.fileDescriptor_;
    other.fileDescriptor_ = -1;
#endif
    other.header_ = nullptr;
    other.mapping_ = nullptr;
    other.mappingBytes_ = 0;
    other.owner_ = false;
    return *this;
}

Status SharedRingBuffer::create(const std::string& name, int channels, double sampleRate,
                                int capacityFrames) {
    close();

    const int useChannels = std::clamp(channels, 1, kMaxStreamChannels);
    const auto capacity = static_cast<std::uint32_t>(
        nextPowerOfTwo(static_cast<std::size_t>(std::max(capacityFrames, 256))));

    const std::size_t headerBytes = sizeof(SharedRingHeader);
    const std::size_t dataBytes = static_cast<std::size_t>(capacity) *
                                  static_cast<std::size_t>(useChannels) * sizeof(Sample);
    mappingBytes_ = headerBytes + dataBytes;

    const std::string systemName = platformName(name);

#if defined(_WIN32)
    fileMapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(mappingBytes_ >> 32),
                                      static_cast<DWORD>(mappingBytes_ & 0xFFFFFFFFu),
                                      systemName.c_str());
    if (fileMapping_ == nullptr)
        return Status::error("Nie można utworzyć segmentu pamięci: " + systemName);

    mapping_ = MapViewOfFile(fileMapping_, FILE_MAP_ALL_ACCESS, 0, 0, mappingBytes_);
    if (mapping_ == nullptr) {
        CloseHandle(fileMapping_);
        fileMapping_ = nullptr;
        return Status::error("Nie można zmapować segmentu pamięci");
    }
#else
    shm_unlink(systemName.c_str());
    fileDescriptor_ = shm_open(systemName.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fileDescriptor_ < 0)
        return Status::error("Nie można utworzyć segmentu pamięci: " + systemName);

    if (ftruncate(fileDescriptor_, static_cast<off_t>(mappingBytes_)) != 0) {
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
        shm_unlink(systemName.c_str());
        return Status::error("Nie można ustawić rozmiaru segmentu pamięci");
    }

    mapping_ = mmap(nullptr, mappingBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescriptor_, 0);
    if (mapping_ == MAP_FAILED) {
        mapping_ = nullptr;
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
        shm_unlink(systemName.c_str());
        return Status::error("Nie można zmapować segmentu pamięci");
    }
#endif

    std::memset(mapping_, 0, mappingBytes_);
    header_ = static_cast<SharedRingHeader*>(mapping_);
    header_->magic          = kSharedRingMagic;
    header_->version        = kSharedRingVersion;
    header_->channels       = static_cast<std::uint32_t>(useChannels);
    header_->sampleRate     = static_cast<std::uint32_t>(sampleRate);
    header_->capacityFrames = capacity;
    header_->headerBytes    = static_cast<std::uint32_t>(headerBytes);
    header_->writeIndex.store(0, std::memory_order_relaxed);
    header_->readIndex.store(0, std::memory_order_relaxed);

    name_  = name;
    owner_ = true;
    return Status::success();
}

Status SharedRingBuffer::open(const std::string& name, SharedRingRole role) {
    close();
    const std::string systemName = platformName(name);

#if defined(_WIN32)
    fileMapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, systemName.c_str());
    if (fileMapping_ == nullptr)
        return Status::error("Segment pamięci nie istnieje: " + systemName);

    mapping_ = MapViewOfFile(fileMapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (mapping_ == nullptr) {
        CloseHandle(fileMapping_);
        fileMapping_ = nullptr;
        return Status::error("Nie można zmapować segmentu pamięci");
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(mapping_, &info, sizeof(info)) != 0) mappingBytes_ = info.RegionSize;
#else
    fileDescriptor_ = shm_open(systemName.c_str(), O_RDWR, 0600);
    if (fileDescriptor_ < 0)
        return Status::error("Segment pamięci nie istnieje: " + systemName);

    struct stat info{};
    if (fstat(fileDescriptor_, &info) != 0) {
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
        return Status::error("Nie można odczytać rozmiaru segmentu");
    }
    mappingBytes_ = static_cast<std::size_t>(info.st_size);

    mapping_ = mmap(nullptr, mappingBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescriptor_, 0);
    if (mapping_ == MAP_FAILED) {
        mapping_ = nullptr;
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
        return Status::error("Nie można zmapować segmentu pamięci");
    }
#endif

    header_ = static_cast<SharedRingHeader*>(mapping_);
    if (header_->magic != kSharedRingMagic) {
        close();
        return Status::error("Nieprawidłowy segment pamięci (zła sygnatura)");
    }
    if (header_->version != kSharedRingVersion) {
        const std::uint32_t found = header_->version;
        close();
        return Status::error("Niezgodna wersja transportu: " + std::to_string(found));
    }

    name_  = name;
    owner_ = false;
    markAlive(role, true);
    return Status::success();
}

void SharedRingBuffer::close() {
    if (header_ != nullptr) {
        header_->producerAlive.store(0, std::memory_order_release);
        header_->consumerAlive.store(0, std::memory_order_release);
    }

#if defined(_WIN32)
    if (mapping_ != nullptr) UnmapViewOfFile(mapping_);
    if (fileMapping_ != nullptr) CloseHandle(fileMapping_);
    fileMapping_ = nullptr;
#else
    if (mapping_ != nullptr) munmap(mapping_, mappingBytes_);
    if (fileDescriptor_ >= 0) ::close(fileDescriptor_);
    if (owner_ && !name_.empty()) shm_unlink(platformName(name_).c_str());
    fileDescriptor_ = -1;
#endif

    mapping_ = nullptr;
    header_ = nullptr;
    mappingBytes_ = 0;
    owner_ = false;
    name_.clear();
}

int SharedRingBuffer::channels() const noexcept {
    return header_ ? static_cast<int>(header_->channels) : 0;
}

double SharedRingBuffer::sampleRate() const noexcept {
    return header_ ? static_cast<double>(header_->sampleRate) : 0.0;
}

int SharedRingBuffer::capacityFrames() const noexcept {
    return header_ ? static_cast<int>(header_->capacityFrames) : 0;
}

Sample* SharedRingBuffer::data() noexcept {
    if (header_ == nullptr) return nullptr;
    return reinterpret_cast<Sample*>(reinterpret_cast<char*>(mapping_) + header_->headerBytes);
}

const Sample* SharedRingBuffer::data() const noexcept {
    if (header_ == nullptr) return nullptr;
    return reinterpret_cast<const Sample*>(reinterpret_cast<const char*>(mapping_) + header_->headerBytes);
}

int SharedRingBuffer::availableToRead() const noexcept {
    if (header_ == nullptr) return 0;
    const std::uint64_t w = header_->writeIndex.load(std::memory_order_acquire);
    const std::uint64_t r = header_->readIndex.load(std::memory_order_acquire);
    return static_cast<int>(w - r);
}

int SharedRingBuffer::availableToWrite() const noexcept {
    if (header_ == nullptr) return 0;
    return static_cast<int>(header_->capacityFrames) - availableToRead();
}

int SharedRingBuffer::write(const Sample* interleaved, int frames) noexcept {
    if (header_ == nullptr || interleaved == nullptr) return 0;

    const int channels = static_cast<int>(header_->channels);
    const auto capacity = static_cast<std::uint64_t>(header_->capacityFrames);
    const std::uint64_t mask = capacity - 1;

    const int canWrite = std::min(frames, availableToWrite());
    if (canWrite < frames)
        header_->overruns.fetch_add(static_cast<std::uint64_t>(frames - canWrite), std::memory_order_relaxed);
    if (canWrite <= 0) return 0;

    std::uint64_t write = header_->writeIndex.load(std::memory_order_relaxed);
    Sample* base = data();

    for (int i = 0; i < canWrite; ++i) {
        const std::uint64_t slot = (write + static_cast<std::uint64_t>(i)) & mask;
        std::memcpy(base + slot * static_cast<std::uint64_t>(channels),
                    interleaved + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels),
                    static_cast<std::size_t>(channels) * sizeof(Sample));
    }

    header_->writeIndex.store(write + static_cast<std::uint64_t>(canWrite), std::memory_order_release);
    return canWrite;
}

int SharedRingBuffer::read(Sample* interleaved, int frames) noexcept {
    if (header_ == nullptr || interleaved == nullptr) return 0;

    const int channels = static_cast<int>(header_->channels);
    const auto capacity = static_cast<std::uint64_t>(header_->capacityFrames);
    const std::uint64_t mask = capacity - 1;

    const int canRead = std::min(frames, availableToRead());
    if (canRead < frames) {
        header_->underruns.fetch_add(static_cast<std::uint64_t>(frames - canRead), std::memory_order_relaxed);
        std::memset(interleaved + static_cast<std::size_t>(canRead) * static_cast<std::size_t>(channels), 0,
                    static_cast<std::size_t>(frames - canRead) * static_cast<std::size_t>(channels)
                        * sizeof(Sample));
    }
    if (canRead <= 0) return 0;

    std::uint64_t read = header_->readIndex.load(std::memory_order_relaxed);
    const Sample* base = data();

    for (int i = 0; i < canRead; ++i) {
        const std::uint64_t slot = (read + static_cast<std::uint64_t>(i)) & mask;
        std::memcpy(interleaved + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels),
                    base + slot * static_cast<std::uint64_t>(channels),
                    static_cast<std::size_t>(channels) * sizeof(Sample));
    }

    header_->readIndex.store(read + static_cast<std::uint64_t>(canRead), std::memory_order_release);
    return canRead;
}

void SharedRingBuffer::markAlive(SharedRingRole role, bool alive) noexcept {
    if (header_ == nullptr) return;
    auto& flag = (role == SharedRingRole::Producer) ? header_->producerAlive : header_->consumerAlive;
    flag.store(alive ? 1u : 0u, std::memory_order_release);
}

bool SharedRingBuffer::peerAlive(SharedRingRole role) const noexcept {
    if (header_ == nullptr) return false;
    const auto& flag = (role == SharedRingRole::Producer) ? header_->producerAlive : header_->consumerAlive;
    return flag.load(std::memory_order_acquire) != 0u;
}

std::uint64_t SharedRingBuffer::overruns() const noexcept {
    return header_ ? header_->overruns.load(std::memory_order_relaxed) : 0;
}

std::uint64_t SharedRingBuffer::underruns() const noexcept {
    return header_ ? header_->underruns.load(std::memory_order_relaxed) : 0;
}

} // namespace helix::virtualaudio
