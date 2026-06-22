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
            this, &CommunicationManager::onNewConnection);//onNewConnection在有新连接时自动调用
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
    //1:在指定的地址和端口上绑定并开始监听传入的 TCP 连接
    //2:内核创建了一个空的 ACCEPT 队列，准备放未来到达的连接
    //3:Qt 事件循环开始监视这个 socket：一旦 ACCEPT 队列非空，就 emit newConnection()
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
    for (auto* client : m_clients)// 范围 for 遍历列表
    {
        client->disconnectFromHost();//等缓冲区数据发完再断
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
    while (m_server->hasPendingConnections())//循环处理所有挂起的连接请求
    {
        QTcpSocket* client = m_server->nextPendingConnection();//当客户端成功连接服务器后，操作系统会把连接请求放入一个挂起连接队列里。
        //nextPendingConnection() 的作用就是从这个队列的头部取出一个连接，并在内部new一个 QTcpSocket 对象。
        m_clients.append(client);

        connect(client, &QTcpSocket::readyRead,
                this, &CommunicationManager::onReadyRead);//readyRead 信号在客户端有数据可读时发出
        connect(client, &QTcpSocket::disconnected,
                this, &CommunicationManager::onClientDisconnected);//disconnected 信号在客户端断开连接时发出

        emit clientStateChanged(true, m_clients.size());
        emit logMessage(
            QStringLiteral("客户端已连接: %1:%2")
                .arg(client->peerAddress().toString())//查询客户端的IP地址
                .arg(client->peerPort()));//查询客户端的端口号

        // 发送欢迎提示
        client->write("WaferMaster 1.0 TCP Server\ncommands: START STOP STATUS\n");
    }
}

void CommunicationManager::onClientDisconnected()
{   //使用 qobject_cast 可以安全地把 QObject* 转为 QTcpSocket*
    auto* client = qobject_cast<QTcpSocket*>(sender());//sender() 返回发出信号的对象指针，这里是断开连接的客户端 socket
    //如果 sender 不是 QTcpSocket 类型，qobject_cast 会返回 nullptr，避免了不安全的强制类型转换
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
    auto* client = qobject_cast<QTcpSocket*>(sender());//sender() 返回发出信号的对象指针，这里是有数据可读的客户端 socket
    if (!client)
        return;

    // 全量读取所有可用数据，返回 QByteArray（字节数组）。
    const QByteArray data = client->readAll();
    //把整段数据按换行符拆分后的多行数据集合。
    const QList<QByteArray> lines = data.split('\n');

    for (const QByteArray& rawLine : lines)
    {
        // 去掉尾部 \r 和首尾空白
        QByteArray line = rawLine.trimmed();
        // 去掉可能残留的 \r
        if (line.endsWith('\r'))
            line.chop(1);
        line = line.trimmed();

        if (line.isEmpty())
            continue;

        const QString command = QString::fromUtf8(line).toUpper().trimmed();//字节 → 字符串，转大写，去空白
        emit logMessage(QStringLiteral("收到命令: %1").arg(command));
        handleCommand(client, command);
    }
}

void CommunicationManager::handleCommand(QTcpSocket* socket, const QString& command)
{
    if (command == QLatin1String("START"))
    {
        socket->write("OK START\n");
        emit startRequested();//通知 MainWindow 执行 onStartClicked()
    }
    else if (command == QLatin1String("STOP"))
    {
        socket->write("OK STOP\n");
        emit stopRequested();//通知 MainWindow 执行 onStopClicked()
    }
    else if (command == QLatin1String("STATUS"))
    {
        sendStatusToClient(socket);//把当前缓存的运行状态 + 检测结果拼接为 STATUS 响应文本，并发送给客户端
    }
    else
    {
        socket->write("ERR unknown_command\n");
        emit logMessage(QStringLiteral("未知命令: %1").arg(command));
    }
}

// ═══════════════════════════════════════════════════════════════════
// 状态回写STATUS
// ═══════════════════════════════════════════════════════════════════

void CommunicationManager::updateLatestStatus(RunState state, const AlgoResult& result)
{
    m_lastState  = state;
    m_lastResult = result;
}

void CommunicationManager::sendStatusToClient(QTcpSocket* socket)
{
    QString text = buildStatusText();
    socket->write(text.toUtf8());//把 STATUS 响应文本转换并发送给客户端
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