#include "ftl/block_mapping.hh"

#include <algorithm>
#include <limits>
#include <random>

#include "util/algorithm.hh"
#include "util/bitset.hh"

namespace SimpleSSD {

namespace FTL {


BlockMapping::BlockMapping(ConfigReader &c, Parameter &p, PAL::PAL *l,
                         DRAM::AbstractDRAM *d)
    : AbstractFTL(p, l, d),
      pPAL(l),
      conf(c),
      lastFreeBlock(param.pageCountToMaxPerf),
      lastFreeBlockIOMap(param.ioUnitInPage),
      bReclaimMore(false) {
  // physical block number -> block infomation
  blocks.reserve(param.totalPhysicalBlocks);
  // logical block number -> physical block number
  blockTable.reserve(param.totalLogicalBlocks);
  // list of block infomations
  for (uint32_t i = 0; i < param.totalPhysicalBlocks; i++) {
    freeBlocks.emplace_back(Block(i, param.pagesInBlock, param.ioUnitInPage));
  }
  nFreeBlocks = param.totalPhysicalBlocks;

  // pageCountToMaxPerf = number of superblocks
  for (uint32_t i = 0; i < param.pageCountToMaxPerf; i++) {
    lastFreeBlock.at(i) = getFreeBlock(i);
  }
  lastFreeBlockIndex = 0;

  memset(&stat, 0, sizeof(stat));

  bRandomTweak = conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = bRandomTweak ? param.ioUnitInPage : 1;
}

BlockMapping::~BlockMapping() {}

bool BlockMapping::initialize() {
  
  uint64_t totalLogicalBlocks;
  uint64_t maxPagesBeforeGC;
  uint64_t tick;
  uint64_t valid;
  uint64_t invalid;
  uint64_t lbn;
  FILLING_MODE mode;

  uint64_t nBlocksToWarmup;
  uint64_t nBlocksToInvalidate;
  
  blockRequest req(param.ioUnitInPage);

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization started");
  
  totalLogicalBlocks = param.totalLogicalBlocks;
  nBlocksToWarmup =
      totalLogicalBlocks * conf.readFloat(CONFIG_FTL, FTL_FILL_RATIO);
  nBlocksToInvalidate =
      totalLogicalBlocks * conf.readFloat(CONFIG_FTL, FTL_INVALID_PAGE_RATIO);
  /*
  #  0: Sequential filling + Sequential invalidation
  #  1: Sequential filling + Random invalidation
  #  2: Random filling + Random invalidation
  */
  mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);


  maxPagesBeforeGC =
      param.pagesInBlock *
      (param.totalPhysicalBlocks *
           (1 - conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO)) -
       param.pageCountToMaxPerf);  // # free blocks to maintain

  if ((nBlocksToWarmup + nBlocksToInvalidate) * param.pagesInBlock > maxPagesBeforeGC) {
    warn("ftl: Too high filling ratio. Adjusting invalidPageRatio.");
    nBlocksToInvalidate = maxPagesBeforeGC / param.pagesInBlock - nBlocksToWarmup;
  }
  
