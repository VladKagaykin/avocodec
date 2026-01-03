#include "avo_codec.h"
#include "network_stream.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <arpa/inet.h>

long long getFileSize(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;
    return static_cast<long long>(file.tellg());
}

void disableAllLogs() {
    setenv("QT_LOGGING_RULES", "*.debug=false;qt.*.debug=false", 1);
    setenv("QT_ASSUME_STDERR_HAS_CONSOLE", "0", 1);
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
}

bool parseAddress(const std::string& address, std::string& ip, int& port) {
    size_t colon_pos = address.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    ip = address.substr(0, colon_pos);
    std::string port_str = address.substr(colon_pos + 1);
    
    struct sockaddr_in sa;
    if (inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) != 1) {
        if (ip != "0.0.0.0" && ip != "any" && ip != "ANY" && ip != "*") {
            return false;
        }
    }
    
    try {
        port = std::stoi(port_str);
        if (port < 1 || port > 65535) {
            return false;
        }
    } catch (...) {
        return false;
    }
    
    return true;
}

void showNetworkInterfaces() {
    std::cout << "\n=== Available Network Interfaces ===" << std::endl;
    std::cout << "Running: ip addr show" << std::endl;
    system("ip addr show | grep 'inet ' | grep -v '127.0.0.1' | awk '{print $2}' | cut -d/ -f1");
    
    std::cout << "\nCommon addresses:" << std::endl;
    std::cout << "  127.0.0.1:7777     - localhost (this computer)" << std::endl;
    std::cout << "  0.0.0.0:7777       - all interfaces (default)" << std::endl;
    std::cout << "  192.168.x.x:7777   - local network" << std::endl;
    std::cout << std::endl;
}

int selectCamera() {
    std::cout << "\n=== Select USB Camera ===\n" << std::endl;
    
    for (int i = 0; i < 4; i++) {
        cv::VideoCapture testCap(i);
        if (testCap.isOpened()) {
            std::cout << "Camera " << i << ": Available";
            double width = testCap.get(cv::CAP_PROP_FRAME_WIDTH);
            double height = testCap.get(cv::CAP_PROP_FRAME_HEIGHT);
            double fps = testCap.get(cv::CAP_PROP_FPS);
            if (width > 0 && height > 0) {
                std::cout << " (" << width << "x" << height;
                if (fps > 0) std::cout << " @" << fps << "fps";
                std::cout << ")";
            }
            std::cout << std::endl;
            testCap.release();
        } else {
            std::cout << "Camera " << i << ": Not available" << std::endl;
        }
    }
    
    int selectedCamera = 0;
    std::cout << "\nEnter camera number (0-3): ";
    std::cin >> selectedCamera;
    
    return selectedCamera;
}

int selectMode() {
    std::cout << "\n=== Mode Selection ===\n" << std::endl;
    std::cout << "1. Start Server (stream video to clients)" << std::endl;
    std::cout << "2. Connect to Server (receive video)" << std::endl;
    std::cout << "3. Codec test (save/load files)" << std::endl;
    std::cout << "4. Camera test (extended diagnostics)" << std::endl;
    
    int mode = 0;
    std::cout << "\nSelect mode (1-4): ";
    std::cin >> mode;
    
    return mode;
}

std::vector<uint8_t> matToRGBVector(const cv::Mat& frame) {
    if (frame.empty()) return {};
    
    cv::Mat rgbFrame;
    
    if (frame.channels() == 1) {
        cv::cvtColor(frame, rgbFrame, cv::COLOR_GRAY2RGB);
    } else if (frame.channels() == 3) {
        cv::cvtColor(frame, rgbFrame, cv::COLOR_BGR2RGB);
    } else if (frame.channels() == 4) {
        cv::cvtColor(frame, rgbFrame, cv::COLOR_BGRA2RGB);
    } else {
        return {};
    }
    
    std::vector<uint8_t> result;
    result.reserve(rgbFrame.rows * rgbFrame.cols * 3);
    
    for (int y = 0; y < rgbFrame.rows; y++) {
        for (int x = 0; x < rgbFrame.cols; x++) {
            cv::Vec3b pixel = rgbFrame.at<cv::Vec3b>(y, x);
            result.push_back(pixel[0]); // R
            result.push_back(pixel[1]); // G
            result.push_back(pixel[2]); // B
        }
    }
    
    return result;
}

