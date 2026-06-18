/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file functional.h
 * @brief Pool-backed type-erased callables: copyable @c vlink::Function and move-only @c vlink::MoveFunction.
 *
 * @details
 * @c vlink::Function tracks the public surface of @c std::function while @c vlink::MoveFunction
 * tracks @c std::move_only_function.  Both store the target in a configurable small-buffer
 * region (64 bytes by default) and spill to @c vlink::MemoryPool::global_instance when the
 * target does not fit.  This eliminates the heap-allocation surprise common to @c std::function
 * on hot paths while still permitting arbitrarily large closures.
 *
 * @par Comparison vs the standard library
 *
 * | Property             | std::function    | std::move_only_function | vlink::Function    | vlink::MoveFunction |
 * | -------------------- | ---------------- | ----------------------- | ------------------ | ------------------- |
 * | Copyable             | yes              | no                      | yes                | no                  |
 * | Movable              | yes              | yes                     | yes                | yes                 |
 * | Default SBO size     | ~16-24 B (impl)  | ~16-24 B (impl)         | 64 B (tunable)     | 64 B (tunable)      |
 * | Spill allocator      | ::operator new   | ::operator new          | @c MemoryPool tier | @c MemoryPool tier  |
 * | Empty-call behaviour | throws bad_func  | UB                      | throws bad_func    | throws bad_func     |
 * | RTTI inspection      | @c target / type | not provided            | @c target / type   | @c target / type    |
 *
 * @par Storage predicate (kIsInline)
 * A target @c FunctorT is held inline only when all three hold simultaneously:
 * @c sizeof(FunctorT) @c <= @c SboSizeT, @c alignof(FunctorT) @c <= @c alignof(std::max_align_t)
 * and @c FunctorT is nothrow move-constructible.  Anything else goes to the heap path through
 * @c MemoryPool::global_instance; the original @c sizeof / @c alignof values are passed back to
 * @c deallocate so the block returns to its source tier.  This mirrors libstdc++'s
 * @c __stored_locally rule.
 *
 * @par SBO sizing
 * Both wrappers expose @c SboSizeT as the second non-type template argument.  Convenience
 * aliases @c vlink::LargeFunction / @c vlink::LargeMoveFunction pick @c 256 bytes; the bound
 * @c SboSizeT @c >= @c sizeof(void*) is enforced by @c static_assert so the heap-fallback
 * pointer always fits in the SBO.  Distinct @c SboSizeT instantiations are distinct types but
 * may still convert through the generic functor path -- a @c Function<Sig, @c 64> serves as a
 * regular callable target for @c Function<Sig, @c 256>.
 *
 * @par Empty-state propagation
 * Constructing or assigning from any of these sources yields an empty wrapper with no vtable
 * and no allocation:
 *  - an empty function-wrapper source (@c std::function, @c vlink::Function, and -- when the
 *    target is @c MoveFunction -- @c std::move_only_function / @c vlink::MoveFunction);
 *  - a null raw function pointer;
 *  - a null pointer-to-member.
 *
 * @par Exception safety
 * Copy operations use copy-and-swap and offer the strong guarantee; @c swap, move construction
 * and move assignment are @c noexcept.  The @c kIsInline predicate guarantees that inline-path
 * moves never throw, while the heap path simply moves a pointer.  Inline construction uses
 * placement-new directly into the SBO; heap construction is wrapped in @c try / @c catch so the
 * pool block is returned on a constructor exception.
 *
 * @par Object-lifetime model
 * Inline targets are placement-new'd into the SBO.  Heap storage holds a @c FunctorT*; the
 * pointer object's lifetime is started explicitly via @c ::new(dst) @c FunctorT*(p) at every
 * site that introduces a new slot, avoiding reliance on the C++20 @c [intro.object]/10
 * implicit-object rule and keeping the code well-defined under C++17.  Subsequent accesses go
 * through @c std::launder to satisfy @c [basic.life]/8.
 *
 * @par RTTI surface
 * When @c __cpp_rtti is defined both wrappers expose @c target_type() and
 * @c target<FunctorT>() with @c std::function semantics.  This is an extension over
 * @c std::move_only_function which omits target inspection.
 *
 * @par Example
 * @code
 *   vlink::Function<int(int)> f = [](int x) { return x * 2; };
 *   int y = f(21);                                              // == 42
 *
 *   auto pkg = std::packaged_task<int()>([] { return 7; });
 *   vlink::MoveFunction<void()> mf = std::move(pkg);            // move-only target
 *   mf();                                                       // runs the task
 *
 *   vlink::LargeMoveFunction<void()> heavy = ...heavy capture...; // 256-byte SBO
 * @endcode
 *
 * @note Only the unqualified @c ReturnT(ArgsT...) signature is specialised; cv / ref /
 *       @c noexcept-qualified function types resolve to the undefined primary template (hard
 *       error).  This header self-defines @c VLINK_ENABLE_BASE_FUNCTIONAL when absent so normal
 *       builds always select the VLink implementation; the standard-library alias block at the
 *       bottom is not exposed via a compile-command opt-out.
 */

#pragma once

#include <functional>

#if !defined(VLINK_ENABLE_BASE_FUNCTIONAL)
#define VLINK_ENABLE_BASE_FUNCTIONAL
#endif

#ifdef VLINK_ENABLE_BASE_FUNCTIONAL
#include <cstddef>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include "./macros.h"
#include "./memory_pool.h"

