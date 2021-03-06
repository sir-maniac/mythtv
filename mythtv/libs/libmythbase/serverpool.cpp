#include <QNetworkAddressEntry>
#include <QReadWriteLock>
#include <QWriteLocker>
#include <QReadLocker>
#include <QTcpSocket>

#include "mythcorecontext.h"
#include "mythlogging.h"
#include "serverpool.h"

#define PRETTYIP(x)      x->protocol() == QAbstractSocket::IPv6Protocol ? \
                                    "[" + x->toString().toLower() + "]" : \
                                          x->toString().toLower()
#define PRETTYIP_(x)      x.protocol() == QAbstractSocket::IPv6Protocol ? \
                                    "[" + x.toString().toLower() + "]" : \
                                          x.toString().toLower()

#define LOC QString("ServerPool: ")

static QList<QNetworkAddressEntry> naList_4;
static QList<QNetworkAddressEntry> naList_6;
static QReadWriteLock naLock;

static QPair<QHostAddress, int> kLinkLocal  =
                            QHostAddress::parseSubnet("169.254.0.0/16");
static QPair<QHostAddress, int> kLinkLocal6 =
                            QHostAddress::parseSubnet("fe80::/10");

class PrivUdpSocket : public QUdpSocket
{
public:
    PrivUdpSocket(QObject *parent, QNetworkAddressEntry host) :
        QUdpSocket(parent), m_host(host) { };
    ~PrivUdpSocket() { };
    QNetworkAddressEntry host()
    {
        return m_host;
    };
    bool contains(QHostAddress addr)
    {
        return contains(m_host, addr);
    };
    static bool contains(QNetworkAddressEntry host, QHostAddress addr)
    {
        if (addr.protocol() == QAbstractSocket::IPv6Protocol &&
            addr.isInSubnet(kLinkLocal6) &&
            host.ip().scopeId() != addr.scopeId())
        {
            return false;
        }
        return addr.isInSubnet(host.ip(), host.prefixLength());
    }
private:
    QNetworkAddressEntry m_host;
};

PrivTcpServer::PrivTcpServer(QObject *parent, PoolServerType type)
              : QTcpServer(parent), m_serverType(type)
{
}

void PrivTcpServer::incomingConnection(qt_socket_fd_t socket)
{
    emit newConnection(socket);
}

ServerPool::ServerPool(QObject *parent) : QObject(parent),
    m_listening(false), m_maxPendingConn(30), m_port(0),
    m_proxy(QNetworkProxy::NoProxy), m_lastUdpSocket(NULL)
{
}

ServerPool::~ServerPool()
{
    close();
}

