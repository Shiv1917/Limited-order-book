// pyob: Python bindings for analysis and plotting.
//
// The heavy loops (replay, backtest) run in C++; Python receives numpy arrays.
//
//   import pyob
//   b = pyob.Book(base=0, tick=1, levels=1000)
//   b.add(1, "buy", 100, 10); b.market(2, "sell", 4)  -> list[pyob.Event]
//   s = pyob.load_stream("data/aapl.obm")               -> dict of numpy arrays
//   tob = pyob.replay_top_of_book("data/aapl.obm", every=100)
//   bt = pyob.backtest("data/aapl.obm", latency_msgs=10, quote_from="09:30:00")
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include "market_maker.hpp"
#include "msgfile.hpp"
#include "ob/book.hpp"

namespace py = pybind11;
using namespace ob;

namespace {

Side parse_side(const std::string& s) {
    if (s == "buy" || s == "B" || s == "b" || s == "bid") return Side::Buy;
    if (s == "sell" || s == "S" || s == "s" || s == "ask") return Side::Sell;
    throw std::invalid_argument("side must be 'buy' or 'sell'");
}
TimeInForce parse_tif(const std::string& s) {
    if (s == "GTC" || s == "gtc") return TimeInForce::GTC;
    if (s == "IOC" || s == "ioc") return TimeInForce::IOC;
    if (s == "FOK" || s == "fok") return TimeInForce::FOK;
    throw std::invalid_argument("tif must be GTC, IOC or FOK");
}
std::uint64_t hms(const std::string& s) {
    if (s.empty()) return 0;
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}
MessageStream load(const std::string& path) {
    MessageStream s;
    std::string err;
    if (!load_stream(path, s, &err)) throw std::runtime_error(err);
    return s;
}

template <class T>
py::array_t<T> to_numpy(std::vector<T>&& v) {
    auto* heap = new std::vector<T>(std::move(v));
    py::capsule owner(heap, [](void* p) { delete static_cast<std::vector<T>*>(p); });
    return py::array_t<T>(static_cast<py::ssize_t>(heap->size()), heap->data(), owner);
}

class PyBook {
public:
    PyBook(Price base, Price tick, std::uint32_t levels, std::uint32_t max_orders)
        : book_(BookConfig{PriceBand{base, tick, levels}, max_orders}) {}

    std::vector<Event> process(const Message& m) {
        out_.clear();
        book_.process(m, out_);
        return {out_.begin(), out_.end()};
    }
    std::vector<Event> add(OrderId id, const std::string& side, Price px, Qty q, const std::string& tif) {
        return process(Message::add(id, parse_side(side), px, q, parse_tif(tif)));
    }
    std::vector<Event> market(OrderId id, const std::string& side, Qty q, const std::string& tif) {
        return process(Message::market(id, parse_side(side), q, parse_tif(tif)));
    }
    std::vector<Event> cancel(OrderId id) { return process(Message::cancel(id)); }
    std::vector<Event> modify(OrderId id, Price px, Qty q) { return process(Message::modify(id, px, q)); }

    std::vector<std::tuple<Price, std::uint64_t, std::uint32_t>> depth(const std::string& side, std::size_t n) const {
        std::vector<std::tuple<Price, std::uint64_t, std::uint32_t>> v;
        for (const auto& l : book_.depth(parse_side(side), n)) v.emplace_back(l.price, l.qty, l.count);
        return v;
    }
    const Book& book() const { return book_; }

private:
    Book book_;
    EventBuffer out_;
};

}  // namespace

