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
#include <condition_variable>
#include <queue>
#include <tuple>
#include <dirent.h>
#include <sys/stat.h>

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
    std::cout << "5. Record video to .avo format" << std::endl;
    std::cout << "6. Play .avo video file" << std::endl;
    
    int mode = 0;
    std::cout << "\nSelect mode (1-6): ";
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
    server.setEncoderThreads(4);
    
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
    
    bool clientConnected = false;
    bool newClientConnected = false;
    int clientConnectionCheckCounter = 0;
    
    int frameCount = 0;
    int fpsCounter = 0;
    auto lastStatsTime = std::chrono::steady_clock::now();
    auto startTime = lastStatsTime;
    
    int delayMs = std::max(1, 1000 / static_cast<int>(actualFps));
    
    cv::namedWindow("UDP Server .AVO Stream", cv::WINDOW_NORMAL);
    cv::resizeWindow("UDP Server .AVO Stream", 640, 480);
    
    auto lastStatPrint = std::chrono::steady_clock::now();
    
    while (true) {
        clientConnectionCheckCounter++;
        if (clientConnectionCheckCounter >= 5) {
            bool hasClient = server.hasUDPClient();
            if (!clientConnected && hasClient) {
                clientConnected = true;
                newClientConnected = true;
                std::cout << "\n✓ Client connected! Sending initial full frame...\n" << std::endl;
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
        
        if (clientConnected) {
            server.sendUDPFrame(currentFrame, width, height);
        }
        
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatPrint).count() >= 3) {
            auto stats = server.getStats();
            std::cout << "[SERVER STATS] Frames: " << stats.framesProcessed
                     << ", Bytes: " << stats.bytesSent
                     << ", Encoding: " << stats.encodingTimeMs << "ms"
                     << ", Dropped: " << stats.bufferDropped << std::endl;
            lastStatPrint = now;
        }
        
        frameCount++;
        fpsCounter++;
        
        cv::Mat displayFrame;
        cv::resize(resizedFrame, displayFrame, cv::Size(640, 480));
        
        std::string statusText = clientConnected ? "CLIENT CONNECTED" : "WAITING FOR CLIENT...";
        cv::Scalar statusColor = clientConnected ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 165, 255);
        
        cv::rectangle(displayFrame, cv::Point(5, 5), cv::Point(635, 150), cv::Scalar(0, 0, 0), -1);
        cv::rectangle(displayFrame, cv::Point(5, 5), cv::Point(635, 150), statusColor, 2);
        
        cv::putText(displayFrame, "UDP SERVER: " + address,
                   cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                   cv::Scalar(255, 255, 255), 2);
        
        cv::putText(displayFrame, "STATUS: " + statusText,
                   cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                   statusColor, 2);
        
        cv::putText(displayFrame, "FRAMES: " + std::to_string(frameCount),
                   cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                   cv::Scalar(255, 255, 255), 1);
        
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastStatsTime).count();
        
        if (elapsedSec >= 2) {
            double fps = fpsCounter / elapsedSec;
            cv::putText(displayFrame, "FPS: " + std::to_string((int)fps),
                       cv::Point(10, 120), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(255, 255, 255), 1);
            lastStatsTime = currentTime;
            fpsCounter = 0;
        }
        
        cv::imshow("UDP Server .AVO Stream", displayFrame);
        
        int key = cv::waitKey(delayMs);
        if (key == 27) {
            std::cout << "\nStopping server..." << std::endl;
            break;
        }
    }
    
    cap.release();
    cv::destroyAllWindows();
    server.stopUDPServer();
    NetworkStream::cleanupNetwork();
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    auto stats = server.getStats();
    
    std::cout << "\n=== Server Summary ===" << std::endl;
    std::cout << "Address: " << address << std::endl;
    std::cout << "Total frames: " << frameCount << std::endl;
    std::cout << "Total time: " << totalElapsed << " sec" << std::endl;
    if (totalElapsed > 0) {
        std::cout << "Average FPS: " << std::fixed << std::setprecision(1)
                  << (frameCount / (double)totalElapsed) << std::endl;
    }
    std::cout << "\n=== Network Statistics ===" << std::endl;
    std::cout << "Frames processed: " << stats.framesProcessed << std::endl;
    std::cout << "Bytes sent: " << stats.bytesSent << std::endl;
    std::cout << "Packets sent: " << stats.packetsSent << std::endl;
    std::cout << "Frames dropped: " << stats.bufferDropped << std::endl;
    std::cout << "Total encoding time: " << stats.encodingTimeMs << " ms" << std::endl;
    std::cout << "Total network time: " << stats.networkTimeMs << " ms" << std::endl;
    if (stats.framesProcessed > 0) {
        std::cout << "Avg encoding time: " 
                  << (stats.encodingTimeMs / stats.framesProcessed) << " ms/frame" << std::endl;
        std::cout << "Avg network time: " 
                  << (stats.networkTimeMs / stats.framesProcessed) << " ms/frame" << std::endl;
    }
    std::cout << "Streaming finished." << std::endl;
}

