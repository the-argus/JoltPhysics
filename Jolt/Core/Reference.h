// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#pragma once

#include <Jolt/Core/Atomics.h>

JPH_NAMESPACE_BEGIN

// Forward declares
template <class T> class Ref;
template <class T> class RefConst;
template <class T> class WeakRef;
template <class T> class WeakRefConst;
template <typename T> class SharedPointerCounters;

template <typename T, typename... Args>
Ref<T> MakeShared(Args&&... args);
template <typename T>
Ref<T> IntoShared(T* ptr);

class RefCounterInterface
{
public:
	virtual uint32 AddStrongRefIfNonZero() const = 0;
	virtual void StrongRelease() const = 0;
	virtual uint32 GetStrongRefCount() const = 0;
};

/// Simple class to facilitate reference counting / releasing
/// Derive your class from RefTarget and you can reference it by using Ref<classname> or RefConst<classname>
///
/// Reference counting classes keep an integer which indicates how many references
/// to the object are active. Reference counting objects are derived from RefTarget
/// and staT & their life with a reference count of zero. They can then be assigned
/// to equivalents of pointers (Ref) which will increase the reference count immediately.
/// If the destructor of Ref is called or another object is assigned to the reference
/// counting pointer it will decrease the reference count of the object again. If this
/// reference count becomes zero, the object is destroyed.
///
/// This provides a very powerful mechanism to prevent memory leaks, but also gives
/// some responsibility to the programmer. The most notable point is that you cannot
/// have one object reference another and have the other reference the first one
/// back, because this way the reference count of both objects will never become
/// lower than 1, resulting in a memory leak. By carefully designing your classes
/// (and particularly identifying who owns who in the class hierarchy) you can avoid
/// these problems.
template <class T>
class RefTarget
{
public:
	template <typename U> friend class Ref;
	template <typename U> friend class RefConst;
	template <typename U> friend class WeakRef;
	template <typename U> friend class WeakRefConst;
	/// Constructor
	inline					RefTarget() = default;
	inline					RefTarget(const RefTarget &)					{ /* Do not copy refcount */ }
	inline					~RefTarget()									{ JPH_IF_ENABLE_ASSERTS(uint32 value = mRefCount.load(memory_order_relaxed);) JPH_ASSERT(value == 0 || (value == 1 && SupportsWeakReferences()) || value == cEmbedded || (value == (cEmbedded + 1) && SupportsWeakReferences())); } ///< assert no one is referencing us

	/// Mark this class as embedded, this means the type can be used in a compound or constructed on the stack.
	/// The Release function will never destruct the object, it is assumed the destructor will be called by whoever allocated
	/// the object and at that point in time it is checked that no references are left to the structure.
	inline void				SetEmbedded() const								{ JPH_IF_ENABLE_ASSERTS(uint32 old = ) mRefCount.fetch_add(cEmbedded, memory_order_relaxed); JPH_ASSERT(old < cEmbedded); }

	/// Assignment operator
	inline RefTarget &		operator = (const RefTarget &)					{ /* Don't copy refcount */ return *this; }

	/// Get current refcount of this object
	uint32					GetRefCount() const
	{
		if (mSharedCounters)
		{
			return mSharedCounters->GetStrongRefCount();
		}
		return mRefCount.load(memory_order_relaxed);
	}

	inline bool				SupportsWeakReferences() const { return mSharedCounters != nullptr; }

	/// Add or release a reference to this object
	inline void				AddRef() const
	{
		if (mSharedCounters)
		{
			JPH_IF_ENABLE_ASSERTS(uint32 value =) mSharedCounters->AddStrongRefIfNonZero();
			JPH_ASSERT(value != 0);
			return;
		}
		// Adding a reference can use relaxed memory ordering
		mRefCount.fetch_add(1, memory_order_relaxed);
	}

