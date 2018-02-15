#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../include/socket_p.h"

#ifndef SOCK_NONBLOCK
# define SOCK_NONBLOCK O_NONBLOCK
#endif

QTNETWORKNG_NAMESPACE_BEGIN

union qt_sockaddr {
    sockaddr a;
    sockaddr_in a4;
    sockaddr_in6 a6;
};

static void qt_ignore_sigpipe()
{
    // Set to ignore SIGPIPE once only.
    static QBasicAtomicInt atom = Q_BASIC_ATOMIC_INITIALIZER(0);
    if (!atom.load()) {
        // More than one thread could turn off SIGPIPE at the same time
        // But that's acceptable because they all would be doing the same
        // action
        struct sigaction noaction;
        memset(&noaction, 0, sizeof(noaction));
        noaction.sa_handler = SIG_IGN;
        ::sigaction(SIGPIPE, &noaction, 0);
        atom.store(1);
    }
}

static inline void qt_socket_getPortAndAddress(const qt_sockaddr *s, quint16 *port, QHostAddress *addr)
{
    if (s->a.sa_family == AF_INET6) {
        Q_IPV6ADDR tmp;
        memcpy(&tmp, &s->a6.sin6_addr, sizeof(tmp));
        if (addr) {
            QHostAddress tmpAddress;
            tmpAddress.setAddress(tmp);
            *addr = tmpAddress;
            if (s->a6.sin6_scope_id) {
                char scopeid[IFNAMSIZ];
                if (::if_indextoname(s->a6.sin6_scope_id, scopeid)) {
                    addr->setScopeId(QLatin1String(scopeid));
                } else {
                    addr->setScopeId(QString::number(s->a6.sin6_scope_id));
                }
            }
        }
        if (port)
            *port = ntohs(s->a6.sin6_port);
    } else if(s->a.sa_family == AF_INET) {
        if (addr) {
            QHostAddress tmpAddress;
            tmpAddress.setAddress(ntohl(s->a4.sin_addr.s_addr));
            *addr = tmpAddress;
        }
        if (port)
            *port = ntohs(s->a4.sin_port);
    } else {
        qFatal("qt_socket_getPortAndAddress() can only handle AF_INET6 and AF_INET.");
    }
}

bool QSocketPrivate::createSocket()
{
    qt_ignore_sigpipe();
    int flags = SOCK_NONBLOCK ; //| SOCK_CLOEXEC
    int family = AF_INET;
    if(protocol == QSocket::IPv6Protocol || protocol == QSocket::AnyIPProtocol) {
        family = AF_INET6;
    }
    if(type == QSocket::TcpSocket)
        flags = SOCK_STREAM | flags;
    else
        flags = SOCK_DGRAM | flags;
    fd = socket(family, flags, 0);
    if(fd < 0 && protocol == QSocket::AnyIPProtocol && errno == EAFNOSUPPORT) {
        fd = socket(AF_INET, flags, 0);
        this->protocol = QSocket::IPv4Protocol;
    }
    if(fd < 0) {
        int ecopy = errno;
        switch(ecopy) {
        case EPROTONOSUPPORT:
        case EAFNOSUPPORT:
        case EINVAL:
            setError(QSocket::UnsupportedSocketOperationError, ProtocolUnsupportedErrorString);
            break;
        case ENFILE:
        case EMFILE:
        case ENOBUFS:
        case ENOMEM:
            setError(QSocket::SocketResourceError, ResourceErrorString);
            break;
        case EACCES:
            setError(QSocket::SocketAccessError, AccessErrorString);
            break;
        default:
            break;
        }
    }
    return fd > 0;
}

inline uint scopeIdFromString(const QString scopeId)
{
    if(scopeId.isEmpty())
        return 0;
    bool ok;
    uint id = scopeId.toUInt(&ok);
    if(!ok)
        id = if_nametoindex(scopeId.toLatin1());
    return id;
}

namespace {
namespace SetSALen {
    template <typename T> void set(T *sa, typename QtPrivate::QEnableIf<(&T::sa_len, true), QT_SOCKLEN_T>::Type len)
    { sa->sa_len = len; }
    template <typename T> void set(T *sin6, typename QtPrivate::QEnableIf<(&T::sin6_len, true), QT_SOCKLEN_T>::Type len)
    { sin6->sin6_len = len; }
    template <typename T> void set(T *, ...) {}
}
}

