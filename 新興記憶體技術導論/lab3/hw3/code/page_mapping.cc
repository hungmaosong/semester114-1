/*
 * Copyright (C) 2017 CAMELab
 *
 * This file is part of SimpleSSD.
 *
 * SimpleSSD is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SimpleSSD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SimpleSSD.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ftl/page_mapping.hh"

#include <algorithm>
#include <limits>
#include <random>

#include "util/algorithm.hh"
#include "util/bitset.hh"

namespace SimpleSSD {

namespace FTL {

PageMapping::PageMapping(ConfigReader &c, Parameter &p, PAL::PAL *l,
                         DRAM::AbstractDRAM *d)
    : AbstractFTL(p, l, d),
      pPAL(l),
      conf(c),
      lastFreeBlock(param.pageCountToMaxPerf),
      lastFreeBlockIOMap(param.ioUnitInPage),
      bReclaimMore(false) {

  /* 這部分只是說明，沒有需要改的地方

  所有的 block 一開始就依照 ratio 分到 hot pool / cold pool

  blocks
  │── hotBlocks
  │   ├── ( PBN 0, Block 0 )      ---> 假設為 freeblock
  │   ├── ( PBN 1, Block 1 ) 
  │   └── ...
  └── coldBlocks   
      ├── ( PBN 888, Block 888 )  
      ├── ( PBN 999, Block 999 )  ---> 假設為 freeblock
      └── ...

  另外維護一個 freeblocks，存哪些 block 被清空過，可以寫入的 ( 紀錄 physical block number )
  
  freeBlocks
  ├── PBN 0
  ├── PBN 999
  └── other freeblock's PBN ...

  決定 hot pool 和 cold pool 要不要交換的變數為 swapThreshold
  這邊設為 2 是因為 IO 的量不多(不然要跑很久)，整體 block erase 的次數很少，為了處發函式才設置這麼低，不然通常會是 block 壽命的 10% 左右

  ┌ ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ┐
  │ *這些是維護 erase count 和 effective erase count 的結構，不一定要用原本寫好的這些，如果想用其他方法紀錄請自行更改                                         │
  │                                                                                                                                                       │
  │ eraseCountOfHotBlocks 負責維護 hotBlocks erase count 的有序陣列                                                                                        │
  │ effectiveEraseCountOfHotBlocks 負責維護 hotBlocks effective erase count 的有序陣列                                                                     │
  │ coldBlock 也有和 hotBlocks 一樣的兩個 multiset                                                                                                         │
  │                                                                                                                                                       │
  │ whereIsEraseCount 紀錄 eraseCountOfHotBlocks 和 eraseCountOfColdBlocks 裡面元素的 iterator 位置，方便更新 erase count 時使用                            │
  │ whereIsEffectiveEraseCount 紀錄 effectiveEraseCountOfHotBlocks 和 effectiveEraseCountOfColdBlocks 裡面元素的 iterator 位置，方便更新 erase count 時使用 │
  │                                                                                                                                                       │                     
  └ ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ┘

  */

  float hotBlockRatio = 0.2;
  swapThreshold = 2;

  table.reserve(param.totalLogicalBlocks * param.pagesInBlock);

  for (uint32_t i = 0; i < param.totalPhysicalBlocks; i++) {
    // 一開始所有 block 都是 free 的
    freeBlocks.emplace_back(i);
  }
  
  for (uint32_t i = 0; i < param.totalPhysicalBlocks; i++) {
    // 預設 20% 的 block 為 hot，80% 的 block 為 cold
    if (i < param.totalPhysicalBlocks * hotBlockRatio){
      hotBlocks.emplace(i, Block(i, param.pagesInBlock, param.ioUnitInPage));
      
      whereIsEraseCount[i] = eraseCountOfHotBlocks.insert(BlockOrder{i, 0});
      whereIsEffectiveEraseCount[i] = effectiveEraseCountOfHotBlocks.insert(BlockOrder{i, 0});

    }
    else{
      coldBlocks.emplace(i, Block(i, param.pagesInBlock, param.ioUnitInPage));
      whereIsEraseCount[i] = eraseCountOfColdBlocks.insert(BlockOrder{i, 0});
      whereIsEffectiveEraseCount[i] = effectiveEraseCountOfColdBlocks.insert(BlockOrder{i, 0});
    }
  }  

  nFreeBlocks = param.totalPhysicalBlocks;

  status.totalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;

  for (uint32_t i = 0; i < param.pageCountToMaxPerf; i++) {
    lastFreeBlock.at(i) = getFreeBlock(i, INT32_MAX);
  }

  lastFreeBlockIndex = 0;

  memset(&stat, 0, sizeof(stat));

  // bRandomTweak 為 flase
  bRandomTweak = conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = bRandomTweak ? param.ioUnitInPage : 1;
}

PageMapping::~PageMapping() {}

