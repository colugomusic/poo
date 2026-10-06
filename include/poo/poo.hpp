#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <bit>
#include <mutex>
#include <vector>

namespace poo::detail {

static constexpr auto MIN_GROUP_SIZE     = size_t{16};
static constexpr auto DEFAULT_ARRAY_SIZE = size_t{16};

struct atomic_ref_count {
	auto ref() -> uint32_t {
		while (true) {
			auto c = count_.load(std::memory_order_acquire);
			if (c == 0) {
				return 0;
			}
			if (count_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel)) {
				return c + 1;
			}
		}
    }
	auto unref() -> uint32_t     { return count_.fetch_sub(1, std::memory_order_acq_rel) - 1; }
	auto get() const -> uint32_t { return count_.load(std::memory_order_acquire); }
private:
	std::atomic<uint32_t> count_ = 0;
};

[[nodiscard]] static inline constexpr
auto is_power_of_two(std::unsigned_integral auto value) {
	return (std::popcount(value) == 1);
}

static_assert (is_power_of_two(MIN_GROUP_SIZE), "MIN_GROUP_SIZE must be a power of 2");

[[nodiscard]] inline
auto nearest_accommodating_power_of_2(size_t size) -> size_t {
	if (size == 0) {
		return 1;
	}
	size--;
	size |= size >> 1;
	size |= size >> 2;
	size |= size >> 4;
	size |= size >> 8;
	size |= size >> 16;
	size |= size >> 32;
	size++;
	return size;
}

[[nodiscard]] inline
auto nearest_accommodating_group_size(size_t size) -> size_t {
	return std::max(nearest_accommodating_power_of_2(size), MIN_GROUP_SIZE);
}

[[nodiscard]] static constexpr
auto to_group_index(size_t pow2) -> size_t {
	assert (is_power_of_two(pow2));
	assert (pow2 > 0);
	assert (pow2 >= MIN_GROUP_SIZE);
	return std::countr_zero(pow2) - std::countr_zero(MIN_GROUP_SIZE);
}

template <typename T>
struct static_vector {
	static_vector(size_t size) {
		vec_.reserve(nearest_accommodating_group_size(size));
		vec_.resize(size);
	}
	static_vector(const static_vector& rhs) = delete;
	static_vector(static_vector&& rhs) noexcept = default;
	static_vector& operator=(const static_vector& rhs) = delete;
	static_vector& operator=(static_vector&& rhs) noexcept = default;
	auto set(size_t index, T value)          { vec_.at(index) = std::move(value); }
	auto get(size_t index) const -> const T& { return vec_.at(index); }
	auto capacity() const                    { return vec_.capacity(); }
	auto begin()                             { return vec_.begin(); }
	auto end()                               { return vec_.end(); }
	auto begin() const                       { return vec_.begin(); }
	auto end() const                         { return vec_.end(); }
	auto cbegin() const                      { return vec_.cbegin(); }
	auto cend() const                        { return vec_.cend(); }
	auto size() const                        { return vec_.size(); }
	auto is_empty() const                    { return vec_.empty(); }
	auto resize(size_t new_size) {
		if (new_size <= vec_.capacity()) { vec_.resize(new_size); }
		else                             { throw std::runtime_error("Cannot resize static_vector beyond its capacity"); }
	}
private:
	std::vector<T> vec_;
};

} // poo::detail

///////////////////////////////////////////////////////////////////////////
// Unsynchronized (single-threaded) copy-on-write pool arrays
///////////////////////////////////////////////////////////////////////////

namespace poo::unsynchronized {

template <typename T> struct array;
template <typename T> struct pool;

} // poo::unsynchronized

namespace poo::unsynchronized::detail {

template <typename T> struct pool_impl;

template <typename T> auto acquire(pool_impl<T>* pool, size_t size) -> std::shared_ptr<poo::detail::static_vector<T>>;
template <typename T> auto release(pool_impl<T>* pool, std::shared_ptr<poo::detail::static_vector<T>> vec) -> void;

template <typename T>
struct pool_group {
	std::vector<std::shared_ptr<poo::detail::static_vector<T>>> v;
};

template <typename T>
struct pool_impl {
	std::vector<pool_group<T>> groups;
};

} // poo::unsynchronized::detail

