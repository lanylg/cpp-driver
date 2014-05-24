/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_CONNECTION_HPP_INCLUDED__
#define __CASS_CONNECTION_HPP_INCLUDED__

#include <uv.h>

#include "common.hpp"
#include "host.hpp"
#include "message.hpp"
#include "session.hpp"
#include "ssl_context.hpp"
#include "ssl_session.hpp"
#include "stream_manager.hpp"
#include "connecter.hpp"
#include "writer.hpp"
#include "timer.hpp"

namespace cass {

class Connection {
  public:
    class StartupHandler : public ResponseCallback {
      public:
        StartupHandler(Connection* connection, Message* request)
          : connection_(connection)
          , request_(request) { }

        virtual Message* request() const {
          return request_.get();
        }

        virtual void on_set(Message* response) {
          switch(response->opcode) {
            case CQL_OPCODE_SUPPORTED:
              connection_->on_supported(response);
              break;
            case CQL_OPCODE_ERROR:
              connection_->notify_error("Error during startup"); // TODO(mpenick): Better error
              connection_->defunct();
              break;
            case CQL_OPCODE_READY:
              connection_->on_ready();
              break;
            default:
              connection_->notify_error("Invalid opcode during startup");
              connection_->defunct();
              break;
          }
        }

        virtual void on_error(CassError code, const std::string& message) {
          connection_->notify_error("Error during startup");
          connection_->defunct();
        }

        virtual void on_timeout() {
          connection_->notify_error("Timed out during startup");
          connection_->defunct();
        }
      private:
        Connection* connection_;
        std::unique_ptr<Message> request_;
    };

    struct Request {
        enum State {
          REQUEST_STATE_NEW,
          REQUEST_STATE_WRITING,
          REQUEST_STATE_READING,
          REQUEST_STATE_TIMED_OUT,
          REQUEST_STATE_DONE,
        };

        Request(Connection* connection,
                ResponseCallback* response_callback)
          : connection(connection)
          , response_callback(response_callback)
          , stream(0)
          , timer(nullptr)
          , state(REQUEST_STATE_NEW) { }

        void on_set(Message* response) {
          if(maybe_stop_timer()) {
            response_callback->on_set(response);
          }
          connection->request_count_--;
          connection->maybe_close();
        }

        void on_error(CassError code, const std::string& message) {
          if(maybe_stop_timer()) {
            response_callback->on_error(code, message);
          }
          connection->stream_manager_.release_stream(stream);
          connection->request_count_--;
          connection->maybe_close();
        }

        void on_timeout() {
          connection->logger_->info("Request timed out to '%s'",
                                    connection->host_string_.c_str());
          timer = nullptr;
          state = REQUEST_STATE_TIMED_OUT;
          response_callback->on_timeout();
          connection->timed_out_requests_.push_back(this);
          connection->maybe_close();
        }

        void start_write() {
          state = REQUEST_STATE_WRITING;
          connection->request_count_++;
          timer = Timer::start(connection->loop_, connection->config_.write_timeout(), this, on_request_timeout);
        }

        void start_read() {
          state = REQUEST_STATE_READING;
          timer = Timer::start(connection->loop_, connection->config_.read_timeout(), this, on_request_timeout);
        }

        bool maybe_stop_timer() {
          if(state == REQUEST_STATE_TIMED_OUT) {
            connection->timed_out_requests_.remove(this);
            return false; // We timed out, end of the request
          } else if(timer) {
            Timer::stop(timer);
            timer = nullptr;
          }
          return true; // We stoppped the timer, continue on
        }

        static void on_request_timeout(Timer* timer) {
          Request *request = static_cast<Request*>(timer->data());
          request->on_timeout();
        }

        Connection* connection;
        std::unique_ptr<ResponseCallback> response_callback;
        int8_t stream;
        Timer* timer;
        State state;
    };

    typedef std::function<void(Connection*)> ConnectCallback;
    typedef std::function<void(Connection*)> CloseCallback;