// 根據設定檔，在 IO 進入前先 write/invalid 一部份的 block，模擬已經被用過的 SSD 
bool PageMapping::initialize() {
  uint64_t nPagesToWarmup;
  uint64_t nPagesToInvalidate;
  uint64_t nTotalLogicalPages;
  uint64_t maxPagesBeforeGC;
  uint64_t tick;
  uint64_t valid;
  uint64_t invalid;
  FILLING_MODE mode;

  Request req(param.ioUnitInPage);

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization started");

  nTotalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;
  nPagesToWarmup =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_FILL_RATIO);
  nPagesToInvalidate =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_INVALID_PAGE_RATIO);
  mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);
  maxPagesBeforeGC =
      param.pagesInBlock *
      (param.totalPhysicalBlocks *
           (1 - conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO)) -
       param.pageCountToMaxPerf);  // # free blocks to maintain

  if (nPagesToWarmup + nPagesToInvalidate > maxPagesBeforeGC) {
    warn("ftl: Too high filling ratio. Adjusting invalidPageRatio.");
    nPagesToInvalidate = maxPagesBeforeGC - nPagesToWarmup;
  }

  debugprint(LOG_FTL_PAGE_MAPPING, "Total logical pages: %" PRIu64,
             nTotalLogicalPages);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total logical pages to fill: %" PRIu64 " (%.2f %%)",
             nPagesToWarmup, nPagesToWarmup * 100.f / nTotalLogicalPages);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total invalidated pages to create: %" PRIu64 " (%.2f %%)",
             nPagesToInvalidate,
             nPagesToInvalidate * 100.f / nTotalLogicalPages);

  req.ioFlag.set();

  // Step 1. Filling
  if (mode == FILLING_MODE_0 || mode == FILLING_MODE_1) {
    // Sequential
    for (uint64_t i = 0; i < nPagesToWarmup; i++) {
      tick = 0;
      req.lpn = i;
      writeInternal(req, tick, false);
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

    for (uint64_t i = 0; i < nPagesToWarmup; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }

  // Step 2. Invalidating
  if (mode == FILLING_MODE_0) {
    // Sequential
    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = i;
      writeInternal(req, tick, false);
    }
  }
  else if (mode == FILLING_MODE_1) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nPagesToWarmup - 1);

    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }

  // Report
  calculateTotalPages(valid, invalid);
  debugprint(LOG_FTL_PAGE_MAPPING, "Filling finished. Page status:");
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total valid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             valid, valid * 100.f / nTotalLogicalPages, nPagesToWarmup,
             (int64_t)(valid - nPagesToWarmup));
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total invalid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             invalid, invalid * 100.f / nTotalLogicalPages, nPagesToInvalidate,
             (int64_t)(invalid - nPagesToInvalidate));
  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization finished");

  return true;
}

// 接收上層傳下來的 read request
void PageMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.ioFlag.count() > 0) {
    readInternal(req, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "READ  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ);
}

// 接收上層傳下來的 write request
void PageMapping::write(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.ioFlag.count() > 0) {
    writeInternal(req, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);
}

// 接收上層傳下來的 trim request
void PageMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  trimInternal(req, tick);

  debugprint(LOG_FTL_PAGE_MAPPING,
             "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
             ")",
             req.lpn, begin, tick, tick - begin);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM);
}

