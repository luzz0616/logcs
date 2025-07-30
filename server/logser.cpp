// logser.cpp
#include "protocol.h"
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <atomic>
#include <unordered_map>
#include <getopt.h>
#include <cstdarg>

// DLT 日志库接口
extern "C" {
    void dlt_init_client(const char *appid);
    void dlt_log_fatal(const char *appid, const char *fmt, ...);
    void dlt_log_warn(const char *appid, const char *fmt, ...);
    void dlt_log_info(const char *appid, const char *fmt, ...);
    void dlt_free_client(const char *appid);
}

// 命令处理器接口
class CommandHandler {
protected:
    const char* appid_;

public:
    CommandHandler(const char* appid) : appid_(appid) {}
    virtual ~CommandHandler() = default;
    virtual void handle(const Message& req, Message& resp) = 0;
};

// INFO 命令处理器
class InfoCommandHandler : public CommandHandler {
public:
    InfoCommandHandler(const char* appid) : CommandHandler(appid) {}

    void handle(const Message& req, Message& resp) override {
        dlt_log_info(appid_, "Received INFO command: %s", req.body.c_str());
        std::cout << "[INFO] " << req.body << std::endl;
        resp.header.cmd = CommandType::INFO;
        resp.header.seq = req.header.seq;
        resp.body = "OK";
        resp.header.bodySize = resp.body.size();
    }
};

// WARN 命令处理器
class WarnCommandHandler : public CommandHandler {
public:
    WarnCommandHandler(const char* appid) : CommandHandler(appid) {}

    void handle(const Message& req, Message& resp) override {
        dlt_log_warn(appid_, "Received WARN command: %s", req.body.c_str());
        std::cerr << "[WARN] " << req.body << std::endl;
        resp.header.cmd = CommandType::WARN;
        resp.header.seq = req.header.seq;
        resp.body = "OK";
        resp.header.bodySize = resp.body.size();
    }
};

// ERROR 命令处理器
class ErrorCommandHandler : public CommandHandler {
public:
    ErrorCommandHandler(const char* appid) : CommandHandler(appid) {}

    void handle(const Message& req, Message& resp) override {
        dlt_log_fatal(appid_, "Received ERROR command: %s", req.body.c_str());
        std::cerr << "[ERROR] " << req.body << std::endl;
        resp.header.cmd = CommandType::ERROR;
        resp.header.seq = req.header.seq;
        resp.body = "OK";
        resp.header.bodySize = resp.body.size();
    }
};

// SHUTDOWN 命令处理器
class ShutdownCommandHandler : public CommandHandler {
private:
    std::atomic<bool>& running_;

public:
    ShutdownCommandHandler(const char* appid, std::atomic<bool>& running) 
        : CommandHandler(appid), running_(running) {}

    void handle(const Message& req, Message& resp) override {
        dlt_log_info(appid_, "Received SHUTDOWN command");
        std::cout << "[INFO] Received shutdown command" << std::endl;
        resp.header.cmd = CommandType::SHUTDOWN;
        resp.header.seq = req.header.seq;
        resp.body = "Shutting down...";
        resp.header.bodySize = resp.body.size();
        running_ = false;
    }
};

// STATS 命令处理器
class StatsCommandHandler : public CommandHandler {
private:
    uint64_t& requestCount_;

public:
    StatsCommandHandler(const char* appid, uint64_t& requestCount) 
        : CommandHandler(appid), requestCount_(requestCount) {}

    void handle(const Message& req, Message& resp) override {
        dlt_log_info(appid_, "Received STATS command");
        std::cout << "[INFO] Received stats request" << std::endl;
        resp.header.cmd = CommandType::STATS;
        resp.header.seq = req.header.seq;
        resp.body = "Total requests: " + std::to_string(requestCount_);
        resp.header.bodySize = resp.body.size();
    }
};

// 客户端连接处理类
class ClientConnection {
private:
    int socket_;
    const char* appid_;

public:
    ClientConnection(int socket, const char* appid) 
        : socket_(socket), appid_(appid) {}
    
