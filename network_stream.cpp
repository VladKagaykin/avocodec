#include "network_stream.h"
#include "avo_codec.h"
#include <iostream>
#include <thread>
#include <cstring>
#include <chrono>
#include <queue>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define close_socket closesocket
    #define SHUT_RDWR SD_BOTH
    #define SOCKET_ERROR -1
    #define INVALID_SOCKET -1
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #define close_socket close
    #define SOCKET_ERROR -1
    #define INVALID_SOCKET -1
#endif

// Реализация ThreadPool
NetworkStream::ThreadPool::ThreadPool(size_t numThreads) : stop(false) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });
                    
                    if (this->stop && this->tasks.empty()) {
                        return;
                    }
                    
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                    taskCount--;
                }
                task();
            }
        });
    }
}

NetworkStream::ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker : workers) {
        worker.join();
    }
}

template<class F>
void NetworkStream::ThreadPool::enqueue(F&& task) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        tasks.emplace(std::forward<F>(task));
        taskCount++;
    }
    condition.notify_one();
}

void NetworkStream::ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(queueMutex);
    condition.wait(lock, [this] { return tasks.empty(); });
}

size_t NetworkStream::ThreadPool::getQueueSize() const {
    return taskCount.load();
}

NetworkStream::NetworkStream() 
    : udpServerSocket(INVALID_SOCKET), udpClientSocket(INVALID_SOCKET),
      udpServerRunning(false), udpServerListenerRunning(false),
      udpServerSenderRunning(false), udpClientConnected(false), 
      hasClient(false), maxPacketSize(60000),
      encoderPool(nullptr), activeEncoders(0),
      frameBufferRunning(false) {
    memset(&udpServerAddr, 0, sizeof(udpServerAddr));
    memset(&udpClientAddr, 0, sizeof(udpClientAddr));
    memset(&udpTargetAddr, 0, sizeof(udpTargetAddr));
    
    // Инициализация пула потоков (по умолчанию 2 потока)
    encoderPool = new ThreadPool(2);
}

NetworkStream::~NetworkStream() {
    stopUDPServer();
    disconnectUDP();
    
    frameBufferRunning = false;
    frameBufferCondVar.notify_all();
    if (frameBufferThread.joinable()) {
        frameBufferThread.join();
    }
    
    delete encoderPool;
}

bool NetworkStream::initializeNetwork() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

void NetworkStream::cleanupNetwork() {
#ifdef _WIN32
    WSACleanup();
#endif
}

NetworkStream::ServerStats NetworkStream::getStats() const {
    ServerStats stats;
    stats.framesProcessed = statsFramesProcessed.load();
    stats.bytesSent = statsBytesSent.load();
    stats.packetsSent = statsPacketsSent.load();
    stats.encodingTimeMs = statsEncodingTimeMs.load();
    stats.networkTimeMs = statsNetworkTimeMs.load();
    stats.bufferDropped = statsBufferDropped.load();
    return stats;
}

void NetworkStream::resetStats() {
    statsFramesProcessed = 0;
    statsBytesSent = 0;
    statsPacketsSent = 0;
    statsEncodingTimeMs = 0;
    statsNetworkTimeMs = 0;
    statsBufferDropped = 0;
}

void NetworkStream::setEncoderThreads(int count) {
    delete encoderPool;
    encoderPool = new ThreadPool(count > 0 ? count : 2);
}

