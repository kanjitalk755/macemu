/*
 * HTTP Server Module
 *
 * Simple HTTP/1.1 server implementation
 */

#include "http_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <errno.h>

namespace http {

// Response implementation
Response::Response()
    : status_code_(200)
    , status_message_("OK")
    , content_type_("text/plain")
{}

void Response::set_status(int code, const std::string& message) {
    status_code_ = code;
    if (!message.empty()) {
        status_message_ = message;
    } else {
        // Default status messages
        switch (code) {
            case 200: status_message_ = "OK"; break;
            case 404: status_message_ = "Not Found"; break;
            case 500: status_message_ = "Internal Server Error"; break;
            default: status_message_ = "Unknown"; break;
        }
    }
}

void Response::set_content_type(const std::string& content_type) {
    content_type_ = content_type;
}

void Response::set_body(const std::string& body) {
    body_ = body;
}

void Response::add_header(const std::string& name, const std::string& value) {
    extra_headers_ += name + ": " + value + "\r\n";
}

std::string Response::build() const {
    std::string response = "HTTP/1.1 " + std::to_string(status_code_) + " " + status_message_ + "\r\n";
    response += "Content-Type: " + content_type_ + "\r\n";
    response += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    response += "Connection: close\r\n";
    if (!extra_headers_.empty()) {
        response += extra_headers_;
    }
    response += "\r\n";
    response += body_;
    return response;
}

Response Response::json(const std::string& json_body) {
    Response resp;
    resp.set_content_type("application/json");
    resp.set_body(json_body);
    return resp;
}

Response Response::text(const std::string& text) {
    Response resp;
    resp.set_content_type("text/plain");
    resp.set_body(text);
    return resp;
}

Response Response::html(const std::string& html) {
    Response resp;
    resp.set_content_type("text/html");
    resp.set_body(html);
    return resp;
}

Response Response::not_found() {
    Response resp;
    resp.set_status(404);
    resp.set_content_type("text/plain");
    resp.set_body("Not Found");
    return resp;
}

// Server implementation
Server::Server()
    : port_(0)
    , server_fd_(-1)
    , running_(false)
{}

Server::~Server() {
    stop();
}

bool Server::start(int port, RequestHandler handler) {
    if (running_) {
        fprintf(stderr, "HTTP: Server already running\n");
        return false;
    }

    port_ = port;
    handler_ = handler;

    // Create socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        fprintf(stderr, "HTTP: Failed to create socket\n");
        return false;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "HTTP: Warning: Failed to set SO_REUSEADDR: %s\n", strerror(errno));
    }

    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    if (flags < 0) {
        fprintf(stderr, "HTTP: Failed to get socket flags: %s\n", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    if (fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "HTTP: Failed to set non-blocking mode: %s\n", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Bind
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "HTTP: Failed to bind port %d: %s\n", port, strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Listen
    if (listen(server_fd_, 10) < 0) {
        fprintf(stderr, "HTTP: Failed to listen: %s\n", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Start thread
    running_ = true;
    thread_ = std::thread(&Server::run, this);

    fprintf(stderr, "HTTP: Server on port %d\n", port);
    return true;
}

void Server::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Server::run() {
    while (running_) {
        struct pollfd pfd;
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        handle_client(client_fd);
        close(client_fd);
    }
}

void Server::handle_client(int client_fd) {
    char buffer[8192];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    buffer[n] = '\0';

    Request req;
    if (!parse_request(buffer, n, req)) {
        // Bad request
        Response resp;
        resp.set_status(400, "Bad Request");
        resp.set_body("Bad Request");
        std::string response_str = resp.build();
        send(client_fd, response_str.c_str(), response_str.size(), 0);
        return;
    }

    // Call handler
    Response resp = handler_(req);
    std::string response_str = resp.build();
    send(client_fd, response_str.c_str(), response_str.size(), 0);
}

bool Server::parse_request(const char* buffer, size_t length, Request& req) {
    std::string request(buffer, length);

    // Parse request line: METHOD PATH HTTP/1.1
    size_t method_end = request.find(' ');
    if (method_end == std::string::npos) return false;

    req.method = request.substr(0, method_end);

    size_t path_start = method_end + 1;
    size_t path_end = request.find(' ', path_start);
    if (path_end == std::string::npos) return false;

    req.path = request.substr(path_start, path_end - path_start);

    // Strip query string from path
    size_t query_pos = req.path.find('?');
    if (query_pos != std::string::npos) {
        req.path = req.path.substr(0, query_pos);
    }

    // Extract body (after \r\n\r\n)
    size_t body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        req.body = request.substr(body_start + 4);
    }

    return true;
}

} // namespace http