// ================= UDP КЛИЕНТ =================
struct ClientProcessing {
    std::queue<std::tuple<std::vector<uint8_t>, uint32_t, uint32_t, bool>> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondVar;
    std::atomic<bool> running{true};
    std::vector<std::thread> processingThreads;
    
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> fragmentMap;
    std::map<uint32_t, uint32_t> fragmentTotal;
    std::map<uint32_t, uint32_t> fragmentWidth;
    std::map<uint32_t, uint32_t> fragmentHeight;
    std::mutex fragmentMutex;
    
    std::vector<uint8_t> currentFrame;
    uint32_t currentWidth{0};
    uint32_t currentHeight{0};
    std::atomic<bool> frameReady{false};
    std::mutex frameMutex;
    std::atomic<uint64_t> framesProcessed{0};
    
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> processingTimeMs{0};
    std::atomic<uint64_t> queueDropped{0};
};

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
    
    ClientProcessing processor;
    const int NUM_PROCESSING_THREADS = 4;
    
    for (int i = 0; i < NUM_PROCESSING_THREADS; i++) {
        processor.processingThreads.emplace_back([&processor, i]() {
            while (processor.running) {
                std::tuple<std::vector<uint8_t>, uint32_t, uint32_t, bool> packet;
                
                {
                    std::unique_lock<std::mutex> lock(processor.queueMutex);
                    processor.queueCondVar.wait(lock, [&processor]() {
                        return !processor.packetQueue.empty() || !processor.running;
                    });
                    
                    if (!processor.running && processor.packetQueue.empty()) {
                        break;
                    }
                    
                    if (!processor.packetQueue.empty()) {
                        packet = processor.packetQueue.front();
                        processor.packetQueue.pop();
                    } else {
                        continue;
                    }
                }
                
                auto startTime = std::chrono::high_resolution_clock::now();
                
                auto& [packetData, width, height, isFullFrame] = packet;
                
                if (packetData.empty()) {
                    continue;
                }
                
                processor.packetsReceived++;
                
                try {
                    uint32_t expectedFullFrameSize = width * height * 3;
                    
                    if (packetData.size() == 1 && packetData[0] == 0) {
                        processor.framesDecoded++;
                        continue;
                    }
                    
                    if (isFullFrame) {
                        {
                            std::lock_guard<std::mutex> lock(processor.frameMutex);
                            processor.currentFrame = packetData;
                            processor.currentWidth = width;
                            processor.currentHeight = height;
                            processor.frameReady = true;
                        }
                        processor.framesDecoded++;
                    } else {
                        std::vector<PixelChange> changes = AVOCodec::decompressRLE(packetData);
                        
                        std::lock_guard<std::mutex> lock(processor.frameMutex);
                        
                        if (processor.currentFrame.empty() || 
                            processor.currentFrame.size() != expectedFullFrameSize ||
                            processor.currentWidth != width || 
                            processor.currentHeight != height) {
                            processor.currentFrame = AVOCodec::createBlackFrame(width, height);
                            processor.currentWidth = width;
                            processor.currentHeight = height;
                        }
                        
                        std::vector<uint8_t> newFrame;
                        AVOCodec::applyChanges(processor.currentFrame, changes, 
                                               newFrame, width, height);
                        processor.currentFrame = newFrame;
                        processor.frameReady = true;
                        processor.framesDecoded++;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[CLIENT Thread " << i << "] Error processing packet: " 
                              << e.what() << std::endl;
                }
                
                auto endTime = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime);
                processor.processingTimeMs += duration.count();
            }
        });
    }
    
    cv::namedWindow("UDP Client .AVO Stream", cv::WINDOW_NORMAL);
    cv::resizeWindow("UDP Client .AVO Stream", 640, 480);
    
    auto frameCallback = [&processor](const std::vector<uint8_t>& packetData,
                                     uint32_t width, uint32_t height, bool isFullFrame) {
        if (packetData.size() == 1 && packetData[0] == 0) {
            std::lock_guard<std::mutex> lock(processor.queueMutex);
            if (processor.packetQueue.size() < 50) {
                processor.packetQueue.emplace(packetData, width, height, isFullFrame);
            }
            processor.queueCondVar.notify_one();
            return;
        }
        
        {
            std::lock_guard<std::mutex> lock(processor.queueMutex);
            if (processor.packetQueue.size() < 50) {
                processor.packetQueue.emplace(packetData, width, height, isFullFrame);
            } else {
                while (processor.packetQueue.size() >= 40) {
                    processor.packetQueue.pop();
                    processor.queueDropped++;
                }
                processor.packetQueue.emplace(packetData, width, height, isFullFrame);
            }
        }
        processor.queueCondVar.notify_one();
    };
    
    if (!client.startUDPReceiver(frameCallback)) {
        std::cerr << "Failed to start UDP receiver" << std::endl;
        
        processor.running = false;
        processor.queueCondVar.notify_all();
        for (auto& thread : processor.processingThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        client.disconnectUDP();
        NetworkStream::cleanupNetwork();
        cv::destroyAllWindows();
        return;
    }
    
    std::cout << "\nConnected to UDP server! Receiving stream..." << std::endl;
    std::cout << "Using " << NUM_PROCESSING_THREADS << " processing threads" << std::endl;
    std::cout << "Press ESC to exit\n" << std::endl;
    
    int fpsCounter = 0;
    auto lastFpsTime = std::chrono::steady_clock::now();
    auto lastStatsTime = lastFpsTime;
    double clientFps = 0;
    int displayFps = 0;
    int waitingFrameCount = 0;
    bool wasShowingVideo = false;
    cv::Mat lastGoodFrame = cv::Mat::zeros(480, 640, CV_8UC3);
    
    while (true) {
        auto frameStart = std::chrono::high_resolution_clock::now();
        
        cv::Mat displayFrame;
        bool hasFrame = false;
        std::vector<uint8_t> localFrame;
        uint32_t localWidth, localHeight;
        
        {
            std::lock_guard<std::mutex> lock(processor.frameMutex);
            if (processor.frameReady && !processor.currentFrame.empty()) {
                hasFrame = true;
                localFrame = processor.currentFrame;
                localWidth = processor.currentWidth;
                localHeight = processor.currentHeight;
                processor.frameReady = false;
            }
        }
        
        if (hasFrame) {
            wasShowingVideo = true;
            waitingFrameCount = 0;
            
            cv::Mat frame = rgbVectorToMat(localFrame, localWidth, localHeight);
            
            if (frame.empty()) {
                frame = cv::Mat::zeros(480, 640, CV_8UC3);
            }
            
            cv::resize(frame, displayFrame, cv::Size(640, 480));
            lastGoodFrame = displayFrame.clone();
            
            std::stringstream info;
            info << "UDP Client: " << address;
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
            
            info.str("");
            info << "Frames: " << processor.framesDecoded;
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
            
            info.str("");
            info << "FPS: " << displayFps;
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
            
            info.str("");
            info << "Res: " << localWidth << "x" << localHeight;
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 120), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
            
            info.str("");
            info << "Queue: " << processor.packetQueue.size();
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 150), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
            
            info.str("");
            info << "Threads: " << NUM_PROCESSING_THREADS;
            cv::putText(displayFrame, info.str(),
                       cv::Point(10, 180), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                       cv::Scalar(0, 255, 255), 2);
        } else {
            waitingFrameCount++;
            
            if (!wasShowingVideo || waitingFrameCount > 30) {
                displayFrame = cv::Mat::zeros(480, 640, CV_8UC3);
                cv::putText(displayFrame, "Waiting for data...",
                           cv::Point(640/2 - 120, 480/2),
                           cv::FONT_HERSHEY_SIMPLEX, 0.7,
                           cv::Scalar(0, 255, 255), 2);
                cv::putText(displayFrame, "Server: " + address,
                           cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                           cv::Scalar(0, 255, 255), 1);
            } else {
                displayFrame = lastGoodFrame.clone();
                cv::putText(displayFrame, "BUFFERING...",
                           cv::Point(640/2 - 80, 480/2),
                           cv::FONT_HERSHEY_SIMPLEX, 0.7,
                           cv::Scalar(0, 165, 255), 2);
            }
        }
        
        cv::imshow("UDP Client .AVO Stream", displayFrame);
        
        fpsCounter++;
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - lastFpsTime).count();
        
        if (elapsedSec >= 1) {
            clientFps = fpsCounter / (double)elapsedSec;
            displayFps = (int)clientFps;
            fpsCounter = 0;
            lastFpsTime = currentTime;
        }
        
        auto elapsedStatsSec = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - lastStatsTime).count();
        
        if (elapsedStatsSec >= 5) {
            std::cout << "\n[CLIENT STATS]" << std::endl;
            std::cout << "  Frames decoded: " << processor.framesDecoded << std::endl;
            std::cout << "  Packets received: " << processor.packetsReceived << std::endl;
            std::cout << "  Queue size: " << processor.packetQueue.size() << std::endl;
            std::cout << "  Queue dropped: " << processor.queueDropped << std::endl;
            std::cout << "  Client FPS: " << std::fixed << std::setprecision(1) 
                      << clientFps << std::endl;
            std::cout << "  Avg processing time: " 
                      << (processor.processingTimeMs / (processor.framesDecoded + 1))
                      << " ms/frame" << std::endl;
            lastStatsTime = currentTime;
        }
        
        auto frameEnd = std::chrono::high_resolution_clock::now();
        auto frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameEnd - frameStart).count();
        
        int delay = std::max(1, 33 - (int)frameTime);
        int key = cv::waitKey(delay);
        
        if (key == 27) {
            break;
        }
    }
    
    processor.running = false;
    processor.queueCondVar.notify_all();
    for (auto& thread : processor.processingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    client.disconnectUDP();
    cv::destroyAllWindows();
    NetworkStream::cleanupNetwork();
    
    std::cout << "\n=== Client Summary ===" << std::endl;
    std::cout << "Total frames decoded: " << processor.framesDecoded << std::endl;
    std::cout << "Total packets received: " << processor.packetsReceived << std::endl;
    std::cout << "Queue packets dropped: " << processor.queueDropped << std::endl;
    std::cout << "Average processing time: " 
              << (processor.processingTimeMs / (processor.framesDecoded + 1))
              << " ms/frame" << std::endl;
    std::cout << "Client stopped." << std::endl;
}

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
    double actualHeight = cap.get(cv::CAP_PROP_FRAME_WIDTH);
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