cv::Mat rgbVectorToMat(const std::vector<uint8_t>& rgbData, int width, int height) {
    if (rgbData.empty() || width <= 0 || height <= 0) {
        return cv::Mat::zeros(height, width, CV_8UC3);
    }
    
    cv::Mat result(height, width, CV_8UC3);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            if (idx + 2 < rgbData.size()) {
                result.at<cv::Vec3b>(y, x) = cv::Vec3b(
                    rgbData[idx + 2], // B
                    rgbData[idx + 1], // G
                    rgbData[idx]      // R
                );
            }
        }
    }
    
    return result;
}

std::vector<uint8_t> createBlackFrame(uint32_t width, uint32_t height) {
    return std::vector<uint8_t>(width * height * 3, 0);
}

std::vector<std::string> getLocalIPs() {
    std::vector<std::string> ips;
    
    FILE* fp = popen("hostname -I 2>/dev/null || ip addr show 2>/dev/null | grep 'inet ' | grep -v '127.0.0.1' | awk '{print $2}' | cut -d/ -f1", "r");
    if (fp) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp)) {
            std::string ip = buffer;
            ip.erase(std::remove(ip.begin(), ip.end(), '\n'), ip.end());
            ip.erase(std::remove(ip.begin(), ip.end(), '\r'), ip.end());
            if (!ip.empty()) {
                ips.push_back(ip);
            }
        }
        pclose(fp);
    }
    
    ips.push_back("0.0.0.0");
    ips.push_back("127.0.0.1");
    
    return ips;
}

