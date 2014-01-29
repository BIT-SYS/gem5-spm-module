from m5.params import *
from m5.SimObject import SimObject
from Controller import RubyController

class RubySpm(SimObject):
    type = 'RubySpm'
    cxx_class = 'ScratchpadMemory'
    cxx_header = "mem/ruby/system/ScratchpadMemory.hh"
    size = Param.MemorySize("capacity in bytes");
    latency = Param.Cycles("");
    assoc = Param.Int("");
    replacement_policy = Param.String("PSEUDO_LRU", "");
    start_index_bit = Param.Int(6, "index start, default 6 for 64-byte line");
    is_icache = Param.Bool(False, "is instruction only cache");

    dataArrayBanks = Param.Int(1, "Number of banks for the data array")
    tagArrayBanks = Param.Int(1, "Number of banks for the tag array")
    dataAccessLatency = Param.Cycles(1, "cycles for a data array access")
    tagAccessLatency = Param.Cycles(1, "cycles for a tag array access")
    resourceStalls = Param.Bool(False, "stall if there is a resource failure")

    # ScratchpadMemory configuration
    spm_size = Param.MemorySize("64kB")
    spm_blksize_bit = Param.Int(6, "block size bitsize")