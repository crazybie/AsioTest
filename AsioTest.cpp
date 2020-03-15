// AsioTest.cpp : This file contains the 'main' function. Program execution
// begins and ends there.
//

#include <array>
#include <asio.hpp>
#include <iostream>
#include <thread>
#include <tuple>

#include "co/coroutine.h"
#include "pch.h"

using namespace std;
using namespace asio;
using namespace asio::ip;
using namespace co;

//////////////////////////////////////////////////////////////////////////

namespace func_helper {

template <typename Idx, typename Tuple>
struct tuple_elems;

template <typename Tuple, size_t... I>
struct tuple_elems<index_sequence<I...>, Tuple> {
  using type = tuple<tuple_element_t<I, Tuple>...>;
};

template <typename T>
struct FuncTrait : FuncTrait<decltype(&T::operator())> {};

template <typename R, typename C, typename... A>
struct FuncTrait<R (C::*)(A...) const> {
  using Args = tuple<A...>;
};

template <typename R, typename C, typename... A>
struct FuncTrait<R (C::*)(A...)> {
  using Args = tuple<A...>;
};

}  // namespace func_helper

namespace asio_co_helper {

using namespace func_helper;

template <typename R, typename ArgsTp, typename F, size_t... idx>
auto make_async_index(F f, index_sequence<idx...>) {
  return [=](tuple_element_t<idx, ArgsTp>... args) {
    CoBegin(R) {
      __state++;
      f(args..., [=](const error_code& err, R r) {
        if (err) {
          __onErr(std::make_exception_ptr(err));
        } else {
          __onOk(r);
        }
      });
    }
    CoEnd();
  };
}

template <typename ArgsTp, typename F, size_t... idx>
auto make_async_index_void(F f, index_sequence<idx...>) {
  return [=](tuple_element_t<idx, ArgsTp>... args) {
    CoBegin(bool) {
      __state++;
      f(args..., [=](const error_code& err) {
        if (err) {
          __onErr(std::make_exception_ptr(err));
        } else {
          __onOk(true);
        }
      });
    }
    CoEnd();
  };
}

template <typename R, typename F>
auto make_async(F f) {
  using Args = typename FuncTrait<F>::Args;
  using ArgsNoCb =
      typename tuple_elems<make_index_sequence<tuple_size_v<Args> - 1>,
                           Args>::type;
  using Idx = make_index_sequence<tuple_size_v<ArgsNoCb>>;

  if constexpr (is_same_v<R, void>)
    return make_async_index_void<ArgsNoCb>(f, Idx{});
  else
    return make_async_index<R, ArgsNoCb>(f, Idx{});
}

template <typename... Args>
using NetAction = gc_function<void(const error_code&, Args...)>;

}  // namespace asio_co_helper

using namespace asio_co_helper;

//////////////////////////////////////////////////////////////////////////

auto async_write_co = make_async<size_t>(
    [](tcp::socket* s, string* data, const NetAction<size_t>& cb) {
      async_write(*s, buffer(*data), cb);
    });

auto async_accept_co = make_async<void>(
    [](gc<tcp::acceptor>& a, tcp::socket* s, const NetAction<>& cb) {
      a->async_accept(*s, cb);
    });

bool server_quit = false;

class TcpConnection {
 public:
  std::string make_daytime_string() {
    time_t now = time(0);
    return ctime(&now);
  }

  PromisePtr<bool> start() {
    size_t len = 0;
    auto cnt = 0;
    CoBegin(bool) {
      message_ = make_daytime_string();
      while (cnt++ < 100) {
        CoAwaitData(len, async_write_co(&socket_, &message_));
      }
      message_ = "quit";
      CoAwait(async_write_co(&socket_, &message_));
      CoReturn(true);
    }
    CoEnd();
  }

  TcpConnection(io_context& io_context) : socket_(io_context) {}
  ~TcpConnection() {}

  tcp::socket socket_;
  std::string message_;
};

class TcpServer {
 public:
  TcpServer(io_context& ctx)
      : acceptor_{gc_new<tcp::acceptor>(ctx, tcp::endpoint(tcp::v4(), 13))} {
    start_accept();
  }

  ~TcpServer() {
    // NOTE:
    // destructors triggered by gc is in undefined order,
    // we need to call them manually.
    gc_delete(acceptor_);
    gc_delete(connList);
    gc_delete(newConn);
  }

 private:
  PromisePtr<bool> start_accept() {
    CoBegin(bool) {
      while (true) {
        newConn = gc_new<TcpConnection>(acceptor_->get_executor().context());
        CoAwait(async_accept_co(acceptor_, &newConn->socket_));
        newConn->start();
        connList->push_back(newConn);
      }
    }
    CoEnd();
  }
  gc<TcpConnection> newConn;
  gc<tcp::acceptor> acceptor_;
  gc_vector<TcpConnection> connList = gc_new_vector<TcpConnection>();
};

int server_main() {
  try {
    auto ctx = gc_new<io_context>();
    co::Executor exc;
    auto server = gc_new<TcpServer>(*ctx);
    while (!server_quit) {
      exc.updateAll();
      ctx->poll();
    }
    gc_delete(server);
    gc_delete(ctx);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
  }
  cout << "server quit" << endl;
  return 0;
}

int client_main() {
  try {
    io_context io_context;
    tcp::socket socket(io_context);
    socket.connect(tcp::endpoint(ip::address_v4::from_string("127.0.0.1"), 13));
    cout << "connected to server" << endl;

    string data;
    for (;;) {
      string buf(128, 0);
      error_code error;
      size_t len = socket.read_some(buffer(buf), error);

      buf[len] = '\0';
      cout << "data len:" << len << endl;
      cout << buf;
      data += buf;
      if (data.rfind("quit") != string::npos)
        break;

      if (error == error::eof)
        break;
      else if (error)
        throw system_error(error);
    }
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
  }

  server_quit = true;
  cout << "client quit" << endl;
  return 0;
}

int main() {
  std::thread s(server_main);
  s.detach();
  client_main();
  gc_collect();
  return 0;
}
