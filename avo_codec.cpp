#include "avo_codec.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

// Вспомогательная функция для получения размера файла
static long long getFileSize(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;
    return static_cast<long long>(file.tellg());
}

bool AVOCodec::encodeFirstFrame(const std::vector<uint8_t>& frameData, 
                               uint32_t width, uint32_t height, 
                               uint32_t fps, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open file for writing: " << filename << std::endl;
        return false;
    }
    
    AVOHeader header;
    header.width = width;
    header.height = height;
    header.fps = fps;
    header.totalFrames = 1;
    header.firstFrameSize = static_cast<uint32_t>(frameData.size());
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
    
    file.close();
    return true;
}

bool AVOCodec::decodeFirstFrame(const std::string& filename, 
                               std::vector<uint8_t>& frameData,
                               AVOHeader& header) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open file for reading: " << filename << std::endl;
        return false;
    }
    
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (header.firstFrameSize == 0 || header.width == 0 || header.height == 0) {
        std::cerr << "Invalid AVO file header" << std::endl;
        return false;
    }
    
    frameData.resize(header.firstFrameSize);
    file.read(reinterpret_cast<char*>(frameData.data()), header.firstFrameSize);
    
    file.close();
    return true;
}

// Старая версия без задержки
bool AVOCodec::encodeFrameDiff(const std::vector<uint8_t>& prevFrame,
                              const std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height,
                              const std::string& filename) {
    // Используем задержку по умолчанию 33ms (30 FPS)
    return encodeFrameDiff(prevFrame, currFrame, width, height, 33, filename);
}

// Старая версия без задержки
bool AVOCodec::decodeFrameDiff(const std::string& filename,
                              const std::vector<uint8_t>& prevFrame,
                              std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height) {
    uint32_t delayMs;
    return decodeFrameDiff(filename, prevFrame, currFrame, width, height, delayMs);
}

// Новая версия с задержкой
bool AVOCodec::encodeFrameDiff(const std::vector<uint8_t>& prevFrame,
                              const std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height,
                              uint32_t delayMs,
                              const std::string& filename) {
    if (prevFrame.size() != currFrame.size()) {
        std::cerr << "Frame sizes don't match!" << std::endl;
        return false;
    }
    
    if (prevFrame.empty() || currFrame.empty()) {
        std::cerr << "Empty frames!" << std::endl;
        return false;
    }
    
    std::vector<PixelChange> changes;
    compareFrames(prevFrame, currFrame, width, height, changes);
    
    std::vector<uint8_t> compressed = compressRLE(changes);
    
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot create file: " << filename << std::endl;
        return false;
    }
    
    // Сохраняем задержку (4 байта)
    uint32_t netDelayMs = htonl(delayMs);
    file.write(reinterpret_cast<const char*>(&netDelayMs), sizeof(netDelayMs));
    
    // Сохраняем размер данных (4 байта)
    uint32_t dataSize = static_cast<uint32_t>(compressed.size());
    uint32_t netDataSize = htonl(dataSize);
    file.write(reinterpret_cast<const char*>(&netDataSize), sizeof(netDataSize));
    
    // Сохраняем данные
    if (!compressed.empty()) {
        file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    }
    
    file.close();
    return true;
}

// Новая версия с задержкой
bool AVOCodec::decodeFrameDiff(const std::string& filename,
                              const std::vector<uint8_t>& prevFrame,
                              std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height,
                              uint32_t& delayMs) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return false;
    }
    
    // Читаем задержку
    uint32_t netDelayMs;
    file.read(reinterpret_cast<char*>(&netDelayMs), sizeof(netDelayMs));
    delayMs = ntohl(netDelayMs);
    
    // Читаем размер данных
    uint32_t netDataSize;
    file.read(reinterpret_cast<char*>(&netDataSize), sizeof(netDataSize));
    uint32_t dataSize = ntohl(netDataSize);
    
    if (dataSize == 0) {
        currFrame = prevFrame;
        file.close();
        return true;
    }
    
    std::vector<uint8_t> compressed(dataSize);
    file.read(reinterpret_cast<char*>(compressed.data()), dataSize);
    
    file.close();
    
    std::vector<PixelChange> changes = decompressRLE(compressed);
    currFrame = prevFrame;
    applyChanges(currFrame, changes, currFrame, width, height);
    
    return true;
}