namespace poo::unsynchronized {

template <typename T>
struct pool {
	friend struct array<T>;
	auto make(size_t size) -> array<T>;
private:
	auto impl() -> detail::pool_impl<T>*;
	detail::pool_impl<T> impl_;
};

} // poo::unsynchronized

namespace poo::unsynchronized::detail {

template <typename T>
static inline pool<T> DEFAULT_POOL;

} // poo::unsynchronized::detail

namespace poo::unsynchronized {

template <typename T>
struct array {
	array(std::shared_ptr<poo::detail::static_vector<T>> vec, detail::pool_impl<T>* pool);
	array(poo::unsynchronized::pool<T>* pool);
	array();
	array(size_t size);
	array(size_t size, poo::unsynchronized::pool<T>* pool);
	array(const array& rhs)                = default;
	array(array&& rhs) noexcept            = default;
	array& operator=(const array& rhs)     = default;
	array& operator=(array&& rhs) noexcept = default;
	~array();
	auto set(size_t index, T value) -> void;
	auto clear() -> void;
	auto resize(size_t new_size) -> void;
	auto get(size_t index) const -> T;
	auto push_back(T value) -> void;
	auto cbegin() const;
	auto cend() const;
	auto begin() const;
	auto end() const;
	auto size() const;
	auto is_empty() const;
private:
	std::shared_ptr<poo::detail::static_vector<T>> vec_;
	detail::pool_impl<T>* pool_;
};

} // poo::unsynchronized

namespace poo::unsynchronized::detail {

template <typename T>
auto acquire(pool_group<T>* pg, size_t size) -> std::shared_ptr<poo::detail::static_vector<T>> {
	if (pg->v.empty()) {
		return std::make_shared<poo::detail::static_vector<T>>(size);
	}
	auto vec = std::move(pg->v.back());
	pg->v.pop_back();
	assert (vec->capacity() >= size);
	vec->resize(size);
	return vec;
}

template <typename T>
auto release(pool_group<T>* pg, std::shared_ptr<poo::detail::static_vector<T>> vec) -> void {
	pg->v.emplace_back(std::move(vec));
}

template <typename T>
auto acquire(pool_impl<T>* pool, size_t size) -> std::shared_ptr<poo::detail::static_vector<T>> {
	const auto group_size  = poo::detail::nearest_accommodating_group_size(size);
	const auto group_index = poo::detail::to_group_index(group_size);
	while (pool->groups.size() <= group_index) {
		pool->groups.emplace_back();
	}
	auto& pg = pool->groups[group_index];
	return acquire(&pg, size);
}

template <typename T>
auto release(pool_impl<T>* pool, std::shared_ptr<poo::detail::static_vector<T>> vec) -> void {
	assert (poo::detail::is_power_of_two(vec->capacity()));
	const auto group_index = poo::detail::to_group_index(vec->capacity());
	while (pool->groups.size() <= group_index) {
		pool->groups.emplace_back();
	}
	auto& pg = pool->groups[group_index];
	release(&pg, std::move(vec));
}

} // poo::unsynchronized::detail

