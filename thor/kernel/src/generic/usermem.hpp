#ifndef THOR_GENERIC_USERMEM_HPP
#define THOR_GENERIC_USERMEM_HPP

#include <frigg/rbtree.hpp>
#include <frigg/vector.hpp>
#include <frg/container_of.hpp>
#include <frg/rcu_radixtree.hpp>
#include "error.hpp"
#include "types.hpp"
#include "futex.hpp"
#include "../arch/x86/paging.hpp"

namespace thor {

struct Memory;
struct Mapping;
struct AddressSpace;
struct ForeignSpaceAccessor;

using GrabIntent = uint32_t;
enum : GrabIntent {
	kGrabQuery = GrabIntent(1) << 0,
	kGrabFetch = GrabIntent(1) << 1,
	kGrabRead = GrabIntent(1) << 2,
	kGrabWrite = GrabIntent(1) << 3,
	kGrabDontRequireBacking = GrabIntent(1) << 4
};

enum class MemoryTag {
	null,
	hardware,
	allocated,
	backing,
	frontal,
	copyOnWrite
};

struct ManageBase {
	void setup(Worklet *worklet) {
		_worklet = worklet;
	}

	Error error() { return _error; }
	uintptr_t offset() { return _offset; }
	size_t size() { return _size; }

	void setup(Error error, uintptr_t offset, size_t size) {
		_error = error;
		_offset = offset;
		_size = size;
	}

	void complete() {
		WorkQueue::post(_worklet);
	}

	frg::default_list_hook<ManageBase> processQueueItem;

private:
	// Results of the operation.
	Error _error;
	uintptr_t _offset;
	size_t _size;

	Worklet *_worklet;
};

using ManageList = frg::intrusive_list<
	ManageBase,
	frg::locate_member<
		ManageBase,
		frg::default_list_hook<ManageBase>,
		&ManageBase::processQueueItem
	>
>;

struct InitiateBase {
	void setup(uintptr_t offset_, size_t length_, Worklet *worklet) {
		offset = offset_;
		length = length_;
		_worklet = worklet;
	}
	
	Error error() { return _error; }
	
	void setup(Error error) {
		_error = error;
	}

	void complete() {
		WorkQueue::post(_worklet);
	}

	uintptr_t offset;
	size_t length;

private:
	Error _error;

	Worklet *_worklet;
public:
	frg::default_list_hook<InitiateBase> processQueueItem;

	// Current progress in bytes.
	size_t progress;
};

using InitiateList = frg::intrusive_list<
	InitiateBase,
	frg::locate_member<
		InitiateBase,
		frg::default_list_hook<InitiateBase>,
		&InitiateBase::processQueueItem
	>
>;

struct FetchNode {
	friend struct MemoryBundle;

	frigg::Tuple<PhysicalAddr, size_t> range() {
		return _range;
	}

private:
	void (*_fetched)(FetchNode *);

	frigg::Tuple<PhysicalAddr, size_t> _range;
};

struct MemoryBundle {	
protected:
	static void setupFetch(FetchNode *node, void (*fetched)(FetchNode *)) {
		node->_fetched = fetched;
	}

	static void completeFetch(FetchNode *node, PhysicalAddr physical, size_t size) {
		node->_range = frigg::Tuple<PhysicalAddr, size_t>{physical, size};
	}
	
	static void callbackFetch(FetchNode *node) {
		node->_fetched(node);
	}

public:
	// Optimistically returns the physical memory that backs a range of memory.
	// Result stays valid until the range is evicted.
	virtual PhysicalAddr peekRange(uintptr_t offset) = 0;

	// Returns the physical memory that backs a range of memory.
	// Ensures that the range is present before returning.
	// Result stays valid until the range is evicted.
	virtual bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) = 0;
	
	PhysicalAddr blockForRange(uintptr_t offset);
};

struct VirtualView {
	virtual frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) = 0;
};

struct CowBundle : MemoryBundle {
	CowBundle(frigg::SharedPtr<VirtualView> view, ptrdiff_t offset, size_t size);

	CowBundle(frigg::SharedPtr<CowBundle> chain, ptrdiff_t offset, size_t size);

	PhysicalAddr peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) override;

private:
	frigg::TicketLock _mutex;

	frigg::SharedPtr<VirtualView> _superRoot;
	frigg::SharedPtr<CowBundle> _superChain;
	ptrdiff_t _superOffset;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
	frigg::SharedPtr<Memory> _copy;
};

struct Memory : MemoryBundle {
	static void transfer(MemoryBundle *dest_memory, uintptr_t dest_offset,
			MemoryBundle *src_memory, uintptr_t src_offset, size_t length);

	Memory(MemoryTag tag)
	: _tag(tag) { }

	Memory(const Memory &) = delete;