  debugprint(LOG_FTL_PAGE_MAPPING, "Total logical blocks: %" PRIu64,
             totalLogicalBlocks);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total logical blocks to fill: %" PRIu64 " (%.2f %%)",
             nBlocksToWarmup, nBlocksToWarmup * 100.f / totalLogicalBlocks);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total invalidated blocks to create: %" PRIu64 " (%.2f %%)",
             nBlocksToInvalidate,
             nBlocksToInvalidate * 100.f / totalLogicalBlocks);


  req.ioFlag.set();
  
  // Step 1. Filling
  if (mode == FILLING_MODE_0 || mode == FILLING_MODE_1) {
    for (uint64_t i = 0; i < nBlocksToWarmup; i++) {
      for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++){
        tick = 0;
        req.lbn = i;
        req.offset = pageIndex;
        writeInternal(req, tick, false);
      }
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, param.totalLogicalBlocks - 1);

    for (uint64_t i = 0; i < nBlocksToWarmup; i++) {
      lbn = dist(gen);
      for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++){
        tick = 0;
        req.lbn = lbn;
        req.offset = pageIndex;
        writeInternal(req, tick, false);
      }
    }
  }

  // Step 2. Invalidating
  if (mode == FILLING_MODE_0) {
    // Sequential
    for (uint64_t i = 0; i < nBlocksToInvalidate; i++) {

      auto L2PMapping = blockTable.find(i);
      
      if (L2PMapping != blockTable.end()){
        
        auto block = blocks.find(L2PMapping->second);
        if (block == blocks.end()){
            panic("block not use when initialize");
        }

        block->second.invalidateBlock();
      }
    }
  }
  else if (mode == FILLING_MODE_1) {

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nBlocksToWarmup - 1);

    for (uint64_t i = 0; i < nBlocksToInvalidate; i++) {
      auto L2PMapping = blockTable.find(dist(gen));
      
      if (L2PMapping != blockTable.end()){
        
        auto block = blocks.find(L2PMapping->second);
        if (block == blocks.end()){
            panic("block not use when initialize");
        }

        block->second.invalidateBlock();
      }
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, param.totalLogicalBlocks - 1);

    for (uint64_t i = 0; i < nBlocksToInvalidate; i++) {
      auto L2PMapping = blockTable.find(dist(gen));
      
      if (L2PMapping != blockTable.end()){
        
        auto block = blocks.find(L2PMapping->second);
        if (block == blocks.end()){
            panic("block not use when initialize");
        }

        block->second.invalidateBlock();
      }
    }
  }

  calculateTotalPages(valid, invalid);
  debugprint(LOG_FTL_PAGE_MAPPING, "Filling finished. Page status:");
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total valid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             valid, valid * 100.f / (totalLogicalBlocks * param.pagesInBlock), nBlocksToWarmup * param.pagesInBlock,
             (int64_t)(valid - nBlocksToWarmup * param.pagesInBlock));
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total invalid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             invalid, invalid * 100.f / (totalLogicalBlocks * param.pagesInBlock), nBlocksToInvalidate * param.pagesInBlock,
             (int64_t)(invalid - nBlocksToInvalidate * param.pagesInBlock));
  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization finished");
  return true;
}

void BlockMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  blockRequest breq(param.pagesInBlock, req);

  if (req.ioFlag.count() > 0) {
    readInternal(breq, tick);
    debugprint(LOG_FTL_PAGE_MAPPING,
               "READ  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               breq.lbn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ);
}

void BlockMapping::write(Request &req, uint64_t &tick) {

  uint64_t begin = tick;
  blockRequest breq(param.pagesInBlock, req);

  if (req.ioFlag.count() > 0) {
    writeInternal(breq, tick);
    debugprint(LOG_FTL_PAGE_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               breq.lbn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);
}

void BlockMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;
  blockRequest breq(param.pagesInBlock, req);

  trimInternal(breq, tick);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
             ")",
             breq.lbn, begin, tick, tick - begin);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM);
}

void BlockMapping::format(LPNRange &range, uint64_t &tick) {
  
  if (range.nlp == 0) return;

  PAL::Request req(param.ioUnitInPage);

  std::vector<uint32_t> list;

  req.ioFlag.set();
  for (auto iter = blockTable.begin(); iter != blockTable.end();) {

    if (iter->first >= range.slpn / param.pagesInBlock && iter->first < (( range.slpn + range.nlp - 1) / param.pagesInBlock) + 1 ) {

      auto block = blocks.find(iter->second);

      if (block == blocks.end()) {
        panic("Block is not in use");
      }

      uint64_t lbn = iter->first;
      uint64_t blockLpn0 = lbn * param.pagesInBlock;
      uint64_t blockLpn1 = blockLpn0 + param.pagesInBlock;

      uint32_t pBegin = (uint32_t)(std::max(range.slpn, blockLpn0) - blockLpn0); 
      uint32_t pEnd   = (uint32_t)(std::min(range.slpn + range.nlp, blockLpn1) - blockLpn0); 

      for (uint32_t idx = pBegin; idx < pEnd; idx++) {

        block->second.invalidate(idx, idx);
      }

      list.push_back(iter->second);

      iter++;
    }
    else {
      iter++;
    }
  }

  // Get blocks to erase

  std::sort(list.begin(), list.end());
  auto last = std::unique(list.begin(), list.end());
  list.erase(last, list.end());

  // Do GC only in specified blocks
  doGarbageCollection(list, tick);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::FORMAT);
}