void AVOCodec::compareFrames(const std::vector<uint8_t>& frame1,
                            const std::vector<uint8_t>& frame2,
                            uint32_t width, uint32_t height,
                            std::vector<PixelChange>& changes) {
    changes.clear();
    
    if (frame1.size() != frame2.size() || frame1.empty()) {
        return;
    }
    
    uint32_t totalPixels = width * height;
    uint32_t pixelIndex = 0;
    
    // Порог изменения (чтобы игнорировать незначительные изменения шума)
    const uint8_t threshold = 10;
    
    while (pixelIndex < totalPixels) {
        size_t idx = pixelIndex * 3;
        
        // Проверяем, изменился ли пиксель с учетом порога
        bool changed = false;
        
        if (abs((int)frame1[idx] - (int)frame2[idx]) > threshold ||
            abs((int)frame1[idx + 1] - (int)frame2[idx + 1]) > threshold ||
            abs((int)frame1[idx + 2] - (int)frame2[idx + 2]) > threshold) {
            changed = true;
        }
        
        if (changed) {
            PixelChange change;
            change.offset = pixelIndex;
            change.r = frame2[idx];
            change.g = frame2[idx + 1];
            change.b = frame2[idx + 2];
            
            // Ищем одинаковые последовательные пиксели
            change.count = 1;
            while (pixelIndex + change.count < totalPixels && 
                   change.count < 255) {
                size_t nextIdx = (pixelIndex + change.count) * 3;
                
                // Проверяем, изменился ли следующий пиксель
                bool nextChanged = false;
                if (abs((int)frame1[nextIdx] - (int)frame2[nextIdx]) > threshold ||
                    abs((int)frame1[nextIdx + 1] - (int)frame2[nextIdx + 1]) > threshold ||
                    abs((int)frame1[nextIdx + 2] - (int)frame2[nextIdx + 2]) > threshold) {
                    nextChanged = true;
                }
                
                if (!nextChanged) {
                    break;
                }
                
                // Проверяем, имеет ли следующий пиксель те же значения
                if (frame2[nextIdx] == change.r &&
                    frame2[nextIdx + 1] == change.g &&
                    frame2[nextIdx + 2] == change.b) {
                    change.count++;
                } else {
                    break;
                }
            }
            
            changes.push_back(change);
            pixelIndex += change.count;
        } else {
            pixelIndex++;
        }
    }
}

std::vector<uint8_t> AVOCodec::compressRLE(const std::vector<PixelChange>& changes) {
    std::vector<uint8_t> result;
    
    for (const auto& change : changes) {
        // Записываем offset (4 байта) в сетевом порядке байт
        uint32_t netOffset = htonl(change.offset);
        result.insert(result.end(), 
                     reinterpret_cast<const uint8_t*>(&netOffset), 
                     reinterpret_cast<const uint8_t*>(&netOffset) + 4);
        
        // Записываем count (1 байт)
        result.push_back(change.count);
        
        // Записываем RGB (3 байта)
        result.push_back(change.r);
        result.push_back(change.g);
        result.push_back(change.b);
    }
    
    return result;
}

std::vector<PixelChange> AVOCodec::decompressRLE(const std::vector<uint8_t>& data) {
    std::vector<PixelChange> changes;
    
    if (data.empty()) {
        return changes;
    }
    
    size_t i = 0;
    
    while (i + 8 <= data.size()) { // 4 байта offset + 1 байт count + 3 байта RGB = 8 байт
        PixelChange change;
        
        // Читаем offset
        uint32_t netOffset;
        memcpy(&netOffset, &data[i], 4);
        i += 4;
        change.offset = ntohl(netOffset);
        
        // Читаем count
        change.count = data[i++];
        
        // Читаем RGB
        change.r = data[i++];
        change.g = data[i++];
        change.b = data[i++];
        
        changes.push_back(change);
    }
    
    return changes;
}