namespace poo::unsynchronized {

template <typename T> auto pool<T>::impl() -> detail::pool_impl<T>* { return &impl_; }
template <typename T> auto pool<T>::make(size_t size) -> array<T>   { return array{ acquire(&impl_, size), &impl_}; }

template <typename T> array<T>::array(std::shared_ptr<poo::detail::static_vector<T>> vec, detail::pool_impl<T>* pool) : vec_{vec}, pool_{pool} {}
template <typename T> array<T>::array(size_t size, poo::unsynchronized::pool<T>* pool) : array{detail::acquire(pool->impl(), size), pool->impl()} {}
template <typename T> array<T>::array(poo::unsynchronized::pool<T>* pool)              : array{poo::detail::DEFAULT_ARRAY_SIZE, pool} {}
template <typename T> array<T>::array(size_t size)                                     : array{size, &detail::DEFAULT_POOL<T>} {}
template <typename T> array<T>::array()                                                : array{poo::detail::DEFAULT_ARRAY_SIZE} {}

template <typename T>
array<T>::~array() {
	if (vec_.use_count() == 1) {
		release(pool_, std::move(vec_));
	}
}

template <typename T>
auto array<T>::set(size_t index, T value) -> void {
	if (vec_.use_count() > 1) {
		auto new_vec = acquire(pool_, vec_->size());
		std::ranges::copy(*vec_, new_vec->begin());
		vec_ = std::move(new_vec);
	}
	vec_->set(index, std::move(value));
}

template <typename T>
auto array<T>::push_back(T value) -> void {
	if (vec_.use_count() > 1) {
		auto new_vec = acquire(pool_, vec_->size() + 1);
		std::ranges::copy(*vec_, new_vec->begin());
		new_vec->at(vec_->size()) = std::move(value);
		vec_ = std::move(new_vec);
		return;
	}
	if (vec_->size() >= vec_->capacity()) {
		auto new_vec = acquire(pool_, vec_->size() + 1);
		std::ranges::copy(*vec_, new_vec->begin());
		new_vec->at(vec_->size()) = std::move(value);
		vec_ = std::move(new_vec);
		return;
	}
	vec_->resize(vec_->size() + 1);
	vec_->set(vec_->size() - 1, std::move(value));
}

template <typename T>
auto array<T>::clear() -> void {
	resize(0);
}

template <typename T>
auto array<T>::resize(size_t new_size) -> void {
	if (vec_.use_count() > 1) {
		auto new_vec = acquire(pool_, new_size);
		const auto n = std::min(vec_->size(), new_size);
		std::ranges::copy_n(vec_->cbegin(), n, new_vec->begin());
		vec_ = std::move(new_vec);
		return;
	}
	if (new_size > vec_->capacity()) {
		auto new_vec = acquire(pool_, new_size);
		std::ranges::copy(*vec_, new_vec->begin());
		release(pool_, std::move(vec_));
		vec_ = std::move(new_vec);
		return;
	}
	vec_->resize(new_size);
}

template <typename T> auto array<T>::get(size_t index) const -> T { return vec_->at(index); }
template <typename T> auto array<T>::cbegin() const               { return vec_->cbegin(); }
template <typename T> auto array<T>::cend() const                 { return vec_->cend(); }
template <typename T> auto array<T>::begin() const                { return vec_->begin(); }
template <typename T> auto array<T>::end() const                  { return vec_->end(); }
template <typename T> auto array<T>::size() const                 { return vec_->size(); }
template <typename T> auto array<T>::is_empty() const             { return vec_->is_empty(); }

} // poo::unsynchronized

//////////////////////////////////////////////////////////////////////////
// Thread-safe copy-on-write pool arrays
//////////////////////////////////////////////////////////////////////////

namespace poo::synchronized {

template <typename T> struct array;
template <typename T> struct pool;

} // poo::synchronized

namespace poo::synchronized::detail {

struct control_block {
	poo::detail::atomic_ref_count ref_count;
};

template <typename T>
struct shared_vector {
	poo::detail::static_vector<T> vector;
	control_block cb;
};

template <typename T> struct pool_impl;

template <typename T> auto acquire(pool_impl<T>* pool, size_t size) -> shared_vector<T>*;
template <typename T> auto release(pool_impl<T>* pool, shared_vector<T>* ptr) -> void;

template <typename T>
struct pool_group {
	std::vector<std::unique_ptr<shared_vector<T>>> v;
};

template <typename T>
struct pool_impl {
	std::vector<pool_group<T>> groups;
	std::mutex mutex;
};

} // poo::synchronized::detail

namespace poo::synchronized {

template <typename T>
struct pool {
	friend struct array<T>;
	auto make(size_t size) -> array<T>;
private:
	auto impl() -> detail::pool_impl<T>*;
	detail::pool_impl<T> impl_;
};

} // poo::synchronized

namespace poo::synchronized::detail {

template <typename T>
static inline pool<T> DEFAULT_POOL;

} // poo::synchronized::detail