  public:
    enum ClientConnectionState {
      CLIENT_STATE_NEW,
      CLIENT_STATE_CONNECTED,
      CLIENT_STATE_HANDSHAKE,
      CLIENT_STATE_SUPPORTED,
      CLIENT_STATE_READY,
      CLIENT_STATE_CLOSING,
      CLIENT_STATE_CLOSED
    };

    enum Compression {
      CLIENT_COMPRESSION_NONE,
      CLIENT_COMPRESSION_SNAPPY,
      CLIENT_COMPRESSION_LZ4
    };

    enum SchemaEventType {
      CLIENT_EVENT_SCHEMA_CREATED,
      CLIENT_EVENT_SCHEMA_UPDATED,
      CLIENT_EVENT_SCHEMA_DROPPED
    };

    Connection(uv_loop_t* loop,
               SSLSession* ssl_session,
               const Host& host,
               Logger* logger,
               const Config& config,
               ConnectCallback connect_callback,
               CloseCallback close_callback)
      : state_(CLIENT_STATE_NEW)
      , is_defunct_(false)
      , request_count_(0)
      , loop_(loop)
      , incoming_(new Message())
      , connect_callback_(connect_callback)
      , close_callback_(close_callback)
      , log_callback_(nullptr)
      , host_(host)
      , host_string_(host.address.to_string())
      , ssl_(ssl_session)
      , ssl_handshake_done_(false)
      , version_("3.0.0")
      , logger_(logger)
      , config_(config)
      , connect_timer_(nullptr) {
      socket_.data = this;
      uv_tcp_init(loop_, &socket_);
      if (ssl_) {
        ssl_->init();
        ssl_->handshake(true);
      }
    }

    ~Connection() {
      for(auto request : timed_out_requests_) {
        delete request;
      }
    }

    void connect() {
      if(state_ == CLIENT_STATE_NEW) {
        connect_timer_ = Timer::start(loop_, config_.connect_timeout(), this, on_connect_timeout);
        Connecter::connect(&socket_, host_.address, this, on_connect);
      }
    }

    bool execute(ResponseCallback* response_callback) {
      std::unique_ptr<Request> request(new Request(this, response_callback));

      Message* message = response_callback->request();

      int8_t stream = stream_manager_.acquire_stream(request.get());
      if(request->stream < 0) {
        return false;
      }

      request->stream = stream;
      message->stream = stream;

      uv_buf_t buf;
      if (!message->prepare(&buf.base, buf.len)) {
        request->on_error(CASS_ERROR_LIB_MESSAGE_PREPARE, "Unable to build request");
        return true;
      }

      logger_->debug("Sending message type %s with %d, size %zd",
                     opcode_to_string(message->opcode).c_str(), message->stream, buf.len);

      request->start_write();
      write(buf, request.release());

      return true;
    }

    void close() {
      state_ = CLIENT_STATE_CLOSING;
      maybe_close();
    }

    void defunct() {
      if(!is_defunct_) {
        is_defunct_ = true;
        close();
      }
    }

    bool is_closing() {
      return state_ == CLIENT_STATE_CLOSING;
    }

    bool is_defunct() {
      return is_defunct_;
    }

    bool is_ready() {
      return state_ == CLIENT_STATE_READY;
    }

    void maybe_close() {
      if(is_closing()
         && outstanding_request_count() <= 0) {
        actually_close();
      }
    }

    inline size_t available_streams() {
      return stream_manager_.available_streams();
    }

    inline int outstanding_request_count() {
      return request_count_ - timed_out_requests_.size();
    }