Status *BlockMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd) {
  

  status.mappedLogicalPages = 0;
  status.freePhysicalBlocks = nFreeBlocks;


  lpnEnd = std::min<uint64_t>(lpnEnd, status.totalLogicalPages);
  
  uint32_t lbnBegin = lpnBegin / param.pagesInBlock;
  uint32_t lbnEnd = (lpnEnd + param.pagesInBlock - 1) / param.pagesInBlock;

  lbnEnd = std::min<uint32_t>(lbnEnd, param.totalLogicalBlocks);
  
  for (uint32_t lbn = lbnBegin; lbn < lbnEnd; lbn++) {
    
    auto bt = blockTable.find(lbn);
    if (bt == blockTable.end()) {
      continue;
    }

    uint32_t pbn = bt->second;

    auto block = blocks.find(pbn);
    if (block == blocks.end()) {
      panic("Block is not in use");
    }
    

    uint64_t blkLpn0 = lbn * param.pagesInBlock;
    uint64_t blkLpn1 = blkLpn0 + param.pagesInBlock;

    uint32_t pBegin = static_cast<uint32_t>(std::max(lpnBegin, blkLpn0) - blkLpn0);
    uint32_t pEnd   = static_cast<uint32_t>(std::min(lpnEnd, blkLpn1) - blkLpn0);

    if (pBegin == 0 && pEnd == param.pagesInBlock) {

      status.mappedLogicalPages += block->second.getValidPageCount();
    } else {

      status.mappedLogicalPages += block->second.getValidPageCountInEdgeBlock(pBegin, pEnd);
    }

  }

  return &status;
}

float BlockMapping::freeBlockRatio() {
  return (float)nFreeBlocks / param.totalPhysicalBlocks;
}

uint32_t BlockMapping::convertBlockIdx(uint32_t blockIdx) {
  return blockIdx % param.pageCountToMaxPerf;
}

uint32_t BlockMapping::getFreeBlock(uint32_t idx) {
  uint32_t blockIndex = 0;

  if (idx >= param.pageCountToMaxPerf) {
    panic("Index out of range");
  }

  if (nFreeBlocks > 0) {

    auto iter = freeBlocks.begin();

    for (; iter != freeBlocks.end(); iter++) {
      blockIndex = iter->getBlockIndex();

      if (blockIndex % param.pageCountToMaxPerf == idx) {
        break;
      }
    }

    if (iter == freeBlocks.end()) {
      iter = freeBlocks.begin();
      blockIndex = iter->getBlockIndex();
    }

    if (blocks.find(blockIndex) != blocks.end()) {
      panic("Corrupted");
    }

    blocks.emplace(blockIndex, std::move(*iter));

    freeBlocks.erase(iter);
    nFreeBlocks--;
  }
  else {
    panic("No free block left");
  }

  return blockIndex;
}

uint32_t BlockMapping::getLastFreeBlock(Bitset &iomap) {
  (void)iomap;
  return UINT32_MAX;
}

void BlockMapping::calculateVictimWeight(
    std::vector<std::pair<uint32_t, float>> &weight, const EVICT_POLICY policy,
    uint64_t tick) {
  float temp;

  weight.reserve(blocks.size());
  uint64_t counter = 0;

  switch (policy) {
    case POLICY_GREEDY:
    case POLICY_RANDOM:
    case POLICY_DCHOICE:
      for (auto &iter : blocks) {
      	if (iter.second.getDirtyPageCount() == 0){
      	    counter++;
            continue;
        }
        weight.push_back({iter.first, iter.second.getValidPageCountRaw()});
      }

      break;
    case POLICY_COST_BENEFIT:
      for (auto &iter : blocks) {
	if (iter.second.getDirtyPageCount() == 0){
	    counter++;
            continue;
        }
        temp = (float)(iter.second.getValidPageCountRaw()) / param.pagesInBlock;

        weight.push_back(
            {iter.first,
             temp / ((1 - temp) * (tick - iter.second.getLastAccessedTime()))});
      }

      break;
    default:
      panic("Invalid evict policy");
  }
  debugprint(LOG_FTL_PAGE_MAPPING, "all valid block count =%" PRIu64 ",  ratio =" "%.2f",counter, 1.0 - ((float)counter / param.totalLogicalBlocks));
}

