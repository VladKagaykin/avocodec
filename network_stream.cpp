#include "network_stream.h"
#include "avo_codec.h"
#include <iostream>
#include <thread>
#include <cstring>
#include <chrono>
#include <queue>
#include <algorithm>
#include <sstream>

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

NetworkStream::NetworkStream() 
    : udpServerSocket(INVALID_SOCKET), udpClientSocket(INVALID_SOCKET),
      udpServerRunning(false), udpServerListenerRunning(false),
      udpClientConnected(false), hasClient(false), maxPacketSize(60000) {
    memset(&udpServerAddr, 0, sizeof(udpServerAddr));
    memset(&udpClientAddr, 0, sizeof(udpClientAddr));
    memset(&udpTargetAddr, 0, sizeof(udpTargetAddr));
}

NetworkStream::~NetworkStream() {
    stopUDPServer();
    disconnectUDP();
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
    hasClient = false;
    
    // Запускаем поток для прослушивания подключений клиентов
    udpServerListenerThreadObj = std::thread(&NetworkStream::udpServerListenerThread, this);
    
    std::cout << "[UDP SERVER] Started on " << (ip.empty() ? "0.0.0.0" : ip) 
              << ":" << port << std::endl;
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
        
        // Устанавливаем таймаут для recvfrom (неблокирующий режим)
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1 секунда
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
            // Таймаут или ошибка - продолжаем ждать
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Если это не таймаут, выводим ошибку
                std::cerr << "[UDP SERVER] Receive error: " << strerror(errno) << std::endl;
            }
        }
        
        // Небольшая задержка для уменьшения нагрузки на CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "[UDP SERVER] Listener thread stopped" << std::endl;
}

bool NetworkStream::sendUDPFrame(const std::vector<uint8_t>& frameData, 
                                uint32_t width, uint32_t height) {
    if (!udpServerRunning || udpServerSocket == INVALID_SOCKET) {
        return false;
    }
    
    // Проверяем, есть ли подключенные клиенты
    {
        std::lock_guard<std::mutex> lock(clientAddrMutex);
        if (!hasClient) {
            // Нет подключенных клиентов, но это не ошибка
            static int noClientCount = 0;
            if (noClientCount++ % 30 == 0) { // Сообщаем раз в 30 кадров
                std::cout << "[UDP SERVER] No clients connected yet" << std::endl;
            }
            return true;
        }
    }
    
    if (frameData.empty()) {
        std::cerr << "[UDP SERVER] Empty frame data" << std::endl;
        return false;
    }
    
    // Проверяем размер данных
    const size_t MAX_UDP_SIZE = 60000; // Безопасный размер для UDP
    static uint32_t frameId = 0;
    frameId++;
    
    bool success = true;
    
    if (frameData.size() <= MAX_UDP_SIZE) {
        // Отправляем одним пакетом
        auto packet = AVOCodec::createNetworkPacket(frameData, frameId, 0, 1, width, height);
        
        socklen_t addrLen = sizeof(udpClientAddr);
        int sent = sendto(udpServerSocket, 
                         (const char*)packet.data(), 
                         packet.size(), 0,
                         (struct sockaddr*)&udpClientAddr, 
                         addrLen);
        
        if (sent != static_cast<int>(packet.size())) {
            std::cerr << "[UDP SERVER] Failed to send packet: " 
                      << strerror(errno) << " (sent " << sent << " of " 
                      << packet.size() << " bytes)" << std::endl;
            success = false;
        }
    } else {
        // Фрагментация на несколько пакетов
        size_t totalPackets = (frameData.size() + MAX_UDP_SIZE - 1) / MAX_UDP_SIZE;
        
        for (size_t packetId = 0; packetId < totalPackets; packetId++) {
            size_t offset = packetId * MAX_UDP_SIZE;
            size_t chunkSize = std::min(MAX_UDP_SIZE, frameData.size() - offset);
            
            std::vector<uint8_t> chunk(frameData.begin() + offset, 
                                      frameData.begin() + offset + chunkSize);
            
            auto packet = AVOCodec::createNetworkPacket(chunk, frameId, 
                                                       packetId, totalPackets, 
                                                       width, height);
            
            socklen_t addrLen = sizeof(udpClientAddr);
            int sent = sendto(udpServerSocket, 
                            (const char*)packet.data(), 
                            packet.size(), 0,
                            (struct sockaddr*)&udpClientAddr, 
                            addrLen);
            
            if (sent != static_cast<int>(packet.size())) {
                std::cerr << "[UDP SERVER] Failed to send chunk " 
                         << packetId << " of " << totalPackets 
                         << ": " << strerror(errno) << std::endl;
                success = false;
                break;
            }
            
            // Небольшая задержка между пакетами
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    return success;
}

void NetworkStream::stopUDPServer() {
    udpServerListenerRunning = false;
    udpServerRunning = false;
    
    if (udpServerListenerThreadObj.joinable()) {
        udpServerListenerThreadObj.join();
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
                                                       uint32_t, uint32_t)> frameCallback) {
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
                    if (frameCallback) {
                        frameCallback(data, width, height);
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
                            frameCallback(completeData, width, height);
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