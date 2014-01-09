
#include "base/intmath.hh"
#include "debug/RubySpm.hh"
#include "debug/RubySpmTrace.hh"
#include "debug/RubyResourceStalls.hh"
#include "debug/RubyStats.hh"
#include "mem/protocol/AccessPermission.hh"
#include "mem/ruby/system/ScratchpadMemory.hh"
#include "mem/ruby/system/System.hh"

using namespace std;

ostream&
operator<<(ostream& out, const ScratchpadMemory& obj)
{
    obj.print(out);
    out << flush;
    return out;
}

ScratchpadMemory *
RubySpmParams::create()
{
    return new ScratchpadMemory(this);
}

ScratchpadMemory::ScratchpadMemory(const Params *p)
    : SimObject(p),
    dataArray(p->dataArrayBanks, p->dataAccessLatency, p->start_index_bit),
    tagArray(p->tagArrayBanks, p->tagAccessLatency, p->start_index_bit)
{
    m_cache_size = p->size;
    m_latency = p->latency;
    m_cache_assoc = p->assoc;
    m_policy = p->replacement_policy;
    m_start_index_bit = p->start_index_bit;
    m_is_instruction_only_cache = p->is_icache;
    m_resource_stalls = p->resourceStalls;
}

void
ScratchpadMemory::init()
{
    m_cache_num_sets = (m_cache_size / m_cache_assoc) /
        RubySystem::getBlockSizeBytes();
    assert(m_cache_num_sets > 1);
    m_cache_num_set_bits = floorLog2(m_cache_num_sets);
    assert(m_cache_num_set_bits > 0);

    if (m_policy == "PSEUDO_LRU")
        m_replacementPolicy_ptr =
            new PseudoLRUPolicy(m_cache_num_sets, m_cache_assoc);
    else if (m_policy == "LRU")
        m_replacementPolicy_ptr =
            new LRUPolicy(m_cache_num_sets, m_cache_assoc);
    else
        assert(false);

    m_cache.resize(m_cache_num_sets);
    for (int i = 0; i < m_cache_num_sets; i++) {
        m_cache[i].resize(m_cache_assoc);
        for (int j = 0; j < m_cache_assoc; j++) {
            m_cache[i][j] = NULL;
        }
    }
}

ScratchpadMemory::~ScratchpadMemory()
{
    if (m_replacementPolicy_ptr != NULL)
        delete m_replacementPolicy_ptr;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            delete m_cache[i][j];
        }
    }
}

// convert a Address to its location in the cache
Index
ScratchpadMemory::addressToCacheSet(const Address& address) const
{
    assert(address == line_address(address));
    return address.bitSelect(m_start_index_bit,
                             m_start_index_bit + m_cache_num_set_bits - 1);
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
ScratchpadMemory::findTagInSet(Index cacheSet, const Address& tag) const
{
    assert(tag == line_address(tag));
    // search the set for the tags
    m5::hash_map<Address, int>::const_iterator it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        if (m_cache[cacheSet][it->second]->m_Permission !=
            AccessPermission_NotPresent)
            return it->second;
    return -1; // Not found
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
ScratchpadMemory::findTagInSetIgnorePermissions(Index cacheSet,
                                           const Address& tag) const
{
    assert(tag == line_address(tag));
    // search the set for the tags
    m5::hash_map<Address, int>::const_iterator it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        return it->second;
    return -1; // Not found
}

bool
ScratchpadMemory::tryCacheAccess(const Address& address, RubyRequestType type,
                            DataBlock*& data_ptr)
{
    assert(address == line_address(address));
    DPRINTF(RubySpm, "address: %s\n", address);
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        if (entry->m_Permission == AccessPermission_Read_Write) {
            return true;
        }
        if ((entry->m_Permission == AccessPermission_Read_Only) &&
            (type == RubyRequestType_LD || type == RubyRequestType_IFETCH)) {
            return true;
        }
        // The line must not be accessible
    }
    data_ptr = NULL;
    return false;
}

bool
ScratchpadMemory::testCacheAccess(const Address& address, RubyRequestType type,
                             DataBlock*& data_ptr)
{
    assert(address == line_address(address));
    DPRINTF(RubySpm, "address: %s\n", address);
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        return m_cache[cacheSet][loc]->m_Permission !=
            AccessPermission_NotPresent;
    }

    data_ptr = NULL;
    return false;
}

