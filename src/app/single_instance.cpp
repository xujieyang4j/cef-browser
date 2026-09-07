#include "app/single_instance.h"

#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

namespace {

constexpr int kConnectTimeoutMs = 1500;
constexpr int kRequestTimeoutMs = 2000;
constexpr int kMaxRequestBytes = 64 * 1024;
constexpr int kMaxForwardedUrlBytes = kMaxRequestBytes / 2;
constexpr int kMaxActiveRequests = 16;
constexpr int kMaxPendingRequests = 32;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

QString ServerName(const QString& data_path, const QString& suffix) {
  const QByteArray identity =
      (QDir(data_path).absolutePath() + QLatin1Char('|') + suffix).toUtf8();
  const QByteArray digest =
      QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
  return QStringLiteral("trail-browser-%1")
      .arg(QString::fromLatin1(digest.first(24)));
}

}  // namespace

SingleInstance::SingleInstance(QString data_path, QString identity_suffix,
                               QObject* parent)
    : QObject(parent),
      server_name_(ServerName(data_path, identity_suffix)),
      lock_(QDir(data_path).filePath(QStringLiteral("instance.lock"))) {
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  server_.setMaxPendingConnections(kMaxActiveRequests);
  connect(&server_, &QLocalServer::newConnection, this,
          &SingleInstance::AcceptConnections);
}

SingleInstance::~SingleInstance() {
  if (!primary_) return;
  server_.close();
  QLocalServer::removeServer(server_name_);
}

SingleInstance::StartResult SingleInstance::Start(const QString& url,
                                                  QString* error) {
  if (!lock_.tryLock()) {
    return ForwardRequest(url, error) ? StartResult::Forwarded
                                      : StartResult::Error;
  }

  QLocalServer::removeServer(server_name_);
  if (!server_.listen(server_name_)) {
    SetError(error, server_.errorString());
    lock_.unlock();
    return StartResult::Error;
  }
  primary_ = true;
  return StartResult::Primary;
}

void SingleInstance::SetActivationHandler(
    std::function<void(const QString&)> activation_handler) {
  activation_handler_ = std::move(activation_handler);
  if (!activation_handler_) return;
  const QStringList pending = std::move(pending_requests_);
  pending_requests_.clear();
  for (const QString& url : pending) activation_handler_(url);
}

void SingleInstance::AcceptConnections() {
  while (QLocalSocket* socket = server_.nextPendingConnection()) {
    if (request_buffers_.size() >= kMaxActiveRequests) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    socket->setReadBufferSize(kMaxRequestBytes + 1);
    request_buffers_.insert(socket, {});
    connect(socket, &QLocalSocket::readyRead, this,
            [this, socket] { ReadRequest(socket); });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
      request_buffers_.remove(socket);
      socket->deleteLater();
    });
    QTimer::singleShot(kRequestTimeoutMs, socket, [this, socket] {
      if (!request_buffers_.remove(socket)) return;
      socket->abort();
    });
    ReadRequest(socket);
  }
}

void SingleInstance::ReadRequest(QLocalSocket* socket) {
  if (!request_buffers_.contains(socket)) return;
  QByteArray& buffer = request_buffers_[socket];
  buffer += socket->readAll();
  if (buffer.size() > kMaxRequestBytes) {
    request_buffers_.remove(socket);
    socket->disconnectFromServer();
    return;
  }
  const qsizetype newline = buffer.indexOf('\n');
  if (newline < 0) return;

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(buffer.first(newline), &parse_error);
  request_buffers_.remove(socket);
  socket->disconnectFromServer();
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return;
  }
  const QJsonObject object = document.object();
  if (object.value(QStringLiteral("version")).toInt() != 1) return;
  const QString url = object.value(QStringLiteral("url")).toString();
  if (url.toUtf8().size() > kMaxForwardedUrlBytes) return;
  DispatchRequest(url);
}

void SingleInstance::DispatchRequest(const QString& url) {
  if (activation_handler_) {
    activation_handler_(url);
  } else {
    if (pending_requests_.size() >= kMaxPendingRequests) {
      pending_requests_.removeFirst();
    }
    pending_requests_.append(url);
  }
}

bool SingleInstance::ForwardRequest(const QString& url, QString* error) {
  if (url.toUtf8().size() > kMaxForwardedUrlBytes) {
    SetError(error, QStringLiteral("Open request is too large"));
    return false;
  }
  QLocalSocket socket;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < kConnectTimeoutMs) {
    socket.abort();
    socket.connectToServer(server_name_, QIODevice::WriteOnly);
    if (socket.waitForConnected(100)) break;
    QThread::msleep(25);
  }
  if (socket.state() != QLocalSocket::ConnectedState) {
    SetError(error, socket.errorString());
    return false;
  }
  QByteArray payload = QJsonDocument(QJsonObject{
                                        {QStringLiteral("version"), 1},
                                        {QStringLiteral("url"), url},
                                    })
                           .toJson(QJsonDocument::Compact);
  payload.append('\n');
  if (socket.write(payload) != payload.size() ||
      (socket.bytesToWrite() > 0 &&
       !socket.waitForBytesWritten(kConnectTimeoutMs))) {
    SetError(error, socket.errorString());
    return false;
  }
  socket.disconnectFromServer();
  return true;
}