void BlockMapping::selectVictimBlock(std::vector<uint32_t> &list,
                                    uint64_t &tick) {
  
  static const GC_MODE mode = (GC_MODE)conf.readInt(CONFIG_FTL, FTL_GC_MODE);
  static const EVICT_POLICY policy =
      (EVICT_POLICY)conf.readInt(CONFIG_FTL, FTL_GC_EVICT_POLICY);
  static uint32_t dChoiceParam =
      conf.readUint(CONFIG_FTL, FTL_GC_D_CHOICE_PARAM);
  uint64_t nBlocks = conf.readUint(CONFIG_FTL, FTL_GC_RECLAIM_BLOCK);
  std::vector<std::pair<uint32_t, float>> weight;

  list.clear();

  // Calculate number of blocks to reclaim
  if (mode == GC_MODE_0) {
    // DO NOTHING
  }
  else if (mode == GC_MODE_1) {
    static const float t = conf.readFloat(CONFIG_FTL, FTL_GC_RECLAIM_THRESHOLD);

    nBlocks = param.totalPhysicalBlocks * t - nFreeBlocks;
  }
  else {
    panic("Invalid GC mode");
  }

  // Calculate weights of all blocks
  calculateVictimWeight(weight, policy, tick);
  debugprint(LOG_FTL_PAGE_MAPPING, "selecting victims, list length = %" PRIu32 , weight.size());
  debugprint(LOG_FTL_PAGE_MAPPING, "reclaim goat = %" PRIu64 , nBlocks);
  debugprint(LOG_FTL_PAGE_MAPPING, "all blocks = %" PRIu64 , param.totalPhysicalBlocks);
  debugprint(LOG_FTL_PAGE_MAPPING, "freeblocks = %" PRIu32 , nFreeBlocks);
  debugprint(LOG_FTL_PAGE_MAPPING, "freeblock ratio = " "%.2f" , freeBlockRatio());
  
  if (policy == POLICY_RANDOM || policy == POLICY_DCHOICE) {
    uint64_t randomRange =
        policy == POLICY_RANDOM ? nBlocks : dChoiceParam * nBlocks;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, weight.size() - 1);
    std::vector<std::pair<uint32_t, float>> selected;

    while (selected.size() < randomRange) {
      uint64_t idx = dist(gen);

      if (weight.at(idx).first < std::numeric_limits<uint32_t>::max()) {
        selected.push_back(weight.at(idx));
        weight.at(idx).first = std::numeric_limits<uint32_t>::max();
      }
    }

    weight = std::move(selected);
  }

  // Sort weights
  std::sort(
      weight.begin(), weight.end(),
      [](std::pair<uint32_t, float> a, std::pair<uint32_t, float> b) -> bool {
        return a.second < b.second;
      });

  // Select victims from the blocks with the lowest weight
  nBlocks = MIN(nBlocks, weight.size());

  for (uint64_t i = 0; i < nBlocks; i++) {
    list.push_back(weight.at(i).first);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::SELECT_VICTIM_BLOCK);
}