// tests to see if an address is present in the cache
bool
ScratchpadMemory::isTagPresent(const Address& address) const
{
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc == -1) {
        // We didn't find the tag
        DPRINTF(RubySpm, "No tag match for address: %s\n", address);
        return false;
    }
    DPRINTF(RubySpm, "address: %s found\n", address);
    return true;
}

// Returns true if there is:
//   a) a tag match on this address or there is
//   b) an unused line in the same cache "way"
bool
ScratchpadMemory::cacheAvail(const Address& address) const
{
    assert(address == line_address(address));

    Index cacheSet = addressToCacheSet(address);

    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                // Already in the cache or we found an empty entry
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

AbstractCacheEntry*
ScratchpadMemory::allocate(const Address& address, AbstractCacheEntry* entry)
{
    assert(address == line_address(address));
    assert(!isTagPresent(address));
    assert(cacheAvail(address));
    DPRINTF(RubySpm, "address: %s\n", address);

    // Find the first open slot
    Index cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    for (int i = 0; i < m_cache_assoc; i++) {
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubySpm, "Allocate clearing lock for addr: %x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;

            m_replacementPolicy_ptr->touch(cacheSet, i, curTick());

            return entry;
        }
    }
    panic("Allocate didn't find an available entry");
}

void
ScratchpadMemory::deallocate(const Address& address)
{
    assert(address == line_address(address));
    assert(isTagPresent(address));
    DPRINTF(RubySpm, "address: %s\n", address);
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        delete m_cache[cacheSet][loc];
        m_cache[cacheSet][loc] = NULL;
        m_tag_index.erase(address);
    }
}

// Returns with the physical address of the conflicting cache line
Address
ScratchpadMemory::cacheProbe(const Address& address) const
{
    assert(address == line_address(address));
    assert(!cacheAvail(address));

    Index cacheSet = addressToCacheSet(address);
    return m_cache[cacheSet][m_replacementPolicy_ptr->getVictim(cacheSet)]->
        m_Address;
}

// looks an address up in the cache
AbstractCacheEntry*
ScratchpadMemory::lookup(const Address& address)
{
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if(loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// looks an address up in the cache
const AbstractCacheEntry*
ScratchpadMemory::lookup(const Address& address) const
{
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if(loc == -1) return NULL;
    return m_cache[cacheSet][loc];
}

// Sets the most recently used bit for a cache block
void
ScratchpadMemory::setMRU(const Address& address)
{
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if(loc != -1)
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
}

void
ScratchpadMemory::recordCacheContents(int cntrl, CacheRecorder* tr) const
{
    uint64 warmedUpBlocks = 0;
    uint64 totalBlocks M5_VAR_USED = (uint64)m_cache_num_sets
                                                  * (uint64)m_cache_assoc;

    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                AccessPermission perm = m_cache[i][j]->m_Permission;
                RubyRequestType request_type = RubyRequestType_NULL;
                if (perm == AccessPermission_Read_Only) {
                    if (m_is_instruction_only_cache) {
                        request_type = RubyRequestType_IFETCH;
                    } else {
                        request_type = RubyRequestType_LD;
                    }
                } else if (perm == AccessPermission_Read_Write) {
                    request_type = RubyRequestType_ST;
                }

                if (request_type != RubyRequestType_NULL) {
                    tr->addRecord(cntrl, m_cache[i][j]->m_Address.getAddress(),
                                  0, request_type,
                                  m_replacementPolicy_ptr->getLastAccess(i, j),
                                  m_cache[i][j]->getDataBlk());
                    warmedUpBlocks++;
                }
            }
        }
    }

    DPRINTF(RubySpmTrace, "%s: %lli blocks of %lli total blocks"
            "recorded %.2f%% \n", name().c_str(), warmedUpBlocks,
            (uint64)m_cache_num_sets * (uint64)m_cache_assoc,
            (float(warmedUpBlocks)/float(totalBlocks))*100.0);
}

void
ScratchpadMemory::print(ostream& out) const
{
    out << "Cache dump: " << m_cache_name << endl;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: " << *m_cache[i][j] << endl;
            } else {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: NULL" << endl;
            }
        }
    }
}

void
ScratchpadMemory::printData(ostream& out) const
{
    out << "printData() not supported" << endl;
}

void
ScratchpadMemory::setLocked(const Address& address, int context)
{
    DPRINTF(RubySpm, "Setting Lock for addr: %x to %d\n", address, context);
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    m_cache[cacheSet][loc]->m_locked = context;
}