namespace vlink {

[[maybe_unused]] static constexpr bool kIsSupportMoveFunction = true;

template <typename SignatureT, size_t SboSizeT = 64U>
class Function;

template <typename SignatureT, size_t SboSizeT = 64U>
class MoveFunction;

template <typename SignatureT>
using LargeFunction = Function<SignatureT, 256U>;

template <typename SignatureT>
using LargeMoveFunction = MoveFunction<SignatureT, 256U>;

template <typename SignatureT>
using function = Function<SignatureT>;

template <typename SignatureT>
using move_only_function = MoveFunction<SignatureT>;

namespace detail {

template <typename TypeT>
struct IsStdFunction : std::false_type {};

template <typename SignatureT>
struct IsStdFunction<std::function<SignatureT>> : std::true_type {};

template <typename TypeT>
struct IsVlinkFunction : std::false_type {};

template <typename SignatureT, size_t SboSizeT>
struct IsVlinkFunction<Function<SignatureT, SboSizeT>> : std::true_type {};

template <typename TypeT>
struct IsVlinkMoveFunction : std::false_type {};

template <typename SignatureT, size_t SboSizeT>
struct IsVlinkMoveFunction<MoveFunction<SignatureT, SboSizeT>> : std::true_type {};

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
template <typename TypeT>
struct IsStdMoveOnlyFunction : std::false_type {};

template <typename SignatureT>
struct IsStdMoveOnlyFunction<std::move_only_function<SignatureT>> : std::true_type {};
#endif

[[noreturn]] VLINK_EXPORT void throw_bad_function_call();

}  // namespace detail

/**
 * @class Function
 * @brief Copyable type-erased callable analogue of @c std::function with a tunable SBO and pool spill.
 *
 * @details
 * Holds any copy-constructible target invocable as @c ReturnT(ArgsT...).  Default-instantiated
 * @c Function uses a 64-byte SBO; @c Function<Sig, @c N> picks an arbitrary inline budget.
 * @c operator() is @c const (matching @c std::function 's logical-const convention -- the
 * placement-newed target itself is non-const, so the @c const_cast pattern inside the invoker is
 * well-defined).  Empty construction, @c nullptr-from-nullptr, and the @c std::bad_function_call
 * empty-call exception all match @c std::function.  Copy operations follow copy-and-swap and
 * provide the strong exception guarantee; move construction, move assignment and @c swap are
 * @c noexcept.
 *
 * @tparam ReturnT  Result type of the invocable target.
 * @tparam ArgsT    Argument types of the invocable target.
 * @tparam SboSizeT Inline storage budget in bytes; must be @c >= @c sizeof(void*).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
class Function<ReturnT(ArgsT...), SboSizeT> {
  static_assert(SboSizeT >= sizeof(void*),
                "Function: SboSizeT must be at least sizeof(void*) so the heap-fallback "
                "pointer can fit in the inline storage.");

 public:
  /**
   * @brief Inline storage budget in bytes before the heap-pool fallback kicks in.
   */
  static constexpr size_t kSboSize = SboSizeT;

  /**
   * @brief Result type alias matching @c std::function.
   */
  using result_type = ReturnT;

  /**
   * @brief Constructs an empty wrapper with no stored target.
   */
  Function() noexcept = default;

  /**
   * @brief Constructs an empty wrapper from a null pointer literal.
   */
  Function(std::nullptr_t) noexcept;  // NOLINT(google-explicit-constructor)

  /**
   * @brief Copy constructor; deep-copies the stored target or stays empty.
   */
  Function(const Function& other);

  /**
   * @brief Move constructor; transfers the target and leaves @p other empty.
   */
  Function(Function&& other) noexcept;

  /**
   * @brief Constructs from any compatible copy-constructible callable.
   *
   * @details
   * Null function pointers, null pointer-to-members and empty function wrappers all yield an
   * empty result rather than storing a tombstone callable.
   *
   * @tparam FunctorT  Callable type compatible with @c std::invoke.
   */
  template <typename FunctorT, typename DecayFunctorT = std::decay_t<FunctorT>,
            // NOLINTNEXTLINE(modernize-use-constraints)
            typename = std::enable_if_t<std::conjunction_v<std::negation<std::is_same<DecayFunctorT, Function>>,
                                                           std::negation<std::is_same<DecayFunctorT, std::nullptr_t>>,
                                                           std::is_invocable_r<ReturnT, DecayFunctorT&, ArgsT...>,
                                                           std::is_copy_constructible<DecayFunctorT>>>>
  Function(FunctorT&& f);  // NOLINT(google-explicit-constructor)

  /**
   * @brief Copy assignment; replaces the target with a deep copy of @p other.
   */
  Function& operator=(const Function& other);

  /**
   * @brief Move assignment; replaces the target with @p other 's target.
   */
  Function& operator=(Function&& other) noexcept;

  /**
   * @brief Clears the wrapper to the empty state.
   */
  Function& operator=(std::nullptr_t) noexcept;

  /**
   * @brief Replaces the target with a compatible copy-constructible callable.
   *
   * @tparam FunctorT  Callable type compatible with @c std::invoke.
   */
  template <typename FunctorT, typename DecayFunctorT = std::decay_t<FunctorT>,
            // NOLINTNEXTLINE(modernize-use-constraints)
            typename = std::enable_if_t<std::conjunction_v<std::negation<std::is_same<DecayFunctorT, Function>>,
                                                           std::negation<std::is_same<DecayFunctorT, std::nullptr_t>>,
                                                           std::is_invocable_r<ReturnT, DecayFunctorT&, ArgsT...>,
                                                           std::is_copy_constructible<DecayFunctorT>>>>
  Function& operator=(FunctorT&& f);

