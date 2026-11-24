#define _POSIX_C_SOURCE 200809L

#include "download.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

enum { buffer_size = 16384, url_capacity = 4096, max_redirects = 3 };
static const u64 operation_timeout = UINT64_C(20000000000);
static const u64 download_timeout = UINT64_C(900000000000);
static const u64 store_timeout = UINT64_C(25000000000);

typedef struct {
    atomic_uint *downloaded;
    const atomic_bool *cancel;
    StoreInstallError *error;
    unsigned limit;
    bool from_store;
    u64 deadline;
} Transfer;

typedef struct {
    SslContext context;
    SslConnection connection;
    bool context_open;
    bool connection_open;
    int socket;
} Tls;

typedef struct {
    int status;
    uint64_t content_length;
    bool has_content_length;
    bool transfer_encoding;
    bool chunked;
    bool transformed_body;
    char location[url_capacity];
} Response;

bool store_install_error(StoreInstallError *error, const char *english, const char *spanish, ...) {
    va_list arguments, english_arguments;
    va_start(arguments, spanish);
    va_copy(english_arguments, arguments);
    vsnprintf(error->english, sizeof(error->english), english, english_arguments);
    va_end(english_arguments);
    vsnprintf(error->spanish, sizeof(error->spanish), spanish, arguments);
    va_end(arguments);
    return false;
}

static bool may_continue(const Transfer *transfer, u64 operation_deadline) {
    if (atomic_load_explicit(transfer->cancel, memory_order_relaxed))
        return store_install_error(transfer->error, "Download cancelled.",
                                                    "Descarga cancelada.");
    const u64 now = armGetSystemTick();
    if (now >= transfer->deadline || now >= operation_deadline)
        return store_install_error(transfer->error, "The network connection timed out.",
                                                    "Tiempo de espera de la red agotado.");
    return true;
}

static u64 next_deadline(void) {
    return armGetSystemTick() + armNsToTicks(operation_timeout);
}

static bool parse_url(const char *url, bool from_store, char host[64], const char **path) {
    static const char scheme[] = "https://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0)
        return false;
    const char *authority = url + sizeof(scheme) - 1;
    *path = strchr(authority, '/');
    if (!*path || *path == authority || *path - authority >= 64)
        return false;
    const size_t length = (size_t)(*path - authority);
    memcpy(host, authority, length);
    host[length] = '\0';
    if (from_store ? strcmp(host, "switch2homebrew.com") != 0 :
        strcmp(host, "github.com") != 0 &&
        strcmp(host, "release-assets.githubusercontent.com") != 0 &&
        strcmp(host, "objects.githubusercontent.com") != 0 &&
        strcmp(host, "github-releases.githubusercontent.com") != 0 &&
        strcmp(host, "raw.githubusercontent.com") != 0 &&
        strcmp(host, "gitlab.com") != 0 &&
        strcmp(host, "codeberg.org") != 0 &&
        strcmp(host, "storage.googleapis.com") != 0)
        return false;
    for (const unsigned char *p = (const unsigned char *)*path; *p; ++p)
        if (*p <= 0x20 || *p >= 0x7f || *p == '#')
            return false;
    return true;
}

bool download_package_url_allowed(const char *url) {
    char host[64];
    const char *path;
    return url && strlen(url) < url_capacity && parse_url(url, false, host, &path);
}

static int connect_tcp(const Transfer *transfer, const char *host) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *addresses = NULL;
    const u64 deadline = next_deadline();
    if (getaddrinfo(host, "443", &hints, &addresses) != 0) {
        store_install_error(transfer->error, "DNS lookup for %s failed.",
                                             "Sin respuesta DNS de %s.", host);
        return -1;
    }

    int connected_socket = -1;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (!may_continue(transfer, deadline))
            break;
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected_socket = fd;
        } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            struct pollfd wait = { .fd = fd, .events = POLLOUT };
            while (may_continue(transfer, deadline)) {
                const int ready = poll(&wait, 1, 200);
                if (ready < 0 && errno != EINTR)
                    break;
                if (ready > 0) {
                    int socket_error = 0;
                    socklen_t length = sizeof(socket_error);
                    if ((wait.revents & POLLOUT) &&
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                        socket_error == 0)
                        connected_socket = fd;
                    break;
                }
            }
        }
        if (connected_socket >= 0)
            break;
        close(fd);
    }
    freeaddrinfo(addresses);
    if (connected_socket < 0 && may_continue(transfer, deadline))
        store_install_error(transfer->error, "Could not connect to %s.",
                                             "No se pudo conectar con %s.", host);
    return connected_socket;
}