	inline void				Release() const
	{
		if (mSharedCounters)
		{
			mSharedCounters->StrongRelease();
			return;
		}
	#ifndef JPH_TSAN_ENABLED
		// Releasing a reference must use release semantics...
		if (mRefCount.fetch_sub(1, memory_order_release) == 1)
		{
			// ... so that we can use acquire to ensure that we see any updates from other threads that released a ref before deleting the object
			atomic_thread_fence(memory_order_acquire);
			delete static_cast<const T *>(this);
		}
	#else
		// But under TSAN, we cannot use atomic_thread_fence, so we use an acq_rel operation unconditionally instead
		if (mRefCount.fetch_sub(1, memory_order_acq_rel) == 1)
			delete static_cast<const T *>(this);
	#endif
	}

	/// INTERNAL HELPER FUNCTION USED BY SERIALIZATION
	static int				sInternalGetRefCountOffset()					{ return offsetof(T, mRefCount); }

	template <typename U> friend class SharedPointerCounters;
	template <typename U, typename... Args> friend Ref<U> MakeShared(Args&&... args);
	template <typename U> friend Ref<U> IntoShared(U* ptr);

protected:
	static constexpr uint32 cEmbedded = 0x0ebedded;							///< A large value that gets added to the refcount to mark the object as embedded

	mutable atomic<uint32>	mRefCount = 0;									///< Current reference count
	RefCounterInterface* mSharedCounters = nullptr; /// set to a value if we support weak references
};

/// straight from https://stackoverflow.com/questions/73034238/check-if-class-is-derived-from-templated-class
namespace detail {
template<template<class...> class Base, typename... Ts>
void test(Base<Ts...>&);

template<template<class...> class, class, class = void>
constexpr bool is_template_base_of = false;

template<template<class...> class Base, class Derived>
constexpr bool is_template_base_of<Base, Derived,
  std::void_t<decltype(test<Base>(std::declval<Derived&>()))>> = true;
}

template <typename T>
class SharedPointerCounters : public RefCounterInterface
{
public:
	static_assert(detail::is_template_base_of<RefTarget, T>, "weakref can only contain RefTargets");

	uint32 GetStrongRefCount() const final { return mRefCount.load(memory_order_relaxed); }

	bool IsNull() const { return GetStrongRefCount() == 0; }

	/// Returns the refcount before calling this function. Returns zero otherwise
	uint32 AddStrongRefIfNonZero() const override
	{
		uint32 count = GetStrongRefCount();
		do {
			if (count == 0)
				return 0;
			// Replace the current counter value with the old value + 1, as
			// long as it's not changed meanwhile. on failure, loads the actual
			// value of the strong refcoutn into `count`
		} while (!mRefCount.compare_exchange_weak(count, count + 1, memory_order_acq_rel, memory_order_relaxed));
		return count;
	}

	void StrongRelease() const final
	{
	#ifndef JPH_TSAN_ENABLED
		// Releasing a reference must use release semantics...
		if (mRefCount.fetch_sub(1, memory_order_acq_rel) == 1)
		{
			// ... so that we can use acquire to ensure that we see any updates
			// from other threads that released a ref before deleting the object
			atomic_thread_fence(memory_order_acquire);
			Dispose();
		} else {
			return;
		}
	#else
		// But under TSAN, we cannot use atomic_thread_fence, so we use an acq_rel operation unconditionally instead
		if (mRefCount.fetch_sub(1, memory_order_acq_rel) == 1) {
			Dispose();
		} else {
			return;
		}
	#endif

		// we just called Dispose() here
		// TODO: gcc stl implementation only does this fence if _Mutex_base<_Lp>::_S_need_barriers. Do we (always) need it here?
		atomic_thread_fence(memory_order_acq_rel);
		if (mWeakCount.fetch_sub(1, memory_order_acq_rel) == 1)
		{
			Destroy();
		}
	}

	void AddWeakRef() const
	{
		mWeakCount.fetch_add(1, memory_order_acq_rel);
	}

