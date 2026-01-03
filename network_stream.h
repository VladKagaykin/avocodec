#ifndef NETWORK_STREAM_H
#define NETWORK_STREAM_H

#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <atomic>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <errno.h>
#endif

class NetworkStream {
public:
    NetworkStream();
    ~NetworkStream();
    
    // Инициализация сети
    static bool initializeNetwork();
    static void cleanupNetwork();
    
    // UDP СЕРВЕР (трансляция видео)
    bool startUDPServer(const std::string& ip, int port);
    bool sendUDPFrame(const std::vector<uint8_t>& frameData, 
                     uint32_t width, uint32_t height);
    void stopUDPServer();
    
    // UDP КЛИЕНТ (прием видео)
    bool connectToUDPServer(const std::string& host, int port);
    bool startUDPReceiver(std::function<void(const std::vector<uint8_t>&, 
                                            uint32_t, uint32_t)> frameCallback);
    void disconnectUDP();
    
    // Статус
    bool isUDPServerRunning() const { return udpServerRunning; }
    bool isUDPConnected() const { return udpClientConnected; }
    bool hasUDPClient() const { return hasClient; }
    
    // Настройки
    void setMaxPacketSize(size_t size) { maxPacketSize = size; }
    size_t getMaxPacketSize() const { return maxPacketSize; }
    
private:
    void udpServerListenerThread();
    void udpClientReceiverThread();
    
    // Серверные переменные (UDP)
    int udpServerSocket;
    struct sockaddr_in udpServerAddr;
    struct sockaddr_in udpClientAddr;
    std::atomic<bool> udpServerRunning;
    std::atomic<bool> udpServerListenerRunning;
    std::atomic<bool> hasClient;
    std::thread udpServerListenerThreadObj;
    
    // Клиентские переменные (UDP)
    int udpClientSocket;
    struct sockaddr_in udpTargetAddr;
    std::atomic<bool> udpClientConnected;
    std::thread udpClientReceiverThreadObj;
    
    // Общие
    size_t maxPacketSize;
    
    // Callback для клиента
    std::function<void(const std::vector<uint8_t>&, uint32_t, uint32_t)> frameCallback;
    
    // Для сборки фрагментированных пакетов
    struct FragmentedPacket {
        std::vector<std::vector<uint8_t>> chunks;
        uint32_t totalChunks;
        uint32_t width;
        uint32_t height;
        uint32_t frameId;
        std::chrono::steady_clock::time_point lastUpdate;
    };
    
    std::map<uint32_t, FragmentedPacket> fragmentedPackets;
    std::mutex packetMutex;
    std::mutex clientAddrMutex;
};

#endif // NETWORK_STREAM_H