void AVOCodec::applyChanges(const std::vector<uint8_t>& baseFrame,
                           const std::vector<PixelChange>& changes,
                           std::vector<uint8_t>& resultFrame,
                           uint32_t width, uint32_t height) {
    resultFrame = baseFrame;
    
    if (resultFrame.empty() || width == 0 || height == 0) {
        return;
    }
    
    uint32_t totalPixels = width * height;
    
    for (const auto& change : changes) {
        // Проверяем, не выходит ли offset за границы
        if (change.offset >= totalPixels) {
            continue;
        }
        
        // Применяем изменение для каждого пикселя в count
        for (uint8_t i = 0; i < change.count; i++) {
            uint32_t pixelPos = change.offset + i;
            
            // Проверяем, не выходит ли текущий пиксель за границы
            if (pixelPos >= totalPixels) {
                break;
            }
            
            // Вычисляем индекс в массиве RGB
            size_t idx = pixelPos * 3;
            
            // Проверяем, не выходит ли индекс за границы массива
            if (idx + 2 < resultFrame.size()) {
                resultFrame[idx] = change.r;
                resultFrame[idx + 1] = change.g;
                resultFrame[idx + 2] = change.b;
            }
        }
    }
}

std::vector<uint8_t> AVOCodec::createBlackFrame(uint32_t width, uint32_t height) {
    return std::vector<uint8_t>(width * height * 3, 0);
}

float AVOCodec::getDiffPercentage(const std::vector<uint8_t>& prevFrame,
                                 const std::vector<uint8_t>& currFrame,
                                 uint32_t width, uint32_t height) {
    if (prevFrame.size() != currFrame.size() || prevFrame.empty()) {
        return 100.0f;
    }
    
    int changedPixels = 0;
    uint32_t totalPixels = width * height;
    
    for (uint32_t i = 0; i < totalPixels; i++) {
        size_t idx = i * 3;
        if (prevFrame[idx] != currFrame[idx] || 
            prevFrame[idx + 1] != currFrame[idx + 1] || 
            prevFrame[idx + 2] != currFrame[idx + 2]) {
            changedPixels++;
        }
    }
    
    return (changedPixels * 100.0f) / totalPixels;
}

std::vector<uint8_t> AVOCodec::createNetworkPacket(const std::vector<uint8_t>& data,
                                                  uint32_t frameId,
                                                  uint32_t packetId,
                                                  uint32_t totalPackets,
                                                  uint32_t width,
                                                  uint32_t height) {
    std::vector<uint8_t> packet;
    packet.resize(24 + data.size()); // 24 байта заголовка
    
    // Заголовок (все в сетевом порядке байт)
    uint32_t netFrameId = htonl(frameId);
    uint32_t netPacketId = htonl(packetId);
    uint32_t netTotalPackets = htonl(totalPackets);
    uint32_t netWidth = htonl(width);
    uint32_t netHeight = htonl(height);
    uint32_t netDataSize = htonl(static_cast<uint32_t>(data.size()));
    
    memcpy(packet.data(), &netFrameId, 4);
    memcpy(packet.data() + 4, &netPacketId, 4);
    memcpy(packet.data() + 8, &netTotalPackets, 4);
    memcpy(packet.data() + 12, &netWidth, 4);
    memcpy(packet.data() + 16, &netHeight, 4);
    memcpy(packet.data() + 20, &netDataSize, 4);
    
    // Данные
    if (!data.empty()) {
        memcpy(packet.data() + 24, data.data(), data.size());
    }
    
    return packet;
}

