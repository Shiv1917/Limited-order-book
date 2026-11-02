"""Smoke tests for the Python bindings. Run: PYTHONPATH=build python python/tests/test_pyob.py"""
import pyob


def test_matching():
    b = pyob.Book(base=0, tick=1, levels=1000)
    assert [e.type for e in b.add(1, "sell", 101, 10)] == ["ACK"]
    b.add(2, "sell", 102, 10)
    ev = b.add(3, "buy", 102, 15, tif="IOC")
    trades = [e for e in ev if e.type == "TRADE"]
    assert [(t.maker_id, t.price, t.qty) for t in trades] == [(1, 101, 10), (2, 102, 5)]
    assert b.best_ask() == 102 and b.best_bid() is None
    assert b.depth("sell") == [(102, 5, 1)]
    assert b.find(2) == ("SELL", 102, 5)
    assert b.cancel(99)[0].reason == "unknown-order"
    assert b.order_count == 1


if __name__ == "__main__":
    test_matching()
    print("pyob ok")
