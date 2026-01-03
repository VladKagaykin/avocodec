#include "avo_codec.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

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

bool AVOCodec::encodeFrameDiff(const std::vector<uint8_t>& prevFrame,
                              const std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height,
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
    
    uint32_t numChanges = static_cast<uint32_t>(compressed.size());
    file.write(reinterpret_cast<const char*>(&numChanges), sizeof(numChanges));
    
    if (!compressed.empty()) {
        file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    }
    
    file.close();
    return true;
}

bool AVOCodec::decodeFrameDiff(const std::string& filename,
                              const std::vector<uint8_t>& prevFrame,
                              std::vector<uint8_t>& currFrame,
                              uint32_t width, uint32_t height) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return false;
    }
    
    uint32_t dataSize;
    file.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
    
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
        if (change.count == 1) {
            // Для одиночных пикселей: 3 байта RGB
            result.push_back(change.r);
            result.push_back(change.g);
            result.push_back(change.b);
        } else {
            // Для RLE: 1 байт count + 3 байта RGB
            result.push_back(change.count);
            result.push_back(change.r);
            result.push_back(change.g);
            result.push_back(change.b);
        }
    }
    
    return result;
}

std::vector<PixelChange> AVOCodec::decompressRLE(const std::vector<uint8_t>& data) {
    std::vector<PixelChange> changes;
    
    if (data.empty()) {
        return changes;
    }
    
    uint32_t currentOffset = 0;
    size_t i = 0;
    
    while (i < data.size()) {
        PixelChange change;
        change.offset = currentOffset;
        
        if (i + 3 <= data.size()) {
            // Проверяем, является ли первый байт счетчиком RLE (>1)
            if (data[i] > 1 && data[i] <= 255) {
                // RLE запись: count + RGB
                change.count = data[i++];
                change.r = data[i++];
                change.g = data[i++];
                change.b = data[i++];
                currentOffset += change.count;
            } else {
                // Одиночный пиксель: RGB
                change.count = 1;
                change.r = data[i++];
                change.g = data[i++];
                change.b = data[i++];
                currentOffset += 1;
            }
            
            changes.push_back(change);
        } else {
            break;
        }
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
        if (change.offset >= totalPixels) {
            continue;
        }
        
        for (uint8_t i = 0; i < change.count; i++) {
            uint32_t pixelPos = change.offset + i;
            
            if (pixelPos >= totalPixels) {
                break;
            }
            
            size_t idx = pixelPos * 3;
            
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