// ================= UDP СЕРВЕР =================
bool NetworkStream::startUDPServer(const std::string& ip, int port) {
    udpServerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpServerSocket == INVALID_SOCKET) {
        std::cerr << "[UDP SERVER] Error creating socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    int reuse = 1;
    setsockopt(udpServerSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    
    // Увеличиваем буферы
    int bufSize = 1024 * 1024; // 1MB
    setsockopt(udpServerSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));
    
    udpServerAddr.sin_family = AF_INET;
    udpServerAddr.sin_port = htons(port);
    
    if (ip.empty() || ip == "0.0.0.0" || ip == "any") {
        udpServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, ip.c_str(), &udpServerAddr.sin_addr) <= 0) {
            std::cerr << "[UDP SERVER] Invalid IP address: " << ip << std::endl;
            close_socket(udpServerSocket);
            udpServerSocket = INVALID_SOCKET;
            return false;
        }
    }
    
    if (bind(udpServerSocket, (struct sockaddr*)&udpServerAddr, sizeof(udpServerAddr)) == SOCKET_ERROR) {
        std::cerr << "[UDP SERVER] Error binding socket: " << strerror(errno) << std::endl;
        close_socket(udpServerSocket);
        udpServerSocket = INVALID_SOCKET;
        return false;
    }
    
    udpServerRunning = true;
    udpServerListenerRunning = true;
    udpServerSenderRunning = true;
    hasClient = false;
    frameBufferRunning = true;
    
    // Запускаем поток для прослушивания подключений клиентов
    udpServerListenerThreadObj = std::thread(&NetworkStream::udpServerListenerThread, this);
    
    // Запускаем поток для отправки данных
    udpServerSenderThreadObj = std::thread(&NetworkStream::udpServerSenderThread, this);
    
    // Запускаем поток для обработки буфера кадров
    frameBufferThread = std::thread(&NetworkStream::frameBufferWorker, this);
    
    std::cout << "[UDP SERVER] Started on " << (ip.empty() ? "0.0.0.0" : ip) 
              << ":" << port << std::endl;
    std::cout << "[UDP SERVER] Encoder threads: " << encoderPool->getQueueSize() << std::endl;
    std::cout << "[UDP SERVER] Waiting for client connection..." << std::endl;
    
    return true;
}

void NetworkStream::udpServerListenerThread() {
    std::cout << "[UDP SERVER] Listener thread started" << std::endl;
    
    const int BUFFER_SIZE = 1024;
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    while (udpServerListenerRunning) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        
        // Устанавливаем таймаут для recvfrom
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(udpServerSocket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout));
        
        int bytesReceived = recvfrom(udpServerSocket, 
                                    (char*)buffer.data(), 
                                    BUFFER_SIZE, 0,
                                    (struct sockaddr*)&clientAddr, &clientLen);
        
        if (bytesReceived > 0) {
            // Получили пакет от клиента - запоминаем его адрес
            {
                std::lock_guard<std::mutex> lock(clientAddrMutex);
                udpClientAddr = clientAddr;
                hasClient = true;
            }
            
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
            
            std::cout << "[UDP SERVER] Client connected from " 
                      << clientIP << ":" 
                      << ntohs(clientAddr.sin_port) << std::endl;
            
            // Отправляем подтверждение клиенту
            const char* ack = "ACK";
            sendto(udpServerSocket, ack, strlen(ack), 0,
                  (struct sockaddr*)&clientAddr, sizeof(clientAddr));
            
        } else if (bytesReceived < 0) {
            // Таймаут или ошибка
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[UDP SERVER] Receive error: " << strerror(errno) << std::endl;
            }
        }
        
        // Небольшая задержка для уменьшения нагрузки на CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "[UDP SERVER] Listener thread stopped" << std::endl;
}

void NetworkStream::frameBufferWorker() {
    std::cout << "[UDP SERVER] Frame buffer worker started" << std::endl;
    
    while (frameBufferRunning) {
        FrameBuffer frameBuffer;
        bool hasFrame = false;
        
        {
            std::unique_lock<std::mutex> lock(frameBufferMutex);
            frameBufferCondVar.wait_for(lock, std::chrono::milliseconds(10), 
                [this]() { return !frameBufferQueue.empty() || !frameBufferRunning; });
            
            if (!frameBufferRunning && frameBufferQueue.empty()) {
                break;
            }
            
            if (!frameBufferQueue.empty()) {
                frameBuffer = frameBufferQueue.front();
                frameBufferQueue.pop();
                hasFrame = true;
            }
        }
        
        if (!hasFrame) {
            continue;
        }
        
        // Проверяем, не устарел ли кадр
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if (now - frameBuffer.timestamp > 500) { // Кадр старше 500ms
            std::cout << "[UDP SERVER] Skipping stale frame (age: " 
                     << (now - frameBuffer.timestamp) << "ms)" << std::endl;
            statsBufferDropped++;
            continue;
        }
        
        // Проверяем подключение клиента
        {
            std::lock_guard<std::mutex> lock(clientAddrMutex);
            if (!hasClient) {
                continue;
            }
        }
        
        // Кодируем и отправляем в отдельном потоке пула
        activeEncoders++;
        encoderPool->enqueue([this, frameBuffer]() {
            auto encodeStart = std::chrono::high_resolution_clock::now();
            encodeAndSendFrame(frameBuffer);
            auto encodeEnd = std::chrono::high_resolution_clock::now();
            auto encodeTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                encodeEnd - encodeStart);
            statsEncodingTimeMs += encodeTime.count();
            activeEncoders--;
        });
    }
    
    std::cout << "[UDP SERVER] Frame buffer worker stopped" << std::endl;
}