    ~ClientConnection() { 
        if (socket_ >= 0) {
            dlt_log_info(appid_, "Closing client socket %d", socket_);
            shutdown(socket_, SHUT_RDWR);
            close(socket_);
        }
    }

    // 禁用拷贝构造和赋值
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    // 允许移动构造
    ClientConnection(ClientConnection&& other) noexcept 
        : socket_(other.socket_), appid_(other.appid_) {
        other.socket_ = -1;
    }

    bool receiveMessage(Message& msg) {
        // 接收消息头
        MessageHeader header;
        ssize_t bytesRead = recv(socket_, &header, sizeof(header), 0);
        if (bytesRead != sizeof(header)) {
            dlt_log_warn(appid_, "Failed to receive complete header: %zd bytes", bytesRead);
            std::cerr << "Failed to receive complete header: " << bytesRead << " bytes" << std::endl;
            return false;
        }

        // 转换字节序
        header.fromNetworkByteOrder();

        // 验证魔数和头部大小
        const uint32_t EXPECTED_MAGIC = 0x12345678;
        if (header.magic != EXPECTED_MAGIC) {
            dlt_log_fatal(appid_, "Invalid magic number: 0x%08X, expected 0x%08X", 
                         header.magic, EXPECTED_MAGIC);
            std::cerr << "Invalid magic number: 0x" << std::hex << header.magic 
                      << ", expected 0x" << EXPECTED_MAGIC << std::dec << std::endl;
            return false;
        }

        if (header.headerSize != sizeof(MessageHeader)) {
            dlt_log_fatal(appid_, "Invalid header size: %u, expected: %zu", 
                         header.headerSize, sizeof(MessageHeader));
            std::cerr << "Invalid header size: " << header.headerSize 
                      << ", expected: " << sizeof(MessageHeader) << std::endl;
            return false;
        }

        // 接收消息体
        msg.header = header;
        msg.body.resize(header.bodySize);
        if (header.bodySize > 0) {
            bytesRead = recv(socket_, &msg.body[0], header.bodySize, 0);
            if (bytesRead != static_cast<ssize_t>(header.bodySize)) {
                dlt_log_warn(appid_, "Failed to receive complete body: %zd of %zu bytes", 
                           bytesRead, header.bodySize);
                std::cerr << "Failed to receive complete body: " << bytesRead 
                          << " of " << header.bodySize << " bytes" << std::endl;
                return false;
            }
        }

        dlt_log_info(appid_, "Received message: cmd=%d, seq=%u, size=%zu", 
                    header.cmd, header.seq, header.bodySize);
        return true;
    }

    bool sendMessage(const Message& msg) {
        // 复制并转换头部字节序
        MessageHeader header = msg.header;
        header.toNetworkByteOrder();

        // 发送头部
        ssize_t bytesSent = send(socket_, &header, sizeof(header), 0);
        if (bytesSent != sizeof(header)) {
            dlt_log_warn(appid_, "Failed to send header: %zd of %zu bytes", 
                        bytesSent, sizeof(header));
            std::cerr << "Failed to send header: " << bytesSent 
                      << " of " << sizeof(header) << " bytes" << std::endl;
            return false;
        }

        // 发送消息体
        if (msg.body.size() > 0) {
            bytesSent = send(socket_, msg.body.data(), msg.body.size(), 0);
            if (bytesSent != static_cast<ssize_t>(msg.body.size())) {
                dlt_log_warn(appid_, "Failed to send body: %zd of %zu bytes", 
                            bytesSent, msg.body.size());
                std::cerr << "Failed to send body: " << bytesSent 
                          << " of " << msg.body.size() << " bytes" << std::endl;
                return false;
            }
        }

        dlt_log_info(appid_, "Sent response: cmd=%d, seq=%u, size=%zu", 
                    msg.header.cmd, msg.header.seq, msg.header.bodySize);
        return true;
    }
};

