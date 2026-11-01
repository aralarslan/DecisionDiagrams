#ifndef GRAPHREL_SRC_LBL_STATE_POOL_HPP
#define GRAPHREL_SRC_LBL_STATE_POOL_HPP

#include "common/decomp_state.hpp"
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace teddy::graphrel {

/**
 * @brief Pool allocator for subgraph objects
 *
 * Inspired by DecisionDiagrams' node_pool, this allocator pre-allocates
 * contiguous blocks of subgraph objects to avoid per-object heap
 * allocations. Uses placement new for construction and maintains a free
 * list for recycled states.
 */
class state_pool {
public:
  using state_t = decomp_state;

public:
  /**
   * @brief Construct pool with initial and overflow sizes
   * @param mainPoolSize Initial number of states to allocate
   * @param extraPoolSize Size of additional pools when main pool is exhausted
   */
  state_pool(std::size_t mainPoolSize, std::size_t extraPoolSize);

  state_pool(state_pool &&other) noexcept;
  ~state_pool();

  state_pool(const state_pool &) = delete;
  auto operator=(const state_pool &) = delete;
  auto operator=(state_pool &&) = delete;

  /**
   * @brief Get number of available states in pool
   */
  [[nodiscard]] std::size_t get_available_count() const;

  /**
   * @brief Get main pool size
   */
  [[nodiscard]] std::size_t get_main_pool_size() const;

  /**
   * @brief Create a new state using placement new
   * @param args Arguments to forward to subgraph constructor
   * @return Pointer to constructed state
   */
  template <class... Args> [[nodiscard]] state_t *create(Args &&...args);

  /**
   * @brief Return a state to the pool (add to free list)
   * @param state State to recycle
   */
  void destroy(state_t *state);

private:
  /// Allocate a new pool when the current one is exhausted.
  void grow();

  static void assign_from(state_t &dst, const state_t &src) { dst = src; }

  struct pool_item {
    state_t *pool_;
    pool_item *next_;
  };

  /**
   * @brief Allocate a new pool block
   * @param size Number of states in pool
   * @param next Next pool in linked list
   * @return New pool item
   */
  [[nodiscard]] static pool_item *allocate_pool(std::size_t size,
                                                pool_item *next);

  /**
   * @brief Deallocate a pool block
   * @param pool Pool to deallocate
   * @param lastNode Last node in pool (for bounds checking)
   * @return Next pool in list
   */
  static pool_item *deallocate_pool(pool_item *pool, state_t *lastNode);

private:
  /**
   * @brief Get the end pointer of the current pool
   * @return Pointer to one past the last node in current pool
   */
  [[nodiscard]] state_t *get_current_pool_end() const;

private:
  pool_item *pools_;
  state_t *nextPoolNode_;
  state_t *freeNodes_; // Free list head (recycled states)
  std::size_t mainPoolSize_;
  std::size_t extraPoolSize_;
  std::size_t availableNodeCount_;
};

// Implementation

// Block sizes are clamped to >= 1: an empty block would make create() write
// past its end.
inline state_pool::state_pool(std::size_t mainPoolSize,
                              std::size_t extraPoolSize)
    : pools_(allocate_pool(mainPoolSize > 0 ? mainPoolSize : 1, nullptr)),
      nextPoolNode_(pools_->pool_), freeNodes_(nullptr),
      mainPoolSize_(mainPoolSize > 0 ? mainPoolSize : 1),
      extraPoolSize_(extraPoolSize > 0 ? extraPoolSize : 1),
      availableNodeCount_(mainPoolSize_) {}

inline state_pool::state_pool(state_pool &&other) noexcept
    : pools_(std::exchange(other.pools_, nullptr)),
      nextPoolNode_(std::exchange(other.nextPoolNode_, nullptr)),
      freeNodes_(std::exchange(other.freeNodes_, nullptr)),
      mainPoolSize_(std::exchange(other.mainPoolSize_, 0)),
      extraPoolSize_(std::exchange(other.extraPoolSize_, 0)),
      availableNodeCount_(std::exchange(other.availableNodeCount_, 0)) {}

inline state_pool::~state_pool() {
  // Deallocate current pool
  pools_ = deallocate_pool(pools_, nextPoolNode_);

  // Deallocate extra pools
  while (pools_ && pools_->next_) {
    state_t *const lastNode = pools_->pool_ + extraPoolSize_;
    pools_ = deallocate_pool(pools_, lastNode);
  }

  // Deallocate main pool
  if (pools_) {
    state_t *const lastNode = pools_->pool_ + mainPoolSize_;
    pools_ = deallocate_pool(pools_, lastNode);
  }
}

inline std::size_t state_pool::get_available_count() const {
  return availableNodeCount_;
}

inline std::size_t state_pool::get_main_pool_size() const {
  return mainPoolSize_;
}

// Exception safety and allocation reuse:
// - A recycled state is overwritten by assignment, so its vectors keep their
//   capacity (no free + malloc per state). If the assignment or the
//   construction of the source throws, the node is still a valid object on
//   the free list and nothing leaks or is destroyed twice.
// - A fresh slot only counts as constructed (nextPoolNode_ advances) after
//   its constructor returns, so the destructor never destroys a slot that
//   was not constructed.
template <class... Args>
inline state_pool::state_t *state_pool::create(Args &&...args) {
  if (freeNodes_) {
    state_t *const state = freeNodes_;
    state_t *const next_free = state->next_;
    if constexpr (sizeof...(Args) == 1 &&
                  (std::is_same_v<std::remove_cvref_t<Args>, state_t> && ...)) {
      assign_from(*state, std::forward<Args>(args)...); // reuses capacity
    } else {
      *state = state_t(std::forward<Args>(args)...);
    }
    state->next_ = nullptr;
    freeNodes_ = next_free;
    --availableNodeCount_;
    return state;
  }

  if (nextPoolNode_ >= get_current_pool_end()) {
    grow();
  }
  state_t *const state = nextPoolNode_;
  ::new (state) state_t(std::forward<Args>(args)...);
  ++nextPoolNode_;
  --availableNodeCount_;
  return state;
}

inline void state_pool::destroy(state_pool::state_t *state) {
  ++availableNodeCount_;
  // Add to free list
  state->next_ = freeNodes_;
  freeNodes_ = state;
}

inline void state_pool::grow() {
  pools_ = allocate_pool(extraPoolSize_, pools_);
  nextPoolNode_ = pools_->pool_;
  availableNodeCount_ += extraPoolSize_;
}

inline state_pool::pool_item *
state_pool::allocate_pool(std::size_t size, pool_item *next) {
  auto *pool = static_cast<state_t *>(std::malloc(size * sizeof(state_t)));
  if (!pool) {
    throw std::bad_alloc();
  }
  try {
    return new pool_item{pool, next};
  } catch (...) {
    std::free(pool);
    throw;
  }
}

inline state_pool::pool_item *
state_pool::deallocate_pool(pool_item *pool, state_t *lastNode) {
  if (!pool) {
    return nullptr;
  }
  state_t *state = pool->pool_;
  while (state < lastNode) {
    state->~state_t();
    ++state;
  }
  pool_item *next = pool->next_;
  std::free(pool->pool_);
  delete pool;
  return next;
}

inline state_pool::state_t *
state_pool::get_current_pool_end() const {
  if (!pools_) {
    return nullptr;
  }
  // Current pool is always the first in the list (grow() prepends)
  // If it has a next, it's an extra pool, otherwise it's the main pool
  std::size_t currentPoolSize =
      (pools_->next_ != nullptr) ? extraPoolSize_ : mainPoolSize_;
  return pools_->pool_ + currentPoolSize;
}

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_LBL_STATE_POOL_HPP