void NetworkStream::encodeAndSendFrame(FrameBuffer frameBuffer) {
    if (frameBuffer.frame.empty()) {
        return;
    }
    
    // Статическое хранение предыдущего кадра для каждого размера
    static std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> prevFrames;
    static std::mutex prevFramesMutex;
    
    std::vector<uint8_t> prevFrame;
    auto key = std::make_pair(frameBuffer.width, frameBuffer.height);
    
    {
        std::lock_guard<std::mutex> lock(prevFramesMutex);
        if (prevFrames.find(key) == prevFrames.end()) {
            prevFrames[key] = AVOCodec::createBlackFrame(frameBuffer.width, frameBuffer.height);
        }
        prevFrame = prevFrames[key];
    }
    
    // Кодируем разницу
    std::vector<PixelChange> changes;
    AVOCodec::compareFrames(prevFrame, frameBuffer.frame, 
                           frameBuffer.width, frameBuffer.height, changes);
    
    if (changes.empty()) {
        // Нет изменений - отправляем минимальный пакет
        FramePacket packet;
        packet.data = {0}; // Один байт - маркер "нет изменений"
        packet.width = frameBuffer.width;
        packet.height = frameBuffer.height;
        packet.isFullFrame = false;
        
        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            if (sendQueue.size() < 10) {
                sendQueue.push(packet);
                statsPacketsSent++;
            } else {
                statsBufferDropped++;
            }
        }
        sendQueueCondVar.notify_one();
        return;
    }
    
    // Сжимаем изменения
    std::vector<uint8_t> compressed = AVOCodec::compressRLE(changes);
    
    // Обновляем предыдущий кадр
    {
        std::lock_guard<std::mutex> lock(prevFramesMutex);
        prevFrames[key] = frameBuffer.frame;
    }
    
    // Отправляем
    FramePacket packet;
    packet.data = compressed;
    packet.width = frameBuffer.width;
    packet.height = frameBuffer.height;
    packet.isFullFrame = false;
    
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex);
        if (sendQueue.size() < 10) {
            sendQueue.push(packet);
            statsPacketsSent++;
            statsBytesSent += compressed.size();
        } else {
            // Удаляем самые старые пакеты при переполнении
            while (sendQueue.size() >= 8) {
                sendQueue.pop();
                statsBufferDropped++;
            }
            sendQueue.push(packet);
            statsPacketsSent++;
            statsBytesSent += compressed.size();
        }
    }
    sendQueueCondVar.notify_one();
    
    // Обновляем статистику
    statsFramesProcessed++;
}