// 服务器类
class LogServer {
private:
    const char* appid_ = "logser";
    int port_;
    int tcpSocket_;
    int unixSocket_;
    std::string unixSocketPath_;
    std::atomic<bool> running_;
    uint64_t requestCount_;
    std::unordered_map<CommandType, std::unique_ptr<CommandHandler>> commandHandlers_;

public:
    LogServer(int port = 8999, const std::string& unixSocketPath = "/tmp/logser.sock")
        : appid_("logser"), port_(port), tcpSocket_(-1), unixSocket_(-1), 
          unixSocketPath_(unixSocketPath), running_(true), requestCount_(0) {
        
        dlt_log_info(appid_, "Initializing LogServer on port %d", port_);
        
        // 注册命令处理器
        commandHandlers_[CommandType::INFO] = std::make_unique<InfoCommandHandler>(appid_);
        commandHandlers_[CommandType::WARN] = std::make_unique<WarnCommandHandler>(appid_);
        commandHandlers_[CommandType::ERROR] = std::make_unique<ErrorCommandHandler>(appid_);
        commandHandlers_[CommandType::SHUTDOWN] = std::make_unique<ShutdownCommandHandler>(appid_, running_);
        commandHandlers_[CommandType::STATS] = std::make_unique<StatsCommandHandler>(appid_, requestCount_);
    }

    ~LogServer() {
        dlt_log_info(appid_, "Shutting down LogServer");
        stop();
        dlt_free_client(appid_);
    }

