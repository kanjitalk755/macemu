/*
 * HTTP Server Module
 *
 * Simple HTTP/1.1 server for serving static files and JSON APIs
 * Non-blocking socket with polling, handles one request per connection
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <thread>
#include <atomic>
#include <functional>

namespace http {

/**
 * HTTP Request Information
 */
struct Request {
    std::string method;      // GET, POST, etc.
    std::string path;        // URL path without query string
    std::string body;        // Request body content
};

/**
 * HTTP Response Builder
 */
class Response {
public:
    Response();

    void set_status(int code, const std::string& message = "");
    void set_content_type(const std::string& content_type);
    void set_body(const std::string& body);
    void add_header(const std::string& name, const std::string& value);

    std::string build() const;

    // Convenience methods
    static Response json(const std::string& json_body);
    static Response text(const std::string& text);
    static Response html(const std::string& html);
    static Response not_found();

private:
    int status_code_;
    std::string status_message_;
    std::string content_type_;
    std::string body_;
    std::string extra_headers_;
};

/**
 * HTTP Server
 *
 * Listens on a port and handles HTTP requests.
 * Uses a callback for request routing/handling.
 */
class Server {
public:
    // Request handler callback: receives request, returns response
    using RequestHandler = std::function<Response(const Request&)>;

    Server();
    ~Server();

    // Start server on specified port
    bool start(int port, RequestHandler handler);

    // Stop server and wait for thread to join
    void stop();

    // Check if server is running
    bool is_running() const { return running_; }

private:
    void run();
    void handle_client(int client_fd);
    bool parse_request(const char* buffer, size_t length, Request& req);

    int port_;
    int server_fd_;
    std::atomic<bool> running_;
    std::thread thread_;
    RequestHandler handler_;
};

} // namespace http

#endif // HTTP_SERVER_H