bool AVOCodec::parseNetworkPacket(const std::vector<uint8_t>& packet,
                                 std::vector<uint8_t>& data,
                                 uint32_t& frameId,
                                 uint32_t& packetId,
                                 uint32_t& totalPackets,
                                 uint32_t& width,
                                 uint32_t& height) {
    if (packet.size() < 24) {
        return false;
    }
    
    // Читаем заголовок
    memcpy(&frameId, packet.data(), 4);
    memcpy(&packetId, packet.data() + 4, 4);
    memcpy(&totalPackets, packet.data() + 8, 4);
    memcpy(&width, packet.data() + 12, 4);
    memcpy(&height, packet.data() + 16, 4);
    
    uint32_t dataSize;
    memcpy(&dataSize, packet.data() + 20, 4);
    
    // Конвертируем из сетевого порядка байт
    frameId = ntohl(frameId);
    packetId = ntohl(packetId);
    totalPackets = ntohl(totalPackets);
    width = ntohl(width);
    height = ntohl(height);
    dataSize = ntohl(dataSize);
    
    // Проверяем размер данных
    if (packet.size() < 24 + dataSize) {
        return false;
    }
    
    // Копируем данные
    data.resize(dataSize);
    memcpy(data.data(), packet.data() + 24, dataSize);
    
    return true;
}

// Исправленная функция для создания архива с реальными задержками
bool AVOCodec::encodeVideoArchive(const std::vector<AVOFrame>& frames,
                                 uint32_t width, uint32_t height, 
                                 uint32_t fps, const std::string& filename) {
    if (frames.empty()) {
        std::cerr << "No frames to encode!" << std::endl;
        return false;
    }
    
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot create archive: " << filename << std::endl;
        return false;
    }
    
    // Заголовок архива
    AVOHeader header;
    header.width = width;
    header.height = height;
    header.fps = 0; // НЕ используем FPS, так как у нас реальные задержки
    header.totalFrames = static_cast<uint32_t>(frames.size());
    header.firstFrameSize = static_cast<uint32_t>(frames[0].data.size());
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Сохраняем первый кадр (полный)
    AVOFrame firstFrame = frames[0];
    if (!firstFrame.isFullFrame) {
        std::cerr << "First frame must be full frame!" << std::endl;
        file.close();
        return false;
    }
    
    // Сохраняем задержку первого кадра
    uint32_t netFirstDelay = htonl(firstFrame.delayMs);
    file.write(reinterpret_cast<const char*>(&netFirstDelay), sizeof(netFirstDelay));
    
    // Сохраняем первый кадр
    file.write(reinterpret_cast<const char*>(firstFrame.data.data()), firstFrame.data.size());
    
    // Сохраняем остальные кадры как изменения
    std::vector<uint8_t> prevFrame = firstFrame.data;
    
    for (size_t i = 1; i < frames.size(); i++) {
        const AVOFrame& frame = frames[i];
        
        // Получаем текущий кадр
        std::vector<uint8_t> currFrame;
        if (frame.isFullFrame) {
            currFrame = frame.data;
        } else {
            std::vector<PixelChange> changes = decompressRLE(frame.data);
            applyChanges(prevFrame, changes, currFrame, width, height);
        }
        
        // Находим разницу между кадрами
        std::vector<PixelChange> changes;
        compareFrames(prevFrame, currFrame, width, height, changes);
        
        // Сжимаем изменения
        std::vector<uint8_t> compressed = compressRLE(changes);
        
        // Сохраняем тип кадра (1 байт): 0 = изменения, 1 = полный кадр
        uint8_t frameType = frame.isFullFrame ? 1 : 0;
        file.write(reinterpret_cast<const char*>(&frameType), sizeof(frameType));
        
        // Сохраняем реальную задержку (4 байта)
        uint32_t netDelay = htonl(frame.delayMs);
        file.write(reinterpret_cast<const char*>(&netDelay), sizeof(netDelay));
        
        // Сохраняем размер сжатых данных (4 байта)
        uint32_t dataSize = static_cast<uint32_t>(compressed.size());
        uint32_t netDataSize = htonl(dataSize);
        file.write(reinterpret_cast<const char*>(&netDataSize), sizeof(netDataSize));
        
        // Сохраняем сжатые данные
        if (!compressed.empty()) {
            file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        }
        
        // Обновляем предыдущий кадр для следующей итерации
        prevFrame = currFrame;
    }
    
    file.close();
    
    // Статистика
    long long totalRawSize = width * height * 3 * frames.size();
    long long archiveSize = getFileSize(filename);
    float compressionRatio = (archiveSize * 100.0f) / totalRawSize;
    
    std::cout << "Archive created: " << filename << std::endl;
    std::cout << "  Frames: " << frames.size() << std::endl;
    std::cout << "  Raw size: " << totalRawSize << " bytes" << std::endl;
    std::cout << "  Archive size: " << archiveSize << " bytes" << std::endl;
    std::cout << "  Compression: " << std::fixed << std::setprecision(1) 
              << compressionRatio << "%" << std::endl;
    
    return true;
}