void QSocketPrivate::setPortAndAddress(quint16 port, const QHostAddress &address, qt_sockaddr *aa, QT_SOCKLEN_T *sockAddrSize)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol
        || address.protocol() == QAbstractSocket::AnyIPProtocol
        || protocol == QSocket::IPv6Protocol
        || protocol == QSocket::AnyIPProtocol) {
        memset(&aa->a6, 0, sizeof(sockaddr_in6));
        aa->a6.sin6_family = AF_INET6;
        aa->a6.sin6_scope_id = scopeIdFromString(address.scopeId());
        aa->a6.sin6_port = htons(port);
        Q_IPV6ADDR tmp = address.toIPv6Address();
        memcpy(&aa->a6.sin6_addr, &tmp, sizeof(tmp));
        *sockAddrSize = sizeof(sockaddr_in6);
        SetSALen::set(&aa->a, sizeof(sockaddr_in6));
    } else {
        memset(&aa->a, 0, sizeof(sockaddr_in));
        aa->a4.sin_family = AF_INET;
        aa->a4.sin_port = htons(port);
        aa->a4.sin_addr.s_addr = htonl(address.toIPv4Address());
        *sockAddrSize = sizeof(sockaddr_in);
        SetSALen::set(&aa->a, sizeof(sockaddr_in));
    }
}

bool QSocketPrivate::bind(const QHostAddress &address, quint16 port, QSocket::BindMode mode)
{
    if(!isValid())
        return false;
    if(state != QSocket::UnconnectedState)
        return false;
    qt_sockaddr aa;
    QT_SOCKLEN_T sockAddrSize;
    setPortAndAddress(port, address, &aa, &sockAddrSize);

    if(mode & QSocket::ReuseAddressHint) {
        setOption(QSocket::AddressReusable, true);
    }
#ifdef IPV6_V6ONLY
    if (aa.a.sa_family == AF_INET6) {
        int ipv6only = 0;
        if (address.protocol() == QAbstractSocket::IPv6Protocol)
            ipv6only = 1;
        //default value of this socket option varies depending on unix variant (or system configuration on BSD), so always set it explicitly
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&ipv6only, sizeof(ipv6only) );
    }
#endif

    int bindResult = ::bind(fd, &aa.a, sockAddrSize);
    if (bindResult < 0 && errno == EAFNOSUPPORT && address.protocol() == QAbstractSocket::AnyIPProtocol)
    {
        // retry with v4
        aa.a4.sin_family = AF_INET;
        aa.a4.sin_port = htons(port);
        aa.a4.sin_addr.s_addr = htonl(address.toIPv4Address());
        sockAddrSize = sizeof(aa.a4);
        bindResult = ::bind(fd, &aa.a, sockAddrSize);
    }
    if (bindResult < 0)
    {
        switch(errno)
        {
        case EADDRINUSE:
            setError(QSocket::AddressInUseError, AddressInuseErrorString);
            break;
        case EACCES:
            setError(QSocket::SocketAccessError, AddressProtectedErrorString);
            break;
        case EINVAL:
            setError(QSocket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
            break;
        case EADDRNOTAVAIL:
            setError(QSocket::SocketAddressNotAvailableError,AddressNotAvailableErrorString);
            break;
        default:
            setError(QSocket::UnknownSocketError, UnknownSocketErrorString);
            break;
        }
        return false;
    }
    state = QSocket::BoundState;
    return true;
}