  private:
    void actually_close() {
      if(!uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) {
        state_ = CLIENT_STATE_CLOSING;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&socket_));
        uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
      }
    }

    void write(uv_buf_t buf, Request* request) {
      Writer::Bufs* bufs = new Writer::Bufs({ buf });
      Writer::write(reinterpret_cast<uv_stream_t*>(&socket_), bufs, request, on_write);
    }

    void event_received() {
      switch (state_) {
        case CLIENT_STATE_CONNECTED:
          ssl_handshake();
          break;
        case CLIENT_STATE_HANDSHAKE:
          send_options();
          break;
        case CLIENT_STATE_SUPPORTED:
          send_startup();
          break;
        case CLIENT_STATE_READY:
          notify_ready();
          break;
        case CLIENT_STATE_CLOSED:
          break;
        default:
          assert(false);
      }
    }

    void consume(char* input, size_t size) {
      char* buffer    = input;
      int   remaining = size;

      while (remaining != 0) {
        int consumed = incoming_->consume(buffer, remaining);
        if (consumed < 0) {
          // TODO(mstump) probably means connection closed/failed
          // Can this even happen right now?
          logger_->error("Error consuming message on '%s'",
                         host_string_.c_str());
        }

        if (incoming_->body_ready) {
          std::unique_ptr<Message> response(std::move(incoming_));
          incoming_.reset(new Message());

          logger_->debug("Consumed message type %s with stream %d, input %zd, remaining %d on '%s'",
                         opcode_to_string(response->opcode).c_str(), response->stream,
                         size, remaining, host_string_.c_str());

          if (response->stream < 0) {
            // TODO(mstump) system events
            assert(false);
          } else {
            Request* request = nullptr;
            if(stream_manager_.get_item(response->stream, request)) {
              request->on_set(response.get());
              if(request->state == Request::REQUEST_STATE_READING) {
                delete request;
              } else {
                // The read came before the write came back
                request->state = Request::REQUEST_STATE_DONE;
              }
            } else {
              logger_->error("Invalid stream returnd from server on '%s'",
                             host_string_.c_str());
              defunct();
            }
          }
        }
        remaining -= consumed;
        buffer    += consumed;
      }
    }

    static void on_read(uv_stream_t* client, ssize_t nread, uv_buf_t buf) {
      Connection* connection =
          reinterpret_cast<Connection*>(client->data);

      if (nread == -1) {
        if (uv_last_error(connection->loop_).code != UV_EOF) {
          connection->logger_->info("Read error '%s' on '%s'",
                                    connection->host_string_.c_str(),
                                    uv_err_name(uv_last_error(connection->loop_)));
        }
        connection->actually_close();
        free_buffer(buf);
        return;
      }

      if (connection->ssl_) {
        char*  read_input        = buf.base;
        size_t read_input_size   = nread;

        for (;;) {
          size_t read_size         = 0;
          char*  read_output       = nullptr;
          size_t read_output_size  = 0;
          char*  write_output      = nullptr;
          size_t write_output_size = 0;

          // TODO(mstump) error handling for SSL decryption
          std::string error;
          connection->ssl_->read_write(read_input,
                                       read_input_size,
                                       read_size,
                                       &read_output,
                                       read_output_size,
                                       nullptr,
                                       0,
                                       &write_output,
                                       write_output_size,
                                       &error);

          if (read_output && read_output_size) {
            // TODO(mstump) error handling
            connection->consume(read_output, read_output_size);
            delete read_output;
          }

          if (write_output && write_output_size) {
            Request* request = new Request(connection, nullptr);
            connection->write(uv_buf_init(write_output, write_output_size), request);
            // delete of write_output will be handled by on_write
          }

          if (read_size < read_input_size) {
            read_input += read_size;
            read_input_size -= read_size;
          } else {
            break;
          }

          if (!connection->ssl_handshake_done_) {
            if (connection->ssl_->handshake_done()) {
              connection->state_ = CLIENT_STATE_HANDSHAKE;
              connection->event_received();
            }
          }
        }
      } else {
        connection->consume(buf.base, nread);
      }
      free_buffer(buf);
    }

    static void on_connect(Connecter* connecter) {
      Connection* connection
          = reinterpret_cast<Connection*>(connecter->data());

      if(connection->is_defunct()) { // Timed out
        return;
      }

      Timer::stop(connection->connect_timer_);
      connection->connect_timer_ = nullptr;

      if(connecter->status() == Connecter::SUCCESS) {
        connection->logger_->debug("Connected to '%s'",
                                   connection->host_string_.c_str());
        uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->socket_),
                      alloc_buffer,
                      on_read);
        connection->state_ = CLIENT_STATE_CONNECTED;
        connection->event_received();
      } else {
        connection->notify_error("Unable to connect");
        connection->defunct();
      }
    }

    static void on_connect_timeout(Timer* timer) {
      Connection* connection
          = reinterpret_cast<Connection*>(timer->data());
      connection->notify_error("Connection timeout");
      connection->connect_timer_ = nullptr;
      connection->defunct();
    }

    static void on_close(uv_handle_t *handle) {
      Connection* connection
          = reinterpret_cast<Connection*>(handle->data);

      connection->logger_->debug("Connection to '%s' closed",
                                 connection->host_string_.c_str());
      connection->state_ = CLIENT_STATE_CLOSED;
      connection->event_received();

      connection->close_callback_(connection);

      // TODO(mpenick): Delete the connection
    }

    void ssl_handshake() {
      if (ssl_) {
        // calling read on a handshaked initiated ssl_ pipe
        // will gives us the first message to send to the server
        on_read(
              reinterpret_cast<uv_stream_t*>(&socket_),
              0,
              alloc_buffer(0));
      } else {
        state_ = CLIENT_STATE_HANDSHAKE;
        event_received();
      }
    }

    void on_ready() {
      state_ = CLIENT_STATE_READY;
      event_received();
    }

    void on_supported(Message* response) {
      SupportedResponse* supported
          = static_cast<SupportedResponse*>(response->body.get());

      // TODO(mstump) do something with the supported info
      (void) supported;

      state_ = CLIENT_STATE_SUPPORTED;
      event_received();
    }

