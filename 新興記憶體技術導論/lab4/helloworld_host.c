#include <assert.h>
#include <dpu.h>
#include <dpu_log.h>
#include <stdio.h>

//定義一個名為 DPU_BINARY 的常數字串，它的值是 "./helloworld"。
#ifndef DPU_BINARY
#define DPU_BINARY "./helloworld" 
#endif

int main(void) {
  struct dpu_set_t set, dpu;

  DPU_ASSERT(dpu_alloc(1, "backend=simulator", &set)); // 分配 n 顆 DPU(向系統要求分配 DPU 資源。)
  DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));         // 將 DPU 執行檔載入到這些 DPU 上(把剛剛編譯好的 DPU 程式（helloworld binary）載入到剛分配的每顆 DPU 上。)
  DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));        // 啟動所有 DPU 來執行剛剛載入的程式。因為使用同步模式，所以 Host 會等 DPU 都跑完再繼續。

  DPU_FOREACH(set, dpu) { // 逐一讀取每顆 DPU 的輸出（log）
    DPU_ASSERT(dpu_log_read(dpu, stdout));
  }

  DPU_ASSERT(dpu_free(set));  // 釋放 DPU 資源

  return 0;
}