bool QSocketPrivate::connect(const QHostAddress &address, quint16 port)
{
    if(!isValid())
        return false;
    if(state != QSocket::UnconnectedState && state != QSocket::BoundState && state != QSocket::ConnectingState)
        return false;
    qt_sockaddr aa;
    QT_SOCKLEN_T sockAddrSize;
    setPortAndAddress(port, address, &aa, &sockAddrSize);
    state = QSocket::ConnectingState;
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
    while(true)
    {
        if(!isValid())
            return false;
        if(state != QSocket::ConnectingState)
            return false;
        int result;
        do {
            result = ::connect(fd, &aa.a, sockAddrSize);
        } while(result < 0 && errno == EINTR);
        if(result >= 0)
        {
            state = QSocket::ConnectedState;
            fetchConnectionParameters();
            return true;
        }
        int t = errno;
        switch (t) {
        case EISCONN:
            state = QSocket::ConnectedState;
            fetchConnectionParameters();
            return true;
        case EINPROGRESS:
        case EALREADY:
        case EAGAIN:
            break;

        case ECONNREFUSED:
        case EINVAL:
            setError(QSocket::ConnectionRefusedError, ConnectionRefusedErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case ETIMEDOUT:
            setError(QSocket::NetworkError, ConnectionTimeOutErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case EHOSTUNREACH:
            setError(QSocket::NetworkError, HostUnreachableErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case ENETUNREACH:
            setError(QSocket::NetworkError, NetworkUnreachableErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case EADDRINUSE:
            setError(QSocket::NetworkError, AddressInuseErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case EADDRNOTAVAIL:
            setError(QSocket::NetworkError, UnknownSocketErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case EACCES:
        case EPERM:
            setError(QSocket::SocketAccessError, AccessErrorString);
            state = QSocket::UnconnectedState;
            return false;
        case EAFNOSUPPORT:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            fd = -1;
            setError(QSocket::UnsupportedSocketOperationError, UnknownSocketErrorString);
            state = QSocket::UnconnectedState;
            return false;
        default:
            qDebug() << t << strerror(t);
            setError(QSocket::UnknownSocketError, UnknownSocketErrorString);
            state = QSocket::UnconnectedState;
            return false;
        }
        watcher.start();
    }
}



bool QSocketPrivate::close()
{
    if(fd > 0)
    {
        ::close(fd);
        EventLoopCoroutine::get()->triggerIoWatchers(fd);
        fd = -1;
    }
    state = QSocket::UnconnectedState;
    localAddress.clear();
    localPort = 0;
    peerAddress.clear();
    peerPort = 0;
    return true;
}

bool QSocketPrivate::listen(int backlog)
{
    if(!isValid())
        return false;
    if(state != QSocket::BoundState && state != QSocket::UnconnectedState)
        return false;

    if (::listen(fd, backlog) < 0) {
        switch (errno) {
        case EADDRINUSE:
            setError(QSocket::AddressInUseError, PortInuseErrorString);
            break;
        default:
            setError(QSocket::UnknownSocketError, UnknownSocketErrorString);
            break;
        }
        return false;
    }
    state = QSocket::ListeningState;
    fetchConnectionParameters();
    return true;
}


bool QSocketPrivate::fetchConnectionParameters()
{
    localPort = 0;
    localAddress.clear();
    peerPort = 0;
    peerAddress.clear();

    if (fd == -1)
        return false;

    qt_sockaddr sa;
    QT_SOCKLEN_T sockAddrSize = sizeof(sa);

    // Determine local address
    memset(&sa, 0, sizeof(sa));
    if (::getsockname(fd, &sa.a, &sockAddrSize) == 0)
    {
        qt_socket_getPortAndAddress(&sa, &localPort, &localAddress);

        // Determine protocol family
        switch (sa.a.sa_family)
        {
        case AF_INET:
            protocol = QSocket::IPv4Protocol;
            break;
        case AF_INET6:
            protocol = QSocket::IPv6Protocol;
            break;
        default:
            protocol = QSocket::UnknownNetworkLayerProtocol;
            break;
        }
    }
    else if (errno == EBADF)
    {
        setError(QSocket::UnsupportedSocketOperationError, InvalidSocketErrorString);
        return false;
    }

#if defined (IPV6_V6ONLY)
    // determine if local address is dual mode
    // On linux, these are returned as "::" (==AnyIPv6)
    // On OSX, these are returned as "::FFFF:0.0.0.0" (==AnyIPv4)
    // in either case, the IPV6_V6ONLY option is cleared
    int ipv6only = 0;
    socklen_t optlen = sizeof(ipv6only);
    if (protocol == QSocket::IPv6Protocol
        && (localAddress == QHostAddress::AnyIPv4 || localAddress == QHostAddress::AnyIPv6)
        && !getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&ipv6only, &optlen )) {
            if (optlen != sizeof(ipv6only))
                qWarning("unexpected size of IPV6_V6ONLY socket option");
            if (!ipv6only) {
                protocol = QSocket::AnyIPProtocol;
                localAddress = QHostAddress::Any;
            }
    }
#endif

    // Determine the remote address
    if (!::getpeername(fd, &sa.a, &sockAddrSize))
        qt_socket_getPortAndAddress(&sa, &peerPort, &peerAddress);

    // Determine the socket type (UDP/TCP)
    int value = 0;
    socklen_t valueSize = sizeof(int);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &valueSize) == 0)
    {
        if (value == SOCK_STREAM)
            type = QSocket::TcpSocket;
        else if (value == SOCK_DGRAM)
            type = QSocket::UdpSocket;
        else
            type = QSocket::UnknownSocketType;
    }
    return true;
}


qint64 QSocketPrivate::recv(char *data, qint64 size, bool all)
{
    if(!isValid()) {
        return -1;
    }
    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    qint64 total = 0;
    while(total < size) {
        if(!isValid()) {
            setError(QSocket::SocketAccessError, AccessErrorString);
            return total == 0 ? -1: total;
        }
        if(type == QSocket::TcpSocket) {
            if(state != QSocket::ConnectedState) {
                setError(QSocket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
                return total == 0 ? -1 : total;
            }
        } else if(type == QSocket::UdpSocket) {
            if(state != QSocket::UnconnectedState && state != QSocket::BoundState) {
                setError(QSocket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
                return total == 0 ? -1 : total;
            }
        } else {
            setError(QSocket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
            return total == 0 ? -1 : total;
        }

        ssize_t r = 0;
        do {
            r = ::recv(fd, data + total, size - total, 0);
        } while(r < 0 && errno == EINTR);

        if (r < 0) {
            switch (errno) {
#if EWOULDBLOCK-0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case ECONNRESET:
#if defined(Q_OS_VXWORKS)
            case ESHUTDOWN:
#endif
                if(type == QSocket::TcpSocket)
                {
                    setError(QSocket::RemoteHostClosedError, RemoteHostClosedErrorString);
                    close();
                }
                return total;
            case EBADF:
            case EINVAL:
            case EIO:
            default:
                setError(QSocket::NetworkError, InvalidSocketErrorString);
                close();
                return total == 0 ? -1 : total;
            }
        } else if(r == 0 && type == QSocket::TcpSocket) {
            setError(QSocket::RemoteHostClosedError, RemoteHostClosedErrorString);
            close();
            return total;
        } else {
            total += r;
            if(all) continue; else return total;
        }
        watcher.start();
    }
    return total;
}

//qint64 QSocketPrivate::send(const char *data, qint64 size)
//{
//    if(!isValid()) {
//        return -1;
//    }
//    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
//    while(true)
//    {
//        ssize_t w;
//        do {
//            w = ::send(fd, data, size, 0);
//        } while(w < 0 && errno == EINTR);

//        if(w > 0)
//        {
//            return w;
//        }
//        else if(w < 0)
//        {
//            switch(errno)
//            {
//            case EAGAIN:
//                break;
//            case EACCES:
//                setError(QSocket::SocketAccessError);
//                return -1;
//            case EBADF:
//            case EFAULT:
//            case EINVAL:
//            case ENOTCONN:
//            case ENOTSOCK:
//                setError(QSocket::UnsupportedSocketOperationError);
//                close();
//                return -1;
//            case EMSGSIZE:
//            case ENOBUFS:
//            case ENOMEM:
//                setError(QSocket::DatagramTooLargeError);
//                return -1;
//            case EPIPE:
//            case ECONNRESET:
//            default:
//                setError(QSocket::RemoteHostClosedError);
//                close();
//                return -1;
//            }
//        }
//        watcher.start();
//    }
//}

// openbsd do not support MSG_MORE?
#ifndef MSG_MORE
#define MSG_MORE 0
#endif

qint64 QSocketPrivate::send(const char *data, qint64 size, bool all)
{
    if(!isValid()) {
        return 0;
    }
    qint64 sent = 0;
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
    // TODO UDP socket may send zero length packet

    while(sent < size)
    {
        ssize_t w;
        do {
            w = ::send(fd, data + sent, size - sent, MSG_MORE);
        } while(w < 0 && errno == EINTR);
        if(w > 0) {
            if(!all) {
                return w;
            } else {
                sent += w;
            }
        } else if(w < 0) {
            switch(errno)
            {
            case EAGAIN:
                break;
            case EACCES:
                setError(QSocket::SocketAccessError, AccessErrorString);
                close();
                return sent;
            case EBADF:
            case EFAULT:
            case EINVAL:
            case ENOTCONN:
            case ENOTSOCK:
                setError(QSocket::UnsupportedSocketOperationError, InvalidSocketErrorString);
                close();
                return sent;
            case EMSGSIZE:
            case ENOBUFS:
            case ENOMEM:
                setError(QSocket::DatagramTooLargeError, DatagramTooLargeErrorString);
                return sent;
            case EPIPE:
            case ECONNRESET:
                setError(QSocket::RemoteHostClosedError, RemoteHostClosedErrorString);
                close();
                return sent;
            default:
                setError(QSocket::UnknownSocketError, UnknownSocketErrorString);
                close();
                return sent;
            }
        }
        watcher.start();
    }
    return sent;
}

qint64 QSocketPrivate::recvfrom(char *data, qint64 maxSize, QHostAddress *addr, quint16 *port)
{
    if(!isValid()) {
        return 0;
    }

    if(maxSize <= 0)
        return 0;

    struct msghdr msg;
    struct iovec vec;
    qt_sockaddr aa;
    memset(&msg, 0, sizeof(msg));
    memset(&aa, 0, sizeof(aa));

    vec.iov_base = data;
    vec.iov_len = maxSize;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_name = &aa;
    msg.msg_namelen = sizeof(aa);

    ssize_t recvResult = 0;
    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    while(true)
    {
        do {
            recvResult = ::recvmsg(fd, &msg, 0);
        } while (recvResult == -1 && errno == EINTR);

        if (recvResult < 0) {
            switch (errno) {
#if EWOULDBLOCK-0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case ECONNRESET:
            case ECONNREFUSED:
            case ENOTCONN:
#if defined(Q_OS_VXWORKS)
            case ESHUTDOWN:
#endif
                if(type == QSocket::TcpSocket)
                {
                    setError(QSocket::RemoteHostClosedError, RemoteHostClosedErrorString);
                    close();
                }
                return -1;
            case ENOMEM:
                setError(QSocket::SocketResourceError, ResourceErrorString);
                return -1;
            case ENOTSOCK:
            case EBADF:
            case EINVAL:
            case EIO:
            case EFAULT:
            default:
                setError(QSocket::NetworkError, InvalidSocketErrorString);
                close();
                return -1;
            }
        } else{
            qt_socket_getPortAndAddress(&aa, port, addr);
            //return qint64(maxSize ? recvResult : recvResult == -1 ? -1 : 0);
            return qint64(recvResult);
        }
        watcher.start();
    }
}

qint64 QSocketPrivate::sendto(const char *data, qint64 size, const QHostAddress &addr, quint16 port)
{
    if(!isValid()) {
        return -1;
    }
    struct msghdr msg;
    struct iovec vec;
    qt_sockaddr aa;
    QT_SOCKLEN_T len;

    memset(&msg, 0, sizeof(msg));
    memset(&aa, 0, sizeof(aa));
    vec.iov_base = const_cast<char *>(data);
    vec.iov_len = size;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_name = &aa.a;

    setPortAndAddress(port, addr, &aa, &len);
    msg.msg_namelen = len;

    ssize_t sentBytes = 0;
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    while(true)
    {
        do {
            sentBytes = ::sendmsg(fd, &msg, flags);
        } while(sentBytes == -1 && error == EINTR);

        if(sentBytes < 0)
        {
            switch(errno)
            {
#if EWOULDBLOCK-0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case EACCES:
                setError(QSocket::SocketAccessError, AccessErrorString);
                return -1;
            case EMSGSIZE:
                setError(QSocket::DatagramTooLargeError, DatagramTooLargeErrorString);
                return -1;
            case ECONNRESET:
            case ENOTSOCK:
                if(type == QSocket::TcpSocket)
                {
                    setError(QSocket::RemoteHostClosedError, RemoteHostClosedErrorString);
                    close();
                }
                return -1;
            case EDESTADDRREQ: // not happen in sendto()
            case EISCONN: // happens in udp socket
            case ENOTCONN: // happens in tcp socket
                setError(QSocket::UnsupportedSocketOperationError,InvalidSocketErrorString);
                return -1;
            case ENOBUFS:
            case ENOMEM:
                setError(QSocket::SocketResourceError, ResourceErrorString);
                return -1;
            case EFAULT:
            case EINVAL:
            default:
                setError(QSocket::NetworkError, InvalidSocketErrorString);
                return -1;
            }
        }
        else
        {
            return qint64(sentBytes);
        }
        watcher.start();
    }
}


static void convertToLevelAndOption(QSocket::SocketOption opt,
                                    QSocket::NetworkLayerProtocol socketProtocol, int *level, int *n)
{
    *n = -1;
    *level = SOL_SOCKET; // default

    switch (opt) {
    case QSocket::BroadcastSocketOption:
        *n = SO_BROADCAST;
        break;
    case QSocket::ReceiveBufferSizeSocketOption:
        *n = SO_RCVBUF;
        break;
    case QSocket::SendBufferSizeSocketOption:
        *n = SO_SNDBUF;
        break;
    case QSocket::AddressReusable:
        *n = SO_REUSEADDR;
        break;
    case QSocket::ReceiveOutOfBandData:
        *n = SO_OOBINLINE;
        break;
    case QSocket::LowDelayOption:
        *level = IPPROTO_TCP;
        *n = TCP_NODELAY;
        break;
    case QSocket::KeepAliveOption:
        *n = SO_KEEPALIVE;
        break;
    case QSocket::MulticastTtlOption:
        if (socketProtocol == QSocket::IPv6Protocol || socketProtocol == QSocket::AnyIPProtocol) {
            *level = IPPROTO_IPV6;
            *n = IPV6_MULTICAST_HOPS;
        } else
        {
            *level = IPPROTO_IP;
            *n = IP_MULTICAST_TTL;
        }
        break;
    case QSocket::MulticastLoopbackOption:
        if (socketProtocol == QSocket::IPv6Protocol || socketProtocol == QSocket::AnyIPProtocol) {
            *level = IPPROTO_IPV6;
            *n = IPV6_MULTICAST_LOOP;
        } else
        {
            *level = IPPROTO_IP;
            *n = IP_MULTICAST_LOOP;
        }
        break;
    case QSocket::TypeOfServiceOption:
        if (socketProtocol == QSocket::IPv4Protocol) {
            *level = IPPROTO_IP;
            *n = IP_TOS;
        }
        break;
    case QSocket::ReceivePacketInformation:
        if (socketProtocol == QSocket::IPv6Protocol || socketProtocol == QSocket::AnyIPProtocol) {
            *level = IPPROTO_IPV6;
            *n = IPV6_RECVPKTINFO;
        } else if (socketProtocol == QSocket::IPv4Protocol) {
            *level = IPPROTO_IP;
#ifdef IP_PKTINFO
            *n = IP_PKTINFO;
#elif defined(IP_RECVDSTADDR)
            // variant found in QNX and FreeBSD; it will get us only the
            // destination address, not the interface; we need IP_RECVIF for that.
            *n = IP_RECVDSTADDR;
#endif
        }
        break;
    case QSocket::ReceiveHopLimit:
        if (socketProtocol == QSocket::IPv6Protocol || socketProtocol == QSocket::AnyIPProtocol) {
            *level = IPPROTO_IPV6;
            *n = IPV6_RECVHOPLIMIT;
        } else if (socketProtocol == QSocket::IPv4Protocol) {
#ifdef IP_RECVTTL               // IP_RECVTTL is a non-standard extension supported on some OS
            *level = IPPROTO_IP;
            *n = IP_RECVTTL;
#endif
        }
        break;
    case QSocket::MaxStreamsSocketOption:
        // FIXME support stcp
        break;
    case QSocket::NonBlockingSocketOption:
    case QSocket::BindExclusively:
        Q_UNREACHABLE();
    }
}


QVariant QSocketPrivate::option(QSocket::SocketOption option) const
{
    if(!isValid())
        return QVariant();

    if(option == QSocket::BroadcastSocketOption) {
        return QVariant(true);
    }
    int n, level;
    int v = -1;
    QT_SOCKLEN_T len = sizeof(v);
    convertToLevelAndOption(option, protocol, &level, &n);
    if (n != -1 && ::getsockopt(fd, level, n, reinterpret_cast<char*>(&v), &len) != -1)
    {
        return QVariant(v);
    }
    return QVariant();
}


bool QSocketPrivate::setOption(QSocket::SocketOption option, const QVariant &value)
{
    if(!isValid())
        return false;

    if(option == QSocket::BroadcastSocketOption) {
        return true;
    }

    int n, level;
    bool ok;
    int v = value.toInt(&ok);
    if(!ok)
        return false;

    convertToLevelAndOption(option, protocol, &level, &n);

#if defined(SO_REUSEPORT) && !defined(Q_OS_LINUX)
    if (option == QSocket::AddressReusable) {
        // on OS X, SO_REUSEADDR isn't sufficient to allow multiple binds to the
        // same port (which is useful for multicast UDP). SO_REUSEPORT is, but
        // we most definitely do not want to use this for TCP. See QTBUG-6305.
        if (type == QSocket::UdpSocket)
            n = SO_REUSEPORT;
    }
#endif
    return ::setsockopt(fd, level, n, reinterpret_cast<char*>(&v), sizeof(v)) == 0;
}

bool QSocketPrivate::setNonblocking()
{
#if !defined(Q_OS_VXWORKS)
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            return false;
        }
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            return false;
        }
#else // Q_OS_VXWORKS
        int onoff = 1;
        if (::ioctl(fd, FIONBIO, (int)&onoff) < 0) {
            return false;
        }
#endif // Q_OS_VXWORKS
        return true;
}

// Tru64 redefines accept -> _accept with _XOPEN_SOURCE_EXTENDED
static inline int qt_safe_accept(int s, struct sockaddr *addr, socklen_t *addrlen, int flags = 0)
{
    Q_ASSERT((flags & ~O_NONBLOCK) == 0);

    int fd;
#if QT_UNIX_SUPPORTS_THREADSAFE_CLOEXEC && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    // use accept4
    int sockflags = SOCK_CLOEXEC;
    if (flags & O_NONBLOCK)
        sockflags |= SOCK_NONBLOCK;
    fd = ::accept4(s, addr, static_cast<QT_SOCKLEN_T *>(addrlen), sockflags);
    if (fd != -1 || !(errno == ENOSYS || errno == EINVAL))
        return fd;
#endif

    fd = ::accept(s, addr, addrlen);
    if (fd == -1)
        return -1;

    ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    // set non-block too?
    if (flags & O_NONBLOCK)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

    return fd;
}

QSocket *QSocketPrivate::accept()
{
    if(!isValid()) {
        return 0;
    }

    if(state != QSocket::ListeningState || type != QSocket::TcpSocket) {
        return 0;
    }

    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    while(true)
    {
        int acceptedDescriptor = qt_safe_accept(fd, 0, 0);
        if (acceptedDescriptor == -1) {
            switch (errno) {
            case EBADF:
            case EOPNOTSUPP:
                setError(QSocket::UnsupportedSocketOperationError, InvalidSocketErrorString);
                return 0;
            case ECONNABORTED:
                setError(QSocket::NetworkError, RemoteHostClosedErrorString);
                return 0;
            case EFAULT:
            case ENOTSOCK:
                setError(QSocket::SocketResourceError, NotSocketErrorString);
                return 0;
            case EPROTONOSUPPORT:
            case EPROTO:
            case EAFNOSUPPORT:
            case EINVAL:
                setError(QSocket::UnsupportedSocketOperationError, ProtocolUnsupportedErrorString);
                return 0;
            case ENFILE:
            case EMFILE:
            case ENOBUFS:
            case ENOMEM:
                setError(QSocket::SocketResourceError, ResourceErrorString);
                return 0;
            case EACCES:
            case EPERM:
                setError(QSocket::SocketAccessError, AccessErrorString);
                return 0;
            default:
                setError(QSocket::UnknownSocketError, UnknownSocketErrorString);
                return 0;
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            }
        }
        else
        {
            QSocket *conn = new QSocket(acceptedDescriptor);
            return conn;
        }
        watcher.start();
    }
}


QTNETWORKNG_NAMESPACE_END