namespace poo::synchronized {

template <typename T>
struct array {
	array(detail::shared_vector<T>* ptr, detail::pool_impl<T>* pool);
	array(poo::synchronized::pool<T>* pool);
	array();
	array(size_t size);
	array(size_t size, poo::synchronized::pool<T>* pool);
	array(const array& rhs);
	array& operator=(const array& rhs);
	array(array&& rhs) noexcept;
	array& operator=(array&& rhs) noexcept;
	~array();
	auto set(size_t index, T value) -> void;
	auto clear() -> void;
	auto resize(size_t new_size) -> void;
	auto get(size_t index) const -> T;
	auto push_back(T value) -> void;
	auto cbegin() const;
	auto cend() const;
	auto begin() const;
	auto end() const;
	auto size() const;
	auto is_empty() const;
private:
	detail::shared_vector<T>* ptr_ = nullptr;
	detail::pool_impl<T>* pool_;
};

} // poo::synchronized

namespace poo::synchronized::detail {

template <typename T>
auto acquire(pool_group<T>* pg, size_t size) -> shared_vector<T>* {
	if (pg->v.empty()) {
		return new shared_vector<T>{size};
	}
	auto ptr = std::move(pg->v.back());
	pg->v.pop_back();
	assert (ptr->vector.capacity() >= size);
	ptr->vector.resize(size);
	return ptr.release();
}

template <typename T>
auto release(pool_group<T>* pg, shared_vector<T>* ptr) -> void {
	pg->v.emplace_back(ptr);
}

template <typename T>
auto acquire(pool_impl<T>* pool, size_t size) -> shared_vector<T>* {
	const auto group_size  = poo::detail::nearest_accommodating_group_size(size);
	const auto group_index = poo::detail::to_group_index(group_size);
	auto lock = std::lock_guard{pool->mutex};
	while (pool->groups.size() <= group_index) {
		pool->groups.emplace_back();
	}
	auto& pg = pool->groups[group_index];
	return acquire(&pg, size);
}

template <typename T>
auto release(pool_impl<T>* pool, shared_vector<T>* ptr) -> void {
	assert (poo::detail::is_power_of_two(ptr->vector.capacity()));
	const auto group_index = poo::detail::to_group_index(ptr->vector.capacity());
	auto lock = std::lock_guard{pool->mutex};
	while (pool->groups.size() <= group_index) {
		pool->groups.emplace_back();
	}
	auto& pg = pool->groups[group_index];
	release(&pg, ptr);
}

} // poo::synchronized::detail