// TODO(mpenick):
//    void set_keyspace(const std::string& keyspace) {
//      Message message(CQL_OPCODE_QUERY);
//      Query* query = static_cast<Query*>(message.body.get());
//      query->statement("USE " + keyspace);
//      execute(&message, nullptr);
//    }

    void notify_ready() {
      connect_callback_(this);
    }

    void notify_error(const std::string& error) {
      logger_->error("'%s' error on startup for '%s'",
                     error.c_str(),
                     host_string_.c_str());
      connect_callback_(this);
    }

    void send_options() {
      execute(new StartupHandler(this, new Message(CQL_OPCODE_OPTIONS)));
    }

    void send_startup() {
      Message* message = new Message(CQL_OPCODE_STARTUP);
      StartupRequest* startup = static_cast<StartupRequest*>(message->body.get());
      startup->version = version_;
      execute(new StartupHandler(this, message));
    }

    static void on_write(Writer* writer) {
      Request* request = static_cast<Request*>(writer->data());
      Connection* connection = request->connection;

      if(!request->maybe_stop_timer()) {
        return; // Timed out
      }

      if(request->state == Request::REQUEST_STATE_DONE) {
        // The read came before the write came back
        delete request;
        return;
      }

      if(writer->status() == Writer::SUCCESS) {
        request->start_read();
      } else {
        connection->defunct();
        request->on_error(CASS_ERROR_LIB_WRITE_ERROR, "Unable to write to socket");
        delete request;
        connection->logger_->info("Write error '%s' on '%s'",
                                  connection->host_string_.c_str(),
                                  uv_err_name(uv_last_error(connection->loop_)));
      }
    }

  private:
    ClientConnectionState       state_;
    bool is_defunct_;
    int request_count_;

    uv_loop_t*                  loop_;
    std::unique_ptr<Message>    incoming_;
    StreamManager<Request*>     stream_manager_;
    ConnectCallback             connect_callback_;
    CloseCallback               close_callback_;
    LogCallback                 log_callback_;
    // DNS and hostname stuff
    Host                        host_;
    std::string                 host_string_;
    // the actual connection
    uv_tcp_t                    socket_;
    // ssl stuff
    SSLSession*                 ssl_;
    bool                        ssl_handshake_done_;
    // supported stuff sent in start up message
    std::string                 compression_;
    std::string                 version_;


    std::list<Request*> timed_out_requests_;
    Logger* logger_;
    const Config& config_;
    Timer* connect_timer_;

    DISALLOW_COPY_AND_ASSIGN(Connection);
};

} // namespace cass

#endif
