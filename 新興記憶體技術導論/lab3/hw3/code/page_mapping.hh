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

#ifndef __FTL_PAGE_MAPPING__
#define __FTL_PAGE_MAPPING__

#include <cinttypes>
#include <unordered_map>
#include <vector>
#include <set>

#include "ftl/abstract_ftl.hh"
#include "ftl/common/block.hh"
#include "ftl/ftl.hh"
#include "pal/pal.hh"

namespace SimpleSSD {

namespace FTL {

class PageMapping : public AbstractFTL {
 private:
  PAL::PAL *pPAL;

  ConfigReader &conf;

  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>
      table;
  // std::unordered_map<uint32_t, Block> blocks;
  // std::list<Block> freeBlocks;

  // 所有的block被分到這兩個裡面
  // 另外維護一個表(block ID)紀錄裡面有哪些是完全乾淨的
  std::unordered_map<uint32_t, Block> hotBlocks;
  std::unordered_map<uint32_t, Block> coldBlocks;
  std::list<uint32_t> freeBlocks;

  uint32_t swapThreshold;

  struct BlockOrder {
    uint32_t blockIndex;
    uint32_t eraseCount;

    //加上建構子
    BlockOrder() = default;
    BlockOrder(uint32_t id, uint32_t ec) : blockIndex(id), eraseCount(ec) {}
  };

  // 比較函式：eraseCount 小的在前面，如果一樣就用 blockIndex 當 tie-breaker
  struct ByEraseCount {
      bool operator()(const BlockOrder& a, const BlockOrder& b) const {
          if (a.eraseCount != b.eraseCount)
              return a.eraseCount < b.eraseCount;
          return a.blockIndex < b.blockIndex;
      }
  };

  std::multiset<BlockOrder, ByEraseCount> eraseCountOfHotBlocks;
  std::multiset<BlockOrder, ByEraseCount> eraseCountOfColdBlocks;
  std::multiset<BlockOrder, ByEraseCount> effectiveEraseCountOfHotBlocks;
  std::multiset<BlockOrder, ByEraseCount> effectiveEraseCountOfColdBlocks;

  std::unordered_map<uint32_t, std::multiset<BlockOrder, ByEraseCount>::iterator> 
    whereIsEraseCount, whereIsEffectiveEraseCount;

  
  uint32_t nFreeBlocks;  // For some libraries which std::list::size() is O(n)
  std::vector<uint32_t> lastFreeBlock;
  Bitset lastFreeBlockIOMap;
  uint32_t lastFreeBlockIndex;

  bool bReclaimMore;
  bool bRandomTweak;
  uint32_t bitsetSize;

  struct {
    uint64_t gcCount;
    uint64_t reclaimedBlocks;
    uint64_t validSuperPageCopies;
    uint64_t validPageCopies;
  } stat;

  float freeBlockRatio();
  uint32_t convertBlockIdx(uint32_t);
  uint32_t getFreeBlock(uint32_t, uint32_t);
  uint32_t getLastFreeBlock(Bitset &, uint32_t);
  void calculateVictimWeight(std::vector<std::pair<uint32_t, float>> &,
                             const EVICT_POLICY, uint64_t);
  void selectVictimBlock(std::vector<uint32_t> &, uint64_t &);
  void doGarbageCollection(std::vector<uint32_t> &, uint64_t &);

  float calculateWearLeveling();
  void calculateTotalPages(uint64_t &, uint64_t &);

  void readInternal(Request &, uint64_t &);
  void writeInternal(Request &, uint64_t &, bool = true);
  void trimInternal(Request &, uint64_t &);
  void eraseInternal(PAL::Request &, uint64_t &);

 public:
  PageMapping(ConfigReader &, Parameter &, PAL::PAL *, DRAM::AbstractDRAM *);
  ~PageMapping();

  bool initialize() override;

  void read(Request &, uint64_t &) override;
  void write(Request &, uint64_t &) override;
  void trim(Request &, uint64_t &) override;

  void format(LPNRange &, uint64_t &) override;

  Status *getStatus(uint64_t, uint64_t) override;

  void getStatList(std::vector<Stats> &, std::string) override;
  void getStatValues(std::vector<double> &) override;
  void resetStatValues() override;

  void dirtySwap();
  void hotPoolResize();
  void coldPoolResize();
  std::unordered_map<uint32_t, Block>::iterator findBlock(uint32_t);
};

}  // namespace FTL

}  // namespace SimpleSSD

#endif