// Исправленная функция для чтения архива с реальными задержками
bool AVOCodec::decodeVideoArchive(const std::string& filename,
                                 std::vector<AVOFrame>& frames,
                                 AVOHeader& header) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open archive: " << filename << std::endl;
        return false;
    }
    
    // Читаем заголовок
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    frames.clear();
    frames.reserve(header.totalFrames);
    
    // Читаем первый кадр (полный)
    AVOFrame firstFrame;
    
    // Читаем задержку первого кадра
    uint32_t netFirstDelay;
    file.read(reinterpret_cast<char*>(&netFirstDelay), sizeof(netFirstDelay));
    firstFrame.delayMs = ntohl(netFirstDelay);
    firstFrame.isFullFrame = true;
    
    // Читаем первый кадр
    firstFrame.data.resize(header.firstFrameSize);
    file.read(reinterpret_cast<char*>(firstFrame.data.data()), header.firstFrameSize);
    
    frames.push_back(firstFrame);
    
    // Читаем остальные кадры
    std::vector<uint8_t> prevFrame = firstFrame.data;
    
    for (uint32_t i = 1; i < header.totalFrames; i++) {
        AVOFrame frame;
        frame.isFullFrame = true; // После декодирования это будет полный кадр
        
        // Читаем тип кадра
        uint8_t frameType;
        file.read(reinterpret_cast<char*>(&frameType), sizeof(frameType));
        bool wasDiffFrame = (frameType == 0);
        
        // Читаем реальную задержку
        uint32_t netDelay;
        file.read(reinterpret_cast<char*>(&netDelay), sizeof(netDelay));
        frame.delayMs = ntohl(netDelay);
        
        // Читаем размер данных
        uint32_t netDataSize;
        file.read(reinterpret_cast<char*>(&netDataSize), sizeof(netDataSize));
        uint32_t dataSize = ntohl(netDataSize);
        
        if (dataSize > 0) {
            // Читаем сжатые данные
            std::vector<uint8_t> compressedData(dataSize);
            file.read(reinterpret_cast<char*>(compressedData.data()), dataSize);
            
            if (wasDiffFrame) {
                // Декомпрессируем изменения
                std::vector<PixelChange> changes = decompressRLE(compressedData);
                
                // Применяем изменения к предыдущему кадру
                std::vector<uint8_t> currFrame;
                applyChanges(prevFrame, changes, currFrame, header.width, header.height);
                
                // Обновляем данные кадра
                frame.data = currFrame;
                
                // Обновляем предыдущий кадр для следующей итерации
                prevFrame = currFrame;
            } else {
                // Это был полный кадр
                frame.data = compressedData;
                prevFrame = compressedData;
            }
        } else {
            // Нет изменений - используем предыдущий кадр
            frame.data = prevFrame;
        }
        
        frames.push_back(frame);
    }
    
    file.close();
    return true;
}