static void tls_close(Tls *tls) {
    if (tls->connection_open)
        sslConnectionClose(&tls->connection);
    if (tls->context_open)
        sslContextClose(&tls->context);
    if (tls->socket >= 0)
        close(tls->socket);
}

static bool retry_tls(Result result) {
    return R_VALUE(result) == MAKERESULT(123, 204);
}

static bool tls_open(Tls *tls, const Transfer *transfer, const char *host) {
    tls->socket = connect_tcp(transfer, host);
    if (tls->socket < 0)
        return false;

    Result result = sslCreateContext(&tls->context, SslVersion_TlsV12);
    if (R_FAILED(result))
        return store_install_error(transfer->error, "TLS context error (0x%08lx).",
                                                    "Error de contexto TLS (0x%08lx).", (unsigned long)result);
    tls->context_open = true;
    result = sslContextCreateConnection(&tls->context, &tls->connection);
    if (R_FAILED(result))
        return store_install_error(transfer->error, "TLS connection error (0x%08lx).",
                                                    "Error de conexión TLS (0x%08lx).", (unsigned long)result);
    tls->connection_open = true;

    result = sslConnectionSetOption(&tls->connection, SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(result))
        return store_install_error(transfer->error, "TLS socket error (0x%08lx).",
                                                    "Error de socket TLS (0x%08lx).", (unsigned long)result);
    if (socketSslConnectionSetSocketDescriptor(&tls->connection, tls->socket) < 0 && errno != ENOENT)
        return store_install_error(transfer->error, "Could not set up TLS (0x%08lx).",
                                                    "Error al preparar TLS (0x%08lx).", (unsigned long)socketGetLastResult());
    result = sslConnectionSetHostName(&tls->connection, host, (u32)strlen(host) + 1);
    if (R_SUCCEEDED(result))
        result = sslConnectionSetVerifyOption(&tls->connection,
                    SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck);
    if (R_SUCCEEDED(result))
        result = sslConnectionSetIoMode(&tls->connection, SslIoMode_NonBlocking);
    if (R_FAILED(result))
        return store_install_error(transfer->error, "Could not configure TLS (0x%08lx).",
                                                    "Error al configurar TLS (0x%08lx).", (unsigned long)result);

    const u64 deadline = next_deadline();
    while (may_continue(transfer, deadline)) {
        result = sslConnectionDoHandshake(&tls->connection, NULL, NULL, NULL, 0);
        if (R_SUCCEEDED(result))
            return true;
        if (!retry_tls(result))
            return store_install_error(transfer->error,
                "HTTPS failed for %s (0x%08lx).",
                "Fallo HTTPS en %s (0x%08lx).", host, (unsigned long)result);
        svcSleepThread(20000000);
    }
    return false;
}

static bool send_request(Tls *tls, const Transfer *transfer, const char *host, const char *path) {
    char request[url_capacity + 256];
    const int size = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Switch2Homebrew-Store/0.3\r\n"
        "Accept: %s\r\nAccept-Encoding: identity\r\n%sConnection: close\r\n\r\n", path, host,
        transfer->from_store ? "*/*" : "application/octet-stream",
        transfer->from_store ? "Cache-Control: no-cache\r\n" : "");
    if (size < 0 || (size_t)size >= sizeof(request))
        return store_install_error(transfer->error, "The download URL is too long.",
                                                    "La URL de descarga es demasiado larga.");

    const u64 deadline = next_deadline();
    size_t position = 0;
    while (position < (size_t)size && may_continue(transfer, deadline)) {
        u32 sent = 0;
        const Result result = sslConnectionWrite(&tls->connection, request + position,
                                                  (u32)((size_t)size - position), &sent);
        if (R_SUCCEEDED(result) && sent > 0) {
            position += sent;
        } else if (retry_tls(result)) {
            svcSleepThread(10000000);
        } else {
            return store_install_error(transfer->error,
                "Could not send the HTTPS request (0x%08lx).",
                "Fallo al enviar la petición HTTPS (0x%08lx).", (unsigned long)result);
        }
    }
    return position == (size_t)size;
}