void NetworkStream::udpServerSenderThread() {
    std::cout << "[UDP SERVER] Sender thread started" << std::endl;
    
    while (udpServerSenderRunning) {
        FramePacket packet;
        bool hasPacket = false;
        
        {
            std::unique_lock<std::mutex> lock(sendQueueMutex);
            sendQueueCondVar.wait_for(lock, std::chrono::milliseconds(100), 
                [this]() { return !sendQueue.empty() || !udpServerSenderRunning; });
            
            if (!udpServerSenderRunning && sendQueue.empty()) {
                break;
            }
            
            if (!sendQueue.empty()) {
                packet = sendQueue.front();
                sendQueue.pop();
                hasPacket = true;
            }
        }
        
        if (!hasPacket) {
            continue;
        }
        
        // Проверяем, есть ли клиент
        {
            std::lock_guard<std::mutex> lock(clientAddrMutex);
            if (!hasClient) {
                continue;
            }
        }
        
        // Разбиваем данные на части и отправляем
        const size_t MAX_UDP_SIZE = 60000;
        static uint32_t frameId = 0;
        frameId++;
        
        if (packet.data.size() <= MAX_UDP_SIZE) {
            // Отправляем одним пакетом
            auto networkPacket = AVOCodec::createNetworkPacket(packet.data, frameId, 0, 1, 
                                                              packet.width, packet.height);
            
            socklen_t addrLen = sizeof(udpClientAddr);
            auto sendStart = std::chrono::high_resolution_clock::now();
            int sent = sendto(udpServerSocket, 
                            (const char*)networkPacket.data(), 
                            networkPacket.size(), 0,
                            (struct sockaddr*)&udpClientAddr, 
                            addrLen);
            auto sendEnd = std::chrono::high_resolution_clock::now();
            auto sendTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                sendEnd - sendStart);
            statsNetworkTimeMs += sendTime.count();
            
            if (sent != static_cast<int>(networkPacket.size())) {
                std::cerr << "[UDP SERVER] Failed to send packet: " 
                          << strerror(errno) << " (sent " << sent << " of " 
                          << networkPacket.size() << " bytes)" << std::endl;
            }
        } else {
            // Фрагментация на несколько пакетов
            size_t totalPackets = (packet.data.size() + MAX_UDP_SIZE - 1) / MAX_UDP_SIZE;
            
            for (size_t packetId = 0; packetId < totalPackets; packetId++) {
                size_t offset = packetId * MAX_UDP_SIZE;
                size_t chunkSize = std::min(MAX_UDP_SIZE, packet.data.size() - offset);
                
                std::vector<uint8_t> chunk(packet.data.begin() + offset, 
                                          packet.data.begin() + offset + chunkSize);
                
                auto networkPacket = AVOCodec::createNetworkPacket(chunk, frameId, 
                                                                  packetId, totalPackets, 
                                                                  packet.width, packet.height);
                
                socklen_t addrLen = sizeof(udpClientAddr);
                auto sendStart = std::chrono::high_resolution_clock::now();
                int sent = sendto(udpServerSocket, 
                                (const char*)networkPacket.data(), 
                                networkPacket.size(), 0,
                                (struct sockaddr*)&udpClientAddr, 
                                addrLen);
                auto sendEnd = std::chrono::high_resolution_clock::now();
                auto sendTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    sendEnd - sendStart);
                statsNetworkTimeMs += sendTime.count();
                
                if (sent != static_cast<int>(networkPacket.size())) {
                    std::cerr << "[UDP SERVER] Failed to send chunk " 
                             << packetId << " of " << totalPackets 
                             << ": " << strerror(errno) << std::endl;
                    break;
                }
                
                // Небольшая задержка между пакетами
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
    
    std::cout << "[UDP SERVER] Sender thread stopped" << std::endl;
}

bool NetworkStream::sendUDPFrame(const std::vector<uint8_t>& frameData, 
                                uint32_t width, uint32_t height, bool isFullFrame) {
    if (!udpServerRunning || udpServerSocket == INVALID_SOCKET) {
        return false;
    }
    
    // Проверяем, есть ли подключенные клиенты
    {
        std::lock_guard<std::mutex> lock(clientAddrMutex);
        if (!hasClient) {
            static int noClientCount = 0;
            if (noClientCount++ % 60 == 0) {
                std::cout << "[UDP SERVER] No clients connected yet" << std::endl;
            }
            return true;
        }
    }
    
    if (frameData.empty()) {
        std::cerr << "[UDP SERVER] Empty frame data" << std::endl;
        return false;
    }
    
    // Создаем буфер кадра
    FrameBuffer buffer;
    buffer.frame = frameData;
    buffer.width = width;
    buffer.height = height;
    buffer.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static std::atomic<uint32_t> frameCounter(0);
    buffer.frameId = frameCounter++;
    
    // Помещаем в очередь буферов
    {
        std::lock_guard<std::mutex> lock(frameBufferMutex);
        if (frameBufferQueue.size() < 15) {
            frameBufferQueue.push(buffer);
        } else {
            // Если очередь полна, удаляем самый старый кадр
            frameBufferQueue.pop();
            frameBufferQueue.push(buffer);
            statsBufferDropped++;
        }
    }
    frameBufferCondVar.notify_one();
    
    return true;
}

void NetworkStream::stopUDPServer() {
    udpServerListenerRunning = false;
    udpServerSenderRunning = false;
    udpServerRunning = false;
    frameBufferRunning = false;
    
    sendQueueCondVar.notify_all();
    frameBufferCondVar.notify_all();
    
    if (udpServerListenerThreadObj.joinable()) {
        udpServerListenerThreadObj.join();
    }
    
    if (udpServerSenderThreadObj.joinable()) {
        udpServerSenderThreadObj.join();
    }
    
    if (frameBufferThread.joinable()) {
        frameBufferThread.join();
    }
    
    if (udpServerSocket != INVALID_SOCKET) {
        close_socket(udpServerSocket);
        udpServerSocket = INVALID_SOCKET;
    }
    
    {
        std::lock_guard<std::mutex> lock(clientAddrMutex);
        hasClient = false;
        memset(&udpClientAddr, 0, sizeof(udpClientAddr));
    }
    
    // Очищаем очереди
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex);
        while (!sendQueue.empty()) {
            sendQueue.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(frameBufferMutex);
        std::queue<FrameBuffer> empty;
        std::swap(frameBufferQueue, empty);
    }
    
    std::cout << "[UDP SERVER] Stopped" << std::endl;
}

