#pragma once

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QStringList>

class QLocalSocket;

class SingleInstance final : public QObject {
  Q_OBJECT

 public:
  enum class StartResult { Primary, Forwarded, Error };

  explicit SingleInstance(QString data_path, QString identity_suffix = {},
                          QObject* parent = nullptr);
  ~SingleInstance() override;

  StartResult Start(const QString& url, QString* error = nullptr);
  void SetActivationHandler(
      std::function<void(const QString&)> activation_handler);
  QString server_name_for_testing() const { return server_name_; }
  int active_request_count_for_testing() const {
    return request_buffers_.size();
  }
  int pending_request_count_for_testing() const {
    return pending_requests_.size();
  }
  void QueueRequestForTesting(const QString& url) { DispatchRequest(url); }

 private:
  void AcceptConnections();
  void ReadRequest(QLocalSocket* socket);
  void DispatchRequest(const QString& url);
  bool ForwardRequest(const QString& url, QString* error);

  QString server_name_;
  QLockFile lock_;
  QLocalServer server_;
  QHash<QLocalSocket*, QByteArray> request_buffers_;
  QStringList pending_requests_;
  std::function<void(const QString&)> activation_handler_;
  bool primary_ = false;

  Q_DISABLE_COPY_MOVE(SingleInstance)
};