    bool start() {
        dlt_log_info(appid_, "Starting server on port %d and socket %s", 
                    port_, unixSocketPath_.c_str());
        
        try {
            // 创建并绑定 TCP 套接字
            tcpSocket_ = socket(AF_INET, SOCK_STREAM, 0);
            if (tcpSocket_ < 0) {
                dlt_log_fatal(appid_, "Failed to create TCP socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to create TCP socket");
            }

            // 设置套接字选项
            int opt = 1;
            if (setsockopt(tcpSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                dlt_log_fatal(appid_, "Failed to set TCP socket options: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to set TCP socket options");
            }

            // 绑定地址
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port_);

            if (bind(tcpSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                dlt_log_fatal(appid_, "Failed to bind TCP socket on port %d: %s", 
                             port_, strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to bind TCP socket");
            }

            // 监听连接
            if (listen(tcpSocket_, 5) < 0) {
                dlt_log_fatal(appid_, "Failed to listen on TCP socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to listen on TCP socket");
            }

            dlt_log_info(appid_, "TCP server listening on port %d", port_);

            // 创建并绑定 Unix 域套接字
            unixSocket_ = socket(AF_UNIX, SOCK_STREAM, 0);
            if (unixSocket_ < 0) {
                dlt_log_fatal(appid_, "Failed to create Unix domain socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to create Unix domain socket");
            }

            // 删除可能存在的旧套接字文件
            if (unlink(unixSocketPath_.c_str()) == -1 && errno != ENOENT) {
                dlt_log_warn(appid_, "Failed to remove existing Unix domain socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to remove existing Unix domain socket");
            }

            // 绑定地址
            sockaddr_un unixAddr{};
            unixAddr.sun_family = AF_UNIX;
            strncpy(unixAddr.sun_path, unixSocketPath_.c_str(), sizeof(unixAddr.sun_path) - 1);

            if (bind(unixSocket_, reinterpret_cast<sockaddr*>(&unixAddr), sizeof(unixAddr)) < 0) {
                dlt_log_fatal(appid_, "Failed to bind Unix domain socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to bind Unix domain socket");
            }

            // 设置权限
            if (chmod(unixSocketPath_.c_str(), 0666) < 0) {
                dlt_log_warn(appid_, "Failed to set permissions on Unix domain socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to set permissions on Unix domain socket");
            }

            // 监听连接
            if (listen(unixSocket_, 5) < 0) {
                dlt_log_fatal(appid_, "Failed to listen on Unix domain socket: %s", strerror(errno));
                throw std::system_error(errno, std::system_category(), "Failed to listen on Unix domain socket");
            }

            dlt_log_info(appid_, "Unix domain server listening on %s", unixSocketPath_.c_str());
            std::cout << "Server started on port " << port_ 
                      << " and Unix socket " << unixSocketPath_ << std::endl;
            return true;
        } catch (const std::exception& e) {
            dlt_log_fatal(appid_, "Failed to start server: %s", e.what());
            std::cerr << "Failed to start server: " << e.what() << std::endl;
            stop();
            return false;
        }
    }

    void stop() {
        if (!running_.exchange(false)) {
            return; // 已经停止
        }

        dlt_log_info(appid_, "Stopping server...");

        if (tcpSocket_ >= 0) {
            dlt_log_info(appid_, "Closing TCP socket %d", tcpSocket_);
            close(tcpSocket_);
            tcpSocket_ = -1;
        }

        if (unixSocket_ >= 0) {
            dlt_log_info(appid_, "Closing Unix socket %d", unixSocket_);
            close(unixSocket_);
            unixSocket_ = -1;
        }

        // 删除 Unix 域套接字文件
        if (!unixSocketPath_.empty()) {
            dlt_log_info(appid_, "Removing Unix domain socket file %s", unixSocketPath_.c_str());
            unlink(unixSocketPath_.c_str());
        }
    }

    void run() {
        dlt_log_info(appid_, "Server entering main loop");
        
        fd_set readfds;
        int maxFd;

        while (running_) {
            // 重置文件描述符集
            FD_ZERO(&readfds);
            FD_SET(tcpSocket_, &readfds);
            FD_SET(unixSocket_, &readfds);
            maxFd = std::max(tcpSocket_, unixSocket_);

            // 等待连接（添加超时避免阻塞）
            timeval timeout{1, 0};  // 1秒超时
            int activity = select(maxFd + 1, &readfds, nullptr, nullptr, &timeout);
            
            if (activity < 0 && errno != EINTR) {
                dlt_log_fatal(appid_, "Select error: %s", strerror(errno));
                std::cerr << "Select error: " << strerror(errno) << std::endl;
                continue;
            }

            if (activity == 0) {
                // 超时，检查 running_ 状态
                continue;
            }

            // 处理 TCP 连接
            if (FD_ISSET(tcpSocket_, &readfds)) {
                dlt_log_info(appid_, "New incoming TCP connection");
                handleConnection(tcpSocket_);
            }

            // 处理 Unix 域套接字连接
            if (FD_ISSET(unixSocket_, &readfds)) {
                dlt_log_info(appid_, "New incoming Unix domain connection");
                handleConnection(unixSocket_);
            }
        }

        dlt_log_info(appid_, "Server exiting main loop");
    }

private:
    void handleConnection(int serverSocket) {
        sockaddr_storage clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);

        int clientSocket = accept(serverSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSocket < 0) {
            dlt_log_warn(appid_, "Accept error: %s", strerror(errno));
            std::cerr << "Accept error: " << strerror(errno) << std::endl;
            return;
        }

        // 打印客户端连接信息
        char clientIP[INET6_ADDRSTRLEN] = {0};
        void* addrPtr;
        uint16_t clientPort = 0;
        
        if (clientAddr.ss_family == AF_INET) {
            sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(&clientAddr);
            addrPtr = &(addr->sin_addr);
            clientPort = ntohs(addr->sin_port);
            inet_ntop(AF_INET, addrPtr, clientIP, INET_ADDRSTRLEN);
        } else if (clientAddr.ss_family == AF_UNIX) {
            strncpy(clientIP, "Unix Domain Socket", sizeof(clientIP) - 1);
        }
        
        dlt_log_info(appid_, "New connection from %s:%u (socket %d)", 
                    clientIP, clientPort, clientSocket);
        std::cout << "New connection from " << clientIP;
        if (clientPort != 0) std::cout << ":" << clientPort;
        std::cout << " (socket " << clientSocket << ")" << std::endl;

        // 处理客户端请求
        handleClient(clientSocket);
        
        dlt_log_info(appid_, "Closing client connection %d", clientSocket);
        close(clientSocket);
    }

    void handleClient(int clientSocket) {
        ClientConnection conn(clientSocket, appid_);
        Message req, resp;

        // 接收请求
        if (!conn.receiveMessage(req)) {
            dlt_log_warn(appid_, "Failed to receive message from client %d", clientSocket);
            std::cerr << "Error: Failed to receive message from client" << std::endl;
            return;
        }

        // 增加请求计数
        ++requestCount_;
        dlt_log_info(appid_, "Request count incremented to %lu", requestCount_);

        // 查找命令处理器
        auto it = commandHandlers_.find(req.header.cmd);
        if (it != commandHandlers_.end()) {
            // 处理命令
            dlt_log_info(appid_, "Processing command %d from client %d", 
                        req.header.cmd, clientSocket);
            it->second->handle(req, resp);
        } else {
            // 未知命令
            dlt_log_warn(appid_, "Unknown command %d from client %d", 
                        req.header.cmd, clientSocket);
            resp.header.cmd = CommandType::UNKNOWN;
            resp.header.seq = req.header.seq;
            resp.body = "ERROR: Unknown command";
            resp.header.bodySize = resp.body.size();
        }

        // 设置通用响应头字段
        resp.header.magic = 0x12345678;
        resp.header.version = 1;
        resp.header.headerSize = sizeof(MessageHeader);
        resp.header.timestamp = time(nullptr);

        // 发送响应
        if (!conn.sendMessage(resp)) {
            dlt_log_warn(appid_, "Failed to send response to client %d", clientSocket);
            std::cerr << "Error: Failed to send response to client" << std::endl;
        }
    }
};

// 信号处理
void signalHandler(int signum) {
    dlt_log_info("logser", "Received signal %d, shutting down...", signum);
    std::cout << "Received signal " << signum << ", shutting down..." << std::endl;
    exit(signum);
}

// 显示帮助信息
void printUsage() {
    std::cout << "Usage: logser [OPTIONS]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -p, --port <PORT>    Port to listen on (default: 8999)" << std::endl;
    std::cout << "  -u, --unix-socket <PATH> Unix domain socket path (default: /tmp/logser.sock)" << std::endl;
    std::cout << "  -h, --help           Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    // 初始化 DLT 日志客户端
    const char* appid = "logser";
    dlt_init_client(appid);
    dlt_log_info(appid, "Starting logser server");

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 默认参数
    int port = 8999;
    std::string unixSocketPath = "/tmp/logser.sock";

    // 解析命令行参数
    static struct option longOptions[] = {
        {"port", required_argument, 0, 'p'},
        {"unix-socket", required_argument, 0, 'u'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int optionIndex = 0;
    while ((opt = getopt_long(argc, argv, "p:u:h", longOptions, &optionIndex)) != -1) {
        switch (opt) {
            case 'p':
                port = std::stoi(optarg);
                dlt_log_info(appid, "Port set to %d via command line", port);
                break;
            case 'u':
                unixSocketPath = optarg;
                dlt_log_info(appid, "Unix socket path set to %s via command line", unixSocketPath.c_str());
                break;
            case 'h':
                printUsage();
                dlt_log_info(appid, "Help message displayed, exiting");
                return 0;
            default:
                dlt_log_fatal(appid, "Invalid command line argument");
                printUsage();
                return 1;
        }
    }

    // 创建并启动服务器
    LogServer server(port, unixSocketPath);
    if (server.start()) {
        dlt_log_info(appid, "Server started successfully, entering run loop");
        server.run();
        dlt_log_info(appid, "Server run loop exited normally");
    } else {
        dlt_log_fatal(appid, "Failed to start server, exiting");
    }

    dlt_log_info(appid, "Exiting main program");
    return 0;
}