void ServerPool::SelectDefaultListen(bool force)
{
    if (!force)
    {
        QReadLocker rlock(&naLock);
        if (!naList_4.isEmpty() || !naList_6.isEmpty())
            // lists are already populated, do nothing
            return;
    }

    QWriteLocker wlock(&naLock);
    naList_4.clear();
    naList_6.clear();

    if (gCoreContext->GetNumSetting("ListenOnAllIps",1))
    {
        QNetworkAddressEntry entry;
        entry.setIp(QHostAddress(QHostAddress::AnyIPv4));
        naList_4.append(entry);
        entry.setIp(QHostAddress(QHostAddress::AnyIPv6));
        naList_6.append(entry);
        return;
    }

    // populate stored IPv4 and IPv6 addresses
    QHostAddress config_v4(gCoreContext->resolveSettingAddress(
                                           "BackendServerIP",
                                           QString(),
                                           gCoreContext->ResolveIPv4, true));
    bool v4IsSet = config_v4.isNull() ? true : false;
    QHostAddress config_v6(gCoreContext->resolveSettingAddress(
                                           "BackendServerIP6",
                                           QString(),
                                           gCoreContext->ResolveIPv6, true));
    bool v6IsSet = config_v6.isNull() ? true : false;
    bool allowLinkLocal = gCoreContext->GetNumSetting("AllowLinkLocal", true) > 0;

    // loop through all available interfaces
    QList<QNetworkInterface> IFs = QNetworkInterface::allInterfaces();
    QList<QNetworkInterface>::const_iterator qni;
    for (qni = IFs.begin(); qni != IFs.end(); ++qni)
    {
        if ((qni->flags() & QNetworkInterface::IsRunning) == 0)
            continue;

        QList<QNetworkAddressEntry> IPs = qni->addressEntries();
        QList<QNetworkAddressEntry>::iterator qnai;
        for (qnai = IPs.begin(); qnai != IPs.end(); ++qnai)
        {
            QHostAddress ip = qnai->ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol)
            {
                if (naList_4.contains(*qnai))
                    // already defined, skip
                    continue;

                else if (!config_v4.isNull() && (ip == config_v4))
                {
                    // IPv4 address is defined, add it
                    LOG(VB_GENERAL, LOG_DEBUG,
                        QString("Adding BackendServerIP to address list."));
                    naList_4.append(*qnai);
                    v4IsSet = true;

                }

                else if (ip == QHostAddress::LocalHost)
                {
                    // always listen on LocalHost
                    LOG(VB_GENERAL, LOG_DEBUG,
                        QString("Adding IPv4 loopback to address list."));
                    naList_4.append(*qnai);
                    if (!v4IsSet && (config_v4 == ip))
                        v4IsSet = true;
                }

                else if (ip.isInSubnet(kLinkLocal) && allowLinkLocal)
                {
                    // optionally listen on linklocal
                    // the next clause will enable it anyway if no IP address
                    // has been set
                    LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding link-local '%1' to address list.")
                                .arg(PRETTYIP_(ip)));
                    naList_4.append(*qnai);
                }

                else if (config_v4.isNull())
                {
                    // IPv4 address is not defined, populate one
                    // restrict autoconfiguration to RFC1918 private networks
                    static QPair<QHostAddress, int>
                       privNet1 = QHostAddress::parseSubnet("10.0.0.0/8"),
                       privNet2 = QHostAddress::parseSubnet("172.16.0.0/12"),
                       privNet3 = QHostAddress::parseSubnet("192.168.0.0/16");

                    if (ip.isInSubnet(privNet1) || ip.isInSubnet(privNet2) ||
                        ip.isInSubnet(privNet3))
                    {
                        LOG(VB_GENERAL, LOG_DEBUG,
                                QString("Adding '%1' to address list.")
                                    .arg(PRETTYIP_(ip)));
                        naList_4.append(*qnai);
                    }
                    else if (ip.isInSubnet(kLinkLocal))
                    {
                        LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding link-local '%1' to address list.")
                                    .arg(PRETTYIP_(ip)));
                        naList_4.append(*qnai);
                    }
                    else
                        LOG(VB_GENERAL, LOG_DEBUG, QString("Skipping "
                           "non-private address during IPv4 autoselection: %1")
                                    .arg(PRETTYIP_(ip)));
                }

                else
                    LOG(VB_GENERAL, LOG_DEBUG, QString("Skipping address: %1")
                                .arg(PRETTYIP_(ip)));

            }
            else
            {
                if (ip.isInSubnet(kLinkLocal6))
                {
                    // set scope id for link local address
                    ip.setScopeId(qni->name());
                    qnai->setIp(ip);
                }

                if (naList_6.contains(*qnai))
                    // already defined, skip
                    continue;

                else if ((!config_v6.isNull()) && (ip == config_v6))
                {
                // IPv6 address is defined, add it
                    LOG(VB_GENERAL, LOG_DEBUG,
                        QString("Adding BackendServerIP6 to address list."));
                    naList_6.append(*qnai);
                    v6IsSet = true;
                }

                else if (ip == QHostAddress::LocalHostIPv6)
                {
                // always listen on LocalHost
                    LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding IPv6 loopback to address list."));
                    naList_6.append(*qnai);
                    if (!v6IsSet && (config_v6 == ip))
                        v6IsSet = true;
                }

                else if (ip.isInSubnet(kLinkLocal6) && allowLinkLocal)
                {
                    // optionally listen on linklocal
                    // the next clause will enable it anyway if no IP address
                    // has been set
                    LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding link-local '%1' to address list.")
                                .arg(ip.toString()));
                    naList_6.append(*qnai);
                }

                else if (config_v6.isNull())
                {
                    if (ip.isInSubnet(kLinkLocal6))
                        LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding link-local '%1' to address list.")
                                .arg(PRETTYIP_(ip)));
                    else
                        LOG(VB_GENERAL, LOG_DEBUG,
                            QString("Adding '%1' to address list.")
                                .arg(PRETTYIP_(ip)));

                    naList_6.append(*qnai);
                }

                else
                    LOG(VB_GENERAL, LOG_DEBUG, QString("Skipping address: %1")
                                .arg(PRETTYIP_(ip)));
            }
        }
    }

    if (!v4IsSet && (config_v4 != QHostAddress::LocalHost)
                 && !naList_4.isEmpty())
    {
        LOG(VB_GENERAL, LOG_CRIT, LOC + QString("Host is configured to listen "
                "on %1, but address is not used on any local network "
                "interfaces.").arg(config_v4.toString()));
    }

    if (!v6IsSet && (config_v6 != QHostAddress::LocalHostIPv6)
                 && !naList_6.isEmpty())
    {
        LOG(VB_GENERAL, LOG_CRIT, LOC + QString("Host is configured to listen "
                "on %1, but address is not used on any local network "
                "interfaces.").arg(PRETTYIP_(config_v6)));
    }

    // NOTE: there is no warning for the case where both defined addresses
    //       are localhost, and neither are found. however this would also
    //       mean there is no configured network at all, and should be
    //       sufficiently rare a case as to not worry about it.
}