// erase 指定 range 的 block
void PageMapping::format(LPNRange &range, uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<uint32_t> list;

  req.ioFlag.set();

  for (auto iter = table.begin(); iter != table.end();) {
    if (iter->first >= range.slpn && iter->first < range.slpn + range.nlp) {
      auto &mappingList = iter->second;

      // Do trim
      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        auto &mapping = mappingList.at(idx);

        auto block = findBlock(mapping.first);

        if (block == hotBlocks.end()){
          panic("Block is not in use");
        }

        block->second.invalidate(mapping.second, idx);

        // Collect block indices
        list.push_back(mapping.first);
      }

      iter = table.erase(iter);
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

Status *PageMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd) {
  status.freePhysicalBlocks = nFreeBlocks;

  if (lpnBegin == 0 && lpnEnd >= status.totalLogicalPages) {
    status.mappedLogicalPages = table.size();
  }
  else {
    status.mappedLogicalPages = 0;

    for (uint64_t lpn = lpnBegin; lpn < lpnEnd; lpn++) {
      if (table.count(lpn) > 0) {
        status.mappedLogicalPages++;
      }
    }
  }

  return &status;
}

float PageMapping::freeBlockRatio() {
  return (float)nFreeBlocks / param.totalPhysicalBlocks;
}

uint32_t PageMapping::convertBlockIdx(uint32_t blockIdx) {
  return blockIdx % param.pageCountToMaxPerf;
}

/*
從 freeBlocks 拿一個 "全新的" block 出來，並存入 lastFreeBlock 中
                      └── block 內的 page 都被 erase 過
*/
uint32_t PageMapping::getFreeBlock(uint32_t idx, uint32_t badIndex) {
  uint32_t blockIndex = UINT32_MAX;

  if (idx >= param.pageCountToMaxPerf) {
    panic("Index out of range");
  }

  if (nFreeBlocks > 0) {
    // Search block which is blockIdx % param.pageCountToMaxPerf == idx
    auto iter = freeBlocks.begin();

    for (; iter != freeBlocks.end(); iter++) {
      if (hotBlocks.find(*iter) == hotBlocks.end() || *iter == badIndex){
        continue;
      }
      blockIndex = *iter;

      if (blockIndex % param.pageCountToMaxPerf == idx) {
        break;
      }
    }
    if (blockIndex == UINT32_MAX){
      iter = freeBlocks.begin();
      for (; iter != freeBlocks.end(); iter++) {
        if (*iter == badIndex){
          continue;
        }
        blockIndex = *iter;

        if (blockIndex % param.pageCountToMaxPerf == idx) {
          break;
        }
      }
    }

    // Sanity check
    if (iter == freeBlocks.end()) {
      // Just use first one
      iter = freeBlocks.begin();
      blockIndex = *iter;
    }

    if (findBlock(blockIndex)->second.getErasedPageCount() != param.pagesInBlock) {
      panic("Corrupted");
    }

    // Remove found block from free block list
    freeBlocks.erase(iter);
    nFreeBlocks--;
  }
  else {
    panic("No free block left");
  }

  return blockIndex;
}

/*
從 lastFreeBlock 拿一個 "用過但還沒用完的" block 出來
                         └── block 被 getFreeBlock() 挑到 lastFreeBlock 後，會在 lastFreeBlock 待到 page 被寫滿為止，然後 getFreeBlock() 再去找新的 block 來替代它的位置
*/
uint32_t PageMapping::getLastFreeBlock(Bitset &iomap, uint32_t blockIndex) {

  if (!bRandomTweak || (lastFreeBlockIOMap & iomap).any()) {
    // Update lastFreeBlockIndex
    lastFreeBlockIndex++;

    if (lastFreeBlockIndex == param.pageCountToMaxPerf) {
      lastFreeBlockIndex = 0;
    }

    lastFreeBlockIOMap = iomap;
  }
  else {
    lastFreeBlockIOMap |= iomap;
  }
  size_t guard = 0;
  while (lastFreeBlock.at(lastFreeBlockIndex) == blockIndex) {
      lastFreeBlockIndex++;
      if (lastFreeBlockIndex == param.pageCountToMaxPerf) {
          lastFreeBlockIndex = 0;
      }
      guard++;
      if (guard > param.pageCountToMaxPerf) {
          panic("No alternative free block available (all lastFreeBlock entries are victim)");
      }
  }
  auto freeBlock = findBlock(lastFreeBlock.at(lastFreeBlockIndex));

  // Sanity check
  if (freeBlock == hotBlocks.end()) {
    panic("Corrupted");
  }

  // If current free block is full, get next block
  if (freeBlock->second.getNextWritePageIndex() == param.pagesInBlock) {
    lastFreeBlock.at(lastFreeBlockIndex) = getFreeBlock(lastFreeBlockIndex, blockIndex);

    bReclaimMore = true;
  }

  return lastFreeBlock.at(lastFreeBlockIndex);
}


/*
只有被寫滿的 block 會被當作 victim 的候選人
根據不同 policy 決定受害候選人的 weight
*/
void PageMapping::calculateVictimWeight(
    std::vector<std::pair<uint32_t, float>> &weight, const EVICT_POLICY policy,
    uint64_t tick) {
  // float temp;

  weight.reserve(hotBlocks.size() + coldBlocks.size());

  switch (policy) {
    
    case POLICY_GREEDY:
    case POLICY_RANDOM:
    case POLICY_DCHOICE:
      // 這三個的 weight 都為 block 內 ValidPage 的數量
      
      for (auto &iter : hotBlocks) {
        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }
        weight.push_back({iter.first, iter.second.getValidPageCountRaw()});
      }

      for (auto &iter : coldBlocks) {
        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }
        // 額外增加 cold pool 裡面候選人的權重，讓 hot pool 的候選人被選完之後才會選到 cold pool 的候選人
        weight.push_back({iter.first, iter.second.getValidPageCountRaw() + param.pagesInBlock});
      }

      break;
    case POLICY_COST_BENEFIT: {

        auto calc_cb_weight = [&](Block &blk) -> float {
        const uint32_t total   = param.pagesInBlock;
        const uint32_t valid   = blk.getValidPageCountRaw();
        const uint32_t invalid = (total > valid) ? (total - valid) : 0;

        const float valid_ratio   = total ? static_cast<float>(valid)   / total : 1.0f;
        const float invalid_ratio = total ? static_cast<float>(invalid) / total : 0.0f;

        const uint64_t last = blk.getLastAccessedTime();
        const uint64_t age  = (tick > last) ? (tick - last) : 1; // 避免 0 或負值

        const float denom = invalid_ratio * static_cast<float>(age);
        if (denom <= 0.0f) {
          return std::numeric_limits<float>::max(); // 沒無效頁或 age 幾乎為 0 → 不選
        }
        return valid_ratio / denom; // 越小越優先
      };
      
      for (auto &it : hotBlocks) {
        if (it.second.getNextWritePageIndex() == param.pagesInBlock) {
          weight.emplace_back(it.first, calc_cb_weight(it.second));
        }
      }
      for (auto &it : coldBlocks) {
        if (it.second.getNextWritePageIndex() == param.pagesInBlock) {
          weight.emplace_back(it.first, calc_cb_weight(it.second));
        }
      }
      break;
    }
    default:
      panic("Invalid evict policy");
  }
}