// ================= ЗАПИСЬ В АРХИВ =================
// Функция для записи видео в один .avo архив (с сохранением реальных задержек)
void recordAVOArchiveMode() {
    std::cout << "\n=== Record Video to .avo Archive ===\n" << std::endl;
    
    int cameraIndex = selectCamera();
    
    int width, height, fps;
    std::string filename;
    
    std::cout << "\n=== Recording Settings ===\n" << std::endl;
    std::cout << "Enter frame width (recommended 640): ";
    std::cin >> width;
    std::cout << "Enter frame height (recommended 480): ";
    std::cin >> height;
    std::cout << "Enter FPS (15-30 recommended, но камера может быть медленнее): ";
    std::cin >> fps;
    
    std::cout << "Enter output filename (with .avo extension): ";
    std::cin >> filename;
    
    if (filename.find(".avo") == std::string::npos) {
        filename += ".avo";
    }
    
    cv::VideoCapture cap(cameraIndex);
    
    if (!cap.isOpened()) {
        std::cerr << "Error: Cannot open camera " << cameraIndex << std::endl;
        return;
    }
    
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    
    double actualWidth = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double actualHeight = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    
    width = static_cast<int>(actualWidth);
    height = static_cast<int>(actualHeight);
    
    std::cout << "\nRecording parameters:" << std::endl;
    std::cout << "  Resolution: " << width << "x" << height << std::endl;
    std::cout << "  Note: Recording at camera's actual speed (real FPS)" << std::endl;
    std::cout << "  Output: " << filename << std::endl;
    
    std::cout << "\nPress SPACE to start recording, ESC to stop\n" << std::endl;
    
    cv::namedWindow("AVO Archive Recorder", cv::WINDOW_NORMAL);
    cv::resizeWindow("AVO Archive Recorder", width, height);
    
    bool recording = false;
    bool firstFrameCaptured = false;
    std::vector<uint8_t> prevFrame;
    std::vector<AVOFrame> videoFrames;
    int frameCount = 0;
    
    auto startTime = std::chrono::steady_clock::now();
    auto lastFrameTime = startTime;
    auto lastStatTime = startTime;
    int statFrameCount = 0;
    
    while (true) {
        cv::Mat frame;
        cap >> frame;
        
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        
        cv::Mat resizedFrame;
        cv::resize(frame, resizedFrame, cv::Size(width, height));
        
        std::vector<uint8_t> currentFrame = matToRGBVector(resizedFrame);
        
        cv::Mat displayFrame = resizedFrame.clone();
        
        if (recording) {
            // ИЗМЕРЕНИЕ РЕАЛЬНОГО ВРЕМЕНИ МЕЖДУ КАДРАМИ
            auto currentTime = std::chrono::steady_clock::now();
            auto realDelayMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                currentTime - lastFrameTime).count();
            
            // Ограничиваем максимальную задержку
            if (realDelayMs > 1000) realDelayMs = 1000;
            
            AVOFrame avoFrame;
            avoFrame.delayMs = static_cast<uint32_t>(realDelayMs); // Сохраняем РЕАЛЬНУЮ задержку
            
            if (!firstFrameCaptured) {
                // Первый кадр - полный
                avoFrame.data = currentFrame;
                avoFrame.isFullFrame = true;
                firstFrameCaptured = true;
                prevFrame = currentFrame;
                videoFrames.push_back(avoFrame);
            } else {
                // Последующие кадры - только изменения
                std::vector<PixelChange> changes;
                AVOCodec::compareFrames(prevFrame, currentFrame, width, height, changes);
                
                if (changes.empty()) {
                    // Нет изменений - создаем пустой кадр изменений
                    avoFrame.data.clear();
                } else {
                    // Есть изменения - сжимаем их
                    avoFrame.data = AVOCodec::compressRLE(changes);
                }
                
                avoFrame.isFullFrame = false;
                videoFrames.push_back(avoFrame);
                
                // Обновляем предыдущий кадр
                prevFrame = currentFrame;
            }
            
            frameCount++;
            statFrameCount++;
            lastFrameTime = currentTime;
            
            // Статистика
            auto now = std::chrono::steady_clock::now();
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - lastStatTime).count();
            
            if (elapsedSec >= 2) {
                double currentFps = statFrameCount / (double)elapsedSec;
                std::cout << "Frame " << frameCount 
                          << ": real FPS=" << std::fixed << std::setprecision(1) << currentFps
                          << ", delay=" << realDelayMs << "ms"
                          << ", archive frames: " << videoFrames.size() << std::endl;
                lastStatTime = now;
                statFrameCount = 0;
            }
            
            // Отображение
            cv::putText(displayFrame, "RECORDING - Frame: " + std::to_string(frameCount), 
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 0, 255), 2);
            cv::putText(displayFrame, "Real FPS: " + std::to_string((int)(1000.0/(realDelayMs > 0 ? realDelayMs : 1))), 
                       cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.6, 
                       cv::Scalar(0, 0, 255), 1);
            cv::putText(displayFrame, "Delay: " + std::to_string(realDelayMs) + "ms", 
                       cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                       cv::Scalar(0, 0, 255), 1);
            cv::putText(displayFrame, "Archive: " + std::to_string(videoFrames.size()) + " frames",
                       cv::Point(10, 120), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                       cv::Scalar(0, 0, 255), 1);
            cv::circle(displayFrame, cv::Point(width - 30, 30), 10, cv::Scalar(0, 0, 255), -1);
        } else {
            cv::putText(displayFrame, "READY - Press SPACE to start recording", 
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, 
                       cv::Scalar(0, 255, 0), 2);
            cv::putText(displayFrame, "Will record at camera's actual speed", 
                       cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.6, 
                       cv::Scalar(0, 255, 0), 1);
            cv::putText(displayFrame, "Press ESC to exit", 
                       cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.5, 
                       cv::Scalar(0, 255, 0), 1);
        }
        
        cv::imshow("AVO Archive Recorder", displayFrame);
        
        int key = cv::waitKey(1);
        
        if (key == 27) { // ESC
            break;
        } else if (key == 32) { // SPACE
            recording = !recording;
            if (recording) {
                std::cout << "Recording started to archive!" << std::endl;
                std::cout << "Recording at camera's actual speed (real FPS)" << std::endl;
                startTime = std::chrono::steady_clock::now();
                lastFrameTime = startTime;
                lastStatTime = startTime;
                frameCount = 0;
                statFrameCount = 0;
                firstFrameCaptured = false;
                videoFrames.clear();
                prevFrame.clear();
            } else {
                // Сохраняем архив при остановке записи
                if (!videoFrames.empty()) {
                    std::cout << "Saving archive to " << filename << "..." << std::endl;
                    
                    // Рассчитываем средний FPS
                    double totalDelayMs = 0;
                    for (const auto& frame : videoFrames) {
                        totalDelayMs += frame.delayMs;
                    }
                    double avgFps = (videoFrames.size() * 1000.0) / (totalDelayMs > 0 ? totalDelayMs : 1);
                    
                    std::cout << "Average real FPS: " << std::fixed << std::setprecision(1) << avgFps << std::endl;
                    std::cout << "Total recording time: " << (totalDelayMs / 1000.0) << " sec" << std::endl;
                    
                    // Передаем 0 как FPS, так как используем реальные задержки
                    if (AVOCodec::encodeVideoArchive(videoFrames, width, height, 0, filename)) {
                        std::cout << "Archive saved successfully!" << std::endl;
                        std::cout << "Video will playback at the same speed it was recorded" << std::endl;
                    } else {
                        std::cerr << "Failed to save archive!" << std::endl;
                    }
                }
                videoFrames.clear();
            }
        }
    }
    
    // Сохраняем архив если запись была активна
    if (recording && !videoFrames.empty()) {
        std::cout << "Saving archive to " << filename << "..." << std::endl;
        
        // Рассчитываем средний FPS
        double totalDelayMs = 0;
        for (const auto& frame : videoFrames) {
            totalDelayMs += frame.delayMs;
        }
        double avgFps = (videoFrames.size() * 1000.0) / (totalDelayMs > 0 ? totalDelayMs : 1);
        
        std::cout << "Average real FPS: " << std::fixed << std::setprecision(1) << avgFps << std::endl;
        std::cout << "Total recording time: " << (totalDelayMs / 1000.0) << " sec" << std::endl;
        
        // Передаем 0 как FPS, так как используем реальные задержки
        if (AVOCodec::encodeVideoArchive(videoFrames, width, height, 0, filename)) {
            std::cout << "Archive saved successfully!" << std::endl;
        }
    }
    
    cap.release();
    cv::destroyAllWindows();
    
    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
    
    std::cout << "\n=== Recording Summary ===" << std::endl;
    std::cout << "Total frames captured: " << frameCount << std::endl;
    std::cout << "Frames in archive: " << videoFrames.size() << std::endl;
    std::cout << "Total time: " << totalElapsed << " sec" << std::endl;
    if (totalElapsed > 0) {
        std::cout << "Average FPS: " << std::fixed << std::setprecision(1)
                  << (frameCount / (double)totalElapsed) << std::endl;
    }
    std::cout << "Archive file: " << filename << std::endl;
    std::cout << "Recording finished." << std::endl;
}

