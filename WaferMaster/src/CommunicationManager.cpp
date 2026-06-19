#include "CommunicationManager.h"
#include <QHostAddress>
#include <QRegularExpression>

// ═══════════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════════

CommunicationManager::CommunicationManager(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection,
            this, &CommunicationManager::onNewConnection);
}

CommunicationManager::~CommunicationManager()
{
    stopServer();
}

// ═══════════════════════════════════════════════════════════════════
// 服务器启停
// ═══════════════════════════════════════════════════════════════════

bool CommunicationManager::startServer(const QHostAddress& address, quint16 port)
{
    if (m_server->isListening())
    {
        emit logMessage(QStringLiteral("TCP 已在监听，跳过重复启动"));
        return true;
    }

    if (m_server->listen(address, port))
    {
        emit listeningStateChanged(
            true,
            QStringLiteral("%1:%2").arg(address.toString()).arg(port));
        emit logMessage(
            QStringLiteral("TCP 服务器启动成功: %1:%2")
                .arg(address.toString())
                .arg(port));
        return true;
    }

    emit listeningStateChanged(false, m_server->errorString());
    emit logMessage(
        QStringLiteral("TCP 服务器启动失败: %1").arg(m_server->errorString()));
    return false;
}

void CommunicationManager::stopServer()
{
    for (auto* client : m_clients)
    {
        client->disconnectFromHost();
    }
    m_clients.clear();

    if (m_server->isListening())
    {
        m_server->close();
        emit listeningStateChanged(false, QStringLiteral("已停止"));
        emit logMessage(QStringLiteral("TCP 服务器已停止"));
    }
}

// ═══════════════════════════════════════════════════════════════════
// 客户端连接管理
// ═══════════════════════════════════════════════════════════════════

void CommunicationManager::onNewConnection()
{
    while (m_server->hasPendingConnections())
    {
        QTcpSocket* client = m_server->nextPendingConnection();
        m_clients.append(client);

        connect(client, &QTcpSocket::readyRead,
                this, &CommunicationManager::onReadyRead);
        connect(client, &QTcpSocket::disconnected,
                this, &CommunicationManager::onClientDisconnected);

        emit clientStateChanged(true, m_clients.size());
        emit logMessage(
            QStringLiteral("客户端已连接: %1:%2")
                .arg(client->peerAddress().toString())
                .arg(client->peerPort()));

        // 发送欢迎提示
        client->write("WaferMaster 1.0 TCP Server\ncommands: START STOP STATUS\n");
    }
}

void CommunicationManager::onClientDisconnected()
{
    auto* client = qobject_cast<QTcpSocket*>(sender());
    if (!client)
        return;

    m_clients.removeAll(client);
    client->deleteLater();

    emit clientStateChanged(!m_clients.isEmpty(), m_clients.size());
    emit logMessage(QStringLiteral("客户端已断开"));
}

// ═══════════════════════════════════════════════════════════════════
// 命令接收与解析
// ═══════════════════════════════════════════════════════════════════

void CommunicationManager::onReadyRead()
{
    auto* client = qobject_cast<QTcpSocket*>(sender());
    if (!client)
        return;

    while (client->canReadLine())
    {
        QByteArray line = client->readLine().trimmed();
        if (line.isEmpty())
            continue;

        QString command = QString::fromUtf8(line).toUpper();
        emit logMessage(QStringLiteral("收到命令: %1").arg(QString::fromUtf8(line)));
        handleCommand(client, command);
    }
}

void CommunicationManager::handleCommand(QTcpSocket* socket, const QString& command)
{
    if (command == QLatin1String("START"))
    {
        socket->write("OK START\n");
        emit startRequested();
    }
    else if (command == QLatin1String("STOP"))
    {
        socket->write("OK STOP\n");
        emit stopRequested();
    }
    else if (command == QLatin1String("STATUS"))
    {
        sendStatusToClient(socket);
    }
    else
    {
        socket->write("ERR unknown_command\n");
        emit logMessage(QStringLiteral("未知命令: %1").arg(command));
    }
}

// ═══════════════════════════════════════════════════════════════════
// 状态回写
// ═══════════════════════════════════════════════════════════════════

void CommunicationManager::updateLatestStatus(RunState state, const AlgoResult& result)
{
    m_lastState  = state;
    m_lastResult = result;
}

void CommunicationManager::sendStatusToClient(QTcpSocket* socket)
{
    QString text = buildStatusText();
    socket->write(text.toUtf8());
    socket->write("\n");
}

QString CommunicationManager::buildStatusText() const
{
    QString stateStr = runStateToString(m_lastState);

    return QStringLiteral(
               "STATUS state=%1 frame=%2 fi=%3 p95=%4 hotRatio=%5 level=%6")
        .arg(stateStr)
        .arg(m_lastResult.frameIdx)
        .arg(m_lastResult.fi, 0, 'f', 2)
        .arg(m_lastResult.p95, 0, 'f', 1)
        .arg(m_lastResult.hotRatio, 0, 'f', 2)
        .arg(detectionLevelToString(m_lastResult.level));
}

QString CommunicationManager::runStateToString(RunState state) const
{
    switch (state)
    {
    case RunState::Idle:    return QStringLiteral("Idle");
    case RunState::Running: return QStringLiteral("Running");
    case RunState::Stopped: return QStringLiteral("Stopped");
    case RunState::Error:   return QStringLiteral("Error");
    default:                return QStringLiteral("Unknown");
    }
}