// 把 calculateVictimWeight 回傳的 weight 清單從小至大排序，並根據 GC 的需求取出前 n 個 block 回收
void PageMapping::selectVictimBlock(std::vector<uint32_t> &list,
                                    uint64_t &tick) {
  static const GC_MODE mode = (GC_MODE)conf.readInt(CONFIG_FTL, FTL_GC_MODE);
  static const EVICT_POLICY policy =
      (EVICT_POLICY)conf.readInt(CONFIG_FTL, FTL_GC_EVICT_POLICY);
  static uint32_t dChoiceParam =
      conf.readUint(CONFIG_FTL, FTL_GC_D_CHOICE_PARAM);
  uint64_t nBlocks = conf.readUint(CONFIG_FTL, FTL_GC_RECLAIM_BLOCK);
  std::vector<std::pair<uint32_t, float>> weight;

  list.clear();

  /*
  GC mode0 -> 每次回收固定數量 block
  Gc mode1 -> 回收到不會觸發GC的閾值為止
  */
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

  // reclaim one more if last free block fully used
  if (bReclaimMore) {
    nBlocks += param.pageCountToMaxPerf;

    bReclaimMore = false;
  }

  // Calculate weights of all blocks
  calculateVictimWeight(weight, policy, tick);

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

  nBlocks = MIN(nBlocks, weight.size());
  
  // 取出前 n 個 weight 最低的 block
  for (uint64_t i = 0; i < nBlocks; i++) {
    list.push_back(weight.at(i).first);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::SELECT_VICTIM_BLOCK);
}

// GC 的本體，搬移 victim block 裡面的有效資料，並 erase victim block
void PageMapping::doGarbageCollection(std::vector<uint32_t> &blocksToReclaim,
                                      uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;POLICY_COST_BENEFIT:
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;

  if (blocksToReclaim.size() == 0) {
    return;
  }

  // For all blocks to reclaim, collecting request structure only
  for (auto &iter : blocksToReclaim) {
    auto block = findBlock(iter);

    if (block == hotBlocks.end()) {
      panic("Invalid block");
    }

    // Copy valid pages to free block
    for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++) {
      // Valid?
      if (block->second.getPageInfo(pageIndex, lpns, bit)) {
        if (!bRandomTweak) {
          bit.set();
        }

        // Retrive free block
        auto freeBlock = findBlock(getLastFreeBlock(bit, block->first));

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        // Update mapping table
        uint32_t newBlockIdx = freeBlock->first;

        for (uint32_t idx = 0; idx < bitsetSize; idx++) {
          if (bit.test(idx)) {
            // Invalidate
            block->second.invalidate(pageIndex, idx);

            auto mappingList = table.find(lpns.at(idx));

            if (mappingList == table.end()) {
              panic("Invalid mapping table entry");
            }

            pDRAM->read(&(*mappingList), 8 * param.ioUnitInPage, tick);

            auto &mapping = mappingList->second.at(idx);

            uint32_t newPageIdx = freeBlock->second.getNextWritePageIndex(idx);

            mapping.first = newBlockIdx;
            mapping.second = newPageIdx;

            freeBlock->second.write(newPageIdx, lpns.at(idx), idx, beginAt);

            // Issue Write
            req.blockIndex = newBlockIdx;
            req.pageIndex = newPageIdx;

            if (bRandomTweak) {
              req.ioFlag.reset();
              req.ioFlag.set(idx);
            }
            else {
              req.ioFlag.set();
            }

            writeRequests.push_back(req);

            stat.validPageCopies++;
          }
        }

        stat.validSuperPageCopies++;
      }
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

  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

void PageMapping::readInternal(Request &req, uint64_t &tick) {
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (mapping.first < param.totalPhysicalBlocks &&
            mapping.second < param.pagesInBlock) {
          palRequest.blockIndex = mapping.first;
          palRequest.pageIndex = mapping.second;

          if (bRandomTweak) {
            palRequest.ioFlag.reset();
            palRequest.ioFlag.set(idx);
          }
          else {
            palRequest.ioFlag.set();
          }

          auto block = findBlock(palRequest.blockIndex);

          if (block == hotBlocks.end()) {
            panic("Block is not in use");
          }

          beginAt = tick;

          block->second.read(palRequest.pageIndex, idx, beginAt);
          pPAL->read(palRequest, beginAt);

          finishedAt = MAX(finishedAt, beginAt);
        }
      }
    }

    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ_INTERNAL);
  }
}

