#pragma once

#include <string>
#include <vector>

namespace chronos::paper_trading {

// Transport-independent HTTP messages. The routing and JSON contract are
// defined over these types so the HTTP library can be replaced without
// touching them.
struct HttpRequest {
    std::string method;
    // The decoded path without its query string.
    std::string path;
    // Empty when the request has no Content-Type header.
    std::string content_type;
    std::string body;
};

struct HttpHeader {
    std::string name;
    std::string value;
};

// Every response body is JSON.
struct HttpResponse {
    int status{200};
    std::vector<HttpHeader> headers;
    std::string body;
};

}  // namespace chronos::paper_trading
