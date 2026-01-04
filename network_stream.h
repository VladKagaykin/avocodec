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
#include <queue>
#include <condition_variable>
#include <future>

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

struct FramePacket {
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    bool isFullFrame;
};

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
                     uint32_t width, uint32_t height, bool isFullFrame = false);
    void stopUDPServer();
    
    // UDP КЛИЕНТ (прием видео)
    bool connectToUDPServer(const std::string& host, int port);
    bool startUDPReceiver(std::function<void(const std::vector<uint8_t>&, 
                                            uint32_t, uint32_t, bool)> frameCallback);
    void disconnectUDP();
    
    // Статус
    bool isUDPServerRunning() const { return udpServerRunning; }
    bool isUDPConnected() const { return udpClientConnected; }
    bool hasUDPClient() const { return hasClient; }
    
    // Настройки
    void setMaxPacketSize(size_t size) { maxPacketSize = size; }
    size_t getMaxPacketSize() const { return maxPacketSize; }
    
    // Публичные методы для доступа
    int getServerSocket() const { return udpServerSocket; }
    sockaddr_in getClientAddr() const { return udpClientAddr; }
    bool hasClientConnection() const { return hasClient; }
    
    // Многопоточная обработка
    void setEncoderThreads(int count);
    
    // Статистика
    struct ServerStats {
        uint64_t framesProcessed;
        uint64_t bytesSent;
        uint64_t packetsSent;
        uint64_t encodingTimeMs;
        uint64_t networkTimeMs;
        uint64_t bufferDropped;
    };
    
    ServerStats getStats() const;
    void resetStats();
    
private:
    void udpServerListenerThread();
    void udpServerSenderThread();
    void udpClientReceiverThread();
    
    // Пул потоков для кодирования
    class ThreadPool {
    public:
        ThreadPool(size_t numThreads);
        ~ThreadPool();
        
        template<class F>
        void enqueue(F&& task);
        
        void waitAll();
        size_t getQueueSize() const;
        
    private:
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        mutable std::mutex queueMutex;
        std::condition_variable condition;
        std::atomic<bool> stop;
        std::atomic<size_t> taskCount{0};
    };
    
    // Буферы для кадров
    struct FrameBuffer {
        std::vector<uint8_t> frame;
        uint32_t width;
        uint32_t height;
        uint64_t timestamp;
        uint32_t frameId;
    };
    
    // Методы для многопоточной обработки
    void frameBufferWorker();
    void encodeAndSendFrame(FrameBuffer frameBuffer);
    
    // Серверные переменные (UDP)
    int udpServerSocket;
    struct sockaddr_in udpServerAddr;
    struct sockaddr_in udpClientAddr;
    std::atomic<bool> udpServerRunning;
    std::atomic<bool> udpServerListenerRunning;
    std::atomic<bool> udpServerSenderRunning;
    std::atomic<bool> hasClient;
    std::thread udpServerListenerThreadObj;
    std::thread udpServerSenderThreadObj;
    
    // Очередь для отправки (сервер)
    std::queue<FramePacket> sendQueue;
    std::mutex sendQueueMutex;
    std::condition_variable sendQueueCondVar;
    
    // Клиентские переменные (UDP)
    int udpClientSocket;
    struct sockaddr_in udpTargetAddr;
    std::atomic<bool> udpClientConnected;
    std::thread udpClientReceiverThreadObj;
    
    // Общие
    size_t maxPacketSize;
    
    // Callback для клиента
    std::function<void(const std::vector<uint8_t>&, uint32_t, uint32_t, bool)> frameCallback;
    
    // Для сборки фрагментированных пакетов
    struct FragmentedPacket {
        std::vector<std::vector<uint8_t>> chunks;
        uint32_t totalChunks;
        uint32_t width;
        uint32_t height;
        uint32_t frameId;
        bool isFullFrame;
        std::chrono::steady_clock::time_point lastUpdate;
    };
    
    std::map<uint32_t, FragmentedPacket> fragmentedPackets;
    std::mutex packetMutex;
    std::mutex clientAddrMutex;
    
    // Многопоточные компоненты
    ThreadPool* encoderPool;
    std::atomic<int> activeEncoders;
    
    // Буфер кадров
    std::queue<FrameBuffer> frameBufferQueue;
    std::mutex frameBufferMutex;
    std::condition_variable frameBufferCondVar;
    std::atomic<bool> frameBufferRunning;
    std::thread frameBufferThread;
    
    // Статистика (атомарные переменные)
    std::atomic<uint64_t> statsFramesProcessed{0};
    std::atomic<uint64_t> statsBytesSent{0};
    std::atomic<uint64_t> statsPacketsSent{0};
    std::atomic<uint64_t> statsEncodingTimeMs{0};
    std::atomic<uint64_t> statsNetworkTimeMs{0};
    std::atomic<uint64_t> statsBufferDropped{0};
};

#endif // NETWORK_STREAM_H