// ================= UDP СЕРВЕР =================
void serverMode() {
    std::cout << "\n=== Server Mode (Streaming) ===\n" << std::endl;
    
    int cameraIndex = selectCamera();
    
    int width, height, requestedFps;
    std::string address;
    std::string serverIP;
    int port;
    
    std::cout << "\n=== Server Settings ===\n" << std::endl;
    std::cout << "Enter frame width (recommended 640): ";
    std::cin >> width;
    std::cout << "Enter frame height (recommended 480): ";
    std::cin >> height;
    std::cout << "Enter FPS (15-30 recommended): ";
    std::cin >> requestedFps;
    
    std::cin.ignore();
    
    std::cout << "\nEnter server address (format: IP:PORT)" << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  0.0.0.0:7777       - listen on all interfaces (recommended)" << std::endl;
    std::cout << "  127.0.0.1:7777     - listen only on localhost" << std::endl;
    std::cout << "  192.168.1.158:7777 - listen on specific IP" << std::endl;
    std::cout << "\nEnter address: ";
    
    std::getline(std::cin, address);
    
    if (address.empty()) {
        address = "0.0.0.0:7777";
        std::cout << "Using default: " << address << std::endl;
    }
    
    if (!parseAddress(address, serverIP, port)) {
        std::cerr << "Invalid address format! Use IP:PORT (e.g., 0.0.0.0:7777)" << std::endl;
        return;
    }
    
    std::cout << "Parsed: IP=" << serverIP << ", PORT=" << port << std::endl;
    
    if (!NetworkStream::initializeNetwork()) {
        std::cerr << "Network initialization error!" << std::endl;
        return;
    }
    
    cv::VideoCapture cap(cameraIndex);
    
    if (!cap.isOpened()) {
        std::cerr << "Error: Cannot open camera " << cameraIndex << std::endl;
        NetworkStream::cleanupNetwork();
        return;
    }
    
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    
    double actualWidth = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double actualHeight = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double actualFps = cap.get(cv::CAP_PROP_FPS);
    
    width = static_cast<int>(actualWidth);
    height = static_cast<int>(actualHeight);
    
    if (actualFps <= 0 || actualFps > 120) {
        actualFps = requestedFps > 0 ? requestedFps : 15;
    }
    
    std::cout << "\nCamera parameters:" << std::endl;
    std::cout << "  Resolution: " << width << "x" << height << std::endl;
    std::cout << "  FPS: " << actualFps << std::endl;
    
    NetworkStream server;
    
    if (!server.startUDPServer(serverIP, port)) {
        std::cerr << "Failed to start UDP server on " << serverIP << ":" << port << std::endl;
        cap.release();
        NetworkStream::cleanupNetwork();
        return;
    }
    
    std::cout << "\nUDP Server started! Waiting for client connection..." << std::endl;
    std::cout << "Clients should connect to:" << std::endl;
    std::vector<std::string> localIPs = getLocalIPs();
    for (const auto& ip : localIPs) {
        if (ip != "0.0.0.0" && ip != "127.0.0.1") {
            std::cout << "  " << ip << ":" << port << std::endl;
        }
    }
    std::cout << "\nWaiting for client connection... Press ESC to stop server\n" << std::endl;
    
    // Запоминаем предыдущий кадр для сравнения
    std::vector<uint8_t> prevFrame = AVOCodec::createBlackFrame(width, height);
    bool firstFrame = true;
    bool clientConnected = false;
    int clientConnectionCheckCounter = 0;
    
    int frameCount = 0;
    int fpsCounter = 0;
    int framesSinceLastLog = 0;
    auto lastStatsTime = std::chrono::steady_clock::now();
    auto startTime = lastStatsTime;
    
    int delayMs = std::max(1, 1000 / static_cast<int>(actualFps));
    
    cv::namedWindow("UDP Server .AVO Stream", cv::WINDOW_NORMAL);
    cv::resizeWindow("UDP Server .AVO Stream", 640, 480);
    
    while (true) {
        // Проверяем подключение клиента каждые 10 кадров
        clientConnectionCheckCounter++;
        if (clientConnectionCheckCounter >= 10) {
            bool hasClient = server.hasUDPClient();
            if (!clientConnected && hasClient) {
                clientConnected = true;
                std::cout << "\n✓ Client connected! Starting video stream...\n" << std::endl;
            } else if (clientConnected && !hasClient) {
                clientConnected = false;
                std::cout << "\n⚠ Client disconnected. Waiting for new connection...\n" << std::endl;
            }
            clientConnectionCheckCounter = 0;
        }
        
        cv::Mat frame;
        cap >> frame;
        
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        cv::Mat resizedFrame;
        cv::resize(frame, resizedFrame, cv::Size(width, height));
        
        std::vector<uint8_t> currentFrame = matToRGBVector(resizedFrame);
        
        std::vector<uint8_t> dataToSend;
        bool sendFullFrame = false;
        
        if (firstFrame || frameCount % 30 == 0) {
            // Отправляем полный кадр (первый и каждый 30-й кадр)
            dataToSend = currentFrame;
            sendFullFrame = true;
            if (clientConnected) {
                std::cout << "[SERVER] Sending full frame " << frameCount << std::endl;
            }
            firstFrame = false;
        } else {
            // Отправляем только изменения
            std::vector<PixelChange> changes;
            AVOCodec::compareFrames(prevFrame, currentFrame, width, height, changes);
            
            if (!changes.empty()) {
                std::vector<uint8_t> compressed = AVOCodec::compressRLE(changes);
                
                // Проверяем эффективность сжатия
                float compressionRatio = (compressed.size() * 100.0f) / (width * height * 3);
                
                if (compressionRatio < 50.0f && compressed.size() < currentFrame.size()) {
                    // RLE эффективно - отправляем сжатые данные
                    dataToSend = compressed;
                    
                    // Логируем каждые 15 кадров, чтобы не засорять консоль
                    framesSinceLastLog++;
                    if (clientConnected && framesSinceLastLog >= 15) {
                        std::cout << "[SERVER] Frame " << frameCount 
                                  << ": " << changes.size() << " changes, "
                                  << "compressed to " << compressed.size() << " bytes ("
                                  << std::fixed << std::setprecision(1) << compressionRatio 
                                  << "%)" << std::endl;
                        framesSinceLastLog = 0;
                    }
                } else {
                    // RLE неэффективно - отправляем полный кадр
                    dataToSend = currentFrame;
                    sendFullFrame = true;
                    if (clientConnected && framesSinceLastLog >= 15) {
                        std::cout << "[SERVER] Frame " << frameCount 
                                  << ": RLE inefficient (" << compressionRatio 
                                  << "%), sending full frame" << std::endl;
                        framesSinceLastLog = 0;
                    }
                }
            } else {
                // Нет изменений - отправляем пустой пакет (1 байт)
                dataToSend = std::vector<uint8_t>(1, 0);
                if (clientConnected && frameCount % 60 == 0) {
                    std::cout << "[SERVER] Frame " << frameCount << ": No changes (static frame)" << std::endl;
                }
            }
        }
        
        // Отправка через UDP (только если есть клиент)
        if (clientConnected) {
            bool sendSuccess = server.sendUDPFrame(dataToSend, width, height);
            
            if (!sendSuccess) {
                // Ошибка отправки - возможно, клиент отключился
                std::cerr << "[SERVER] Failed to send frame " << frameCount << std::endl;
            }
        }
        
        // Обновляем предыдущий кадр
        prevFrame = currentFrame;
        frameCount++;
        fpsCounter++;
        
        // Отображаем локальное окно
        cv::Mat displayFrame;
        cv::resize(resizedFrame, displayFrame, cv::Size(640, 480));
        
        // Добавляем информацию о сервере
        std::string statusText = clientConnected ? "CLIENT CONNECTED" : "WAITING FOR CLIENT...";
        cv::Scalar statusColor = clientConnected ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 165, 255);
        
        // Фон для текста
        cv::rectangle(displayFrame, cv::Point(5, 5), cv::Point(635, 150), cv::Scalar(0, 0, 0), -1);
        cv::rectangle(displayFrame, cv::Point(5, 5), cv::Point(635, 150), statusColor, 2);
        
        // Текст информации
        cv::putText(displayFrame, "UDP SERVER: " + address, 
                   cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                   cv::Scalar(255, 255, 255), 2);
        
        cv::putText(displayFrame, "STATUS: " + statusText, 
                   cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                   statusColor, 2);
        
        cv::putText(displayFrame, "FRAMES: " + std::to_string(frameCount), 
                   cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.6, 
                   cv::Scalar(255, 255, 255), 1);
        
        cv::putText(displayFrame, "RESOLUTION: " + std::to_string(width) + "x" + std::to_string(height), 
                   cv::Point(10, 115), cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                   cv::Scalar(200, 200, 200), 1);
        
        if (clientConnected) {
            std::string frameType = sendFullFrame ? "FULL FRAME" : "DIFF FRAME";
            cv::Scalar frameTypeColor = sendFullFrame ? cv::Scalar(0, 200, 255) : cv::Scalar(255, 200, 0);
            
            cv::putText(displayFrame, "TYPE: " + frameType, 
                       cv::Point(10, 140), cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                       frameTypeColor, 1);
        }
        
        cv::imshow("UDP Server .AVO Stream", displayFrame);
        
        // Статистика каждые 2 секунды
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastStatsTime).count();
        
        if (elapsedSec >= 2) {
            double fps = fpsCounter / elapsedSec;
            std::cout << "STATS: " << frameCount << " frames, " 
                      << std::fixed << std::setprecision(1) << fps << " FPS, "
                      << (clientConnected ? "Client: CONNECTED" : "Client: WAITING") << std::endl;
            lastStatsTime = currentTime;
            fpsCounter = 0;
        }
        
        // Обработка клавиш
        int key = cv::waitKey(delayMs);
        if (key == 27) { // ESC
            std::cout << "\nStopping server..." << std::endl;
            break;
        } else if (key == 'r' || key == 'R') {
            // Сброс соединения с клиентом
            std::cout << "\nResetting client connection..." << std::endl;
            clientConnected = false;
        } else if (key == 'i' || key == 'I') {
            // Информация о текущем кадре
            std::cout << "\nFrame #" << frameCount << " info:" << std::endl;
            std::cout << "  Size: " << width << "x" << height << std::endl;
            std::cout << "  Data size: " << dataToSend.size() << " bytes" << std::endl;
            std::cout << "  Client connected: " << (clientConnected ? "Yes" : "No") << std::endl;
        }
    }
    
    // Очистка
    cap.release();
    cv::destroyAllWindows();
    server.stopUDPServer();
    NetworkStream::cleanupNetwork();
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    std::cout << "\n=== Server Summary ===" << std::endl;
    std::cout << "Address: " << address << std::endl;
    std::cout << "Total frames: " << frameCount << std::endl;
    std::cout << "Total time: " << totalElapsed << " sec" << std::endl;
    if (totalElapsed > 0) {
        std::cout << "Average FPS: " << std::fixed << std::setprecision(1) 
                  << (frameCount / (double)totalElapsed) << std::endl;
    }
    std::cout << "Streaming finished." << std::endl;
}