	Memory &operator= (const Memory &) = delete;
	
	MemoryTag tag() const {
		return _tag;
	}

	virtual void resize(size_t new_length);

	virtual void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length);

	size_t getLength();

	// TODO: InitiateLoad does more or less the same as fetchRange(). Remove it.
	void submitInitiateLoad(InitiateBase *initiate);

	void submitManage(ManageBase *handle);
	void completeLoad(size_t offset, size_t length);

private:
	MemoryTag _tag;
};

struct CopyToBundleNode {

};

struct CopyFromBundleNode {

};

void copyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *));

void copyFromBundle(Memory *bundle, ptrdiff_t offset, void *pointer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *));

struct HardwareMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::hardware;
	}

	HardwareMemory(PhysicalAddr base, size_t length);
	~HardwareMemory();

	PhysicalAddr peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) override;

	size_t getLength();

private:
	PhysicalAddr _base;
	size_t _length;
};

struct AllocatedMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::allocated;
	}

	AllocatedMemory(size_t length, size_t chunk_size = kPageSize,
			size_t chunk_align = kPageSize);
	~AllocatedMemory();

	void resize(size_t new_length) override;

	void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length) override;

	PhysicalAddr peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) override;
	
	size_t getLength();

private:
	frigg::TicketLock _mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalChunks;
	size_t _chunkSize, _chunkAlign;
};

struct ManagedSpace {
	enum LoadState {
		kStateMissing,
		kStateLoading,
		kStateLoaded
	};

	ManagedSpace(size_t length);
	~ManagedSpace();
	
	void progressLoads();
	bool isComplete(InitiateBase *initiate);

	frigg::TicketLock mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> physicalPages;
	frigg::Vector<LoadState, KernelAlloc> loadState;

	InitiateList initiateLoadQueue;
	InitiateList pendingLoadQueue;
	InitiateList completedLoadQueue;

	ManageList submittedManageQueue;
	ManageList completedManageQueue;
};

struct BackingMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::backing;
	}

	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::backing), _managed(frigg::move(managed)) { }

	PhysicalAddr peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) override;

	size_t getLength();

	void submitManage(ManageBase *handle);
	void completeLoad(size_t offset, size_t length);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct FrontalMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::frontal;
	}

	FrontalMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::frontal), _managed(frigg::move(managed)) { }

	PhysicalAddr peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) override;

	size_t getLength();

	void submitInitiateLoad(InitiateBase *initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct ExteriorBundleView : VirtualView {
	ExteriorBundleView(frigg::SharedPtr<MemoryBundle> bundle,
			ptrdiff_t view_offset, size_t view_size);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

private:
	frigg::SharedPtr<MemoryBundle> _bundle;
	ptrdiff_t _viewOffset;
	size_t _viewSize;
};

struct FaultNode {
	friend struct AddressSpace;
	friend struct NormalMapping;
	friend struct CowMapping;

	FaultNode()
	: _resolved{false} { }

	FaultNode(const FaultNode &) = delete;

	FaultNode &operator= (const FaultNode &) = delete;

	bool resolved() {
		return _resolved;
	}

private:
	VirtualAddr _address;
	uint32_t _flags;
	void (*_handled)(FaultNode *);

	bool _resolved;

	Mapping *_mapping;
	FetchNode _fetch;
	uintptr_t _bundleOffset;
};

struct ForkItem {
	Mapping *mapping;
	AllocatedMemory *destBundle;
};

struct ForkNode {
	friend struct AddressSpace;

	ForkNode()
	: _items{*kernelAlloc} { }

	frigg::SharedPtr<AddressSpace> forkedSpace() {
		return frigg::move(_fork);
	}

private:
	void (*_forked)(ForkNode *);

	// TODO: This should be a SharedPtr, too.
	AddressSpace *_original;
	frigg::SharedPtr<AddressSpace> _fork;
	frigg::LinkedList<ForkItem, KernelAlloc> _items;
	FetchNode _fetch;
	size_t _progress;
};

struct Hole {
	Hole(VirtualAddr address, size_t length)
	: _address{address}, _length{length}, largestHole{0} { }

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	frigg::rbtree_hook treeNode;

private:
	VirtualAddr _address;
	size_t _length;

public:
	// Largest hole in the subtree of this node.
	size_t largestHole;
};

enum MappingFlags : uint32_t {
	null = 0,

	forkMask = 0x07,
	dropAtFork = 0x01,
	shareAtFork = 0x02,
	copyOnWriteAtFork = 0x04,

	permissionMask = 0x70,
	protRead = 0x10,
	protWrite = 0x20,
	protExecute = 0x40,

	dontRequireBacking = 0x100
};

struct Mapping {
	Mapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags);

	virtual ~Mapping() { }

	AddressSpace *owner() {
		return _owner;
	}

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	MappingFlags flags() const {
		return _flags;
	}

	virtual frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) = 0;

	virtual Mapping *shareMapping(AddressSpace *dest_space) = 0;
	virtual Mapping *copyOnWrite(AddressSpace *dest_space) = 0;

	virtual void install(bool overwrite) = 0;
	virtual void uninstall(bool clear) = 0;
	
	virtual bool handleFault(FaultNode *node) = 0;

	frigg::rbtree_hook treeNode;

