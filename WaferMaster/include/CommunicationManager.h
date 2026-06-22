#pragma once

#include <QObject>
#include <QTcpServer>//服务器类，用于监听本地端口，接收远程客户端连接
#include <QTcpSocket>//通信类，用于服务器与客户端之间的 TCP 数据传输
#include <QList>
#include "Common.h"

/** @brief TCP 通信管理层：监听本地端口，接收远程客户端的 START/STOP/STATUS 指令
           ，并将当前检测状态广播给所有已连接客户端（NetAssist）

    ## 通信协议（纯文本，每行一条命令，UTF-8）
     - START         → 远程启动检测（等价点击"开始检测"）
     - STOP          → 远程停止检测（等价点击"停止检测"）
     - STATUS        → 返回当前运行状态 + 最近一帧检测结果

    ## 工作流程
     1. startServer() 创建 QTcpServer 监听指定地址:端口
     2. 客户端连接 → onNewConnection() 加入 m_clients 列表
     3. 客户端发送指令 → onReadyRead() 逐行读取 → handleCommand() 解析
     4. START/STOP 指令通过信号通知 MainWindow 执行实际操作
     5. 检测状态变化 → updateLatestStatus() → 广播 STATUS 给所有客户端
     6. 客户端断开 → onClientDisconnected() 清理 socket
*/
class CommunicationManager : public QObject
{
    Q_OBJECT

public:
    /// @brief 构造通信管理器
    /// @param parent 父 QObject（通常为 MainWindow）
    explicit CommunicationManager(QObject* parent = nullptr);

    /// @brief 析构，停止服务并断开所有客户端
    ~CommunicationManager();

    /// @brief 启动 TCP 服务器，开始监听指定地址和端口
    /// @param address 监听地址（通常为 QHostAddress::LocalHost）
    /// @param port    监听端口（通常为 9000）
    /// @return true=监听成功，false=端口被占用或地址无效
    bool startServer(const QHostAddress& address, quint16 port);

    /// @brief 停止 TCP 服务器，断开所有客户端连接
    void stopServer();

    /// @brief 由 MainWindow 调用，将最新运行状态和检测结果广播给所有客户端
    /// @param state  当前运行状态
    /// @param result 最近一帧检测结果
    void updateLatestStatus(RunState state, const AlgoResult& result);

    /// @brief 向指定客户端 socket 发送当前缓存的 STATUS 文本
    /// @param socket 目标客户端 socket
    void sendStatusToClient(QTcpSocket* socket);

signals:
    // ========================================================================
    // 指令信号 → MainWindow 执行实际操作
    // ========================================================================

    /// @brief 客户端发送 START 指令时发出，由 MainWindow 执行 onStartClicked()
    void startRequested();

    /// @brief 客户端发送 STOP 指令时发出，由 MainWindow 执行 onStopClicked()
    void stopRequested();

    // ========================================================================
    // 状态信号 → 通知 UI 更新通信面板
    // ========================================================================

    /// @brief 服务器监听状态变化（启动成功/端口占用/已停止）
    /// @param listening 是否正在监听
    /// @param message   状态描述文本
    void listeningStateChanged(bool listening, const QString& message);

    /// @brief 客户端连接/断开时发出
    /// @param connected  是否有客户端连接
    /// @param clientCount 当前连接客户端数量
    void clientStateChanged(bool connected, int clientCount);

    /// @brief 通信层日志（客户端连接/断开/指令），转发到 MainWindow 日志控件
    /// @param message 日志文本
    void logMessage(const QString& message);

private slots:
    /// @brief QTcpServer 有新连接接入 → 创建 QTcpSocket 并加入 m_clients 列表
    ///触发：m_server emit newConnection → 此函数自动执行
    void onNewConnection();

    /// @brief 客户端 socket 有数据到达 → 逐行读取并调用 handleCommand() 解析
    void onReadyRead();

    /// @brief 客户端 socket 断开连接 → 从 m_clients 列表中移除并清理
    void onClientDisconnected();

private:
    // ========================================================================
    // 内部辅助函数
    // ========================================================================

    /// @brief 解析客户端发来的单行指令，执行对应操作
    /// @param socket 发送指令的客户端 socket
    /// @param command 指令文本（START / STOP / STATUS）
    void handleCommand(QTcpSocket* socket, const QString& command);

    /// @brief 将当前缓存的运行状态 + 检测结果拼接为 STATUS 响应文本
    /// @return 格式化的状态文本（多行 key: value）
    QString buildStatusText() const;

    /// @brief 将 RunState 枚举转为可读字符串
    /// @param state 运行状态枚举值
    /// @return 中文状态文本（就绪/运行中/已停止/错误）
    QString runStateToString(RunState state) const;

    // ========================================================================
    // 成员变量
    // ========================================================================

    QTcpServer*       m_server  = nullptr;  ///< TCP 服务器实例
    QList<QTcpSocket*> m_clients;           ///< 记录所有当前活跃的客户端连接
    RunState  m_lastState  = RunState::Idle; ///< 最近一次缓存的运行状态
    AlgoResult m_lastResult;                 ///< 最近一次缓存的检测结果
};