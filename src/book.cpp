// Explicit instantiations of every book stage, so tests/bench/tools don't
// each re-instantiate the templates.
#include "ob/book.hpp"

namespace ob {
template class BasicBook<OrderLoose, HeapStorage, StdIdMap>;
template class BasicBook<OrderLoose, PoolStorage, StdIdMap>;
template class BasicBook<OrderLoose, PoolStorage, FlatIdMap>;
template class BasicBook<OrderPacked, PoolStorage, FlatIdMap>;
}  // namespace ob