	void ReleaseWeak() const
	{
		if (mWeakCount.fetch_sub(1, memory_order_acq_rel) == 1)
		{
			// TODO: gcc stl implementation only does this fence if _Mutex_base<_Lp>::_S_need_barriers. Do we (always) need it here?
			atomic_thread_fence(memory_order_acq_rel);
			Destroy();
		}
	}

	const T* GetPayload() const { return payload; }
	T* GetPayload() { return payload; }

	template <typename U, typename... Args> friend Ref<U> MakeShared(Args&&... args);
	template <typename U> friend Ref<U> IntoShared(U* ptr);

	// this doesnt make sense to use outside of MakeShared
	SharedPointerCounters() = default;

private:
	void SetPtrs(T* p, void (*del)(const SharedPointerCounters<T>*))
	{
		JPH_ASSERT(!payload);
		payload = p;
		deleter = del;
	}

	void Dispose() const
	{
		if (IsSingleAllocationWithPayload())
		{
			payload->~T();
		}
		else
		{
			delete payload;
		}
	}

	void Destroy() const
	{
		if (IsSingleAllocationWithPayload())
		{
			deleter(this);
		}
		else
		{
			delete this;
		}
	}

	bool IsSingleAllocationWithPayload() const { return deleter != nullptr; }

	// this has to be tracked by pointer to support type erasure and forward declarations
	T* payload = nullptr;
	void (*deleter)(const SharedPointerCounters<T>* self) = nullptr;
	mutable atomic<uint32> mRefCount = 1;
	mutable atomic<uint32> mWeakCount = 1;
};

/// Pure virtual version of RefTarget
class JPH_EXPORT RefTargetVirtual
{
public:
	/// Virtual destructor
	virtual					~RefTargetVirtual() = default;

	/// Virtual add reference
	virtual void			AddRef() = 0;

	/// Virtual release reference
	virtual void			Release() = 0;
};

/// Class for automatic referencing, this is the equivalent of a pointer to type T
/// if you assign a value to this class it will increment the reference count by one
/// of this object, and if you assign something else it will decrease the reference
/// count of the first object again. If it reaches a reference count of zero it will
/// be deleted
template <class T>
class Ref
{
public:
	template <typename T2> friend class Ref;
	template <typename T2> friend class RefConst;
	template <typename T2> friend class WeakRefConst;
	template <typename T2> friend class WeakRef;
	template <typename U, typename... Args> friend Ref<U> MakeShared(Args&&... args);
	template <typename U> friend Ref<U> IntoShared(U* ptr);

	/// Constructor
	inline					Ref()											: mPtr(nullptr) { }
	inline					Ref(T *inRHS)									: mPtr(inRHS) { AddRef(); }
	inline					Ref(const Ref<T> &inRHS)						: mPtr(inRHS.mPtr) { AddRef(); }
	inline					Ref(Ref<T> &&inRHS) noexcept					: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }

	template <typename T2, typename std::enable_if_t<std::is_convertible_v<T2&, T&>, bool> = true>
	inline					Ref(const Ref<T2> &inRHS)				: mPtr(inRHS.mPtr) { AddRef(); }
	template <typename T2, typename std::enable_if_t<std::is_convertible_v<T2&, T&>, bool> = true>
	inline					Ref(Ref<T2> &&inRHS) noexcept			: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }

	inline					~Ref()											{ Release(); }

	/// Assignment operators
	inline Ref<T> &			operator = (T *inRHS)							{ if (mPtr != inRHS) { Release(); mPtr = inRHS; AddRef(); } return *this; }
	inline Ref<T> &			operator = (const Ref<T> &inRHS)				{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; AddRef(); } return *this; }
	inline Ref<T> &			operator = (Ref<T> &&inRHS) noexcept			{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }

	/// Casting operators
	inline					operator T *() const							{ return mPtr; }

	/// Access like a normal pointer
	inline T *				operator -> () const							{ return mPtr; }
	inline T &				operator * () const								{ return *mPtr; }

	/// Comparison
	inline bool				operator == (const T * inRHS) const				{ return mPtr == inRHS; }
	inline bool				operator == (const Ref<T> &inRHS) const			{ return mPtr == inRHS.mPtr; }
	inline bool				operator != (const T * inRHS) const				{ return mPtr != inRHS; }
	inline bool				operator != (const Ref<T> &inRHS) const			{ return mPtr != inRHS.mPtr; }

	/// Get pointer
	inline T *				GetPtr() const&									{ return mPtr; }
	inline bool				IsNull() const									{ return mPtr == nullptr; }

	/// Get hash for this object
	uint64					GetHash() const
	{
		return Hash<T *> { } (mPtr);
	}

	/// INTERNAL HELPER FUNCTION USED BY SERIALIZATION
	void **					InternalGetPointer()							{ return reinterpret_cast<void **>(&mPtr); }

	struct NoAddRefTag {};