void ServerPool::RefreshDefaultListen(void)
{
    SelectDefaultListen(true);
    // TODO:
    // send signal for any running servers to cycle their addresses
    // will allow address configuration to be modified without a server
    // reset for use with the migration to the mythtv-setup replacement
}

QList<QHostAddress> ServerPool::DefaultListen(void)
{
    QList<QHostAddress> alist;
    alist << DefaultListenIPv4()
          << DefaultListenIPv6();
    return alist;
}

QList<QHostAddress> ServerPool::DefaultListenIPv4(void)
{
    SelectDefaultListen();
    QReadLocker rlock(&naLock);

    QList<QHostAddress> alist;
    QList<QNetworkAddressEntry>::const_iterator it;
    for (it = naList_4.begin(); it != naList_4.end(); ++it)
        if (!alist.contains(it->ip()))
            alist << it->ip();

    return alist;
}

QList<QHostAddress> ServerPool::DefaultListenIPv6(void)
{
    SelectDefaultListen();
    QReadLocker rlock(&naLock);

    QList<QHostAddress> alist;
    QList<QNetworkAddressEntry>::const_iterator it;
    for (it = naList_6.begin(); it != naList_6.end(); ++it)
        if (!alist.contains(it->ip()))
            alist << it->ip();

    return alist;
}

QList<QHostAddress> ServerPool::DefaultBroadcast(void)
{
    QList<QHostAddress> blist;
    if (!gCoreContext->GetNumSetting("ListenOnAllIps",1))
    {
        blist << DefaultBroadcastIPv4();
        blist << DefaultBroadcastIPv6();
    }
    return blist;
}

QList<QHostAddress> ServerPool::DefaultBroadcastIPv4(void)
{
    SelectDefaultListen();
    QReadLocker rlock(&naLock);

    QList<QHostAddress> blist;
    QList<QNetworkAddressEntry>::const_iterator it;
    for (it = naList_4.begin(); it != naList_4.end(); ++it)
        if (!blist.contains(it->broadcast()) && (it->prefixLength() != 32) &&
                (it->ip() != QHostAddress::LocalHost))
            blist << it->broadcast();

    return blist;
}

QList<QHostAddress> ServerPool::DefaultBroadcastIPv6(void)
{
    QList<QHostAddress> blist;
    blist << QHostAddress("FF02::1");
    return blist;
}


void ServerPool::close(void)
{
    while (!m_tcpServers.isEmpty())
    {
        PrivTcpServer *server = m_tcpServers.takeLast();
        server->disconnect();
        server->close();
        server->deleteLater();
    }

    while (!m_udpSockets.isEmpty())
    {
        PrivUdpSocket *socket = m_udpSockets.takeLast();
        socket->disconnect();
        socket->close();
        socket->deleteLater();
    }
    m_lastUdpSocket = NULL;
    m_listening = false;
}