void BlockMapping::doGarbageCollection(std::vector<uint32_t> &blocksToReclaim,
                                      uint64_t &tick) {
  debugprint(LOG_FTL_PAGE_MAPPING, "GC triggered, reclaim blocks: %" PRIu32 , blocksToReclaim.size());
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;

  Bitset bit(param.ioUnitInPage);//?R
  bool noValidPageInBLock = true;
  bool alreadyChooseAFreeBlock = false;
  uint64_t lbn;
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;
  std::unordered_map<uint32_t, Block>::iterator freeBlock;
  
  
  // a PBN list
  if (blocksToReclaim.size() == 0) {
    return;
  }

  // For all blocks to reclaim, collecting request structure only
  for (auto &iter : blocksToReclaim) {
  
    noValidPageInBLock = true;
    alreadyChooseAFreeBlock = false;
    auto block = blocks.find(iter);
    if (block == blocks.end()) {
      panic("Invalid block");
    }

    if (!bRandomTweak) {
      bit.set();
    }

    for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++) {
      if (block->second.pageIsValid(pageIndex)) {

        noValidPageInBLock = false;

        block->second.getLBN(pageIndex, lbn);

        if (!bRandomTweak) {
          bit.set();
        }

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        block->second.invalidate(pageIndex, 0);
        
        if (!alreadyChooseAFreeBlock){
          freeBlock = blocks.find(getFreeBlock(0));
          alreadyChooseAFreeBlock = true;
        }

        // Update mapping table
        uint32_t newBlockIdx = freeBlock->first;
        uint32_t newPageIdx = pageIndex;

        freeBlock->second.write(newPageIdx, lbn, 0, beginAt);

        req.blockIndex = newBlockIdx;
        req.pageIndex = newPageIdx;
        req.ioFlag.set();

        writeRequests.push_back(req);

        stat.validPageCopies++;
        stat.validSuperPageCopies++;
      }
    }

    if (noValidPageInBLock){
      block->second.getLBN(0, lbn);
      blockTable.erase(lbn);
    }
    else {
      auto L2PMapping = blockTable.find(lbn);
      L2PMapping->second = freeBlock->first;
    }


    // Erase block
    req.blockIndex = block->first;
    req.pageIndex = 0;
    req.ioFlag.set();

    eraseRequests.push_back(req);
  }

  // Do actual I/O here
  // This handles PAL2 limitation (SIGSEGV, infinite loop, or so-on)
  for (auto &iter : readRequests) {
    beginAt = tick;

    pPAL->read(iter, beginAt);

    readFinishedAt = MAX(readFinishedAt, beginAt);
  }

  for (auto &iter : writeRequests) {
    beginAt = readFinishedAt;

    pPAL->write(iter, beginAt);

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
  }

  for (auto &iter : eraseRequests) {
    beginAt = readFinishedAt;

    eraseInternal(iter, beginAt);

    eraseFinishedAt = MAX(eraseFinishedAt, beginAt);
  }
  warn("GC finished");
  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

void BlockMapping::readInternal(blockRequest &req, uint64_t &tick, bool sendToPAL) {
  
  PAL::Request palRequest(req);
  std::unordered_map<uint64_t, uint32_t>::iterator L2PMapping;
  
  -ad(req, tick, L2PMapping, sendToPAL);

  if (sendToPAL){
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ_INTERNAL);
  }

}

void BlockMapping::hw2Read(blockRequest &req, uint64_t &tick, std::unordered_map<uint64_t, uint32_t>::iterator &L2PMapping, bool sendToPAL){
  
  std::unordered_map<uint32_t, Block>::iterator block; //宣告一個迭代器 block，它可以用來走訪或操作 unordered_map<uint32_t, Block> 裡的元素。

  //取出request中的LBA和block offset的值
  const uint64_t lbn = req.lbn;
  const uint64_t blockOffset = req.offset;

  //查表mapping table
  L2PMapping = blockTable.find(lbn); // L2PMapping = map 裡面的一個元素，也就是一個 key-value pair。
  if(L2PMapping != blockTable.end()){ 
    const uint32_t PBN = L2PMapping->second; //有找到 -> 取得PBN值
    block = blocks.find(PBN); // 到 blocks 裡面找 physical block number -> block 的對應
    bool block_page_valid = block->second.pageIsValid(blockOffset); //取得valid值

    if(block_page_valid){  // if block 上 pageIndex = offset 的 page 為 valid
      readBlock(req,block,blockOffset,tick,sendToPAL); //if block 上 pageIndex = offset 的 page 為 valid，對物理 block 送出 read 請求
    }
    else{  //不是valid -> do nothing
      return;
    }
  }
  else { //沒找到（blocktable沒有）-> do nothing
    return;
  }
   
  /*
  需要實作的區域:
  pseudocode  ->  在 blockTable 裡找 logical -> physical 的對應
                  if 存在 logical -> physical 的對應 {
                    到 blocks 裡面找 physical block number -> block 的對應
                    if block 上 pageIndex = offset 的 page 為 valid{
                      對物理 block 送出 read 請求
                    }
                    else {
                      do nothing
                    }
                  }
                  else {
                    do nothing
                  }
  
  req 的架構
  |── reqID
  |── reqSubID
  |── logical block number
  |── offset
  └── ioFlag ( 有需要用到的話設成 1 就好，不用管 )

  blockTable 的架構 ( 存邏輯到物理位址的對應 )
  |── (LBN 0, PBN 0)
  |── (LBN 1, PBN 1)
  ...
  └── (LBN k, PBN k)

  blocks 的架構 ( 存物理位址到實際 block 的對應 )
  |── (PBN 0, block 0)
  |── (PBN 1, block 1)
  ...
  └── (PBN k, block k)

  對物理 block 送出 read 請求的函式: readBlock (req : 只要確保裡面的 lbn 跟 offset 是對的就好，ID 設成 0 也沒關係,
                                              block : 目標 block 的 PBN->block 對應, 
                                              pageIndex : 目標 page 的 offset, 
                                              tick : 計算時間 ( 不用管 ), 
                                              sendToPAL : 決定是否要往下傳到 PAL 層 ( 不用管 )
                                              )
  
  
  */
}

