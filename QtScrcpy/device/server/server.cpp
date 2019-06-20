#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QTimerEvent>
#include <QCoreApplication>
#include <QFileInfo>

#include "server.h"

#define DEVICE_SERVER_PATH "/data/local/tmp/scrcpy-server.jar"
#define DEVICE_NAME_FIELD_LENGTH 64
#define SOCKET_NAME "qtscrcpy"

Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this](){
        QTcpSocket* tmp = m_serverSocket.nextPendingConnection();
        if (dynamic_cast<VideoSocket*>(tmp)) {
            m_videoSocket = dynamic_cast<VideoSocket*>(tmp);
            if (!m_videoSocket->isValid() || !readInfo(m_deviceName, m_deviceSize)) {
                stop();
                emit connectToResult(false);
            }
        } else {
            m_controlSocket = tmp;
            if (m_controlSocket && m_controlSocket->isValid()) {
                // we don't need the server socket anymore
                // just m_videoSocket is ok
                m_serverSocket.close();
                // we don't need the adb tunnel anymore
                disableTunnelReverse();
                m_tunnelEnabled = false;
                emit connectToResult(true, m_deviceName, m_deviceSize);
            } else {
                stop();
                emit connectToResult(false);
            }
            stopAcceptTimeoutTimer();
        }
    });
}

Server:: ~Server()
{

}

const QString& Server::getServerPath()
{
    if (m_serverPath.isEmpty()) {
        m_serverPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
        QFileInfo fileInfo(m_serverPath);
        if (m_serverPath.isEmpty() || !fileInfo.isFile()) {
            m_serverPath = QCoreApplication::applicationDirPath() + "/scrcpy-server.jar";
        }
    }
    return m_serverPath;
}

bool Server::pushServer()
{    
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, getServerPath(), DEVICE_SERVER_PATH);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, SOCKET_NAME, m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    AdbProcess* adb = new AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &AdbProcess::adbProcessResult, this, [this](AdbProcess::ADB_EXEC_RESULT processResult){
        if (AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, SOCKET_NAME);
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, SOCKET_NAME);
    return true;
}
bool Server::disableTunnelForward()
{
    AdbProcess* adb = new AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &AdbProcess::adbProcessResult, this, [this](AdbProcess::ADB_EXEC_RESULT processResult){
        if (AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }
    QStringList args;
    args << "shell";
    args << QString("CLASSPATH=%1").arg(DEVICE_SERVER_PATH);
    args << "app_process";
    args << "/"; // unused;
    args << "com.genymobile.scrcpy.Server";
    args << QString::number(m_params.maxSize);
    args << QString::number(m_params.bitRate);
    args << (m_tunnelForward ? "true" : "false");
    if (m_params.crop.isEmpty()) {
        args << "-";
    } else {
        args << m_params.crop;
    }
    args << (m_params.sendFrameMeta ? "true" : "false");
    args << (m_params.control ? "true" : "false");

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // mark: crop input format: "width:height:x:y" or - for no crop, for example: "100:200:0:0"
    // 这条adb命令是阻塞运行的，m_serverProcess进程不会退出了
    m_serverProcess.execute(m_params.serial, args);
    return true;
}

bool Server::start(Server::ServerParams params)
{    
    m_params = params;
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }    

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer();
        return true;
    }

    // device server need time to start
    // TODO:电脑配置太低的话，这里有可能时间不够导致连接太早，安卓监听socket还没有建立
    // 后续研究其他办法
    // wait for devices server start
    QTimer::singleShot(1000, this, [this](){
        QString deviceName;
        QSize deviceSize;
        bool success = false;

        // video socket
        m_videoSocket = new VideoSocket();
        m_videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
        if (!m_videoSocket->waitForConnected(1000)) {
            stop();
            qWarning("video socket connect to server failed");
            emit connectToResult(false, "", QSize());
            return false;
        }
        if (QTcpSocket::ConnectedState == m_videoSocket->state()) {
            // connect will success even if devices offline, recv data is real connect success
            // because connect is to pc adb server
            m_videoSocket->waitForReadyRead(1000);
            // devices will send 1 byte first on tunnel forward mode
            QByteArray data = m_videoSocket->read(1);
            if (!data.isEmpty() && readInfo(deviceName, deviceSize)) {
                success = true;
            } else {
                qWarning("video socket connect to server read device info failed");
                success = false;
            }
        } else {
            qWarning("connect to server failed");
            m_videoSocket->deleteLater();
            success = false;
        }

        // control socket
        m_controlSocket = new QTcpSocket();
        m_controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
        if (!m_controlSocket->waitForConnected(1000)) {
            stop();
            qWarning("control socket connect to server failed");
            emit connectToResult(false, "", QSize());
            return false;
        }

        if (success) {            
            // we don't need the adb tunnel anymore
            disableTunnelForward();
            m_tunnelEnabled = false;
        } else {
            stop();
        }
        emit connectToResult(success, deviceName, deviceSize);
    });

    return true;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        emit connectToResult(false, "", QSize());
    }
}