bool ServerPool::listen(QList<QHostAddress> addrs, quint16 port,
                        bool requireall, PoolServerType servertype)
{
    m_port = port;
    QList<QHostAddress>::const_iterator it;

    for (it = addrs.begin(); it != addrs.end(); ++it)
    {
        // If IPV4 support is disabled and this is an IPV4 address,
        // bypass this address
        if (it->protocol() == QAbstractSocket::IPv4Protocol
          && ! gCoreContext->GetNumSetting("IPv4Support",1))
            continue;
        // If IPV6 support is disabled and this is an IPV6 address,
        // bypass this address
        if (it->protocol() == QAbstractSocket::IPv6Protocol
          && ! gCoreContext->GetNumSetting("IPv6Support",1))
            continue;

        PrivTcpServer *server = new PrivTcpServer(this, servertype);
            connect(server, &PrivTcpServer::newConnection,
                this,   &ServerPool::newTcpConnection);

        server->setProxy(m_proxy);
        server->setMaxPendingConnections(m_maxPendingConn);

        if (server->listen(*it, m_port))
        {
            LOG(VB_GENERAL, LOG_INFO, QString("Listening on TCP %1:%2")
                    .arg(PRETTYIP(it)).arg(port));
            if (servertype == kTCPServer)
                m_tcpServers.append(server);
            if (m_port == 0)
                m_port = server->serverPort();
        }
        else
        {
            LOG(VB_GENERAL, LOG_ERR,
                    QString("Failed listening on TCP %1:%2 - Error %3: %4")
                        .arg(PRETTYIP(it))
                        .arg(port)
                        .arg(server->serverError())
                        .arg(server->errorString()));
            server->disconnect();
            server->deleteLater();

            if (server->serverError() == QAbstractSocket::HostNotFoundError ||
                server->serverError() == QAbstractSocket::SocketAddressNotAvailableError)
            {
                LOG(VB_GENERAL, LOG_ERR,
                    QString("Address %1 no longer exists - ignoring")
                    .arg(PRETTYIP(it)));
                continue;
            }

            if (server->serverError() == QAbstractSocket::UnsupportedSocketOperationError
                 && it->protocol() == QAbstractSocket::IPv4Protocol)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("IPv4 support failed for this port."));
                 continue;
            }

            if (server->serverError() == QAbstractSocket::UnsupportedSocketOperationError
                 && it->protocol() == QAbstractSocket::IPv6Protocol)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("IPv6 support failed for this port."));
                 continue;
            }

            if (requireall)
            {
                close();
                return false;
            }
        }
    }

    if (m_tcpServers.size() == 0)
        return false;

    m_listening = true;
    return true;
}

bool ServerPool::listen(QStringList addrstr, quint16 port, bool requireall,
                        PoolServerType servertype)
{
    QList<QHostAddress> addrs;
    QStringList::const_iterator it;
    for (it = addrstr.begin(); it != addrstr.end(); ++it)
        addrs << QHostAddress(*it);
    return listen(addrs, port, requireall, servertype);
}

bool ServerPool::listen(quint16 port, bool requireall,
                        PoolServerType servertype)
{
    return listen(DefaultListen(), port, requireall, servertype);
}

bool ServerPool::bind(QList<QHostAddress> addrs, quint16 port,
                      bool requireall)
{
    m_port = port;
    QList<QHostAddress>::const_iterator it;

    for (it = addrs.begin(); it != addrs.end(); ++it)
    {
        // If IPV4 support is disabled and this is an IPV4 address,
        // bypass this address
        if (it->protocol() == QAbstractSocket::IPv4Protocol
          && ! gCoreContext->GetNumSetting("IPv4Support",1))
            continue;
        // If IPV6 support is disabled and this is an IPV6 address,
        // bypass this address
        if (it->protocol() == QAbstractSocket::IPv6Protocol
          && ! gCoreContext->GetNumSetting("IPv6Support",1))
            continue;

        QNetworkAddressEntry host;

        if (it->protocol() == QAbstractSocket::IPv6Protocol)
        {
            QList<QNetworkAddressEntry>::iterator iae;
            for (iae = naList_6.begin(); iae != naList_6.end(); ++iae)
            {
                if (PrivUdpSocket::contains(*iae, *it))
                {
                    host = *iae;
                    break;
                }
            }
        }
        else
        {
            QList<QNetworkAddressEntry>::iterator iae;
            for (iae = naList_4.begin(); iae != naList_4.end(); ++iae)
            {
                if (PrivUdpSocket::contains(*iae, *it))
                {
                    host = *iae;
                    break;
                }
            }
        }

        PrivUdpSocket *socket = new PrivUdpSocket(this, host);

        if (socket->bind(*it, port))
        {
            LOG(VB_GENERAL, LOG_INFO, QString("Binding to UDP %1:%2")
                    .arg(PRETTYIP(it)).arg(port));
            m_udpSockets.append(socket);
            connect(socket, SIGNAL(readyRead()),
                    this,   SLOT(newUdpDatagram()));
        }
        else
        {
            LOG(VB_GENERAL, LOG_ERR,
                    QString("Failed binding to UDP %1:%2 - Error %3: %4")
                        .arg(PRETTYIP(it))
                        .arg(port)
                        .arg(socket->error())
                        .arg(socket->errorString()));
            socket->disconnect();
            socket->deleteLater();

            if (socket->error() == QAbstractSocket::SocketAddressNotAvailableError)
            {
                LOG(VB_GENERAL, LOG_ERR,
                    QString("Address %1 no longer exists - ignoring")
                    .arg(PRETTYIP(it)));
                continue;
            }

            if (requireall)
            {
                close();
                return false;
            }
        }
    }

    if (m_udpSockets.size() == 0)
        return false;

    m_listening = true;
    return true;
}