void BlockMapping::readBlock(blockRequest &req, std::unordered_map<uint32_t, Block>::iterator block, uint32_t pageIndex, uint64_t &tick, bool sendToPAL){

  if (sendToPAL){
    pDRAM->read(&(*&tick), 8, tick);
  }

  block->second.read(pageIndex, 0, tick);

  PAL::Request palRequest(req);
  
  if (sendToPAL){
    palRequest.blockIndex = block->first;
    palRequest.pageIndex = pageIndex;
    palRequest.ioFlag.set();
    pPAL->read(palRequest, tick);
  }
}

void BlockMapping::writeInternal(blockRequest &req, uint64_t &tick, bool sendToPAL) {

  PAL::Request palRequest(req);
  std::unordered_map<uint32_t, Block>::iterator block;
  std::unordered_map<uint64_t, uint32_t>::iterator L2PMapping;

  uint64_t finishedAt = tick;
  hw2Write(req, finishedAt, L2PMapping, sendToPAL);
  
  // Exclude CPU operation when initializing
  if (sendToPAL) {
    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);
  }

  // GC if needed
  // I assumed that init procedure never invokes GC
  static float gcThreshold = conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);
  
  if (freeBlockRatio() < gcThreshold) {
    if (!sendToPAL) {
      panic("ftl: GC triggered while in initialization");
    }

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    selectVictimBlock(list, beginAt);

    doGarbageCollection(list, beginAt);

    stat.gcCount++;
    stat.reclaimedBlocks += list.size();
  }
}