// ================= UDP КЛИЕНТ =================
void clientMode() {
    std::cout << "\n=== Client Mode (Receive) ===\n" << std::endl;
    
    std::string address;
    std::string serverIP;
    int port;
    
    std::cout << "Enter server address to connect to (format: IP:PORT)" << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  127.0.0.1:7777     - localhost server" << std::endl;
    std::cout << "  192.168.1.158:7777 - server on another computer" << std::endl;
    std::cout << "\nEnter address: ";
    
    std::cin.ignore();
    std::getline(std::cin, address);
    
    if (address.empty()) {
        address = "127.0.0.1:7777";
        std::cout << "Using default: " << address << std::endl;
    }
    
    if (!parseAddress(address, serverIP, port)) {
        std::cerr << "Invalid address format! Use IP:PORT (e.g., 192.168.1.158:7777)" << std::endl;
        return;
    }
    
    std::cout << "Parsed: IP=" << serverIP << ", PORT=" << port << std::endl;
    
    if (!NetworkStream::initializeNetwork()) {
        std::cerr << "Network initialization error!" << std::endl;
        return;
    }
    
    NetworkStream client;
    
    if (!client.connectToUDPServer(serverIP, port)) {
        std::cerr << "Failed to connect to UDP server " << serverIP << ":" << port << std::endl;
        NetworkStream::cleanupNetwork();
        return;
    }
    
    std::atomic<int> receivedFrames(0);
    std::atomic<bool> dataReceived(false);
    std::vector<uint8_t> currentFrame;
    std::atomic<uint32_t> frameWidth(320);
    std::atomic<uint32_t> frameHeight(240);
    std::atomic<bool> expectingFullFrame(true); // Первый кадр должен быть полным
    std::mutex frameMutex;
    
    cv::namedWindow("UDP Client .AVO Stream", cv::WINDOW_NORMAL);
    cv::resizeWindow("UDP Client .AVO Stream", 640, 480);
    
    auto frameCallback = [&](const std::vector<uint8_t>& packetData, 
                            uint32_t width, uint32_t height) {
        receivedFrames++;
        
        // Обновляем размеры если они изменились
        if (width > 0 && height > 0) {
            frameWidth = width;
            frameHeight = height;
        }
        
        if (packetData.empty() || (packetData.size() == 1 && packetData[0] == 0)) {
            // Пустой пакет - нет изменений
            // Для пустого пакета ничего не меняем, оставляем предыдущий кадр
            return;
        }
        
        try {
            if (expectingFullFrame || packetData.size() == width * height * 3) {
                // Полный кадр
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    currentFrame = packetData;
                    dataReceived = true;
                }
                expectingFullFrame = false;
                std::cout << "[CLIENT] Received full frame " << receivedFrames 
                         << " (" << packetData.size() << " bytes)" << std::endl;
            } else {
                // Разностный кадр - декомпрессируем RLE и применяем изменения
                std::vector<PixelChange> changes = AVOCodec::decompressRLE(packetData);
                
                if (!changes.empty()) {
                    // Создаем базовый кадр из предыдущего состояния
                    std::vector<uint8_t> baseFrame;
                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        baseFrame = currentFrame;
                    }
                    
                    // Если baseFrame пустой, создаем черный кадр
                    if (baseFrame.empty()) {
                        baseFrame = AVOCodec::createBlackFrame(frameWidth, frameHeight);
                    }
                    
                    // Применяем изменения
                    std::vector<uint8_t> newFrame;
                    AVOCodec::applyChanges(baseFrame, changes, newFrame, 
                                          frameWidth, frameHeight);
                    
                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        currentFrame = newFrame;
                        dataReceived = true;
                    }
                    
                    // Периодически выводим статистику
                    if (receivedFrames % 30 == 0) {
                        float compressionRatio = (packetData.size() * 100.0f) / 
                                               (frameWidth * frameHeight * 3);
                        std::cout << "[CLIENT] Frame " << receivedFrames << ": " 
                                 << packetData.size() << " bytes (" 
                                 << std::fixed << std::setprecision(1) << compressionRatio 
                                 << "%), changes: " << changes.size() << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[CLIENT] Error processing frame: " << e.what() << std::endl;
        }
    };
    
    if (!client.startUDPReceiver(frameCallback)) {
        std::cerr << "Failed to start UDP receiver" << std::endl;
        client.disconnectUDP();
        NetworkStream::cleanupNetwork();
        cv::destroyAllWindows();
        return;
    }
    
    std::cout << "\nConnected to UDP server! Receiving stream..." << std::endl;
    std::cout << "Press ESC to exit\n" << std::endl;
    
    // Основной цикл отображения
    while (true) {
        cv::Mat displayFrame;
        
        bool localDataReceived;
        std::vector<uint8_t> localFrame;
        uint32_t localWidth, localHeight;
        
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            localDataReceived = dataReceived;
            localFrame = currentFrame;
            localWidth = frameWidth;
            localHeight = frameHeight;
        }
        
        if (localDataReceived && !localFrame.empty()) {
            // Есть данные - показываем видео
            cv::Mat frame = rgbVectorToMat(localFrame, localWidth, localHeight);
            
            // Добавляем информацию о потоке
            cv::putText(frame, "UDP Client: " + address, 
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, "Frames: " + std::to_string(receivedFrames), 
                       cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, "Resolution: " + std::to_string(localWidth) + "x" + 
                       std::to_string(localHeight), 
                       cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                       cv::Scalar(0, 255, 255), 1);
            
            displayFrame = frame;
        } else {
            // Нет данных - показываем ожидание
            displayFrame = cv::Mat::zeros(480, 640, CV_8UC3);
            cv::putText(displayFrame, "Connecting to server...", 
                       cv::Point(640/2 - 120, 480/2), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 255, 255), 2);
            cv::putText(displayFrame, "Server: " + address, 
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                       cv::Scalar(0, 255, 255), 1);
        }
        
        cv::imshow("UDP Client .AVO Stream", displayFrame);
        
        if (cv::waitKey(30) == 27) {
            break;
        }
    }
    
    client.disconnectUDP();
    cv::destroyAllWindows();
    NetworkStream::cleanupNetwork();
    std::cout << "\nClient stopped. Total frames: " << receivedFrames << std::endl;
}