bool ServerPool::bind(QStringList addrstr, quint16 port, bool requireall)
{
    QList<QHostAddress> addrs;
    QStringList::const_iterator it;
    for (it = addrstr.begin(); it != addrstr.end(); ++it)
        addrs << QHostAddress(*it);
    return bind(addrs, port, requireall);
}

bool ServerPool::bind(quint16 port, bool requireall)
{
    return bind(DefaultListen(), port, requireall);
}

qint64 ServerPool::writeDatagram(const char * data, qint64 size,
                                 const QHostAddress &addr, quint16 port)
{
    if (!m_listening || m_udpSockets.size() == 0)
    {
        LOG(VB_GENERAL, LOG_ERR, "Trying to write datagram to disconnected "
                            "ServerPool instance.");
        return -1;
    }

    // check if can re-use the last one, so there's no need for a linear search
    if (!m_lastUdpSocket || !m_lastUdpSocket->contains(addr))
    {
        // find the right socket to use
        QList<PrivUdpSocket*>::iterator it;
        for (it = m_udpSockets.begin(); it != m_udpSockets.end(); ++it)
        {
            PrivUdpSocket *val = *it;
            if (val->contains(addr))
            {
                m_lastUdpSocket = val;
                break;
            }
        }
    }
    if (!m_lastUdpSocket)
        return -1;

    qint64 ret = m_lastUdpSocket->writeDatagram(data, size, addr, port);
    if (ret != size)
    {
        LOG(VB_GENERAL, LOG_DEBUG, QString("Error = %1 : %2")
                .arg(ret).arg(m_lastUdpSocket->error()));
    }
    return ret;
}

qint64 ServerPool::writeDatagram(const QByteArray &datagram,
                                 const QHostAddress &addr, quint16 port)
{
    return writeDatagram(datagram.data(), datagram.size(), addr, port);
}

void ServerPool::newTcpConnection(qt_socket_fd_t socket)
{
    // Ignore connections from an SSL server for now, these are only handled
    // by HttpServer which overrides newTcpConnection
    PrivTcpServer *server = dynamic_cast<PrivTcpServer *>(QObject::sender());
    if (!server || server->GetServerType() == kSSLServer)
        return;

    QTcpSocket *qsock = new QTcpSocket(this);
    if (qsock->setSocketDescriptor(socket)
       && gCoreContext->CheckSubnet(qsock))
    {
        emit newConnection(qsock);
    }
    else
        delete qsock;
}