void BlockMapping::hw2Write(blockRequest &req, uint64_t &tick, std::unordered_map<uint64_t, uint32_t>::iterator &L2PMapping, bool sendToPAL){
  std::unordered_map<uint32_t, Block>::iterator sourceBlock;
  std::unordered_map<uint32_t, Block>::iterator destinationBlock;

  //取的寫入請求的LBA & block offset
  const uint64_t lbn = req.lbn;
  const uint32_t offset = static_cast<uint32_t>(req.offset);

  // 在 blockTable 裡找 logical -> physical 的對應
  L2PMapping = blockTable.find(lbn);

  if(L2PMapping != blockTable.end()){ //已有對應：block0（舊 PBN）
    sourceBlock = blocks.find(L2PMapping->second); //要寫入的實體block
    
    if(!sourceBlock->second.nothingOnThisPage(offset)){ // if block0 offset0 上面已經有存東西了，代表這次寫入是更新

      uint32_t newPBN = getFreeBlock(0); //利用 getFreeBlock(0) 拿一個全新的 block
      destinationBlock = blocks.find(newPBN);
    
      for(uint64_t i = 0; i < param.pagesInBlock; i++){
        blockRequest tempBreq(param.ioUnitInPage);
        tempBreq.lbn = req.lbn;
        tempBreq.offset = i;
        readBlock(tempBreq, sourceBlock, i, tick, sendToPAL);
      }

      auto validPageList = sourceBlock->second.getValidPageList();

      for (uint64_t i = 0; i < validPageList.size(); i++){

        if ( static_cast<uint64_t>(validPageList[i]) == req.offset ){
          continue;
        }

        writeBlock(req, destinationBlock, validPageList[i], tick, sendToPAL);
      }

      sourceBlock->second.invalidateBlock(); // 再把 block0 上的所有 page 設為 invalid，等待 GC 回收它

      writeBlock(req, destinationBlock, offset, tick, sendToPAL); //對 block1 的 offset0 送出寫入請求（把新資料寫進去）

      L2PMapping = blockTable.find(lbn);
      L2PMapping->second = destinationBlock->first;

      return;
    }
    else{ //block0 offset0 上面沒有存東西，表這次寫入是在舊 block 寫一筆新資料進來，所以直接寫在這個offset上就好
      
      writeBlock(req,sourceBlock,offset,tick,sendToPAL);
      return;
    }
  }
  else{ //代表這次寫入是在新 block 寫一筆新資料進來
    
    uint32_t newPBN = getFreeBlock(0); //利用 getFreeBlock(0) 拿一個全新的 block1
    destinationBlock = blocks.find(newPBN);
    writeBlock(req, destinationBlock, offset, tick, sendToPAL); //對 block1 的 offset0 送出寫入請求
    blockTable.emplace(static_cast<uint32_t>(lbn),newPBN); //在 blockTable 裡將前面建立的 logical block number 對應到 block1 的 physical block number 
    return;
  }
  /*
  需要實作的區域:
  假設寫入操作發生在 physical block = block0, offset = offset0 的 page 上
  pseudocode  ->  在 blockTable 裡找 logical -> physical 的對應
                  if 存在 logical -> physical 的對應 {
                    到 blocks 裡面找 physical block number -> block0 的對應
                    if offset0 上面已經有存東西了 {                                         //代表這次寫入是更新
                      利用 getFreeBlock(0) 拿一個全新的 block1
                      將 block0 裡除了 offset = offset0 的 valid page 全部搬移到 block1 上 ---> 先對 block0 上的 valid page 全部送一次讀取請求(readBlock)
                      對 block1 的 offset0 送出寫入請求                                         再根據這些 valid page 的 offset 對 block1 送出寫入請求(writeBlock) //因為搬移 page 的時候必須搬到同一個 offset 上                                     
                      再把 block0 上的所有 page 設為 invalid，等待 GC 回收它
                      最後在 blockTable 裡將 logical block number 改成對應到 block1 的 physical block number 
                    }
                    else {                                                                 //代表這次寫入是在舊 block 寫一筆新資料進來，所以直接寫在這個offset上就好
                      對 block1 的 offset0 送出寫入請求
                    }
                  }
                  else {                                                                   //代表這次寫入是在新 block 寫一筆新資料進來
                    在 blockTable 裡建立一個新的 logical block number
                    利用 getFreeBlock(0) 拿一個全新的 block1
                    對 block1 的 offset0 送出寫入請求
                    在 blockTable 裡將前面建立的 logical block number 對應到 block1 的 physical block number 
                  }
  
  req 的架構
  |── ID
  |── logical block number
  └── offset

  blockTable 的架構 ( 存邏輯到物理位址的對應 )
  |── (LBN 0, PBN 0)
  |── (LBN 1, PBN 1)
  ...
  └── (LBN k, PBN k)

  blocks 的架構 ( 存物理位址到 block 詳細資料的對應 )
  |── (PBN 0, block 0)
  |── (PBN 1, block 1)
  ...
  └── (PBN k, block k)
  
  對物理 block 送出 write 請求的函式: writeBlock(req : 只要確保裡面的 lbn 跟 offset 是對的就好，ID 設成 0 也沒關係,
                                              block : 目標 block 的 PBN->block 對應, 
                                              pageIndex : 目標 page 的 offset, 
                                              tick : 計算時間 ( 不用管 ), 
                                              sendToPAL : 決定是否要往下傳到 PAL 層 ( 不用管 )
                                              )
  
  */
}

void BlockMapping::writeBlock(blockRequest &req, std::unordered_map<uint32_t, Block>::iterator block, uint32_t pageIndex, uint64_t &tick, bool sendToPAL){
  
  PAL::Request palRequest(req);
  
  if (sendToPAL) {
    pDRAM->read(&(*&tick), 8, tick);
    pDRAM->write(&(*&tick), 8, tick);
  }

  block->second.write(pageIndex, req.lbn, 0, tick);

  if (sendToPAL) {
    palRequest.blockIndex = block->first;
    palRequest.pageIndex = pageIndex;
    palRequest.ioFlag.set();

    pPAL->write(palRequest, tick);
  }
}