namespace poo::synchronized {

template <typename T> auto pool<T>::impl() -> detail::pool_impl<T>* { return &impl_; }
template <typename T> auto pool<T>::make(size_t size) -> array<T>   { return array{ acquire(&impl_, size), &impl_}; }

template <typename T> array<T>::array(size_t size, poo::synchronized::pool<T>* pool) : array{synchronized::detail::acquire(pool->impl(), size), pool->impl()} {}
template <typename T> array<T>::array(poo::synchronized::pool<T>* pool)              : array{poo::detail::DEFAULT_ARRAY_SIZE, pool} {}
template <typename T> array<T>::array(size_t size)                                   : array{size, &synchronized::detail::DEFAULT_POOL<T>} {}
template <typename T> array<T>::array()                                              : array{poo::detail::DEFAULT_ARRAY_SIZE} {}

template <typename T>
array<T>::array(detail::shared_vector<T>* ptr, detail::pool_impl<T>* pool)
	: ptr_{ptr}
	, pool_{pool}
{
	assert (ptr);
	assert (pool);
	ptr->cb.ref_count.ref();
}

template <typename T>
array<T>::array(const array& rhs)
	: ptr_{rhs.ptr_}
	, pool_{rhs.pool_}
{
	assert (pool_);
	if (ptr_) {
		ptr_->cb.ref_count.ref();
	}
}

template <typename T>
array<T>::array(array&& rhs) noexcept
	: ptr_{rhs.ptr_}
	, pool_{rhs.pool_}
{
	rhs.ptr_ = nullptr;
}

template <typename T>
array<T>& array<T>::operator=(const array& rhs) {
	if (rhs == this)              { return *this; }
	if (ptr_ && ptr_ == rhs.ptr_) { return *this; }
	if (ptr_ && ptr_->cb.ref_count.unref() == 0) {
		release(pool_, ptr_);
	}
	ptr_  = rhs.ptr_;
	pool_ = rhs.pool_;
	if (ptr_) {
		ptr_->cb.ref_count.ref();
	}
	return *this;
}

template <typename T>
array<T>& array<T>::operator=(array&& rhs) noexcept {
	if (rhs == this) { return *this; }
	if (ptr_ && ptr_ == rhs.ptr_) {
		rhs.ptr_ = nullptr;
		return *this;
	}
	if (ptr_ && ptr_->cb.ref_count.unref() == 0) {
		release(pool_, ptr_);
	}
	ptr_  = rhs.ptr_;
	pool_ = rhs.pool_;
	rhs.ptr_ = nullptr;
	return *this;
}

template <typename T>
array<T>::~array() {
	if (ptr_ && ptr_->cb.ref_count.unref() == 0) {
		release(pool_, ptr_);
	}
}

template <typename T>
auto array<T>::set(size_t index, T value) -> void {
	if (!ptr_) {
		throw std::out_of_range{"array::set: index out of range"};
	}
	if (ptr_->cb.ref_count.get() > 1) {
		auto new_ptr = acquire(pool_, ptr_->vector.size());
		std::ranges::copy(ptr_->vector, new_ptr->vector.begin());
		ptr_ = new_ptr;
	}
	ptr_->vector.at(index) = std::move(value);
}

template <typename T>
auto array<T>::push_back(T value) -> void {
	if (!ptr_) {
		ptr_ = acquire(pool_, 1);
		ptr_->vector.set(0, std::move(value));
		return;
	}
	if (ptr_->cb.ref_count.get() > 1) {
		auto new_ptr = acquire(pool_, ptr_->vector.size() + 1);
		const auto n = std::min(ptr_->vector.size(), new_ptr->vector.capacity());
		std::ranges::copy_n(ptr_->vector.cbegin(), n, new_ptr->vector.begin());
		new_ptr->vector.set(ptr_->vector.size(), std::move(value));
		ptr_ = new_ptr;
		return;
	}
	if (ptr_->vector.size() >= ptr_->vector.capacity()) {
		auto new_ptr = acquire(pool_, ptr_->vector.size() + 1);
		std::ranges::copy(ptr_->vector, new_ptr->vector.begin());
		new_ptr->vector.set(ptr_->vector.size(), std::move(value));
		release(pool_, ptr_);
		ptr_ = new_ptr;
		return;
	}
	ptr_->vector.resize(ptr_->vector.size() + 1);
	ptr_->vector.set(ptr_->vector.size() - 1, std::move(value));
}

template <typename T>
auto array<T>::clear() -> void {
	resize(0);
}

template <typename T>
auto array<T>::resize(size_t new_size) -> void {
	if (!ptr_) {
		ptr_ = acquire(pool_, new_size);
		return;
	}
	if (ptr_->cb.ref_count.get() > 1) {
		auto new_ptr = acquire(pool_, new_size);
		const auto n = std::min(ptr_->vector.size(), new_size);
		std::ranges::copy_n(ptr_->vector.begin(), n, new_ptr->vector.begin());
		ptr_ = new_ptr;
		return;
	}
	if (new_size > ptr_->vector.capacity()) {
		auto new_ptr = acquire(pool_, new_size);
		std::ranges::copy(ptr_->vector, new_ptr->vector.begin());
		release(pool_, ptr_);
		ptr_ = new_ptr;
		return;
	}
	ptr_->vector.resize(new_size);
}

template <typename T>
auto array<T>::get(size_t index) const -> T {
	if (!ptr_) {
		throw std::out_of_range{"array::get: index out of range"};
	}
	return ptr_->vector.at(index);
}

template <typename T>
auto array<T>::cbegin() const {
	return ptr_ ? ptr_->vector.cbegin() : typename std::vector<T>::const_iterator{};
}

template <typename T>
auto array<T>::cend() const {
	return ptr_ ? ptr_->vector.cend() : typename std::vector<T>::const_iterator{};
}

template <typename T>
auto array<T>::begin() const {
	return ptr_ ? ptr_->vector.begin() : typename std::vector<T>::const_iterator{};
}

template <typename T>
auto array<T>::end() const {
	return ptr_ ? ptr_->vector.end() : typename std::vector<T>::const_iterator{};
}

template <typename T>
auto array<T>::size() const {
	return ptr_ ? ptr_->vector.size() : 0;
}

template <typename T>
auto array<T>::is_empty() const {
	return ptr_ ? ptr_->vector.is_empty() : true;
}

} // poo::synchronized