// ... (остальные функции testCodecMode и cameraTestMode без изменений)
void testCodecMode() {
    std::cout << "\n=== Codec Test ===\n" << std::endl;
    
    const int width = 320;
    const int height = 240;
    const int fps = 30;
    
    std::vector<uint8_t> testFrame1(width * height * 3);
    std::vector<uint8_t> testFrame2(width * height * 3);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            
            testFrame1[idx] = static_cast<uint8_t>((x * 255) / width);
            testFrame1[idx + 1] = static_cast<uint8_t>((y * 100) / height);
            testFrame1[idx + 2] = 50;
            
            testFrame2[idx] = static_cast<uint8_t>(((width - x) * 200) / width);
            testFrame2[idx + 1] = static_cast<uint8_t>((y * 255) / height);
            testFrame2[idx + 2] = static_cast<uint8_t>((x * 100) / width);
        }
    }
    
    std::cout << "1. Testing frame difference encoding (.avop)..." << std::endl;
    
    std::vector<PixelChange> changes;
    AVOCodec::compareFrames(testFrame1, testFrame2, width, height, changes);
    
    std::vector<uint8_t> compressed = AVOCodec::compressRLE(changes);
    
    std::cout << "   Changes: " << changes.size() << ", Compressed: " 
              << compressed.size() << " bytes, Ratio: "
              << std::fixed << std::setprecision(1)
              << (compressed.size() * 100.0f / (width * height * 3)) 
              << "%" << std::endl;
    
    std::vector<PixelChange> decompressed = AVOCodec::decompressRLE(compressed);
    
    if (changes.size() == decompressed.size()) {
        std::cout << "   ✓ RLE compression/decompression works!" << std::endl;
    } else {
        std::cout << "   ✗ RLE error!" << std::endl;
    }
    
    std::cout << "2. Testing black frame creation..." << std::endl;
    std::vector<uint8_t> blackFrame = AVOCodec::createBlackFrame(width, height);
    if (blackFrame.size() == width * height * 3) {
        std::cout << "   ✓ Success! Black frame size: " << blackFrame.size() << " bytes" << std::endl;
    }
    
    cv::Mat frame1 = rgbVectorToMat(testFrame1, width, height);
    cv::Mat frame2 = rgbVectorToMat(testFrame2, width, height);
    
    cv::imwrite("test_frame1.png", frame1);
    cv::imwrite("test_frame2.png", frame2);
    
    std::cout << "\nSaved test images:" << std::endl;
    std::cout << "  - test_frame1.png (original frame 1)" << std::endl;
    std::cout << "  - test_frame2.png (original frame 2)" << std::endl;
}