private:
	AddressSpace *_owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;
};

struct NormalMapping : Mapping {
	NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<VirtualView> view, uintptr_t offset);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	bool handleFault(FaultNode *node) override;

private:
	frigg::SharedPtr<VirtualView> _view;
	size_t _offset;
};

struct CowMapping : Mapping {
	CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<CowBundle> chain);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	bool handleFault(FaultNode *node) override;

private:
	frigg::SharedPtr<CowBundle> _cowBundle;
};

struct HoleLess {
	bool operator() (const Hole &a, const Hole &b) {
		return a.address() < b.address();
	}
};

struct HoleAggregator;

using HoleTree = frigg::rbtree<
	Hole,
	&Hole::treeNode,
	HoleLess,
	HoleAggregator
>;

struct HoleAggregator {
	static bool aggregate(Hole *node);
	static bool check_invariant(HoleTree &tree, Hole *node);
};

struct MappingLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.address() < b.address();
	}
};

using MappingTree = frigg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct AddressUnmapNode {
	friend struct AddressSpace;

private:
	AddressSpace *_space;
	ShootNode _shootNode;
};

class AddressSpace {
	friend struct ForeignSpaceAccessor;
	friend struct NormalMapping;
	friend struct CowMapping;

public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapDropAtFork = 0x40,
		kMapShareAtFork = 0x80,
		kMapCopyOnWriteAtFork = 0x100,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	AddressSpace();

	~AddressSpace();

	void setupDefaultMappings();

	void map(Guard &guard, frigg::UnsafePtr<VirtualView> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);
	
	void unmap(Guard &guard, VirtualAddr address, size_t length,
			AddressUnmapNode *node);

	bool handleFault(VirtualAddr address, uint32_t flags,
			FaultNode *node, void (*handled)(FaultNode *));
	
	bool fork(ForkNode *node);
	
	void activate();

	Lock lock;
	
	Futex futexSpace;

private:
	
	// Allocates a new mapping of the given length somewhere in the address space.
	VirtualAddr _allocate(size_t length, MapFlags flags);

	VirtualAddr _allocateAt(VirtualAddr address, size_t length);
	
	Mapping *_getMapping(VirtualAddr address);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);
	
	HoleTree _holes;
	MappingTree _mappings;

	ClientPageSpace _pageSpace;
};

struct AcquireNode {
	friend struct ForeignSpaceAccessor;

	AcquireNode()
	: _acquired{nullptr}, _progress{0} { }

private:
	void (*_acquired)(AcquireNode *);

	ForeignSpaceAccessor *_accessor;
	FetchNode _fetch;
	size_t _progress;
};

struct ForeignSpaceAccessor {
private:
	static bool _processAcquire(AcquireNode *node);
	static void _fetchedAcquire(FetchNode *node);

public:
	friend void swap(ForeignSpaceAccessor &a, ForeignSpaceAccessor &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
		frigg::swap(a._acquired, b._acquired);
	}

	ForeignSpaceAccessor()
	: _acquired{false} { }
	
	ForeignSpaceAccessor(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length)
	: _space(frigg::move(space)), _address(address), _length(length) { }

	ForeignSpaceAccessor(const ForeignSpaceAccessor &other) = delete;

	ForeignSpaceAccessor(ForeignSpaceAccessor &&other)
	: ForeignSpaceAccessor() {
		swap(*this, other);
	}
	
	ForeignSpaceAccessor &operator= (ForeignSpaceAccessor other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	uintptr_t address() {
		return (uintptr_t)_address;
	}
	size_t length() {
		return _length;
	}

	bool acquire(AcquireNode *node, void (*acquired)(AcquireNode *));
	
	PhysicalAddr getPhysical(size_t offset);

	void load(size_t offset, void *pointer, size_t size);
	Error write(size_t offset, const void *pointer, size_t size);

	template<typename T>
	T read(size_t offset) {
		T value;
		load(offset, &value, sizeof(T));
		return value;
	}

	template<typename T>
	Error write(size_t offset, T value) {
		return write(offset, &value, sizeof(T));
	}

private:
	PhysicalAddr _resolvePhysical(VirtualAddr vaddr);

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
	size_t _length;
	bool _acquired;
};

} // namespace thor

#endif // THOR_GENERIC_USERMEM_HPP