PYBIND11_MODULE(pyob, m) {
    m.doc() = "Limit order book matching engine (C++20) - Python bindings";

    py::class_<Event>(m, "Event")
        .def_property_readonly("type", [](const Event& e) { return to_string(e.type); })
        .def_property_readonly("reason", [](const Event& e) { return to_string(e.reason); })
        .def_property_readonly("side", [](const Event& e) { return to_string(e.side); })
        .def_readonly("id", &Event::id)
        .def_readonly("maker_id", &Event::maker_id)
        .def_readonly("price", &Event::price)
        .def_readonly("qty", &Event::qty)
        .def("__repr__", [](const Event& e) {
            std::ostringstream os;
            os << e;
            return os.str();
        });

    py::class_<PyBook>(m, "Book")
        .def(py::init<Price, Price, std::uint32_t, std::uint32_t>(), py::arg("base") = 0, py::arg("tick") = 1,
             py::arg("levels") = 1u << 16, py::arg("max_orders") = 1u << 16)
        .def("add", &PyBook::add, py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"), py::arg("tif") = "GTC")
        .def("market", &PyBook::market, py::arg("id"), py::arg("side"), py::arg("qty"), py::arg("tif") = "IOC")
        .def("cancel", &PyBook::cancel, py::arg("id"))
        .def("modify", &PyBook::modify, py::arg("id"), py::arg("price"), py::arg("qty"))
        .def("best_bid", [](const PyBook& b) { return b.book().best_bid(); })
        .def("best_ask", [](const PyBook& b) { return b.book().best_ask(); })
        .def("depth", &PyBook::depth, py::arg("side"), py::arg("n") = 10)
        .def("find",
             [](const PyBook& b, OrderId id) -> py::object {
                 auto o = b.book().find(id);
                 if (!o) return py::none();
                 return py::make_tuple(to_string(o->side), o->price, o->qty);
             })
        .def_property_readonly("order_count", [](const PyBook& b) { return b.book().order_count(); });

    m.def(
        "load_stream",
        [](const std::string& path) {
            auto s = load(path);
            const std::size_t n = s.msgs.size();
            std::vector<std::uint64_t> id(n);
            std::vector<std::int64_t> price(n);
            std::vector<std::uint32_t> qty(n);
            std::vector<std::uint8_t> type(n), side(n), tif(n);
            for (std::size_t i = 0; i < n; ++i) {
                const auto& msg = s.msgs[i];
                id[i] = msg.id, price[i] = msg.price, qty[i] = msg.qty;
                type[i] = static_cast<std::uint8_t>(msg.type);
                side[i] = static_cast<std::uint8_t>(msg.side);
                tif[i] = static_cast<std::uint8_t>(msg.tif);
            }
            py::dict d;
            d["symbol"] = s.symbol;
            d["band"] = py::make_tuple(s.band.base, s.band.tick, s.band.levels);
            d["id"] = to_numpy(std::move(id));
            d["price"] = to_numpy(std::move(price));
            d["qty"] = to_numpy(std::move(qty));
            d["type"] = to_numpy(std::move(type));  // 0 add, 1 cancel, 2 modify, 3 market
            d["side"] = to_numpy(std::move(side));  // 0 buy, 1 sell
            d["tif"] = to_numpy(std::move(tif));
            d["ts_ns"] = to_numpy(std::move(s.ts_ns));
            return d;
        },
        py::arg("path"), "Load an .obm message stream as numpy arrays.");

    m.def(
        "replay_top_of_book",
        [](const std::string& path, std::size_t every) {
            auto s = load(path);
            Book book(BookConfig{s.band, 1u << 20});
            EventBuffer out;
            std::vector<std::uint64_t> t;
            std::vector<double> bid, ask, bid_qty, ask_qty, traded;
            double vol = 0;
            py::gil_scoped_release nogil;  // pure C++ below; no Python objects touched
            for (std::size_t i = 0; i < s.msgs.size(); ++i) {
                out.clear();
                book.process(s.msgs[i], out);
                for (const auto& e : out)
                    if (e.type == EventType::Trade) vol += e.qty;
                if (every && i % every) continue;
                const auto b = book.depth(Side::Buy, 1), a = book.depth(Side::Sell, 1);
                t.push_back(s.ts_ns.size() == s.msgs.size() ? s.ts_ns[i] : i);
                bid.push_back(b.empty() ? NAN : static_cast<double>(b[0].price));
                ask.push_back(a.empty() ? NAN : static_cast<double>(a[0].price));
                bid_qty.push_back(b.empty() ? 0 : static_cast<double>(b[0].qty));
                ask_qty.push_back(a.empty() ? 0 : static_cast<double>(a[0].qty));
                traded.push_back(vol);
            }
            py::gil_scoped_acquire gil;
            py::dict d;
            d["t"] = to_numpy(std::move(t));
            d["bid"] = to_numpy(std::move(bid));
            d["ask"] = to_numpy(std::move(ask));
            d["bid_qty"] = to_numpy(std::move(bid_qty));
            d["ask_qty"] = to_numpy(std::move(ask_qty));
            d["cum_volume"] = to_numpy(std::move(traded));
            return d;
        },
        py::arg("path"), py::arg("every") = 100,
        "Replay a stream through the engine, sampling top of book every `every` messages.");

    m.def(
        "backtest",
        [](const std::string& path, Qty quote_size, std::int64_t max_inventory, double skew, int half_spread,
           bool improve, std::size_t latency_msgs, double rebate, const std::string& quote_from,
           const std::string& quote_until, std::size_t equity_every) {
            auto s = load(path);
            strategy::MMConfig cfg;
            cfg.quote_size = quote_size;
            cfg.max_inventory = max_inventory;
            cfg.skew_ticks_per_lot = skew;
            cfg.min_half_spread_ticks = half_spread;
            cfg.improve_wide_spread = improve;
            cfg.latency_msgs = latency_msgs;
            cfg.rebate_per_share = rebate;
            cfg.quote_from_ns = hms(quote_from);
            cfg.quote_until_ns = quote_until.empty() ? UINT64_MAX : hms(quote_until);
            cfg.equity_sample_every = equity_every;
            strategy::MMResult r;
            {
                py::gil_scoped_release nogil;
                r = strategy::run_backtest(s, cfg);
            }
            std::vector<std::uint64_t> et;
            std::vector<double> ev;
            for (const auto& [t, v] : r.equity) et.push_back(t), ev.push_back(v);
            std::vector<std::uint64_t> fi;
            std::vector<std::int64_t> fpx, fside;
            std::vector<std::uint32_t> fq;
            std::vector<double> fmid;
            for (const auto& f : r.fill_log) {
                fi.push_back(f.msg_index);
                fpx.push_back(f.price);
                fside.push_back(f.side == Side::Buy ? 1 : -1);
                fq.push_back(f.qty);
                fmid.push_back(f.mid_at_fill);
            }
            py::dict d;
            d["summary"] = r.summary();
            d["pnl"] = r.pnl;
            d["pnl_ex_rebates"] = r.pnl_ex_rebates;
            d["rebates"] = r.rebates;
            d["fills"] = r.fills;
            d["taker_fills"] = r.taker_fills;
            d["inventory"] = r.inventory;
            d["max_abs_inventory"] = r.max_abs_inventory;
            d["max_drawdown"] = r.max_drawdown;
            d["avg_markout"] = r.avg_markout_per_share;
            d["equity_t"] = to_numpy(std::move(et));
            d["equity"] = to_numpy(std::move(ev));
            d["fill_msg_index"] = to_numpy(std::move(fi));
            d["fill_price"] = to_numpy(std::move(fpx));
            d["fill_side"] = to_numpy(std::move(fside));
            d["fill_qty"] = to_numpy(std::move(fq));
            d["fill_mid"] = to_numpy(std::move(fmid));
            return d;
        },
        py::arg("path"), py::arg("quote_size") = 100, py::arg("max_inventory") = 500, py::arg("skew") = 1.0,
        py::arg("half_spread") = 0, py::arg("improve") = true, py::arg("latency_msgs") = 0, py::arg("rebate") = 0.002,
        py::arg("quote_from") = "", py::arg("quote_until") = "", py::arg("equity_every") = 1000,
        "Run the market-making backtest; returns summary stats and numpy series.");
}