void
ScratchpadMemory::clearLocked(const Address& address)
{
    DPRINTF(RubySpm, "Clear Lock for addr: %x\n", address);
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    m_cache[cacheSet][loc]->m_locked = -1;
}

bool
ScratchpadMemory::isLocked(const Address& address, int context)
{
    assert(address == line_address(address));
    Index cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    DPRINTF(RubySpm, "Testing Lock for addr: %llx cur %d con %d\n",
            address, m_cache[cacheSet][loc]->m_locked, context);
    return m_cache[cacheSet][loc]->m_locked == context;
}

void
ScratchpadMemory::regStats()
{
    m_demand_hits
        .name(name() + ".demand_hits")
        .desc("Number of cache demand hits")
        ;

    m_demand_misses
        .name(name() + ".demand_misses")
        .desc("Number of cache demand misses")
        ;

    m_demand_accesses
        .name(name() + ".demand_accesses")
        .desc("Number of cache demand accesses")
        ;

    m_demand_accesses = m_demand_hits + m_demand_misses;

    m_sw_prefetches
        .name(name() + ".total_sw_prefetches")
        .desc("Number of software prefetches")
        .flags(Stats::nozero)
        ;

    m_hw_prefetches
        .name(name() + ".total_hw_prefetches")
        .desc("Number of hardware prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches
        .name(name() + ".total_prefetches")
        .desc("Number of prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches = m_sw_prefetches + m_hw_prefetches;

    m_accessModeType
        .init(RubyRequestType_NUM)
        .name(name() + ".access_mode")
        .flags(Stats::pdf | Stats::total)
        ;
    for (int i = 0; i < RubyAccessMode_NUM; i++) {
        m_accessModeType
            .subname(i, RubyAccessMode_to_string(RubyAccessMode(i)))
            .flags(Stats::nozero)
            ;
    }

    numDataArrayReads
        .name(name() + ".num_data_array_reads")
        .desc("number of data array reads")
        .flags(Stats::nozero)
        ;

    numDataArrayWrites
        .name(name() + ".num_data_array_writes")
        .desc("number of data array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayReads
        .name(name() + ".num_tag_array_reads")
        .desc("number of tag array reads")
        .flags(Stats::nozero)
        ;

    numTagArrayWrites
        .name(name() + ".num_tag_array_writes")
        .desc("number of tag array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayStalls
        .name(name() + ".num_tag_array_stalls")
        .desc("number of stalls caused by tag array")
        .flags(Stats::nozero)
        ;

    numDataArrayStalls
        .name(name() + ".num_data_array_stalls")
        .desc("number of stalls caused by data array")
        .flags(Stats::nozero)
        ;
}

void
ScratchpadMemory::recordRequestType(CacheRequestType requestType)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            CacheRequestType_to_string(requestType));
    switch(requestType) {
    case CacheRequestType_DataArrayRead:
        numDataArrayReads++;
        return;
    case CacheRequestType_DataArrayWrite:
        numDataArrayWrites++;
        return;
    case CacheRequestType_TagArrayRead:
        numTagArrayReads++;
        return;
    case CacheRequestType_TagArrayWrite:
        numTagArrayWrites++;
        return;
    default:
        warn("ScratchpadMemory access_type not found: %s",
             CacheRequestType_to_string(requestType));
    }
}

bool
ScratchpadMemory::checkResourceAvailable(CacheResourceType res, Address addr)
{
    if (!m_resource_stalls) {
        return true;
    }

    if (res == CacheResourceType_TagArray) {
        if (tagArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Tag array stall on addr %s in set %d\n",
                    addr, addressToCacheSet(addr));
            numTagArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_DataArray) {
        if (dataArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Data array stall on addr %s in set %d\n",
                    addr, addressToCacheSet(addr));
            numDataArrayStalls++;
            return false;
        }
    } else {
        assert(false);
        return true;
    }
}

bool
ScratchpadMemory::isInSpm(const Address& address)
{
    DPRINTF(RubySpm, "checking address: %s", address);
    return false;
}
void
ScratchpadMemory::readSpmData(const Address& address, DataBlock& datablock)
{
    DPRINTF(RubySpm, "readSpmData NOT IMPL!\n");
    return;
}
void
ScratchpadMemory::writeSpmData(const Address& address, DataBlock& datablock)
{
    DPRINTF(RubySpm, "writeSpmData NOT IMPL!\n");
    return;
}