void PageMapping::writeInternal(Request &req, uint64_t &tick, bool sendToPAL) {
  PAL::Request palRequest(req);
  std::unordered_map<uint32_t, Block>::iterator block;
  auto mappingList = table.find(req.lpn);
  uint64_t beginAt;
  uint64_t finishedAt = tick;
  bool readBeforeWrite = false;

  if (mappingList != table.end()) {
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (mapping.first < param.totalPhysicalBlocks &&
            mapping.second < param.pagesInBlock) {
          block = findBlock(mapping.first);

          // Invalidate current page
          block->second.invalidate(mapping.second, idx);
        }
      }
    }
  }
  else {
    // Create empty mapping
    auto ret = table.emplace(
        req.lpn,
        std::vector<std::pair<uint32_t, uint32_t>>(
            bitsetSize, {param.totalPhysicalBlocks, param.pagesInBlock}));

    if (!ret.second) {
      panic("Failed to insert new mapping");
    }

    mappingList = ret.first;
  }

  // Write data to free block
  block = findBlock(getLastFreeBlock(req.ioFlag, INT32_MAX));

  if (block == hotBlocks.end()) {
    panic("No such block");
  }

  if (sendToPAL) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
      pDRAM->write(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
      pDRAM->write(&(*mappingList), 8, tick);
    }
  }

  if (!bRandomTweak && !req.ioFlag.all()) {
    // We have to read old data
    readBeforeWrite = true;
  }

  for (uint32_t idx = 0; idx < bitsetSize; idx++) {
    if (req.ioFlag.test(idx) || !bRandomTweak) {
      uint32_t pageIndex = block->second.getNextWritePageIndex(idx);
      auto &mapping = mappingList->second.at(idx);

      beginAt = tick;

      block->second.write(pageIndex, req.lpn, idx, beginAt);

      // Read old data if needed (Only executed when bRandomTweak = false)
      // Maybe some other init procedures want to perform 'partial-write'
      // So check sendToPAL variable
      if (readBeforeWrite && sendToPAL) {
        palRequest.blockIndex = mapping.first;
        palRequest.pageIndex = mapping.second;

        // We don't need to read old data
        palRequest.ioFlag = req.ioFlag;
        palRequest.ioFlag.flip();

        pPAL->read(palRequest, beginAt);
      }

      // update mapping to table
      mapping.first = block->first;
      mapping.second = pageIndex;

      if (sendToPAL) {
        palRequest.blockIndex = block->first;
        palRequest.pageIndex = pageIndex;

        if (bRandomTweak) {
          palRequest.ioFlag.reset();
          palRequest.ioFlag.set(idx);
        }
        else {
          palRequest.ioFlag.set();
        }

        pPAL->write(palRequest, beginAt);
      }

      finishedAt = MAX(finishedAt, beginAt);
    }
  }

  // Exclude CPU operation when initializing
  if (sendToPAL) {
    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);
  }

  // LAB3 函式被呼叫的地方 ( 這裡不用改 )
  // 根據論文，DS、CPR、HPR 需要在每次讀取後依序執行
  dirtySwap();
  coldPoolResize();
  hotPoolResize();
  

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

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | On-demand | %u blocks will be reclaimed", list.size());

    doGarbageCollection(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
               beginAt, beginAt - tick);

    stat.gcCount++;
    stat.reclaimedBlocks += list.size();
  }
}

void PageMapping::trimInternal(Request &req, uint64_t &tick) {
  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    // Do trim
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      auto &mapping = mappingList->second.at(idx);
      auto block = findBlock(mapping.first);

      if (block == hotBlocks.end()) {
        panic("Block is not in use");
      }

      block->second.invalidate(mapping.second, idx);
    }

    // Remove mapping
    table.erase(mappingList);

    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM_INTERNAL);
  }
}