// ================= UDP КЛИЕНТ =================
bool NetworkStream::connectToUDPServer(const std::string& host, int port) {
    udpClientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpClientSocket == INVALID_SOCKET) {
        std::cerr << "[UDP CLIENT] Error creating socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Увеличиваем буфер приема
    int bufSize = 1024 * 1024; // 1MB
    setsockopt(udpClientSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));
    
    memset(&udpTargetAddr, 0, sizeof(udpTargetAddr));
    udpTargetAddr.sin_family = AF_INET;
    udpTargetAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &udpTargetAddr.sin_addr) <= 0) {
        struct hostent* server = gethostbyname(host.c_str());
        if (server == nullptr) {
            std::cerr << "[UDP CLIENT] Cannot resolve host: " << host << std::endl;
            close_socket(udpClientSocket);
            udpClientSocket = INVALID_SOCKET;
            return false;
        }
        memcpy(&udpTargetAddr.sin_addr.s_addr, server->h_addr, server->h_length);
    }
    
    // Устанавливаем таймаут приема
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(udpClientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    
    // Отправляем пустой пакет для установки соединения
    const char* connectMsg = "CONNECT";
    sendto(udpClientSocket, connectMsg, strlen(connectMsg), 0,
          (struct sockaddr*)&udpTargetAddr, sizeof(udpTargetAddr));
    
    // Ждем подтверждения от сервера
    char ackBuffer[10];
    socklen_t addrLen = sizeof(udpTargetAddr);
    int ackBytes = recvfrom(udpClientSocket, ackBuffer, sizeof(ackBuffer) - 1, 0,
                           (struct sockaddr*)&udpTargetAddr, &addrLen);
    
    if (ackBytes > 0) {
        ackBuffer[ackBytes] = '\0';
        if (strcmp(ackBuffer, "ACK") == 0) {
            udpClientConnected = true;
            std::cout << "[UDP CLIENT] Connected to " << host << ":" << port << std::endl;
            return true;
        }
    }
    
    std::cerr << "[UDP CLIENT] Connection failed - no response from server" << std::endl;
    close_socket(udpClientSocket);
    udpClientSocket = INVALID_SOCKET;
    return false;
}

