// Copyright 2013 Tanel Lebedev

#include "./websocket_client.h"

#include <string>
#include <sstream>

#include "Poco/Exception.h"
#include "Poco/InflatingStream.h"
#include "Poco/DeflatingStream.h"
#include "Poco/Logger.h"
#include "Poco/URI.h"
#include "Poco/NumberParser.h"
#include "Poco/Net/Context.h"
#include "Poco/Net/NameValueCollection.h"
#include "Poco/Net/HTTPMessage.h"
#include "Poco/Net/HTTPBasicCredentials.h"
#include "Poco/ScopedLock.h"
#include "Poco/Mutex.h"

#include "./libjson.h"
#include "./version.h"

namespace kopsik {

const int kWebsocketBufSize = 1024 * 10;

const std::string kPong("{\"type\": \"pong\"}");

error WebSocketClient::Start(void *ctx,
    std::string api_token,
    WebSocketMessageCallback on_websocket_message) {
  poco_assert(ctx);
  poco_assert(!api_token.empty());
  poco_assert(on_websocket_message);

  Poco::Mutex mutex;
  Poco::Mutex::ScopedLock lock(mutex);

  ctx_ = ctx;
  on_websocket_message_ = on_websocket_message;

  Poco::Logger &logger = Poco::Logger::get("websocket_client");
  logger.debug("WebSocketClient::Start");
  try {
    const Poco::URI uri(websocket_url_);

    // FIXME: check certification
    const Poco::Net::Context::Ptr context(new Poco::Net::Context(
      Poco::Net::Context::CLIENT_USE, "", "", "",
      Poco::Net::Context::VERIFY_NONE, 9, false,
      "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH"));

    if (session_) {
      delete session_;
    }
    session_ = new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort(),
      context);
    if (req_) {
      delete req_;
    }
    req_ = new Poco::Net::HTTPRequest(Poco::Net::HTTPRequest::HTTP_GET, "/ws",
      Poco::Net::HTTPMessage::HTTP_1_1);
    req_->set("Origin", "https://localhost");
    req_->set("User-Agent", kopsik::UserAgent(app_name_, app_version_));
    if (res_) {
      delete res_;
    }
    res_ = new Poco::Net::HTTPResponse();
    if (ws_) {
      delete ws_;
    }
    ws_ = new Poco::Net::WebSocket(*session_, *req_, *res_);
    ws_->setBlocking(false);
    ws_->setReceiveTimeout(Poco::Timespan(3 * Poco::Timespan::SECONDS));
    ws_->setSendTimeout(Poco::Timespan(3 * Poco::Timespan::SECONDS));

    Poco::Logger &logger = Poco::Logger::get("websocket_client");
    logger.debug("WebSocket connection established.");

    // Authenticate
    JSONNODE *c = json_new(JSON_NODE);
    json_push_back(c, json_new_a("type", "authenticate"));
    json_push_back(c, json_new_a("api_token", api_token.c_str()));
    json_char *jc = json_write_formatted(c);
    std::string payload(jc);
    json_free(jc);
    json_delete(c);

    ws_->sendFrame(payload.data(),
      static_cast<int>(payload.size()),
      Poco::Net::WebSocket::FRAME_BINARY);

    activity_.start();
  } catch(const Poco::Exception& exc) {
    return exc.displayText();
  } catch(const std::exception& ex) {
    return ex.what();
  } catch(const std::string& ex) {
    return ex;
  }
  return noError;
}

std::string WebSocketClient::parseWebSocketMessageType(std::string json) {
  poco_assert(!json.empty());
  std::string type("data");

  const char *str = json.c_str();

  if (!json_is_valid(str)) {
    return "";
  }

  JSONNODE *root = json_parse(str);
  JSONNODE_ITERATOR i = json_begin(root);
  JSONNODE_ITERATOR e = json_end(root);
  while (i != e) {
    json_char *node_name = json_name(*i);
    if (strcmp(node_name, "type") == 0) {
      type = std::string(json_as_string(*i));
      break;
    }
    ++i;
  }
  json_delete(root);

  return type;
}

error WebSocketClient::receiveWebSocketMessage(std::string *message) {
  int flags = Poco::Net::WebSocket::FRAME_BINARY;
  std::string json("");
  try {
    char buf[kWebsocketBufSize];
    int n = ws_->receiveFrame(buf, kWebsocketBufSize, flags);
    if (n > 0) {
      json.append(buf, n);
    }
  } catch(const Poco::Exception& exc) {
    return error(exc.displayText());
  } catch(const std::exception& ex) {
    return error(ex.what());
  } catch(const std::string& ex) {
    return error(ex);
  }
  *message = json;
  return noError;
}

error WebSocketClient::poll() {
  Poco::Logger &logger = Poco::Logger::get("websocket_client");
  logger.debug("poll");
  try {
    Poco::Timespan span(250 * Poco::Timespan::MILLISECONDS);
    if (!ws_->poll(span, Poco::Net::Socket::SELECT_READ)) {
      return noError;
    }

    std::string json("");
    error err = receiveWebSocketMessage(&json);
    if (err != noError) {
      return err;
    }
    if (json.empty()) {
      return error("WebSocket peer has shut down or closed the connection");
    }
    std::stringstream ss;
    ss << "WebSocket message: " << json;
    logger.debug(ss.str());

    std::string type = parseWebSocketMessageType(json);

    if (activity_.isStopped()) {
      return noError;
    }

    if ("ping" == type) {
      ws_->sendFrame(kPong.data(),
        static_cast<int>(kPong.size()),
        Poco::Net::WebSocket::FRAME_BINARY);
      return noError;
    }

    if ("data" == type) {
      on_websocket_message_(ctx_, json);
    }
  } catch(const Poco::Exception& exc) {
    return error(exc.displayText());
  } catch(const std::exception& ex) {
    return error(ex.what());
  } catch(const std::string& ex) {
    return error(ex);
  }
  return noError;
}

void WebSocketClient::runActivity() {
  Poco::Logger &logger = Poco::Logger::get("websocket_client");
  while (!activity_.isStopped()) {
    error err = poll();
    if (err != noError) {
      logger.error(err);
    }
  }
  logger.debug("WebSocketClient::runActivity finished");
}

void WebSocketClient::Stop() {
  Poco::Logger &logger = Poco::Logger::get("websocket_client");
  logger.debug("shutting down");
  if (ws_) {
    ws_->shutdown();
  }
  activity_.stop();  // request stop
  activity_.wait();  // wait until activity actually stops
  logger.debug("stopped");
}

}   // namespace kopsik