private:
	explicit Ref(NoAddRefTag, T* ptr): mPtr(ptr) {}

	template <class T2> friend class RefConst;

	/// Use "variable = nullptr;" to release an object, do not call these functions
	inline void				AddRef()										{ if (mPtr != nullptr) mPtr->AddRef(); }
	inline void				Release()										{ if (mPtr != nullptr) mPtr->Release(); }

	T *						mPtr;											///< Pointer to object that we are reference counting
};

/// Class for automatic referencing, this is the equivalent of a CONST pointer to type T
/// if you assign a value to this class it will increment the reference count by one
/// of this object, and if you assign something else it will decrease the reference
/// count of the first object again. If it reaches a reference count of zero it will
/// be deleted
template <class T>
class RefConst
{
public:
	template <typename T2> friend class Ref;
	template <typename T2> friend class RefConst;
	template <typename T2> friend class WeakRefConst;
	template <typename T2> friend class WeakRef;
	template <typename U, typename... Args> friend Ref<U> MakeShared(Args&&... args);
	template <typename U> friend Ref<U> IntoShared(U* ptr);

	/// Constructor
	inline					RefConst()										: mPtr(nullptr) { }
	inline					RefConst(const T * inRHS)						: mPtr(inRHS) { AddRef(); }
	inline					RefConst(const RefConst<T> &inRHS)				: mPtr(inRHS.mPtr) { AddRef(); }
	inline					RefConst(RefConst<T> &&inRHS) noexcept			: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }
	inline					RefConst(const Ref<T> &inRHS)					: mPtr(inRHS.mPtr) { AddRef(); }
	inline					RefConst(Ref<T> &&inRHS) noexcept				: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }

	template <typename T2, typename std::enable_if_t<std::is_convertible_v<T2&, T&>, bool> = true>
	inline					RefConst(const RefConst<T2> &inRHS)				: mPtr(inRHS.mPtr) { AddRef(); }
	template <typename T2, typename std::enable_if_t<std::is_convertible_v<T2&, T&>, bool> = true>
	inline					RefConst(RefConst<T2> &&inRHS) noexcept			: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }

	inline					~RefConst()										{ Release(); }

	/// Assignment operators
	inline RefConst<T> &	operator = (const T * inRHS)					{ if (mPtr != inRHS) { Release(); mPtr = inRHS; AddRef(); } return *this; }
	inline RefConst<T> &	operator = (const RefConst<T> &inRHS)			{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; AddRef(); } return *this; }
	inline RefConst<T> &	operator = (RefConst<T> &&inRHS) noexcept		{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }
	inline RefConst<T> &	operator = (const Ref<T> &inRHS)				{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; AddRef(); } return *this; }
	inline RefConst<T> &	operator = (Ref<T> &&inRHS) noexcept			{ if (mPtr != inRHS.mPtr) { Release(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }

	/// Casting operators
	inline					operator const T * () const						{ return mPtr; }

	/// Access like a normal pointer
	inline const T *		operator -> () const							{ return mPtr; }
	inline const T &		operator * () const								{ return *mPtr; }

	/// Comparison
	inline bool				operator == (const T * inRHS) const				{ return mPtr == inRHS; }
	inline bool				operator == (const RefConst<T> &inRHS) const	{ return mPtr == inRHS.mPtr; }
	inline bool				operator == (const Ref<T> &inRHS) const			{ return mPtr == inRHS.mPtr; }
	inline bool				operator != (const T * inRHS) const				{ return mPtr != inRHS; }
	inline bool				operator != (const RefConst<T> &inRHS) const	{ return mPtr != inRHS.mPtr; }
	inline bool				operator != (const Ref<T> &inRHS) const			{ return mPtr != inRHS.mPtr; }

	/// Get pointer
	inline const T *		GetPtr() const&									{ return mPtr; }

	inline bool				IsNull() const									{ return mPtr == nullptr; }

	/// Get hash for this object
	uint64					GetHash() const
	{
		return Hash<const T *> { } (mPtr);
	}

	/// INTERNAL HELPER FUNCTION USED BY SERIALIZATION
	void **					InternalGetPointer()							{ return const_cast<void **>(reinterpret_cast<const void **>(&mPtr)); }

	struct NoAddRefTag {};

private:
	explicit RefConst(NoAddRefTag, const T* ptr): mPtr(ptr) {}

	/// Use "variable = nullptr;" to release an object, do not call these functions
	inline void				AddRef()										{ if (mPtr != nullptr) mPtr->AddRef(); }
	inline void				Release()										{ if (mPtr != nullptr) mPtr->Release(); }

	const T *				mPtr;											///< Pointer to object that we are reference counting
};

template <class T>
class WeakRefConst
{
public:
	template <typename T2> friend class Ref;
	template <typename T2> friend class RefConst;
	template <typename T2> friend class WeakRefConst;
	template <typename T2> friend class WeakRef;

	inline					WeakRefConst()											: mPtr(nullptr) { }
	inline explicit			WeakRefConst(const T* inRHS)							: mPtr(reinterpret_cast<SharedPointerCounters<T>*>(inRHS->mSharedCounters)) { JPH_ASSERT(inRHS->SupportsWeakReferences()); AddWeakRef(); }
	inline					WeakRefConst(const RefConst<T> &inRHS)					: mPtr(reinterpret_cast<SharedPointerCounters<T>*>(inRHS.mPtr->mSharedCounters)) { JPH_ASSERT(inRHS.mPtr->SupportsWeakReferences()); AddWeakRef(); }
	inline					WeakRefConst(const Ref<T> &inRHS)						: mPtr(reinterpret_cast<SharedPointerCounters<T>*>(inRHS.mPtr->mSharedCounters)) { JPH_ASSERT(inRHS.mPtr->SupportsWeakReferences()); AddWeakRef(); }
	inline					WeakRefConst(const WeakRefConst<T> &inRHS)				: mPtr(inRHS.mPtr) { AddWeakRef(); }
	inline					WeakRefConst(WeakRefConst<T> &&inRHS) noexcept			: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }
	inline					WeakRefConst(const WeakRef<T> &inRHS)					: mPtr(inRHS.mPtr) { AddWeakRef(); }
	inline					WeakRefConst(WeakRef<T> &&inRHS) noexcept				: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }
	inline					~WeakRefConst()											{ ReleaseWeak(); }

	/// Assignment operators
	inline WeakRefConst<T> &	operator = (const WeakRefConst<T> &inRHS)			{ if (GetPtr() != inRHS.GetPtr()) { ReleaseWeak(); mPtr = inRHS.mPtr; AddWeakRef(); } return *this; }
	inline WeakRefConst<T> &	operator = (WeakRefConst<T> &&inRHS) noexcept		{ if (GetPtr() != inRHS.GetPtr()) { ReleaseWeak(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }
	inline WeakRefConst<T> &	operator = (const WeakRef<T> &inRHS)				{ if (GetPtr() != inRHS.GetPtr()) { ReleaseWeak(); mPtr = inRHS.mPtr; AddWeakRef(); } return *this; }
	inline WeakRefConst<T> &	operator = (WeakRef<T> &&inRHS) noexcept			{ if (GetPtr() != inRHS.GetPtr()) { ReleaseWeak(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }

	inline RefConst<T> TryToStrongRef() const
	{
		if (mPtr->AddStrongRefIfNonZero() == 0)
			std::abort();
		return RefConst<T>(RefConst<T>::NoAddRefTag(), mPtr->GetPayload());
	}

	inline RefConst<T> ToStrongRefOrNull() const
	{
		if (mPtr->AddStrongRefIfNonZero() == 0)
			return {};
		return RefConst<T>(Ref<T>::NoAddRefTag(), mPtr->GetPayload());
	}

	/// Comparison
	inline bool				operator == (const T * inRHS) const					{ return GetPtr() == inRHS; }
	inline bool				operator == (const WeakRefConst<T> &inRHS) const	{ return GetPtr() == inRHS.GetPtr(); }
	inline bool				operator == (const WeakRef<T> &inRHS) const			{ return GetPtr() == inRHS.GetPtr(); }
	inline bool				operator != (const T * inRHS) const					{ return GetPtr() != inRHS; }
	inline bool				operator != (const WeakRefConst<T> &inRHS) const	{ return GetPtr() != inRHS.GetPtr(); }
	inline bool				operator != (const WeakRef<T> &inRHS) const			{ return GetPtr() != inRHS.GetPtr(); }

	inline bool				IsNull() const										{ return (!mPtr || mPtr->IsNull()); }

	inline SharedPointerCounters<T>* GetCountersPtr() const						{ return mPtr; }

private:
	/// Use "variable = nullptr;" to release an object, do not call these functions
	inline void				AddWeakRef()										{ if (mPtr) mPtr->AddWeakRef(); }
	inline void				ReleaseWeak()										{ if (mPtr) mPtr->ReleaseWeak(); }

	/// Get pointer
	inline const T *		GetPtr() const&										{ if (IsNull()) return nullptr; return mPtr->GetPayload(); }

	SharedPointerCounters<T>*				mPtr;											///< Pointer to object that we are reference counting
};

template <class T>
class WeakRef
{
public:
	template <typename T2> friend class Ref;
	template <typename T2> friend class RefConst;
	template <typename T2> friend class WeakRefConst;
	template <typename T2> friend class WeakRef;

	/// Constructor
	inline					WeakRef()										: mPtr(nullptr) { }
	inline explicit			WeakRef(T* inRHS)								: mPtr(reinterpret_cast<SharedPointerCounters<T>*>(inRHS->mSharedCounters)) { JPH_ASSERT(inRHS->SupportsWeakReferences()); AddWeakRef(); }
	inline					WeakRef(const Ref<T> &inRHS)					: mPtr(reinterpret_cast<SharedPointerCounters<T>*>(inRHS.mPtr->mSharedCounters)) { AddWeakRef(); }
	inline					WeakRef(const WeakRef<T> &inRHS)				: mPtr(inRHS.mPtr) { AddWeakRef(); }
	inline					WeakRef(WeakRef<T> &&inRHS) noexcept			: mPtr(inRHS.mPtr) { inRHS.mPtr = nullptr; }
	inline					~WeakRef()										{ ReleaseWeak(); }

	/// Assignment operators
	inline WeakRef<T> &			operator = (const WeakRef<T> &inRHS)		{ if (mPtr != inRHS.mPtr) { ReleaseWeak(); mPtr = inRHS.mPtr; AddWeakRef(); } return *this; }
	inline WeakRef<T> &			operator = (WeakRef<T> &&inRHS) noexcept	{ if (mPtr != inRHS.mPtr) { ReleaseWeak(); mPtr = inRHS.mPtr; inRHS.mPtr = nullptr; } return *this; }

	inline Ref<T> TryToStrongRef() const
	{
		if (mPtr->AddStrongRefIfNonZero() == 0)
			std::abort();
		return Ref<T>(RefConst<T>::NoAddRefTag(), mPtr->GetPayload());
	}

	inline Ref<T> ToStrongRefOrNull() const
	{
		if (mPtr->AddStrongRefIfNonZero() == 0)
			return {};
		return Ref<T>(Ref<T>::NoAddRefTag(), mPtr->GetPayload());
	}

	/// Comparison
	inline bool				operator == (const WeakRef<T> &inRHS) const		{ return GetPtr() == inRHS.GetPtr(); }
	inline bool				operator != (const WeakRef<T> &inRHS) const		{ return GetPtr() != inRHS.GetPtr(); }

	inline bool				IsNull() const									{ return (!mPtr || mPtr->IsNull()); }

	inline SharedPointerCounters<T>* GetCountersPtr() const					{ return mPtr; }

private:
	template <class T2> friend class WeakRefConst;

	/// Use "variable = nullptr;" to release an object, do not call these functions
	inline void				AddWeakRef()									{ if (mPtr) mPtr->AddWeakRef(); }
	inline void				ReleaseWeak()									{ if (mPtr) mPtr->ReleaseWeak(); }

	inline T *				GetPtr() const&
	{
		if (IsNull())
			return nullptr;
		return mPtr->GetPayload();
	}

	SharedPointerCounters<T>*						mPtr;											///< Pointer to object that we are reference counting
};

template <typename T>
inline Ref<T> IntoShared(T* ptr)
{
	// JPH_ASSERT(ptr->mRefCount.load(memory_order_seq_cst) == 0);
	JPH_ASSERT(!ptr->SupportsWeakReferences());
	SharedPointerCounters<T>* counters = new SharedPointerCounters<T>();
	counters->SetPtrs(ptr, nullptr);
	ptr->mSharedCounters = counters;
	counters->mRefCount.fetch_add(ptr->mRefCount.load(memory_order_seq_cst));
	return Ref<T>(typename Ref<T>::NoAddRefTag(), ptr);
}

template <typename T, typename... Args>
inline Ref<T> MakeShared(Args&&... args)
{
	struct Anonymous
	{
		Anonymous() = default;

		union Uninitialized
		{
			T some;
			uint8_t nothing;
			Uninitialized() {}
			~Uninitialized() {}
		};

		// this must be at the start so pointer casting can work
		SharedPointerCounters<T> counters;
		Uninitialized payload;
	};

	static_assert(std::is_constructible_v<T, Args...>, "No matching constructor with given arguments found");

	Anonymous* out = new Anonymous();

	new (&out->payload.some) T(std::forward<Args>(args)...);
	// give both parts pointers to each other
	out->counters.SetPtrs(&(out->payload.some), [](const SharedPointerCounters<T>* self) { delete reinterpret_cast<const Anonymous*>(self); });
	out->payload.some.mSharedCounters = &out->counters;
	return Ref<T>(std::addressof(out->payload.some));
}

JPH_NAMESPACE_END

JPH_SUPPRESS_WARNING_PUSH
JPH_CLANG_SUPPRESS_WARNING("-Wc++98-compat")

namespace std
{
	/// Declare std::hash for Ref
	template <class T>
	struct hash<JPH::Ref<T>>
	{
		size_t operator () (const JPH::Ref<T> &inRHS) const
		{
			return size_t(inRHS.GetHash());
		}
	};

	/// Declare std::hash for RefConst
	template <class T>
	struct hash<JPH::RefConst<T>>
	{
		size_t operator () (const JPH::RefConst<T> &inRHS) const
		{
			return size_t(inRHS.GetHash());
		}
	};
}

JPH_SUPPRESS_WARNING_POP
