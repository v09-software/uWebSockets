#ifndef NODE_UWS_H
#define NODE_UWS_H

#include "Socket.h"
#include <vector>
#include <mutex>

namespace uS {

enum ListenOptions : int {
    REUSE_PORT = 1,
    ONLY_IPV4 = 2
};

class WIN32_EXPORT Node {
protected:
    uv_loop_t *loop;
    NodeData *nodeData;
    std::mutex asyncMutex;

public:
    Node(int recvLength = 1024, int prePadding = 0, int postPadding = 0, bool useDefaultLoop = false);
    ~Node();
    void run();

    uv_loop_t *getLoop() {
        return loop;
    }

    template <void C(Socket p, bool error)>
    static void connect_cb(uv_poll_t *p, int status, int events) {
        C(p, status < 0);
    }

    template <void C(Socket p, bool error)>
    uS::Socket connect(const char *hostname, int port, bool secure, uS::SocketData *socketData) {
        uv_poll_t *p = new uv_poll_t;
        p->data = socketData;

        addrinfo hints, *result;
        memset(&hints, 0, sizeof(addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostname, std::to_string(port).c_str(), &hints, &result) != 0) {
            C(p, true);
            delete p;
            return nullptr;
        }

        uv_os_sock_t fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (fd == -1) {
            C(p, true);
            delete p;
            return nullptr;
        }
        ::connect(fd, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);

        NodeData *nodeData = socketData->nodeData;
        if (secure) {
            socketData->ssl = SSL_new(nodeData->clientContext);
            SSL_set_fd(socketData->ssl, fd);
            SSL_set_connect_state(socketData->ssl);
            SSL_set_mode(socketData->ssl, SSL_MODE_RELEASE_BUFFERS);
        } else {
            socketData->ssl = nullptr;
        }

        socketData->poll = UV_READABLE;
        uv_poll_init_socket(loop, p, fd);
        uv_poll_start(p, UV_WRITABLE, connect_cb<C>);
        return p;
    }

    template <void A(Socket s)>
    static void accept_cb(uv_poll_t *p, int status, int events) {
        ListenData *listenData = (ListenData *) p->data;
        uv_os_sock_t serverFd = Socket(p).getFd();
        uv_os_sock_t clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd == INVALID_SOCKET) {
            return;
        }

    #ifdef __APPLE__
        int noSigpipe = 1;
        setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(int));
    #endif

        SSL *ssl = nullptr;
        if (listenData->sslContext) {
            ssl = SSL_new(listenData->sslContext.getNativeContext());
            SSL_set_fd(ssl, clientFd);
            SSL_set_accept_state(ssl);
            SSL_set_mode(ssl, SSL_MODE_RELEASE_BUFFERS);
        }

        SocketData *socketData = new SocketData(listenData->nodeData);
        socketData->ssl = ssl;

        uv_poll_t *clientPoll = new uv_poll_t;
        uv_poll_init_socket(listenData->listenPoll->loop, clientPoll, clientFd);
        clientPoll->data = socketData;

        socketData->poll = UV_READABLE;
        A(clientPoll);
    }

    template <void A(Socket s)>
    bool listen(int port, uS::TLS::Context sslContext, int options, uS::NodeData *nodeData, void *user) {
        addrinfo hints, *result;
        memset(&hints, 0, sizeof(addrinfo));

        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(nullptr, std::to_string(port).c_str(), &hints, &result)) {
            return true;
        }

        uv_os_sock_t listenFd = SOCKET_ERROR;
        addrinfo *listenAddr;
        if ((options & uS::ONLY_IPV4) == 0) {
            for (addrinfo *a = result; a && listenFd == SOCKET_ERROR; a = a->ai_next) {
                if (a->ai_family == AF_INET6) {
                    listenFd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                    listenAddr = a;
                }
            }
        }

        for (addrinfo *a = result; a && listenFd == SOCKET_ERROR; a = a->ai_next) {
            if (a->ai_family == AF_INET) {
                listenFd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                listenAddr = a;
            }
        }

        if (listenFd == SOCKET_ERROR) {
            freeaddrinfo(result);
            return true;
        }

#ifdef __linux
        if (options & REUSE_PORT) {
            int optval = 1;
            setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
        }
#endif

        int enabled = true;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        if (bind(listenFd, listenAddr->ai_addr, listenAddr->ai_addrlen) || ::listen(listenFd, 10)) {
            ::close(listenFd);
            freeaddrinfo(result);
            return true;
        }

        ListenData *listenData = new ListenData(nodeData);
        listenData->sslContext = sslContext;
        listenData->nodeData = nodeData;

        uv_poll_t *listenPoll = new uv_poll_t;
        listenPoll->data = listenData;

        listenData->listenPoll = listenPoll;
        listenData->ssl = nullptr;

        uv_poll_init_socket(loop, listenPoll, listenFd);
        uv_poll_start(listenPoll, UV_READABLE, accept_cb<A>);

        // should be vector of listen data! one group can have many listeners!
        nodeData->user = listenData;
        freeaddrinfo(result);
        return false;
    }
};

}

#endif // NODE_UWS_H