void ServerPool::newUdpDatagram(void)
{
    QUdpSocket *socket = dynamic_cast<QUdpSocket*>(sender());

    while (socket->state() == QAbstractSocket::BoundState &&
           socket->hasPendingDatagrams())
    {
        QByteArray buffer;
        buffer.resize(socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        socket->readDatagram(buffer.data(), buffer.size(),
                             &sender, &senderPort);
        if (gCoreContext->CheckSubnet(sender))
            emit newDatagram(buffer, sender, senderPort);
    }
}

/**
 * tryListeningPort
 *
 * Description:
 * Tells the server to listen for incoming connections on port port.
 * The server will attempt to listen on all local interfaces.
 *
 * Usage:
 * baseport: port to listen on.
 * range:    range of ports to try (default 1)
 *
 * Returns port used on success; otherwise returns -1.
 */
int ServerPool::tryListeningPort(int baseport, int range)
{
    // try a few ports in case the first is in use
    int port = baseport;
    while (port < baseport + range)
    {
        if (listen(port))
        {
            break;
        }
        port++;
    }

    if (port >= baseport + range)
    {
        return -1;
    }
    return port;
}

/**
 * tryBindingPort
 *
 * Description:
 * Binds this socket for incoming connections on port port.
 * The socket will attempt to bind on all local interfaces.
 *
 * Usage:
 * baseport: port to bind to.
 * range:    range of ports to try (default 1)
 *
 * Returns port used on success; otherwise returns -1.
 */
int ServerPool::tryBindingPort(int baseport, int range)
{
    // try a few ports in case the first is in use
    int port = baseport;
    while (port < baseport + range)
    {
        if (bind(port))
        {
            break;
        }
        port++;
    }

    if (port >= baseport + range)
    {
        return -1;
    }
    return port;
}

/**
 * tryListeningPort
 *
 * Description:
 * Tells the server to listen for incoming connections on port port.
 * The server will attempt to listen on all IPv6 and IPv4 interfaces.
 * If IPv6 isn't available, the server will listen on all IPv4 network interfaces.
 *
 * Usage:
 * server:   QTcpServer object to use
 * baseport: port to listen on. If port is 0, a port is chosen automatically.
 * range:    range of ports to try (default 1)
 * isipv6:   is set to true if IPv6 was successful (default NULL)
 *
 * Returns port used on success; otherwise returns -1.
 */
int ServerPool::tryListeningPort(QTcpServer *server, int baseport,
                                 int range, bool *isipv6)
{
    bool ipv6 = true;
    // try a few ports in case the first is in use
    int port = baseport;
    while (port < baseport + range)
    {
        if (ipv6)
        {
            if (server->listen(QHostAddress::AnyIPv6, port))
            {
                break;
            }
            else
            {
                // did we fail because IPv6 isn't available?
                QAbstractSocket::SocketError err = server->serverError();
                if (err == QAbstractSocket::UnsupportedSocketOperationError)
                {
                    ipv6 = false;
                }
            }
        }
        if (!ipv6)
        {
            if (server->listen(QHostAddress::Any, port))
            {
                break;
            }
        }
        port++;
    }

    if (isipv6)
    {
        *isipv6 = ipv6;
    }

    if (port >= baseport + range)
    {
        return -1;
    }
    if (port == 0)
    {
        port = server->serverPort();
    }
    return port;
}

/**
 * tryBindingPort
 *
 * Description:
 * Binds this socket for incoming connections on port port.
 * The socket will attempt to bind on all IPv6 and IPv4 interfaces.
 * If IPv6 isn't available, the socket will be bound to all IPv4 network interfaces.
 *
 * Usage:
 * socket:   QUdpSocket object to use
 * baseport: port to bind to.
 * range:    range of ports to try (default 1)
 * isipv6:   is set to true if IPv6 was successful (default NULL)
 *
 * Returns port used on success; otherwise returns -1.
 */
int ServerPool::tryBindingPort(QUdpSocket *socket, int baseport,
                               int range, bool *isipv6)
{
    bool ipv6 = true;
    // try a few ports in case the first is in use
    int port = baseport;
    while (port < baseport + range)
    {
        if (ipv6)
        {
            if (socket->bind(QHostAddress::AnyIPv6, port))
            {
                break;
            }
            else
            {
                // did we fail because IPv6 isn't available?
                QAbstractSocket::SocketError err = socket->error();
                if (err == QAbstractSocket::UnsupportedSocketOperationError)
                {
                    ipv6 = false;
                }
            }
        }
        if (!ipv6)
        {
            if (socket->bind(QHostAddress::Any, port))
            {
                break;
            }
        }
        port++;
    }

    if (isipv6)
    {
        *isipv6 = ipv6;
    }

    if (port >= baseport + range)
    {
        return -1;
    }
    return port;
}