void PageMapping::eraseInternal(PAL::Request &req, uint64_t &tick) {

  static uint64_t threshold =
      conf.readUint(CONFIG_FTL, FTL_BAD_BLOCK_THRESHOLD);
  auto block = findBlock(req.blockIndex);

  // Sanity checks
  if (block == hotBlocks.end()) {
    panic("No such block");
  }

  debugprint(LOG_FTL_PAGE_MAPPING,
             "eraseInternal: %" PRIu32 ,
             req.blockIndex);

  if (block->second.getValidPageCount() != 0) {
    panic("There are valid pages in victim block");
  }
  
  // Erase block
  // block 自己的 erase count 和 effecive erase count 會在 erase() 裡增加1
  block->second.erase();

  pPAL->erase(req, tick);

  // Check erase count
  uint32_t erasedCount = block->second.getEraseCount();
  // uint32_t effectiveErasedCount = block->second.getEffectiveEraseCount();

  // LAB3 需要更改的部分
  const uint32_t id       = req.blockIndex;
  const uint32_t newErase = block->second.getEraseCount();
  const uint32_t newEff   = block->second.getEffectiveEraseCount();
  if (hotBlocks.find(req.blockIndex) != hotBlocks.end()){
    // 更新 hotBlock 的 eraseCount & effectiveEraseCount 的排序列表
    {
      auto it = whereIsEraseCount.find(id);
      if (it != whereIsEraseCount.end()) {
        eraseCountOfHotBlocks.erase(it->second);
        whereIsEraseCount.erase(it);
      }
    }
    {
      auto it = whereIsEffectiveEraseCount.find(id);
      if (it != whereIsEffectiveEraseCount.end()) {
        effectiveEraseCountOfHotBlocks.erase(it->second);
        whereIsEffectiveEraseCount.erase(it);
      }
    }
    // 用新數值插回去，並回存 iterator
    {
      auto it = eraseCountOfHotBlocks.emplace(BlockOrder{id, newErase});
      whereIsEraseCount[id] = it;
    }
    {
      auto it = effectiveEraseCountOfHotBlocks.emplace(BlockOrder{id, newEff});
      whereIsEffectiveEraseCount[id] = it;
    }
  }
  else if (coldBlocks.find(req.blockIndex) != coldBlocks.end()){
    // 更新 coldBlock 的 eraseCount & effectiveEraseCount 的排序列表
   // 先移除舊 iterator（cold 的兩個 multiset）
    {
      auto it = whereIsEraseCount.find(id);
      if (it != whereIsEraseCount.end()) {
        eraseCountOfColdBlocks.erase(it->second);
        whereIsEraseCount.erase(it);
      }
    }
    {
      auto it = whereIsEffectiveEraseCount.find(id);
      if (it != whereIsEffectiveEraseCount.end()) {
        effectiveEraseCountOfColdBlocks.erase(it->second);
        whereIsEffectiveEraseCount.erase(it);
      }
    }
    // 用新數值插回去，並回存 iterator
    {
      auto it = eraseCountOfColdBlocks.emplace(BlockOrder{id, newErase});
      whereIsEraseCount[id] = it;
    }
    {
      auto it = effectiveEraseCountOfColdBlocks.emplace(BlockOrder{id, newEff});
      whereIsEffectiveEraseCount[id] = it;
    }
  }
  else{
    panic("block is neither hot or cold");
  }

  if (erasedCount < threshold) {
    // Reverse search
    auto iter = freeBlocks.end();

    while (true) {
      iter--;
      auto iter_b = findBlock(*iter);
      if (iter_b->second.getEraseCount() <= erasedCount) {
        // emplace: insert before pos
        iter++;

        break;
      }

      if (iter == freeBlocks.begin()) {
        break;
      }
    }

    // Insert block to free block list
    freeBlocks.emplace(iter, block->second.getBlockIndex());
    nFreeBlocks++;
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::ERASE_INTERNAL);
}

