#ifndef AVO_CODEC_H
#define AVO_CODEC_H

#include <vector>
#include <string>
#include <cstdint>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
#endif

struct AVOHeader {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t totalFrames;
    uint32_t firstFrameSize;
};

struct PixelChange {
    uint32_t offset;     // Позиция в кадре
    uint8_t r, g, b;     // Новые значения RGB
    uint8_t count;       // Количество повторений (RLE)
};

class AVOCodec {
public:
    // Кодирование/декодирование .avo файлов
    static bool encodeFirstFrame(const std::vector<uint8_t>& frameData, 
                                 uint32_t width, uint32_t height, 
                                 uint32_t fps, const std::string& filename);
    
    static bool decodeFirstFrame(const std::string& filename, 
                                 std::vector<uint8_t>& frameData,
                                 AVOHeader& header);
    
    // Кодирование/декодирование .avop файлов
    static bool encodeFrameDiff(const std::vector<uint8_t>& prevFrame,
                               const std::vector<uint8_t>& currFrame,
                               uint32_t width, uint32_t height,
                               const std::string& filename);
    
    static bool decodeFrameDiff(const std::string& filename,
                               const std::vector<uint8_t>& prevFrame,
                               std::vector<uint8_t>& currFrame,
                               uint32_t width, uint32_t height);
    
    // Вспомогательные функции
    static std::vector<uint8_t> compressRLE(const std::vector<PixelChange>& changes);
    static std::vector<PixelChange> decompressRLE(const std::vector<uint8_t>& data);
    
    static void compareFrames(const std::vector<uint8_t>& frame1,
                             const std::vector<uint8_t>& frame2,
                             uint32_t width, uint32_t height,
                             std::vector<PixelChange>& changes);
    
    static void applyChanges(const std::vector<uint8_t>& baseFrame,
                            const std::vector<PixelChange>& changes,
                            std::vector<uint8_t>& resultFrame,
                            uint32_t width, uint32_t height);
    
    // Создание черного кадра
    static std::vector<uint8_t> createBlackFrame(uint32_t width, uint32_t height);
    
    // Анализ изменений
    static float getDiffPercentage(const std::vector<uint8_t>& prevFrame,
                                 const std::vector<uint8_t>& currFrame,
                                 uint32_t width, uint32_t height);
    
    // Новые функции для сетевой передачи
    static std::vector<uint8_t> createNetworkPacket(const std::vector<uint8_t>& data,
                                                   uint32_t frameId,
                                                   uint32_t packetId,
                                                   uint32_t totalPackets,
                                                   uint32_t width,
                                                   uint32_t height);
    
    static bool parseNetworkPacket(const std::vector<uint8_t>& packet,
                                  std::vector<uint8_t>& data,
                                  uint32_t& frameId,
                                  uint32_t& packetId,
                                  uint32_t& totalPackets,
                                  uint32_t& width,
                                  uint32_t& height);
};

#endif // AVO_CODEC_H