static bool receive(Tls *tls, const Transfer *transfer, char *buffer, u32 size, u32 *received) {
    const u64 deadline = next_deadline();
    while (may_continue(transfer, deadline)) {
        const Result result = sslConnectionRead(&tls->connection, buffer, size, received);
        if (R_SUCCEEDED(result))
            return true;
        if (!retry_tls(result))
            return store_install_error(transfer->error,
                "Could not receive HTTPS data (0x%08lx).",
                "Fallo al recibir datos HTTPS (0x%08lx).", (unsigned long)result);
        svcSleepThread(10000000);
    }
    return false;
}

static bool parse_headers(char *header, Response *response) {
    char *end = strstr(header, "\r\n");
    if (!end || end - header < 12 ||
        (strncmp(header, "HTTP/1.1 ", 9) != 0 && strncmp(header, "HTTP/1.0 ", 9) != 0))
        return false;
    for (int i = 9; i < 12; ++i) {
        if (header[i] < '0' || header[i] > '9')
            return false;
        response->status = response->status * 10 + header[i] - '0';
    }

    for (char *line = end + 2; *line; line = end + 2) {
        end = strstr(line, "\r\n");
        if (!end)
            return false;
        *end = '\0';
        char *value = strchr(line, ':');
        if (!value)
            return false;
        *value++ = '\0';
        while (*value == ' ' || *value == '\t')
            ++value;
        char *last = value + strlen(value);
        while (last > value && (last[-1] == ' ' || last[-1] == '\t'))
            *--last = '\0';

        if (strcasecmp(line, "Content-Length") == 0) {
            char *tail;
            errno = 0;
            const unsigned long long length = strtoull(value, &tail, 10);
            if (*value < '0' || *value > '9' || *tail || errno || response->has_content_length)
                return false;
            response->has_content_length = true;
            response->content_length = length;
        } else if (strcasecmp(line, "Location") == 0) {
            if (!*value || response->location[0] || last - value >= url_capacity)
                return false;
            memcpy(response->location, value, (size_t)(last - value) + 1);
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (response->transfer_encoding)
                return false;
            response->transfer_encoding = true;
            response->chunked = strcasecmp(value, "chunked") == 0;
        } else if (strcasecmp(line, "Content-Encoding") == 0 && strcasecmp(value, "identity") != 0) {
            response->transformed_body = true;
        }
    }
    return true;
}

static bool read_headers(Tls *tls, const Transfer *transfer, char *buffer, Response *response,
                         size_t *total, size_t *body) {
    *total = 0;
    while (*total < buffer_size) {
        u32 received = 0;
        if (!receive(tls, transfer, buffer + *total, (u32)(buffer_size - *total), &received))
            return false;
        if (!received)
            return store_install_error(transfer->error,
                "The server closed the connection before sending headers.",
                "El servidor cerró la conexión antes de enviar las cabeceras.");
        *total += received;
        buffer[*total] = '\0';
        char *boundary = strstr(buffer, "\r\n\r\n");
        if (boundary) {
            *body = (size_t)(boundary - buffer) + 4;
            boundary[2] = '\0';
            return parse_headers(buffer, response) || store_install_error(transfer->error,
                "Invalid HTTP response.",
                "Respuesta HTTP no válida.");
        }
    }
    return store_install_error(transfer->error, "The HTTP header is too long.",
                                                "Cabecera HTTP demasiado larga.");
}

static bool write_body(Tls *tls, const Transfer *transfer, FILE *out, char *buffer,
                       size_t position, size_t available, unsigned expected_size) {
    const unsigned limit = expected_size ? expected_size : transfer->limit;
    unsigned written = 0;
    for (;;) {
        if (!may_continue(transfer, transfer->deadline))
            return false;
        if (available == 0) {
            u32 received = 0;
            if (!receive(tls, transfer, buffer, buffer_size, &received))
                return false;
            if (!received)
                return (!expected_size && written > 0) || store_install_error(transfer->error,
                    "The download is incomplete.",
                    "La descarga está incompleta.");
            available = received;
            position = 0;
        }
        if (available > limit - written)
            return store_install_error(transfer->error,
                "The download exceeds the allowed size.",
                "La descarga supera el tamaño permitido.");
        if (fwrite(buffer + position, 1, available, out) != available)
            return store_install_error(transfer->error,
                "Could not write the download to the microSD card.",
                "Error al escribir la descarga en la microSD.");
        written += (unsigned)available;
        atomic_store_explicit(transfer->downloaded, written, memory_order_relaxed);
        if (expected_size && written == expected_size)
            return true;
        available = 0;
    }
}

typedef struct {
    Tls *tls;
    const Transfer *transfer;
    char *buffer;
    size_t position, end;
} ChunkReader;

