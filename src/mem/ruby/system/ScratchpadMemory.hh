
#ifndef __MEM_RUBY_SYSTEM_SCRATCHPADMEMORY_HH__
#define __MEM_RUBY_SYSTEM_SCRATCHPADMEMORY_HH__

#include <string>
#include <vector>

#include "base/hashmap.hh"
#include "base/statistics.hh"
#include "mem/protocol/CacheRequestType.hh"
#include "mem/protocol/CacheResourceType.hh"
#include "mem/protocol/RubyRequest.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/recorder/CacheRecorder.hh"
#include "mem/ruby/slicc_interface/AbstractCacheEntry.hh"
#include "mem/ruby/slicc_interface/RubySlicc_ComponentMapping.hh"
#include "mem/ruby/system/BankedArray.hh"
#include "mem/ruby/system/LRUPolicy.hh"
#include "mem/ruby/system/PseudoLRUPolicy.hh"
#include "params/RubySpm.hh"
#include "sim/sim_object.hh"

class ScratchpadMemory : public SimObject
{
public:
    typedef RubySpmParams Params;
    ScratchpadMemory(const Params *p);
    ~ScratchpadMemory();

    void init();

    // Public Methods
    // perform a cache access and see if we hit or not.  Return true on a hit.
    bool tryCacheAccess(const Address& address, RubyRequestType type,
                        DataBlock*& data_ptr);

    // similar to above, but doesn't require full access check
    bool testCacheAccess(const Address& address, RubyRequestType type,
                         DataBlock*& data_ptr);

    // tests to see if an address is present in the cache
    bool isTagPresent(const Address& address) const;

    // Returns true if there is:
    //   a) a tag match on this address or there is
    //   b) an unused line in the same cache "way"
    bool cacheAvail(const Address& address) const;

    // find an unused entry and sets the tag appropriate for the address
    AbstractCacheEntry* allocate(const Address& address, AbstractCacheEntry* new_entry);
    void allocateVoid(const Address& address, AbstractCacheEntry* new_entry)
    {
        allocate(address, new_entry);
    }

    // Explicitly free up this address
    void deallocate(const Address& address);

    // Returns with the physical address of the conflicting cache line
    Address cacheProbe(const Address& address) const;

    // looks an address up in the cache
    AbstractCacheEntry* lookup(const Address& address);
    const AbstractCacheEntry* lookup(const Address& address) const;

    Cycles getLatency() const { return m_latency; }

    // Hook for checkpointing the contents of the cache
    void recordCacheContents(int cntrl, CacheRecorder* tr) const;

    // Set this address to most recently used
    void setMRU(const Address& address);

    void setLocked (const Address& addr, int context);
    void clearLocked (const Address& addr);
    bool isLocked (const Address& addr, int context);

    // Print cache contents
    void print(std::ostream& out) const;
    void printData(std::ostream& out) const;

    void regStats();
    bool checkResourceAvailable(CacheResourceType res, Address addr);
    void recordRequestType(CacheRequestType requestType);
    
    // SPM part
    bool isInSpm(const Address& address) const;
    bool isInLocalSpm(const Address& address)const;
    void readSpmData(const Address& address, DataBlock& datablock);
    void writeSpmData(const Address& address, DataBlock& datablock);
    bool shouldInvokeAllocate(const Address& address) const;
    bool spmAvail() const;
    Address spmProbe(const Address& address) const;
    void spmDeallocate(const Address& address);
    // looks an address up in the spm
    AbstractCacheEntry* lookupSpm(const Address& address);
    const AbstractCacheEntry* lookupSpm(const Address& address) const;    



public:
    Stats::Scalar m_demand_hits;
    Stats::Scalar m_demand_misses;
    Stats::Formula m_demand_accesses;

    Stats::Scalar m_sw_prefetches;
    Stats::Scalar m_hw_prefetches;
    Stats::Formula m_prefetches;

    Stats::Vector m_accessModeType;

    Stats::Scalar numDataArrayReads;
    Stats::Scalar numDataArrayWrites;
    Stats::Scalar numTagArrayReads;
    Stats::Scalar numTagArrayWrites;

    Stats::Scalar numTagArrayStalls;
    Stats::Scalar numDataArrayStalls;

private:
    // convert a Address to its location in the cache
    Index addressToCacheSet(const Address& address) const;

    // Given a cache tag: returns the index of the tag in a set.
    // returns -1 if the tag is not found.
    int findTagInSet(Index line, const Address& tag) const;
    int findTagInSetIgnorePermissions(Index cacheSet,
                                      const Address& tag) const;

    // Private copy constructor and assignment operator
    ScratchpadMemory(const ScratchpadMemory& obj);
    ScratchpadMemory& operator=(const ScratchpadMemory& obj);

private:
    const std::string m_cache_name;
    Cycles m_latency;

    // Data Members (m_prefix)
    bool m_is_instruction_only_cache;

    // The first index is the # of cache lines.
    // The second index is the the amount associativity.
    m5::hash_map<Address, int> m_tag_index;
    std::vector<std::vector<AbstractCacheEntry*> > m_cache;

    AbstractReplacementPolicy *m_replacementPolicy_ptr;

    BankedArray dataArray;
    BankedArray tagArray;

    int m_cache_size;
    std::string m_policy;
    int m_cache_num_sets;
    int m_cache_num_set_bits;
    int m_cache_assoc;
    int m_start_index_bit;
    bool m_resource_stalls;
};

std::ostream& operator<<(std::ostream& out, const ScratchpadMemory& obj);

#endif // __MEM_RUBY_SYSTEM_SCRATCHPADMEMORY_HH__