void BlockMapping::trimInternal(blockRequest &req, uint64_t &tick) {
  auto L2PMapping = blockTable.find(req.lbn);

  if (L2PMapping != blockTable.end()) {

    pDRAM->read(&(*L2PMapping), 8, tick);

    auto block = blocks.find(L2PMapping->second);
    if (block == blocks.end()) {
      panic("Block is not in use");
    }

    block->second.invalidate(req.offset, 0);

    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM_INTERNAL);
  }
}

void BlockMapping::eraseInternal(PAL::Request &req, uint64_t &tick) {

  static uint64_t threshold =
      conf.readUint(CONFIG_FTL, FTL_BAD_BLOCK_THRESHOLD);
  auto block = blocks.find(req.blockIndex);

  // Sanity checks
  if (block == blocks.end()) {
    panic("No such block");
  }

  if (block->second.getValidPageCount() != 0) {
    panic("There are valid pages in victim block");
  }

  // Erase block
  block->second.erase();

  pPAL->erase(req, tick);

  // Check erase count
  uint32_t erasedCount = block->second.getEraseCount();

  if (erasedCount < threshold) {
    // Reverse search
    auto iter = freeBlocks.end();

    while (true) {
      iter--;

      if (iter->getEraseCount() <= erasedCount) {
        // emplace: insert before pos
        iter++;

        break;
      }

      if (iter == freeBlocks.begin()) {
        break;
      }
    }

    // Insert block to free block list
    freeBlocks.emplace(iter, std::move(block->second));
    debugprint(LOG_FTL_PAGE_MAPPING, "before erase, freeblocks = %" PRIu32 , nFreeBlocks);
    nFreeBlocks++;
    debugprint(LOG_FTL_PAGE_MAPPING, "after erase, freeblocks = %" PRIu32 , nFreeBlocks);
  }
  else{
    panic("YOOOOOOOO");
  }

  // Remove block from block list
  blocks.erase(block);
   debugprint(LOG_FTL_PAGE_MAPPING, "outsite erase freeblocks = %" PRIu32 , nFreeBlocks);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::ERASE_INTERNAL);
}

float BlockMapping::calculateWearLeveling() {

  uint64_t totalEraseCnt = 0;
  uint64_t sumOfSquaredEraseCnt = 0;
  uint64_t numOfBlocks = param.totalLogicalBlocks;
  uint64_t eraseCnt;

  for (auto &iter : blocks) {
    eraseCnt = iter.second.getEraseCount();
    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  // freeBlocks is sorted
  // Calculate from backward, stop when eraseCnt is zero
  for (auto riter = freeBlocks.rbegin(); riter != freeBlocks.rend(); riter++) {
    eraseCnt = riter->getEraseCount();

    if (eraseCnt == 0) {
      break;
    }

    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  if (sumOfSquaredEraseCnt == 0) {
    return -1;  // no meaning of wear-leveling
  }

  return (float)totalEraseCnt * totalEraseCnt /
         (numOfBlocks * sumOfSquaredEraseCnt);
}

void BlockMapping::calculateTotalPages(uint64_t &valid, uint64_t &invalid) {
  valid = 0;
  invalid = 0;

  for (auto &iter : blocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }
}

void BlockMapping::getStatList(std::vector<Stats> &list, std::string prefix) {
  Stats temp;

  temp.name = prefix + "page_mapping.gc.count";
  temp.desc = "Total GC count";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.reclaimed_blocks";
  temp.desc = "Total reclaimed blocks in GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.superpage_copies";
  temp.desc = "Total copied valid superpages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.page_copies";
  temp.desc = "Total copied valid pages during GC";
  list.push_back(temp);

  // For the exact definition, see following paper:
  // Li, Yongkun, Patrick PC Lee, and John Lui.
  // "Stochastic modeling of large-scale solid-state storage systems: analysis,
  // design tradeoffs and optimization." ACM SIGMETRICS (2013)
  temp.name = prefix + "page_mapping.wear_leveling";
  temp.desc = "Wear-leveling factor";
  list.push_back(temp);
}

void BlockMapping::getStatValues(std::vector<double> &values) {
  values.push_back(stat.gcCount);
  values.push_back(stat.reclaimedBlocks);
  values.push_back(stat.validSuperPageCopies);
  values.push_back(stat.validPageCopies);
  values.push_back(calculateWearLeveling());
}

void BlockMapping::resetStatValues() {
  memset(&stat, 0, sizeof(stat));
}

}  // namespace FTL

}  // namespace SimpleSSD