static bool next_chunk_bytes(ChunkReader *reader) {
    if (reader->position < reader->end)
        return true;
    u32 received = 0;
    if (!receive(reader->tls, reader->transfer, reader->buffer, buffer_size, &received))
        return false;
    if (!received)
        return store_install_error(reader->transfer->error, "The download is incomplete.",
                                   "La descarga está incompleta.");
    reader->position = 0;
    reader->end = received;
    return true;
}

static bool bad_chunked_body(const Transfer *transfer) {
    return store_install_error(transfer->error, "The server sent an invalid chunked response.",
                               "El servidor envió una respuesta fragmentada no válida.");
}

static bool read_chunk_line(ChunkReader *reader, char line[128]) {
    for (size_t length = 0; length < 128; ++length) {
        if (!next_chunk_bytes(reader))
            return false;
        const unsigned char value = (unsigned char)reader->buffer[reader->position++];
        if (value == '\r') {
            if (!next_chunk_bytes(reader))
                return false;
            if (reader->buffer[reader->position++] != '\n')
                return bad_chunked_body(reader->transfer);
            line[length] = '\0';
            return true;
        }
        if ((value < 0x20 && value != '\t') || value >= 0x7f)
            return bad_chunked_body(reader->transfer);
        line[length] = (char)value;
    }
    return bad_chunked_body(reader->transfer);
}

static bool parse_chunk_size(const char *text, unsigned max, unsigned *size) {
    unsigned length = 0;
    const char *start = text;
    for (; *text && *text != ';'; ++text) {
        unsigned digit;
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A' + 10);
        else
            return false;
        if (digit > max || length > (max - digit) / 16)
            return false;
        length = length * 16 + digit;
    }
    if (text == start)
        return false;
    *size = length;
    return true;
}

static bool write_chunked_body(Tls *tls, const Transfer *transfer, FILE *out, char *buffer,
                               size_t position, size_t end) {
    ChunkReader reader = { tls, transfer, buffer, position, end };
    unsigned written = 0;
    char line[128];
    for (;;) {
        if (!may_continue(transfer, transfer->deadline) || !read_chunk_line(&reader, line))
            return false;
        unsigned remaining;
        if (!parse_chunk_size(line, transfer->limit - written, &remaining))
            return bad_chunked_body(transfer);
        if (!remaining)
            break;
        while (remaining > 0) {
            if (!may_continue(transfer, transfer->deadline) || !next_chunk_bytes(&reader))
                return false;
            const size_t available = reader.end - reader.position;
            const size_t count = available < remaining ? available : remaining;
            if (fwrite(buffer + reader.position, 1, count, out) != count)
                return store_install_error(transfer->error, "Could not save the store response.",
                                           "No se pudo guardar la respuesta de la tienda.");
            reader.position += count;
            remaining -= (unsigned)count;
            written += (unsigned)count;
            atomic_store_explicit(transfer->downloaded, written, memory_order_relaxed);
        }
        if (!read_chunk_line(&reader, line))
            return false;
        if (line[0])
            return bad_chunked_body(transfer);
    }
    for (unsigned trailers = 0; trailers < 32; ++trailers) {
        if (!may_continue(transfer, transfer->deadline) || !read_chunk_line(&reader, line))
            return false;
        if (!line[0])
            return written != 0 || bad_chunked_body(transfer);
        if (!strchr(line, ':'))
            return bad_chunked_body(transfer);
    }
    return bad_chunked_body(transfer);
}

static bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool download_https(FILE *out, const char *source_url, uint64_t expected_size, unsigned limit,
                           atomic_uint *downloaded, atomic_uint *total_size, const atomic_bool *cancel,
                           bool from_store, int *http_status, StoreInstallError *error) {
    Transfer transfer = {
        .downloaded = downloaded, .cancel = cancel, .error = error, .limit = limit, .from_store = from_store,
        .deadline = armGetSystemTick() + armNsToTicks(from_store ? store_timeout : download_timeout),
    };
    if (http_status)
        *http_status = 0;
    if (!source_url || strlen(source_url) >= url_capacity || !limit || expected_size > limit)
        return store_install_error(transfer.error, "The download exceeds the store's limits.",
                                                   "La descarga supera los límites de la tienda.");
    if (!may_continue(&transfer, transfer.deadline))
        return false;

    char *buffer = malloc(buffer_size + 1);
    if (!buffer)
        return store_install_error(transfer.error, "There is not enough memory for the download.",
                                                   "Memoria insuficiente para la descarga.");

    bool success = false;
    const Result socket_result = socketInitializeDefault();
    if (R_FAILED(socket_result)) {
        free(buffer);
        return store_install_error(transfer.error, "Could not start networking (0x%08lx).",
                                                   "No se pudo iniciar la red (0x%08lx).", (unsigned long)socket_result);
    }
    const Result ssl_result = sslInitialize(3);
    if (R_FAILED(ssl_result)) {
        socketExit();
        free(buffer);
        return store_install_error(transfer.error, "Could not start HTTPS (0x%08lx).",
                                                   "No se pudo iniciar HTTPS (0x%08lx).", (unsigned long)ssl_result);
    }

    char url[url_capacity];
    strcpy(url, source_url);
    for (unsigned redirects = 0; redirects <= max_redirects; ++redirects) {
        char host[64];
        const char *path;
        if (!parse_url(url, from_store, host, &path)) {
            store_install_error(transfer.error, "The HTTPS redirect is not allowed.",
                                                "Redirección HTTPS no permitida.");
            break;
        }
        Tls tls = { .socket = -1 };
        Response response = { 0 };
        size_t total, body;
        const bool received_headers = tls_open(&tls, &transfer, host) &&
            send_request(&tls, &transfer, host, path) &&
            read_headers(&tls, &transfer, buffer, &response, &total, &body);
        if (!received_headers) {
            tls_close(&tls);
            break;
        }
        if (http_status)
            *http_status = response.status;
        if (!from_store && is_redirect(response.status)) {
            tls_close(&tls);
            if (!response.location[0] || redirects == max_redirects) {
                store_install_error(transfer.error, "The server returned an incomplete redirect.",
                                                    "El servidor devolvió una redirección incompleta.");
                break;
            }
            if (response.location[0] == '/' && response.location[1] != '/') {
                const int length = snprintf(url, sizeof(url), "https://%s%s", host, response.location);
                if (length < 0 || (size_t)length >= sizeof(url)) {
                    store_install_error(transfer.error,
                        "The redirect is too long.",
                        "La redirección es demasiado larga.");
                    break;
                }
            } else {
                strcpy(url, response.location);
            }
            continue;
        }
        if (response.status != 200) {
            store_install_error(transfer.error, "The download returned HTTP %d.",
                                                "La descarga devolvió HTTP %d.", response.status);
        } else if ((response.transfer_encoding &&
                    (!from_store || !response.chunked || response.has_content_length)) ||
                   response.transformed_body || (response.has_content_length && (!response.content_length ||
                   response.content_length > limit ||
                   (expected_size && response.content_length != expected_size)))) {
            store_install_error(transfer.error, "The server returned an unexpected file size or format.",
                                                "El servidor devolvió un archivo de tamaño o formato inesperado.");
        } else {
            const unsigned size = (unsigned)(response.has_content_length ? response.content_length : expected_size);
            atomic_store_explicit(total_size, size, memory_order_relaxed);
            success = response.chunked
                ? write_chunked_body(&tls, &transfer, out, buffer, body, total)
                : write_body(&tls, &transfer, out, buffer, body, total - body, size);
            if (success && size == 0)
                atomic_store_explicit(total_size, atomic_load_explicit(downloaded, memory_order_relaxed),
                                      memory_order_relaxed);
        }
        tls_close(&tls);
        break;
    }
    sslExit();
    socketExit();
    free(buffer);
    return success;
}

bool download_asset(FILE *out, const char *url, uint64_t expected_size, atomic_uint *downloaded,
                    atomic_uint *total, const atomic_bool *cancel, StoreInstallError *error) {
    return download_https(out, url, expected_size, STORE_DOWNLOAD_LIMIT, downloaded, total, cancel,
                          false, NULL, error);
}

bool download_store_resource(FILE *out, const char *path, unsigned limit, const atomic_bool *cancel,
                             int *http_status, StoreInstallError *error) {
    if (http_status)
        *http_status = 0;
    char url[url_capacity];
    if (!path || path[0] != '/' || path[1] == '/' ||
        strlen(path) >= sizeof(url) - sizeof(STORE_WEB_ORIGIN))
        return store_install_error(error, "The store URL is invalid.", "La URL de la tienda no es válida.");
    snprintf(url, sizeof(url), "%s%s", STORE_WEB_ORIGIN, path);
    atomic_uint downloaded = 0, total = 0;
    return download_https(out, url, 0, limit, &downloaded, &total, cancel, true, http_status, error);
}