VideoSocket* Server::getVideoSocket()
{    
    return m_videoSocket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    if (m_videoSocket) {
        m_videoSocket->close();
        m_videoSocket->deleteLater();
    }
    if (m_controlSocket) {
        m_controlSocket->close();
        m_controlSocket->deleteLater();
    }
    // ignore failure
    m_serverProcess.kill();
    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
        m_tunnelForward = false;
        m_tunnelEnabled = false;
    }    
    m_serverSocket.close();    
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // push, enable tunnel et start the server
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // if "adb reverse" does not work (e.g. over "adb connect"), it fallbacks to
            // "adb forward", so the app socket is the client
            if (!m_tunnelForward) {
                // At the application level, the device part is "the server" because it
                // serves video stream and control. However, at the network level, the
                // client listens and the server connects to the client. That way, the
                // client can listen before starting the server app, so there is no need to
                // try to connect until the server socket is listening on the device.
                m_serverSocket.setMaxPendingConnections(2);
                if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                    qCritical(QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str());
                    m_serverStartStep = SSS_NULL;
                    if (m_tunnelForward) {
                        disableTunnelForward();
                    } else {
                        disableTunnelReverse();
                    }
                    emit serverStartResult(false);
                    return false;
                }
            }
            // server will connect to our server socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStartResult(false);
    }
    return stepSuccess;
}

bool Server::readInfo(QString &deviceName, QSize &size)
{
    unsigned char buf[DEVICE_NAME_FIELD_LENGTH + 4];
    if (m_videoSocket->bytesAvailable() <= (DEVICE_NAME_FIELD_LENGTH + 4)) {
        m_videoSocket->waitForReadyRead(300);
    }

    qint64 len = m_videoSocket->read((char*)buf, sizeof(buf));
    if (len < DEVICE_NAME_FIELD_LENGTH + 4) {
        qInfo("Could not retrieve device information");
        return false;
    }
    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\0'; // in case the client sends garbage
    // strcpy is safe here, since name contains at least DEVICE_NAME_FIELD_LENGTH bytes
    // and strlen(buf) < DEVICE_NAME_FIELD_LENGTH
    deviceName = (char*)buf;
    size.setWidth((buf[DEVICE_NAME_FIELD_LENGTH] << 8) | buf[DEVICE_NAME_FIELD_LENGTH + 1]);
    size.setHeight((buf[DEVICE_NAME_FIELD_LENGTH + 2] << 8) | buf[DEVICE_NAME_FIELD_LENGTH + 3]);
    return true;
}

void Server::startAcceptTimeoutTimer()
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(1000);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::onWorkProcessResult(AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (AdbProcess::AER_SUCCESS_EXEC == processResult) {
#if 1
                    m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
#else
                    // test tunnelForward
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
#endif
                    startServerByStep();
                } else if (AdbProcess::AER_SUCCESS_START != processResult){
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStartResult(false);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (AdbProcess::AER_SUCCESS_START != processResult){
                    qCritical("adb reverse failed");
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (AdbProcess::AER_SUCCESS_START != processResult){
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStartResult(false);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                emit serverStartResult(true);
            } else if (AdbProcess::AER_ERROR_START == processResult){
                if (!m_tunnelForward) {
                    m_serverSocket.close();
                    disableTunnelReverse();
                } else {
                    disableTunnelForward();
                }
                qCritical("adb shell start server failed");
                m_serverStartStep = SSS_NULL;
                emit serverStartResult(false);
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            m_serverStartStep = SSS_NULL;
            emit onServerStop();
        }
    }
}