// 作業不會用到
float PageMapping::calculateWearLeveling() {
  uint64_t totalEraseCnt = 0;
  uint64_t sumOfSquaredEraseCnt = 0;
  uint64_t numOfBlocks = param.totalLogicalBlocks;
  uint64_t eraseCnt;

  for (auto &iter : hotBlocks) {
    eraseCnt = iter.second.getEraseCount();
    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  for (auto &iter : coldBlocks) {
    eraseCnt = iter.second.getEraseCount();
    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  if (sumOfSquaredEraseCnt == 0) {
    return -1;  // no meaning of wear-leveling
  }

  return (float)totalEraseCnt * totalEraseCnt /
         (numOfBlocks * sumOfSquaredEraseCnt);
}

void PageMapping::calculateTotalPages(uint64_t &valid, uint64_t &invalid) {
  valid = 0;
  invalid = 0;

  for (auto &iter : hotBlocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }

  for (auto &iter : coldBlocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }
}

void PageMapping::getStatList(std::vector<Stats> &list, std::string prefix) {
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

  temp.name = prefix + "page_mapping.wear_leveling";
  temp.desc = "Wear-leveling factor";
  list.push_back(temp);
}

void PageMapping::getStatValues(std::vector<double> &values) {
  values.push_back(stat.gcCount);
  values.push_back(stat.reclaimedBlocks);
  values.push_back(stat.validSuperPageCopies);
  values.push_back(stat.validPageCopies);
  values.push_back(calculateWearLeveling());
}

void PageMapping::resetStatValues() {
  memset(&stat, 0, sizeof(stat));
}

void PageMapping::dirtySwap(){

  /* LAB3 需要更改的部分
  if hot block 裡最大的 erase count -  cold block 裡最小的 erase count > TH  ---> 觸發 dirtySwap
  
  1. 將最老的 hot block 移到 cold pool
  2. 將最年輕的 cold block 移到 hot pool
  3. 重置兩個 block 的 effective erase count
  4. 更新 hot/cold pool 的 eraseCount/effectiveEraseCount
  */

  // 0) 任一 pool 沒資料就不進行
  if (eraseCountOfHotBlocks.empty() || eraseCountOfColdBlocks.empty()) return;

  // 1) 取得hot 裡「erase 最大」、cold 裡「erase 最小」(erase count已經由小到大排好了)
  const auto hot_max  = *eraseCountOfHotBlocks.rbegin(); // 取hot裡「erase 最大」
  const auto cold_min = *eraseCountOfColdBlocks.begin(); // 取cold裡「erase 最小」

  const uint32_t hot_id  = hot_max.blockIndex;  //取值
  const uint32_t cold_id = cold_min.blockIndex; //取值
  const uint32_t hot_ec  = hot_max.eraseCount;  //取值
  const uint32_t cold_ec = cold_min.eraseCount;  //取值

  // 2) 判斷是否觸發 dirty swap
  const uint32_t TH = swapThreshold;
  if (hot_ec <= cold_ec || (hot_ec - cold_ec) <= TH) return; //不觸發 dirtySwap

  // 3) 從「兩個 pool 的兩種 multiset」與其 iterator 索引表移除該兩顆 block 的紀錄
  //    （先拿到舊 iterator，再 erase，最後從 map 移掉）
  {
    auto itE  = whereIsEraseCount.find(hot_id);
    if (itE != whereIsEraseCount.end()) {
      eraseCountOfHotBlocks.erase(itE->second);
      whereIsEraseCount.erase(itE);
    }
    auto itEE = whereIsEffectiveEraseCount.find(hot_id);
    if (itEE != whereIsEffectiveEraseCount.end()) {
      effectiveEraseCountOfHotBlocks.erase(itEE->second);
      whereIsEffectiveEraseCount.erase(itEE);
    }
  }
  {
    auto itE  = whereIsEraseCount.find(cold_id);
    if (itE != whereIsEraseCount.end()) {
      eraseCountOfColdBlocks.erase(itE->second);
      whereIsEraseCount.erase(itE);
    }
    auto itEE = whereIsEffectiveEraseCount.find(cold_id);
    if (itEE != whereIsEffectiveEraseCount.end()) {
      effectiveEraseCountOfColdBlocks.erase(itEE->second);
      whereIsEffectiveEraseCount.erase(itEE);
    }
  }

  // 4) 交換池（hot→cold、cold→hot）
  //    用 node_handle 避免拷貝（C++17）
  auto hit = hotBlocks.find(hot_id);
  auto cit = coldBlocks.find(cold_id);
  if (hit == hotBlocks.end() || cit == coldBlocks.end()) {
    // 理論上不會發生；保險起見可直接 return 或加 assert
    return;
  }

  // 先把物件搬出來（避免多次拷貝）
  Block hotBlk  = std::move(hit->second);   // hot pool 裡那顆
  Block coldBlk = std::move(cit->second);   // cold pool 裡那顆

  // 從原 pool 移除
  hotBlocks.erase(hit);
  coldBlocks.erase(cit);

  // 插回對調後的 pool
  coldBlocks.emplace(hot_id,  std::move(hotBlk));   // hot 的 block → cold
  hotBlocks.emplace(cold_id, std::move(coldBlk));   // cold 的 block → hot

  // 5) 重置兩顆 block 的「effective erase count」：不動 Block 內部，
  //    直接在新 pool 的 effective-multiset 以 0 作為 key 重新登記
  //    同時在「新 pool」的 erase-multiset 登記原本的 erase count

  // hot->cold：erase=hot_ec，effective=0
  {
    auto itE  = eraseCountOfColdBlocks.emplace(hot_id,hot_ec);
    auto itEE = effectiveEraseCountOfColdBlocks.emplace(hot_id,0u); // ← 重置 effective
    whereIsEraseCount[hot_id]          = itE;
    whereIsEffectiveEraseCount[hot_id] = itEE;
  }

  // cold->hot：erase=cold_ec，effective=0
  {
    auto itE  = eraseCountOfHotBlocks.emplace(cold_id,cold_ec);
    auto itEE = effectiveEraseCountOfHotBlocks.emplace(cold_id,0u); // ← 重置 effective
    whereIsEraseCount[cold_id]          = itE;
    whereIsEffectiveEraseCount[cold_id] = itEE;
  }

}


void PageMapping::hotPoolResize(){

   /* LAB3 需要更改的部分
  if hot block 裡最大的 erase count -  hot block 裡最小的 erase count > TH*2 ---> 觸發 hotPoolResize
  1. 將最年輕的 hot block 移到 cold pool
  2. 更新 hot/cold pool 的 eraseCount/effectiveEraseCount
  */

   // 0) hot pool 沒資料就不動
  if (eraseCountOfHotBlocks.empty()) return;

  // 1) 讀取 hot 內的最小/最大 erase count
  const auto hot_min = *eraseCountOfHotBlocks.begin();   // (erase最小的hot)
  const auto hot_max = *eraseCountOfHotBlocks.rbegin();  // (erase最大的hot)

  const uint32_t min_id = hot_min.blockIndex;
  const uint32_t min_ec = hot_min.eraseCount;
  const uint32_t max_ec = hot_max.eraseCount;

  // 2) 判斷是否觸發：max - min > TH * 2
  const uint32_t TH = swapThreshold; // 和 dirtySwap 同一門檻
  if (max_ec <= min_ec || (max_ec - min_ec) <= (TH * 2)) return;

  // 3) 準備搬家：先把要搬的那顆（最年輕的 hot）從 hot 索引中拔掉
  //    先抓原本的 effective 值（因為這次不重置）
  uint32_t min_eec = 0;
  {
    auto itEE = whereIsEffectiveEraseCount.find(min_id);
    if (itEE != whereIsEffectiveEraseCount.end()) {
      // multiset 內元素是 const，讀值沒問題；此處的 eraseCount 欄位在 effective set 代表 effective 值
      min_eec = itEE->second->eraseCount;
    }
  }

  // 從 hot 的兩個 multiset 與索引表中移除該 block 的紀錄
  {
    auto itE  = whereIsEraseCount.find(min_id);
    if (itE != whereIsEraseCount.end()) {
      eraseCountOfHotBlocks.erase(itE->second);
      whereIsEraseCount.erase(itE);
    }
    auto itEE = whereIsEffectiveEraseCount.find(min_id);
    if (itEE != whereIsEffectiveEraseCount.end()) {
      effectiveEraseCountOfHotBlocks.erase(itEE->second);
      whereIsEffectiveEraseCount.erase(itEE);
    }
  }

  // 4) 真正搬移：hot → cold
  auto hit = hotBlocks.find(min_id);
  if (hit == hotBlocks.end()) return; // 理論上不會發生；保護一下

  // 搬出物件、從 hot 移除，再放入 cold
  Block blk = std::move(hit->second);
  hotBlocks.erase(hit);
  coldBlocks.emplace(min_id, std::move(blk));

  // 5) 在 cold 的兩個 multiset 重新登記（erase 用原值，effective 也保持原值，不重置）
  auto itE_cold  = eraseCountOfColdBlocks.emplace(BlockOrder{min_id, min_ec});
  auto itEE_cold = effectiveEraseCountOfColdBlocks.emplace(BlockOrder{min_id, min_eec});

  // 6) 更新 iterator 索引表（共用一張 map）
  whereIsEraseCount[min_id]          = itE_cold;
  whereIsEffectiveEraseCount[min_id] = itEE_cold;

}

void PageMapping::coldPoolResize(){

  /* LAB3 需要更改的部分
  if cold block 裡最大的 effective erase count - hot block 裡最小的 effective erase count > TH ---> 觸發 coldPoolResize
  1. 將有最大 effective erase count 的 cold block 移到 hot pool
  2. 更新 hot/cold pool 的 eraseCount/effectiveEraseCount
  */

  // 0) 若任一側沒有 effective 索引，無事可做
  if (effectiveEraseCountOfColdBlocks.empty() || effectiveEraseCountOfHotBlocks.empty()) return;

  // 1) 取 cold 最大 effective、hot 最小 effective
  const auto cold_max_eff = *effectiveEraseCountOfColdBlocks.rbegin(); // (id, effective最大)
  const auto hot_min_eff  = *effectiveEraseCountOfHotBlocks.begin();   // (id, effective最小)

  const uint32_t cid   = cold_max_eff.blockIndex;
  const uint32_t ceeff = cold_max_eff.eraseCount; // 在「effective」set 中，eraseCount 欄位存的是 effective 值
  const uint32_t hmeff = hot_min_eff.eraseCount;

  const uint32_t TH = swapThreshold;
  if (ceeff <= hmeff || (ceeff - hmeff) <= TH) return; // 不觸發

  // 2) 準備搬家：需要這顆 cold block 的「erase count」原值（插到 hot 的 erase multiset 用）
  //    透過 whereIsEraseCount 取 iterator，讀出它在「cold erase set」裡的值
  uint32_t ceerase = 0;
  {
    auto itE = whereIsEraseCount.find(cid);
    if (itE != whereIsEraseCount.end()) {
      ceerase = itE->second->eraseCount; // 在 erase set 中，eraseCount 欄位即為 erase 值
    }
  }

  // 3) 先把這顆 block 從「cold 的兩個 multiset」與索引表拔掉
  {
    auto itE = whereIsEraseCount.find(cid);
    if (itE != whereIsEraseCount.end()) {
      eraseCountOfColdBlocks.erase(itE->second);
      whereIsEraseCount.erase(itE);
    }
    auto itEE = whereIsEffectiveEraseCount.find(cid);
    if (itEE != whereIsEffectiveEraseCount.end()) {
      effectiveEraseCountOfColdBlocks.erase(itEE->second);
      whereIsEffectiveEraseCount.erase(itEE);
    }
  }

  // 4) 實際搬移：cold → hot
  auto cit = coldBlocks.find(cid);
  if (cit == coldBlocks.end()) return; // 保險檢查
  Block blk = std::move(cit->second);
  coldBlocks.erase(cit);
  hotBlocks.emplace(cid, std::move(blk));

  // 5) 在 hot 的兩個 multiset 重新登記（保留原本 erase/effective 值，題意未要求 reset）
  auto itE_hot  = eraseCountOfHotBlocks.emplace(BlockOrder{cid, ceerase});
  auto itEE_hot = effectiveEraseCountOfHotBlocks.emplace(BlockOrder{cid, ceeff});

  // 6) 回寫 iterator 到索引表
  whereIsEraseCount[cid]          = itE_hot;
  whereIsEffectiveEraseCount[cid] = itEE_hot;
}

// 從兩個 pool 裡面搜尋，有的話就返回 ( PBN, Block )，都沒有的話返回 hotBlocks.end()
std::unordered_map<uint32_t, Block>::iterator PageMapping::findBlock(uint32_t blockID){
  
  if (hotBlocks.find(blockID) != hotBlocks.end()){
    return hotBlocks.find(blockID);
  }
  else if (coldBlocks.find(blockID) != coldBlocks.end()){
    return coldBlocks.find(blockID);
  }

  return hotBlocks.end();
}

}  // namespace FTL

}  // namespace SimpleSSD