void cameraTestMode() {
    std::cout << "\n=== Camera Test Mode ===\n" << std::endl;
    
    int cameraIndex = selectCamera();
    
    int width, height, requestedFps;
    
    std::cout << "\n=== Test Settings ===\n" << std::endl;
    std::cout << "Enter frame width (recommended 640): ";
    std::cin >> width;
    std::cout << "Enter frame height (recommended 480): ";
    std::cin >> height;
    std::cout << "Enter FPS (recommended 30): ";
    std::cin >> requestedFps;
    
    cv::VideoCapture cap(cameraIndex);
    
    if (!cap.isOpened()) {
        std::cerr << "Error: Cannot open camera " << cameraIndex << std::endl;
        return;
    }
    
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    cap.set(cv::CAP_PROP_FPS, requestedFps);
    
    double actualWidth = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double actualHeight = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double actualFps = cap.get(cv::CAP_PROP_FPS);
    
    if (actualFps <= 0) {
        actualFps = requestedFps;
    }
    
    std::cout << "\nActual parameters:" << std::endl;
    std::cout << "  Resolution: " << actualWidth << "x" << actualHeight << std::endl;
    std::cout << "  FPS: " << actualFps << std::endl;
    
    std::string windowName = "Camera Test - Press ESC to exit";
    cv::namedWindow(windowName, cv::WINDOW_NORMAL);
    cv::resizeWindow(windowName, width, height);
    
    std::cout << "\nCamera ready! Press ESC to exit" << std::endl;
    std::cout << "Press 's' to save frame" << std::endl;
    std::cout << "Press 'i' for frame info\n" << std::endl;
    
    int frameCount = 0;
    int emptyFrames = 0;
    auto startTime = std::chrono::steady_clock::now();
    auto lastStatsTime = startTime;
    int fpsCounter = 0;
    
    while (true) {
        cv::Mat frame;
        cap >> frame;
        
        if (frame.empty()) {
            emptyFrames++;
            if (emptyFrames % 10 == 0) {
                std::cout << "Warning: " << emptyFrames << " empty frames" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        frameCount++;
        fpsCounter++;
        
        cv::Mat displayFrame;
        
        if (frame.channels() == 1) {
            cv::cvtColor(frame, displayFrame, cv::COLOR_GRAY2BGR);
        } else if (frame.channels() == 3) {
            displayFrame = frame.clone();
        } else if (frame.channels() == 4) {
            cv::cvtColor(frame, displayFrame, cv::COLOR_BGRA2BGR);
        } else {
            displayFrame = frame.clone();
        }
        
        if (displayFrame.cols != width || displayFrame.rows != height) {
            cv::resize(displayFrame, displayFrame, cv::Size(width, height));
        }
        
        cv::putText(displayFrame, "Camera Test - ESC to exit", 
                   cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                   cv::Scalar(0, 255, 0), 2);
        
        std::string resText = std::to_string(frame.cols) + "x" + 
                             std::to_string(frame.rows) + " ch:" +
                             std::to_string(frame.channels());
        cv::putText(displayFrame, resText, 
                   cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                   cv::Scalar(0, 255, 0), 2);
        
        cv::putText(displayFrame, "Frame: #" + std::to_string(frameCount), 
                   cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                   cv::Scalar(0, 255, 0), 2);
        
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastStatsTime).count();
        
        if (elapsedMs >= 1000) {
            double fps = (fpsCounter * 1000.0) / elapsedMs;
            std::string fpsText = "FPS: " + std::to_string(static_cast<int>(fps));
            cv::putText(displayFrame, fpsText, 
                       cv::Point(10, 120), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 255, 0), 2);
        }
        
        cv::imshow(windowName, displayFrame);
        
        if (elapsedMs >= 2000) {
            double fps = (fpsCounter * 1000.0) / elapsedMs;
            std::cout << "Stats: " << frameCount << " frames, " 
                      << std::fixed << std::setprecision(1) << fps << " FPS" 
                      << ", empty: " << emptyFrames << std::endl;
            lastStatsTime = currentTime;
            fpsCounter = 0;
        }
        
        int key = cv::waitKey(1);
        if (key == 27) {
            break;
        } else if (key == 's' || key == 'S') {
            std::string filename = "camera_frame_" + std::to_string(frameCount) + ".png";
            cv::imwrite(filename, frame);
            std::cout << "Saved: " << filename << std::endl;
        } else if (key == 'i' || key == 'I') {
            std::cout << "\nFrame #" << frameCount << " info:" << std::endl;
            std::cout << "  Size: " << frame.cols << "x" << frame.rows << std::endl;
            std::cout << "  Channels: " << frame.channels() << std::endl;
            std::cout << "  Type: " << frame.type() << std::endl;
            
            double minVal, maxVal;
            cv::minMaxLoc(frame, &minVal, &maxVal);
            cv::Scalar mean, stddev;
            cv::meanStdDev(frame, mean, stddev);
            
            std::cout << "  Pixel range: " << minVal << " - " << maxVal << std::endl;
            std::cout << "  Mean: " << mean[0];
            if (frame.channels() > 1) std::cout << ", " << mean[1] << ", " << mean[2];
            std::cout << std::endl;
        }
    }
    
    cap.release();
    cv::destroyAllWindows();
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Total frames: " << frameCount << std::endl;
    std::cout << "Empty frames: " << emptyFrames << std::endl;
    std::cout << "Total time: " << totalElapsed << " sec" << std::endl;
    if (totalElapsed > 0) {
        std::cout << "Average FPS: " << std::fixed << std::setprecision(1) 
                  << (frameCount / (double)totalElapsed) << std::endl;
    }
}

int main() {
    disableAllLogs();
    
    std::cout << "=== .AVO Video Format System ===" << std::endl;
    std::cout << "Author: AVCD58 Implementation" << std::endl;
    std::cout << "Version: 4.0 (UDP Streaming with RLE)" << std::endl;
    std::cout << "Uses UDP + RLE compression for diff frames\n" << std::endl;
    
    try {
        int mode = selectMode();
        
        switch (mode) {
            case 1:
                serverMode();
                break;
            case 2:
                clientMode();
                break;
            case 3:
                testCodecMode();
                break;
            case 4:
                cameraTestMode();
                break;
            default:
                std::cout << "Invalid mode selection!" << std::endl;
                break;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nProgram finished." << std::endl;
    return 0;
}