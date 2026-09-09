// Diagnostic for SURVIVING #61. Requires a live TLS1.3 AES-128-GCM server and fails (never skips)
// unless that exact connection entered kTLS. No listener, worker, benchmark, or process is started.
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

void require(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
std::string read_exact(SSL* ssl, size_t size) {
    std::string result(size, '\0'); size_t done = 0;
    while (done < size) {
        const int n = SSL_read(ssl, result.data() + done, size - done);
        require(n > 0, "TLS read failed after a legal KeyUpdate"); done += n;
    }
    return result;
}
std::string read_line(SSL* ssl) {
    std::string line;
    while (!line.ends_with("\r\n")) line += read_exact(ssl, 1);
    return line;
}
void send(SSL* ssl, const std::string& request) {
    size_t done = 0;
    while (done < request.size()) {
        const int n = SSL_write(ssl, request.data() + done, request.size() - done);
        require(n > 0, "TLS write failed"); done += n;
    }
}
int main(int argc, char** argv) {
    require(argc == 3, "usage: ktls-keyupdate HOST PORT");
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method()); require(ctx, "client context");
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    require(SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256") == 1, "TLS1.3 cipher selected");
    const int fd = socket(AF_INET, SOCK_STREAM, 0); require(fd >= 0, "client socket");
    timeval timeout{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(std::atoi(argv[2]));
    require(inet_pton(AF_INET, argv[1], &address.sin_addr) == 1, "numeric host address");
    require(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
    SSL* ssl = SSL_new(ctx); require(ssl && SSL_set_fd(ssl, fd) == 1 && SSL_connect(ssl) == 1, "TLS handshake");
    send(ssl, "INFO STATS\r\n"); const auto header = read_line(ssl);
    require(header[0] == '$', "INFO bulk reply");
    const auto info = read_exact(ssl, std::stoull(header.substr(1)) + 2);
    require(info.find("tls_ktls_active:1\r\n") != std::string::npos,
            "the sole TLS connection must be promoted to kTLS");
    std::puts("armed: sole TLS1.3 AES-128-GCM connection is kTLS"); std::fflush(stdout);
    require(SSL_key_update(ssl, SSL_KEY_UPDATE_REQUESTED) == 1 && SSL_do_handshake(ssl) == 1,
            "legal KeyUpdate sent");
    send(ssl, "PING\r\n"); require(read_line(ssl) == "+PONG\r\n", "PING survives KeyUpdate");
    SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    std::puts("ok: kTLS KeyUpdate");
}