  /**
   * @brief Destructor; releases inline or pooled storage.
   */
  ~Function();

  /**
   * @brief Invokes the stored callable.
   *
   * @throws std::bad_function_call if the wrapper is empty.
   */
  ReturnT operator()(ArgsT... args) const;

  /**
   * @brief Reports whether a callable target is currently stored.
   */
  explicit operator bool() const noexcept;

#if defined(__cpp_rtti)
  /**
   * @brief Returns the dynamic type of the stored target, or @c typeid(void) when empty.
   */
  const std::type_info& target_type() const noexcept;

  /**
   * @brief Returns the stored target when its dynamic type is exactly @c FunctorT.
   *
   * @tparam FunctorT  Expected target type.
   */
  template <typename FunctorT>
  FunctorT* target() noexcept;

  /**
   * @brief Const overload of @c target<FunctorT>.
   *
   * @tparam FunctorT  Expected target type.
   */
  template <typename FunctorT>
  const FunctorT* target() const noexcept;
#endif

  /**
   * @brief Swaps the stored target with @p other.
   */
  void swap(Function& other) noexcept;

 private:
  template <typename FunctorT>
  static constexpr bool kIsPointerLike = std::is_pointer_v<FunctorT> || std::is_member_pointer_v<FunctorT> ||
                                         std::is_function_v<std::remove_pointer_t<FunctorT>>;

  template <typename FunctorT>
  static constexpr bool kIsFunctionWrapper =
      detail::IsStdFunction<FunctorT>::value || detail::IsVlinkFunction<FunctorT>::value;

  template <typename FunctorT>
  static constexpr bool kIsInline = sizeof(FunctorT) <= kSboSize && alignof(FunctorT) <= alignof(std::max_align_t) &&
                                    std::is_nothrow_move_constructible_v<FunctorT>;

  struct VTable final {
    ReturnT (*invoke)(const void* storage, ArgsT... args);

    void (*copy_construct)(void* dst, const void* src);

    void (*move_construct)(void* dst, void* src) noexcept;

    void (*destroy)(void* storage) noexcept;

#if defined(__cpp_rtti)
    const std::type_info& (*target_type)() noexcept;

    void* (*target)(void* storage) noexcept;