// Функция для воспроизведения .avo архива (с реальными задержками)
// Функция для воспроизведения .avo архива (с компенсацией времени обработки)
void playAVOArchiveMode() {
    std::cout << "\n=== Play .avo Video Archive ===\n" << std::endl;
    
    std::string filename;
    
    std::cout << "Enter .avo archive filename: ";
    std::cin >> filename;
    
    if (filename.find(".avo") == std::string::npos) {
        filename += ".avo";
    }
    
    // Загружаем архив
    std::vector<AVOFrame> frames;
    AVOHeader header;
    
    if (!AVOCodec::decodeVideoArchive(filename, frames, header)) {
        std::cerr << "Error loading .avo archive: " << filename << std::endl;
        return;
    }
    
    std::cout << "\nVideo Archive information:" << std::endl;
    std::cout << "  Resolution: " << header.width << "x" << header.height << std::endl;
    std::cout << "  Total frames in archive: " << frames.size() << std::endl;
    std::cout << "  First frame size: " << header.firstFrameSize << " bytes" << std::endl;
    
    // Рассчитываем общее время и средний FPS
    double totalDelayMs = 0;
    for (const auto& frame : frames) {
        totalDelayMs += frame.delayMs;
    }
    
    double totalTimeSec = totalDelayMs / 1000.0;
    double avgFps = (frames.size() * 1000.0) / (totalDelayMs > 0 ? totalDelayMs : 1);
    
    std::cout << "  Total time: " << std::fixed << std::setprecision(1) << totalTimeSec << " sec" << std::endl;
    std::cout << "  Average FPS: " << std::fixed << std::setprecision(1) << avgFps << std::endl;
    
    std::cout << "\nPress any key to start playback, ESC to exit\n" << std::endl;
    
    cv::namedWindow("AVO Archive Player", cv::WINDOW_NORMAL);
    cv::resizeWindow("AVO Archive Player", header.width, header.height);
    
    // Показываем первый кадр
    if (!frames.empty()) {
        cv::Mat firstFrameMat = rgbVectorToMat(frames[0].data, header.width, header.height);
        cv::putText(firstFrameMat, "Press any key to play", 
                   cv::Point(header.width/2 - 100, header.height/2),
                   cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        cv::imshow("AVO Archive Player", firstFrameMat);
        cv::waitKey(0);
    }
    
    // Воспроизведение с КОМПЕНСАЦИЕЙ времени обработки
    auto playbackStartTime = std::chrono::steady_clock::now();
    auto nextFrameTime = playbackStartTime;
    int displayedFrames = 0;
    
    for (size_t i = 0; i < frames.size(); i++) {
        const AVOFrame& frame = frames[i];
        
        // Ждем до времени отображения этого кадра
        std::this_thread::sleep_until(nextFrameTime);
        
        // Измеряем реальное время начала отображения
        auto frameDisplayStart = std::chrono::steady_clock::now();
        
        // Отображение
        cv::Mat displayFrame = rgbVectorToMat(frame.data, header.width, header.height);
        displayedFrames++;
        
        // Информация на кадре
        cv::putText(displayFrame, "Frame: " + std::to_string(displayedFrames) + "/" + std::to_string(frames.size()), 
                   cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6, 
                   cv::Scalar(0, 255, 255), 2);
        
        // Вычисляем реальное время от начала воспроизведения
        auto currentPlaybackTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameDisplayStart - playbackStartTime).count();
        
        std::string timeText = "Time: " + std::to_string(currentPlaybackTime/1000) + "." + 
                              std::to_string(currentPlaybackTime%1000).substr(0,2) + "s";
        cv::putText(displayFrame, timeText,
                   cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                   cv::Scalar(0, 255, 255), 1);
        
        cv::imshow("AVO Archive Player", displayFrame);
        
        // Измеряем время, которое заняло отображение
        auto frameDisplayEnd = std::chrono::steady_clock::now();
        auto displayDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameDisplayEnd - frameDisplayStart).count();
        
        // Рассчитываем время для следующего кадра
        // nextFrameTime = текущее время начала + задержка этого кадра
        // Но вычитаем время, которое уже потратили на отображение
        int adjustedDelay = frame.delayMs - displayDuration;
        if (adjustedDelay < 1) adjustedDelay = 1; // Минимум 1мс
        
        nextFrameTime = frameDisplayStart + std::chrono::milliseconds(frame.delayMs);
        
        // Обработка клавиш с минимальной задержкой
        int key = cv::waitKey(1);
        
        if (key == 27) { // ESC
            break;
        } else if (key == 32) { // SPACE - пауза
            std::cout << "Paused. Press any key to continue..." << std::endl;
            cv::waitKey(0);
            // После паузы корректируем nextFrameTime
            auto pauseEndTime = std::chrono::steady_clock::now();
            nextFrameTime = pauseEndTime;
        }
        
        // Статистика каждые 15 кадров
        if (displayedFrames % 15 == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedTotal = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - playbackStartTime).count();
            
            double currentFps = (displayedFrames * 1000.0) / (elapsedTotal > 0 ? elapsedTotal : 1);
            double expectedTime = (totalDelayMs * displayedFrames) / frames.size();
            
            std::cout << "Frame " << displayedFrames << "/" << frames.size() 
                     << " | Real FPS: " << std::fixed << std::setprecision(1) << currentFps
                     << " | Time: " << (elapsedTotal/1000.0) << "s"
                     << " | Expected: " << (expectedTime/1000.0) << "s"
                     << " | Diff: " << std::fixed << std::setprecision(1) 
                     << ((elapsedTotal - expectedTime)/1000.0) << "s" << std::endl;
        }
    }
    
    cv::destroyAllWindows();
    
    auto playbackEndTime = std::chrono::steady_clock::now();
    auto totalPlaybackTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        playbackEndTime - playbackStartTime).count();
    
    std::cout << "\n=== Playback Summary ===" << std::endl;
    std::cout << "Total frames displayed: " << displayedFrames << std::endl;
    std::cout << "Total playback time: " << (totalPlaybackTime/1000.0) << " sec" << std::endl;
    std::cout << "Original recording time: " << std::fixed << std::setprecision(1) 
              << totalTimeSec << " sec" << std::endl;
    
    double timeDifference = (totalPlaybackTime/1000.0) - totalTimeSec;
    std::cout << "Time difference: " << std::fixed << std::setprecision(3) 
              << timeDifference << " sec" << std::endl;
    
    if (std::abs(timeDifference) < 0.1) {
        std::cout << "✓ Playback matches recording perfectly!" << std::endl;
    } else if (std::abs(timeDifference) < 0.5) {
        std::cout << "~ Playback is close to recording" << std::endl;
    } else {
        std::cout << "✗ Playback differs from recording" << std::endl;
    }
    
    std::cout << "Playback finished." << std::endl;
}

// В функции main() добавьте:
int main() {
    disableAllLogs();
    
    std::cout << "=== .AVO Video Format System ===" << std::endl;
    std::cout << "Author: AVCD58 Implementation" << std::endl;
    std::cout << "Version: 4.0 (Complete AVO Codec System)" << std::endl;
    std::cout << "Features: Recording, Playback, UDP Streaming, RLE compression\n" << std::endl;
    
    std::cout << "\n=== Mode Selection ===\n" << std::endl;
    std::cout << "1. Start Server (stream video to clients)" << std::endl;
    std::cout << "2. Connect to Server (receive video)" << std::endl;
    std::cout << "3. Codec test (save/load files)" << std::endl;
    std::cout << "4. Camera test (extended diagnostics)" << std::endl;
    std::cout << "5. Record video to .avo archive (single file)" << std::endl;  // НОВЫЙ
    std::cout << "6. Play .avo video archive" << std::endl;  // НОВЫЙ
    
    int mode = 0;
    std::cout << "\nSelect mode (1-6): ";
    std::cin >> mode;
    
    try {
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
            case 5:
                recordAVOArchiveMode();  // НОВЫЙ
                break;
            case 6:
                playAVOArchiveMode();  // НОВЫЙ
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