bool NetworkStream::startUDPReceiver(std::function<void(const std::vector<uint8_t>&, 
                                                       uint32_t, uint32_t, bool)> frameCallback) {
    if (!udpClientConnected || udpClientSocket == INVALID_SOCKET) {
        return false;
    }
    
    this->frameCallback = frameCallback;
    
    udpClientReceiverThreadObj = std::thread(&NetworkStream::udpClientReceiverThread, this);
    
    return true;
}

void NetworkStream::udpClientReceiverThread() {
    std::cout << "[UDP CLIENT] Receiver thread started" << std::endl;
    
    const int BUFFER_SIZE = 65507; // Максимальный размер UDP пакета
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    while (udpClientConnected) {
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        
        int bytesReceived = recvfrom(udpClientSocket, 
                                    (char*)buffer.data(), 
                                    BUFFER_SIZE, 0,
                                    (struct sockaddr*)&fromAddr, &fromLen);
        
        if (bytesReceived > 0) {
            std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + bytesReceived);
            
            // Парсим пакет
            std::vector<uint8_t> data;
            uint32_t frameId, packetId, totalPackets, width, height;
            
            if (AVOCodec::parseNetworkPacket(packet, data, frameId, packetId, 
                                             totalPackets, width, height)) {
                
                if (totalPackets == 1) {
                    // Одиночный пакет - сразу обрабатываем
                    bool isFullFrame = (data.size() == width * height * 3);
                    if (frameCallback) {
                        frameCallback(data, width, height, isFullFrame);
                    }
                } else {
                    // Фрагментированный пакет - собираем
                    std::lock_guard<std::mutex> lock(packetMutex);
                    uint32_t packetKey = (frameId << 16) | (width & 0xFFFF);
                    
                    auto& fragPacket = fragmentedPackets[packetKey];
                    fragPacket.frameId = frameId;
                    fragPacket.width = width;
                    fragPacket.height = height;
                    fragPacket.totalChunks = totalPackets;
                    fragPacket.lastUpdate = std::chrono::steady_clock::now();
                    
                    if (fragPacket.chunks.size() < totalPackets) {
                        fragPacket.chunks.resize(totalPackets);
                    }
                    
                    if (packetId < totalPackets) {
                        fragPacket.chunks[packetId] = data;
                        
                        // Проверяем, является ли это полным кадром
                        if (packetId == 0) {
                            uint32_t expectedFullFrameSize = width * height * 3;
                            fragPacket.isFullFrame = (data.size() == expectedFullFrameSize);
                        }
                    }
                    
                    // Проверяем, все ли части получены
                    bool complete = true;
                    for (uint32_t i = 0; i < totalPackets; i++) {
                        if (fragPacket.chunks[i].empty()) {
                            complete = false;
                            break;
                        }
                    }
                    
                    if (complete) {
                        // Собираем полный кадр
                        std::vector<uint8_t> completeData;
                        for (const auto& chunk : fragPacket.chunks) {
                            completeData.insert(completeData.end(), 
                                              chunk.begin(), chunk.end());
                        }
                        
                        if (frameCallback) {
                            frameCallback(completeData, width, height, fragPacket.isFullFrame);
                        }
                        
                        // Удаляем из map
                        fragmentedPackets.erase(packetKey);
                    }
                }
            }
        } else if (bytesReceived < 0) {
            // Таймаут или ошибка
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[UDP CLIENT] Receive error: " << strerror(errno) << std::endl;
            }
        }
        
        // Очищаем старые незавершенные пакеты
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(packetMutex);
        for (auto it = fragmentedPackets.begin(); it != fragmentedPackets.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.lastUpdate).count() > 5) {
                it = fragmentedPackets.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    std::cout << "[UDP CLIENT] Receiver thread stopped" << std::endl;
}

void NetworkStream::disconnectUDP() {
    udpClientConnected = false;
    
    if (udpClientSocket != INVALID_SOCKET) {
        close_socket(udpClientSocket);
        udpClientSocket = INVALID_SOCKET;
    }
    
    if (udpClientReceiverThreadObj.joinable()) {
        udpClientReceiverThreadObj.join();
    }
    
    std::cout << "[UDP CLIENT] Disconnected" << std::endl;
}