    const void* (*target_const)(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  struct InlineVTable {
    static ReturnT invoke(const void* storage, ArgsT... args);

    static void copy_construct(void* dst, const void* src);

    static void move_construct(void* dst, void* src) noexcept;

    static void destroy(void* storage) noexcept;

#if defined(__cpp_rtti)
    static const std::type_info& target_type() noexcept;

    static void* target(void* storage) noexcept;

    static const void* target_const(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  struct HeapVTable {
    static ReturnT invoke(const void* storage, ArgsT... args);

    static void copy_construct(void* dst, const void* src);

    static void move_construct(void* dst, void* src) noexcept;

    static void destroy(void* storage) noexcept;

#if defined(__cpp_rtti)
    static const std::type_info& target_type() noexcept;

    static void* target(void* storage) noexcept;

    static const void* target_const(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  static const VTable* get_vtable() noexcept;

  template <typename FunctorT, typename SourceT>
  void construct_from(SourceT&& src);

  void copy_from(const Function& other);

  void move_from(Function&& other) noexcept;

  void reset() noexcept;

  alignas(std::max_align_t) std::byte storage_[SboSizeT]{};

  const VTable* vtable_{nullptr};
};

/**
 * @brief Free-function swap; defers to the member @c swap of @p lhs.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
void swap(Function<ReturnT(ArgsT...), SboSizeT>& lhs, Function<ReturnT(ArgsT...), SboSizeT>& rhs) noexcept;

/**
 * @brief Equality with @c nullptr; @c true when @p cb has no stored target.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator==(const Function<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept;

/**
 * @brief Commutative overload of @c operator==(Function, nullptr_t).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator==(std::nullptr_t, const Function<ReturnT(ArgsT...), SboSizeT>& cb) noexcept;

/**
 * @brief Inequality with @c nullptr; @c true when @p cb stores a target.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator!=(const Function<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept;

/**
 * @brief Commutative overload of @c operator!=(Function, nullptr_t).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator!=(std::nullptr_t, const Function<ReturnT(ArgsT...), SboSizeT>& cb) noexcept;

/**
 * @class MoveFunction
 * @brief Move-only type-erased callable analogue of @c std::move_only_function with pool spill.
 *
 * @details
 * Holds any move-constructible target invocable as @c ReturnT(ArgsT...).  Shares the SBO and
 * @c MemoryPool fallback used by @c Function.  Diverges from @c std::move_only_function in two
 * deliberate ways and extends it in one: empty calls throw @c std::bad_function_call instead of
 * being UB; only the unqualified signature is specialised (cv / ref / @c noexcept forms hard-
 * fail); and RTTI inspection through @c target_type / @c target is provided.  @c operator() is
 * non-@c const so mutating targets such as @c std::packaged_task work directly without the
 * logical-const dance.  Move operations and @c swap are @c noexcept; copy is deleted.
 *
 * @tparam ReturnT  Result type of the invocable target.
 * @tparam ArgsT    Argument types of the invocable target.
 * @tparam SboSizeT Inline storage budget in bytes; must be @c >= @c sizeof(void*).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
class MoveFunction<ReturnT(ArgsT...), SboSizeT> {
  static_assert(SboSizeT >= sizeof(void*),
                "MoveFunction: SboSizeT must be at least sizeof(void*) so the "
                "heap-fallback pointer can fit in the inline storage.");

 public:
  /**
   * @brief Inline storage budget in bytes before the heap-pool fallback kicks in.
   */
  static constexpr size_t kSboSize = SboSizeT;

  /**
   * @brief Result type alias matching @c std::move_only_function.
   */
  using result_type = ReturnT;

  /**
   * @brief Constructs an empty wrapper with no stored target.
   */
  MoveFunction() noexcept = default;

  /**
   * @brief Constructs an empty wrapper from a null pointer literal.
   */
  MoveFunction(std::nullptr_t) noexcept;  // NOLINT(google-explicit-constructor)

  MoveFunction(const MoveFunction&) = delete;

  MoveFunction& operator=(const MoveFunction&) = delete;

  /**
   * @brief Move constructor; transfers the target and leaves @p other empty.
   */
  MoveFunction(MoveFunction&& other) noexcept;

  /**
   * @brief Constructs from any compatible move-constructible callable.
   *
   * @details
   * Null function pointers, null pointer-to-members and empty function wrappers all yield an
   * empty result rather than storing a tombstone callable.
   *
   * @tparam FunctorT  Callable type compatible with @c std::invoke.
   */
  template <typename FunctorT, typename DecayFunctorT = std::decay_t<FunctorT>,
            // NOLINTNEXTLINE(modernize-use-constraints)
            typename = std::enable_if_t<std::conjunction_v<std::negation<std::is_same<DecayFunctorT, MoveFunction>>,
                                                           std::negation<std::is_same<DecayFunctorT, std::nullptr_t>>,
                                                           std::is_invocable_r<ReturnT, DecayFunctorT&, ArgsT...>,
                                                           std::is_constructible<DecayFunctorT, FunctorT>,
                                                           std::is_move_constructible<DecayFunctorT>>>>
  MoveFunction(FunctorT&& f);  // NOLINT(google-explicit-constructor)

  /**
   * @brief Move assignment; replaces the target with @p other 's target.
   */
  MoveFunction& operator=(MoveFunction&& other) noexcept;

  /**
   * @brief Clears the wrapper to the empty state.
   */
  MoveFunction& operator=(std::nullptr_t) noexcept;

  /**
   * @brief Replaces the target with a compatible move-constructible callable.
   *
   * @tparam FunctorT  Callable type compatible with @c std::invoke.
   */
  template <typename FunctorT, typename DecayFunctorT = std::decay_t<FunctorT>,
            // NOLINTNEXTLINE(modernize-use-constraints)
            typename = std::enable_if_t<std::conjunction_v<std::negation<std::is_same<DecayFunctorT, MoveFunction>>,
                                                           std::negation<std::is_same<DecayFunctorT, std::nullptr_t>>,
                                                           std::is_invocable_r<ReturnT, DecayFunctorT&, ArgsT...>,
                                                           std::is_constructible<DecayFunctorT, FunctorT>,
                                                           std::is_move_constructible<DecayFunctorT>>>>
  MoveFunction& operator=(FunctorT&& f);

  /**
   * @brief Destructor; releases inline or pooled storage.
   */
  ~MoveFunction();

  /**
   * @brief Invokes the stored callable.
   *
   * @throws std::bad_function_call if the wrapper is empty.
   */
  ReturnT operator()(ArgsT... args);

  /**
   * @brief Reports whether a callable target is currently stored.
   */
  explicit operator bool() const noexcept;

#if defined(__cpp_rtti)
  /**
   * @brief Returns the dynamic type of the stored target, or @c typeid(void) when empty.
   */
  const std::type_info& target_type() const noexcept;

  /**
   * @brief Returns the stored target when its dynamic type is exactly @c FunctorT.
   *
   * @tparam FunctorT  Expected target type.
   */
  template <typename FunctorT>
  FunctorT* target() noexcept;

  /**
   * @brief Const overload of @c target<FunctorT>.
   *
   * @tparam FunctorT  Expected target type.
   */
  template <typename FunctorT>
  const FunctorT* target() const noexcept;
#endif

  /**
   * @brief Swaps the stored target with @p other.
   */
  void swap(MoveFunction& other) noexcept;

 private:
  template <typename FunctorT>
  static constexpr bool kIsPointerLike = std::is_pointer_v<FunctorT> || std::is_member_pointer_v<FunctorT> ||
                                         std::is_function_v<std::remove_pointer_t<FunctorT>>;

  template <typename FunctorT>
  static constexpr bool kIsFunctionWrapper =
      detail::IsStdFunction<FunctorT>::value || detail::IsVlinkFunction<FunctorT>::value ||
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
      detail::IsVlinkMoveFunction<FunctorT>::value || detail::IsStdMoveOnlyFunction<FunctorT>::value;
#else
      detail::IsVlinkMoveFunction<FunctorT>::value;
#endif

  template <typename FunctorT>
  static constexpr bool kIsInline = sizeof(FunctorT) <= kSboSize && alignof(FunctorT) <= alignof(std::max_align_t) &&
                                    std::is_nothrow_move_constructible_v<FunctorT>;

  struct VTable final {
    ReturnT (*invoke)(void* storage, ArgsT... args);

    void (*move_construct)(void* dst, void* src) noexcept;

    void (*destroy)(void* storage) noexcept;

#if defined(__cpp_rtti)
    const std::type_info& (*target_type)() noexcept;

    void* (*target)(void* storage) noexcept;

    const void* (*target_const)(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  struct InlineVTable {
    static ReturnT invoke(void* storage, ArgsT... args);

    static void move_construct(void* dst, void* src) noexcept;

    static void destroy(void* storage) noexcept;

#if defined(__cpp_rtti)
    static const std::type_info& target_type() noexcept;

    static void* target(void* storage) noexcept;

    static const void* target_const(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  struct HeapVTable {
    static ReturnT invoke(void* storage, ArgsT... args);

    static void move_construct(void* dst, void* src) noexcept;

    static void destroy(void* storage) noexcept;

#if defined(__cpp_rtti)
    static const std::type_info& target_type() noexcept;

    static void* target(void* storage) noexcept;

    static const void* target_const(const void* storage) noexcept;
#endif
  };

  template <typename FunctorT>
  static const VTable* get_vtable() noexcept;

  template <typename FunctorT, typename SourceT>
  void construct_from(SourceT&& src);

  void move_from(MoveFunction&& other) noexcept;

  void reset() noexcept;

  alignas(std::max_align_t) std::byte storage_[SboSizeT]{};

  const VTable* vtable_{nullptr};
};

/**
 * @brief Free-function swap; defers to the member @c swap of @p lhs.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
void swap(MoveFunction<ReturnT(ArgsT...), SboSizeT>& lhs, MoveFunction<ReturnT(ArgsT...), SboSizeT>& rhs) noexcept;

/**
 * @brief Equality with @c nullptr; @c true when @p cb has no stored target.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator==(const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept;

/**
 * @brief Commutative overload of @c operator==(MoveFunction, nullptr_t).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator==(std::nullptr_t, const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb) noexcept;

/**
 * @brief Inequality with @c nullptr; @c true when @p cb stores a target.
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator!=(const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept;

/**
 * @brief Commutative overload of @c operator!=(MoveFunction, nullptr_t).
 */
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
bool operator!=(std::nullptr_t, const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb) noexcept;

////////////////////////////////////////////////////////////////
/// Details
////////////////////////////////////////////////////////////////

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>::Function(std::nullptr_t) noexcept {}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>::Function(const Function& other) {
  copy_from(other);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>::Function(Function&& other) noexcept {
  move_from(std::move(other));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename DecayFunctorT, typename>
inline Function<ReturnT(ArgsT...), SboSizeT>::Function(FunctorT&& f) {
  if constexpr (kIsFunctionWrapper<DecayFunctorT>) {
    if VUNLIKELY (!f) {
      return;
    }
  } else if constexpr (kIsPointerLike<DecayFunctorT>) {
    if constexpr (std::is_pointer_v<std::remove_reference_t<FunctorT>> ||
                  std::is_member_pointer_v<std::remove_reference_t<FunctorT>>) {
      if VUNLIKELY (f == nullptr) {
        return;
      }
    }
  }

  construct_from<DecayFunctorT>(std::forward<FunctorT>(f));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
// NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
inline Function<ReturnT(ArgsT...), SboSizeT>& Function<ReturnT(ArgsT...), SboSizeT>::operator=(const Function& other) {
  if VLIKELY (this != &other) {
    Function tmp(other);
    swap(tmp);
  }

  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>& Function<ReturnT(ArgsT...), SboSizeT>::operator=(
    Function&& other) noexcept {
  if VLIKELY (this != &other) {
    reset();
    move_from(std::move(other));
  }

  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>& Function<ReturnT(ArgsT...), SboSizeT>::operator=(
    std::nullptr_t) noexcept {
  reset();
  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename DecayFunctorT, typename>
inline Function<ReturnT(ArgsT...), SboSizeT>& Function<ReturnT(ArgsT...), SboSizeT>::operator=(FunctorT&& f) {
  Function tmp(std::forward<FunctorT>(f));
  swap(tmp);
  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>::~Function() {
  reset();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline ReturnT Function<ReturnT(ArgsT...), SboSizeT>::operator()(ArgsT... args) const {
  if VUNLIKELY (vtable_ == nullptr) {
    detail::throw_bad_function_call();
  }

  return vtable_->invoke(&storage_, std::forward<ArgsT>(args)...);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline Function<ReturnT(ArgsT...), SboSizeT>::operator bool() const noexcept {
  return vtable_ != nullptr;
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline const std::type_info& Function<ReturnT(ArgsT...), SboSizeT>::target_type() const noexcept {
  if VLIKELY (vtable_ != nullptr) {
    return vtable_->target_type();
  }

  return typeid(void);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline FunctorT* Function<ReturnT(ArgsT...), SboSizeT>::target() noexcept {
  if constexpr (std::is_object_v<FunctorT>) {
    if VLIKELY (vtable_ != nullptr && typeid(FunctorT) == target_type()) {
      return static_cast<FunctorT*>(vtable_->target(&storage_));
    }
  }

  return nullptr;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const FunctorT* Function<ReturnT(ArgsT...), SboSizeT>::target() const noexcept {
  if constexpr (std::is_object_v<FunctorT>) {
    if VLIKELY (vtable_ != nullptr && typeid(FunctorT) == target_type()) {
      return static_cast<const FunctorT*>(vtable_->target_const(&storage_));
    }
  }

  return nullptr;
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::swap(Function& other) noexcept {
  if VUNLIKELY (this == &other) {
    return;
  }

  Function tmp(std::move(*this));
  *this = std::move(other);
  other = std::move(tmp);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline ReturnT Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::invoke(const void* storage,
                                                                                     ArgsT... args) {
  auto* f = std::launder(reinterpret_cast<FunctorT*>(const_cast<void*>(storage)));

  if constexpr (std::is_void_v<ReturnT>) {
    std::invoke(*f, std::forward<ArgsT>(args)...);
  } else {
    return std::invoke(*f, std::forward<ArgsT>(args)...);
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::copy_construct(void* dst, const void* src) {
  const auto* src_f = std::launder(reinterpret_cast<const FunctorT*>(src));
  ::new (dst) FunctorT(*src_f);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::move_construct(void* dst,
                                                                                          void* src) noexcept {
  auto* src_f = std::launder(reinterpret_cast<FunctorT*>(src));
  ::new (dst) FunctorT(std::move(*src_f));
  src_f->~FunctorT();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::destroy(void* storage) noexcept {
  auto* f = std::launder(reinterpret_cast<FunctorT*>(storage));
  f->~FunctorT();
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const std::type_info& Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target_type() noexcept {
  return typeid(FunctorT);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void* Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target(void* storage) noexcept {
  return storage;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const void* Function<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target_const(
    const void* storage) noexcept {
  return storage;
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline ReturnT Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::invoke(const void* storage, ArgsT... args) {
  FunctorT* f = *std::launder(static_cast<FunctorT* const*>(storage));

  if constexpr (std::is_void_v<ReturnT>) {
    std::invoke(*f, std::forward<ArgsT>(args)...);
  } else {
    return std::invoke(*f, std::forward<ArgsT>(args)...);
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::copy_construct(void* dst, const void* src) {
  FunctorT* src_f = *std::launder(static_cast<FunctorT* const*>(src));

  auto& pool = MemoryPool::global_instance();
  void* mem = pool.allocate(sizeof(FunctorT), alignof(FunctorT));

  if VUNLIKELY (mem == nullptr) {
    throw std::bad_alloc();
  }

  try {
    auto* new_f = ::new (mem) FunctorT(*src_f);
    ::new (dst) FunctorT*(new_f);
  } catch (...) {
    pool.deallocate(mem, sizeof(FunctorT), alignof(FunctorT));
    throw;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::move_construct(void* dst, void* src) noexcept {
  FunctorT** src_slot = std::launder(static_cast<FunctorT**>(src));
  FunctorT* src_f = *src_slot;
  ::new (dst) FunctorT*(src_f);
  *src_slot = nullptr;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::destroy(void* storage) noexcept {
  FunctorT** slot = std::launder(static_cast<FunctorT**>(storage));
  FunctorT* f = *slot;

  if VLIKELY (f != nullptr) {
    f->~FunctorT();

    auto& pool = MemoryPool::global_instance();
    pool.deallocate(f, sizeof(FunctorT), alignof(FunctorT));

    *slot = nullptr;
  }
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const std::type_info& Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target_type() noexcept {
  return typeid(FunctorT);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void* Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target(void* storage) noexcept {
  return *std::launder(static_cast<FunctorT**>(storage));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const void* Function<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target_const(
    const void* storage) noexcept {
  return *std::launder(static_cast<FunctorT* const*>(storage));
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const typename Function<ReturnT(ArgsT...), SboSizeT>::VTable*
Function<ReturnT(ArgsT...), SboSizeT>::get_vtable() noexcept {
  if constexpr (kIsInline<FunctorT>) {
    static constexpr VTable kVTable = {
        &InlineVTable<FunctorT>::invoke,         &InlineVTable<FunctorT>::copy_construct,
        &InlineVTable<FunctorT>::move_construct, &InlineVTable<FunctorT>::destroy,
#if defined(__cpp_rtti)
        &InlineVTable<FunctorT>::target_type,    &InlineVTable<FunctorT>::target,
        &InlineVTable<FunctorT>::target_const,
#endif
    };
    return &kVTable;
  } else {
    static constexpr VTable kVTable = {
        &HeapVTable<FunctorT>::invoke,         &HeapVTable<FunctorT>::copy_construct,
        &HeapVTable<FunctorT>::move_construct, &HeapVTable<FunctorT>::destroy,
#if defined(__cpp_rtti)
        &HeapVTable<FunctorT>::target_type,    &HeapVTable<FunctorT>::target,
        &HeapVTable<FunctorT>::target_const,
#endif
    };
    return &kVTable;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename SourceT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::construct_from(SourceT&& src) {
  if constexpr (kIsInline<FunctorT>) {
    ::new (&storage_) FunctorT(std::forward<SourceT>(src));
  } else {
    auto& pool = MemoryPool::global_instance();
    auto* mem = pool.allocate(sizeof(FunctorT), alignof(FunctorT));

    if VUNLIKELY (mem == nullptr) {
      throw std::bad_alloc();
    }

    try {
      auto* new_f = ::new (mem) FunctorT(std::forward<SourceT>(src));
      ::new (static_cast<void*>(&storage_)) FunctorT*(new_f);
    } catch (...) {
      pool.deallocate(mem, sizeof(FunctorT), alignof(FunctorT));
      throw;
    }
  }

  vtable_ = get_vtable<FunctorT>();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::copy_from(const Function& other) {
  if VLIKELY (other.vtable_ != nullptr) {
    other.vtable_->copy_construct(&storage_, &other.storage_);
    vtable_ = other.vtable_;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::move_from(Function&& other) noexcept {
  if VLIKELY (other.vtable_ != nullptr) {
    other.vtable_->move_construct(&storage_, &other.storage_);
    vtable_ = other.vtable_;
    other.vtable_ = nullptr;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void Function<ReturnT(ArgsT...), SboSizeT>::reset() noexcept {
  if VLIKELY (vtable_ != nullptr) {
    vtable_->destroy(&storage_);
    vtable_ = nullptr;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void swap(Function<ReturnT(ArgsT...), SboSizeT>& lhs, Function<ReturnT(ArgsT...), SboSizeT>& rhs) noexcept {
  lhs.swap(rhs);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator==(const Function<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept {
  return !cb;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator==(std::nullptr_t, const Function<ReturnT(ArgsT...), SboSizeT>& cb) noexcept {
  return !cb;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator!=(const Function<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept {
  return static_cast<bool>(cb);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator!=(std::nullptr_t, const Function<ReturnT(ArgsT...), SboSizeT>& cb) noexcept {
  return static_cast<bool>(cb);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>::MoveFunction(std::nullptr_t) noexcept {}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>::MoveFunction(MoveFunction&& other) noexcept {
  move_from(std::move(other));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename DecayFunctorT, typename>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>::MoveFunction(FunctorT&& f) {
  if constexpr (kIsFunctionWrapper<DecayFunctorT>) {
    if VUNLIKELY (!f) {
      return;
    }
  } else if constexpr (kIsPointerLike<DecayFunctorT>) {
    if constexpr (std::is_pointer_v<std::remove_reference_t<FunctorT>> ||
                  std::is_member_pointer_v<std::remove_reference_t<FunctorT>>) {
      if VUNLIKELY (f == nullptr) {
        return;
      }
    }
  }

  construct_from<DecayFunctorT>(std::forward<FunctorT>(f));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>& MoveFunction<ReturnT(ArgsT...), SboSizeT>::operator=(
    MoveFunction&& other) noexcept {
  if VLIKELY (this != &other) {
    reset();
    move_from(std::move(other));
  }

  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>& MoveFunction<ReturnT(ArgsT...), SboSizeT>::operator=(
    std::nullptr_t) noexcept {
  reset();
  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename DecayFunctorT, typename>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>& MoveFunction<ReturnT(ArgsT...), SboSizeT>::operator=(FunctorT&& f) {
  MoveFunction tmp(std::forward<FunctorT>(f));
  swap(tmp);
  return *this;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>::~MoveFunction() {
  reset();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline ReturnT MoveFunction<ReturnT(ArgsT...), SboSizeT>::operator()(ArgsT... args) {
  if VUNLIKELY (vtable_ == nullptr) {
    detail::throw_bad_function_call();
  }

  return vtable_->invoke(&storage_, std::forward<ArgsT>(args)...);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline MoveFunction<ReturnT(ArgsT...), SboSizeT>::operator bool() const noexcept {
  return vtable_ != nullptr;
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline const std::type_info& MoveFunction<ReturnT(ArgsT...), SboSizeT>::target_type() const noexcept {
  if VLIKELY (vtable_ != nullptr) {
    return vtable_->target_type();
  }

  return typeid(void);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline FunctorT* MoveFunction<ReturnT(ArgsT...), SboSizeT>::target() noexcept {
  if constexpr (std::is_object_v<FunctorT>) {
    if VLIKELY (vtable_ != nullptr && typeid(FunctorT) == target_type()) {
      return static_cast<FunctorT*>(vtable_->target(&storage_));
    }
  }

  return nullptr;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const FunctorT* MoveFunction<ReturnT(ArgsT...), SboSizeT>::target() const noexcept {
  if constexpr (std::is_object_v<FunctorT>) {
    if VLIKELY (vtable_ != nullptr && typeid(FunctorT) == target_type()) {
      return static_cast<const FunctorT*>(vtable_->target_const(&storage_));
    }
  }

  return nullptr;
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::swap(MoveFunction& other) noexcept {
  if VUNLIKELY (this == &other) {
    return;
  }

  MoveFunction tmp(std::move(*this));
  *this = std::move(other);
  other = std::move(tmp);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline ReturnT MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::invoke(void* storage, ArgsT... args) {
  auto* f = std::launder(reinterpret_cast<FunctorT*>(storage));

  if constexpr (std::is_void_v<ReturnT>) {
    std::invoke(*f, std::forward<ArgsT>(args)...);
  } else {
    return std::invoke(*f, std::forward<ArgsT>(args)...);
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::move_construct(void* dst,
                                                                                              void* src) noexcept {
  auto* src_f = std::launder(reinterpret_cast<FunctorT*>(src));
  ::new (dst) FunctorT(std::move(*src_f));
  src_f->~FunctorT();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::destroy(void* storage) noexcept {
  auto* f = std::launder(reinterpret_cast<FunctorT*>(storage));
  f->~FunctorT();
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const std::type_info& MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target_type() noexcept {
  return typeid(FunctorT);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void* MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target(void* storage) noexcept {
  return storage;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const void* MoveFunction<ReturnT(ArgsT...), SboSizeT>::InlineVTable<FunctorT>::target_const(
    const void* storage) noexcept {
  return storage;
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline ReturnT MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::invoke(void* storage, ArgsT... args) {
  FunctorT* f = *std::launder(static_cast<FunctorT**>(storage));

  if constexpr (std::is_void_v<ReturnT>) {
    std::invoke(*f, std::forward<ArgsT>(args)...);
  } else {
    return std::invoke(*f, std::forward<ArgsT>(args)...);
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::move_construct(void* dst,
                                                                                            void* src) noexcept {
  FunctorT** src_slot = std::launder(static_cast<FunctorT**>(src));
  FunctorT* src_f = *src_slot;
  ::new (dst) FunctorT*(src_f);
  *src_slot = nullptr;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::destroy(void* storage) noexcept {
  FunctorT** slot = std::launder(static_cast<FunctorT**>(storage));
  FunctorT* f = *slot;

  if VLIKELY (f != nullptr) {
    f->~FunctorT();

    auto& pool = MemoryPool::global_instance();
    pool.deallocate(f, sizeof(FunctorT), alignof(FunctorT));

    *slot = nullptr;
  }
}

#if defined(__cpp_rtti)
template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const std::type_info& MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target_type() noexcept {
  return typeid(FunctorT);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline void* MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target(void* storage) noexcept {
  return *std::launder(static_cast<FunctorT**>(storage));
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const void* MoveFunction<ReturnT(ArgsT...), SboSizeT>::HeapVTable<FunctorT>::target_const(
    const void* storage) noexcept {
  return *std::launder(static_cast<FunctorT* const*>(storage));
}
#endif

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT>
inline const typename MoveFunction<ReturnT(ArgsT...), SboSizeT>::VTable*
MoveFunction<ReturnT(ArgsT...), SboSizeT>::get_vtable() noexcept {
  if constexpr (kIsInline<FunctorT>) {
    static constexpr VTable kVTable = {
        &InlineVTable<FunctorT>::invoke,       &InlineVTable<FunctorT>::move_construct,
        &InlineVTable<FunctorT>::destroy,
#if defined(__cpp_rtti)
        &InlineVTable<FunctorT>::target_type,  &InlineVTable<FunctorT>::target,
        &InlineVTable<FunctorT>::target_const,
#endif
    };
    return &kVTable;
  } else {
    static constexpr VTable kVTable = {
        &HeapVTable<FunctorT>::invoke,      &HeapVTable<FunctorT>::move_construct, &HeapVTable<FunctorT>::destroy,
#if defined(__cpp_rtti)
        &HeapVTable<FunctorT>::target_type, &HeapVTable<FunctorT>::target,         &HeapVTable<FunctorT>::target_const,
#endif
    };
    return &kVTable;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
template <typename FunctorT, typename SourceT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::construct_from(SourceT&& src) {
  if constexpr (kIsInline<FunctorT>) {
    ::new (&storage_) FunctorT(std::forward<SourceT>(src));
  } else {
    auto& pool = MemoryPool::global_instance();
    auto* mem = pool.allocate(sizeof(FunctorT), alignof(FunctorT));

    if VUNLIKELY (mem == nullptr) {
      throw std::bad_alloc();
    }

    try {
      auto* new_f = ::new (mem) FunctorT(std::forward<SourceT>(src));
      ::new (static_cast<void*>(&storage_)) FunctorT*(new_f);
    } catch (...) {
      pool.deallocate(mem, sizeof(FunctorT), alignof(FunctorT));
      throw;
    }
  }

  vtable_ = get_vtable<FunctorT>();
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::move_from(MoveFunction&& other) noexcept {
  if VLIKELY (other.vtable_ != nullptr) {
    other.vtable_->move_construct(&storage_, &other.storage_);
    vtable_ = other.vtable_;
    other.vtable_ = nullptr;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void MoveFunction<ReturnT(ArgsT...), SboSizeT>::reset() noexcept {
  if VLIKELY (vtable_ != nullptr) {
    vtable_->destroy(&storage_);
    vtable_ = nullptr;
  }
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline void swap(MoveFunction<ReturnT(ArgsT...), SboSizeT>& lhs,
                 MoveFunction<ReturnT(ArgsT...), SboSizeT>& rhs) noexcept {
  lhs.swap(rhs);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator==(const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept {
  return !cb;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator==(std::nullptr_t, const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb) noexcept {
  return !cb;
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator!=(const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb, std::nullptr_t) noexcept {
  return static_cast<bool>(cb);
}

template <typename ReturnT, typename... ArgsT, size_t SboSizeT>
inline bool operator!=(std::nullptr_t, const MoveFunction<ReturnT(ArgsT...), SboSizeT>& cb) noexcept {
  return static_cast<bool>(cb);
}

}  // namespace vlink

#else

namespace vlink {

template <typename SignatureT>
using Function = std::function<SignatureT>;

template <typename SignatureT>
using LargeFunction = std::function<SignatureT>;

template <typename SignatureT>
using function = std::function<SignatureT>;

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
[[maybe_unused]] static constexpr bool kIsSupportMoveFunction = true;
template <typename SignatureT>
using MoveFunction = std::move_only_function<SignatureT>;

template <typename SignatureT>
using LargeMoveFunction = std::move_only_function<SignatureT>;

template <typename SignatureT>
using move_only_function = std::move_only_function<SignatureT>;
#else
[[maybe_unused]] static constexpr bool kIsSupportMoveFunction = false;
template <typename SignatureT>
using MoveFunction = std::function<SignatureT>;

template <typename SignatureT>
using LargeMoveFunction = std::function<SignatureT>;

template <typename SignatureT>
using move_only_function = std::function<SignatureT>;
#endif

}  // namespace vlink

#endif
