/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"
#include <mutex>

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
     // 1.   使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
     // 1.1    未满获得frame
     // 1.2    已满使用lru_replacer中的方法选择淘汰页面
    if (replacer_->victim(frame_id)) {
        return true;
    }
    // 1 使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
    // 1.1 未满获得frame
    // 1.2 已满使用lru_replacer中的方法选择淘汰页面

    return false;
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
// 1. 防御性刷盘：必须同时满足是脏页，且确实是个合法页面，才能往硬盘写
    if (page->is_dirty_ && page->get_page_id().page_no != INVALID_PAGE_ID) {
        disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
        page->is_dirty_ = false;
    }
    
    // 2. 防御性抹除：防止“幽灵擦除”误杀活着的页面
    PageId old_page_id = page->get_page_id();
    if (old_page_id.page_no != INVALID_PAGE_ID) {
        auto it = page_table_.find(old_page_id);
        // 💡 必须交叉验证：名册上这个 old_page_id 真的坐在当前这个桌子 (new_frame_id) 上，才能划掉它！
        if (it != page_table_.end() && it->second == new_frame_id) {
            page_table_.erase(it);
        }
    }
    
    // 3. 登记新客人
    if (new_page_id.page_no != INVALID_PAGE_ID) {
        page_table_[new_page_id] = new_frame_id;
    }
    
    // 4. 重置页面信息
    page->id_ = new_page_id;
    page->reset_memory();
    // 1 如果是脏页，写回磁盘，并且把dirty置为false
    // 2 更新page table
    // 3 重置page的data，更新page id

}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
std::scoped_lock lock{latch_};

    // 1. 查大堂名册，看看页面是不是已经在内存里了
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        // 1.1 命中缓存！已经在内存里了
        frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        
        // 保护起来：有新的人在用了，引用计数 +1
        page->pin_count_++;
        // 告诉 LRU 赶客经理：这桌正在吃，绝对不准踢（固定它）
        replacer_->pin(frame_id);
        
        return page; // 直接交差
    }

    // 1.2 缓存没命中，得从硬盘读。先找个空桌子
    frame_id_t victim_frame_id;
    if (!find_victim_page(&victim_frame_id)) {
        return nullptr; // 惨！全满了且都在用，只能告诉上层“拿不到”
    }

    // 拿到空出来的物理格子
    Page* victim_page = &pages_[victim_frame_id];

    // 2. 打扫桌子（写回上一个客人的脏数据，并在名册里换上新客人的 page_id，重置内存）
    update_page(victim_page, page_id, victim_frame_id);

    // 3. 从硬盘读取真正需要的目标页面的数据，端上桌子
    disk_manager_->read_page(page_id.fd, page_id.page_no, victim_page->get_data(), PAGE_SIZE);

    // 4. 固定目标页，更新引用计数
    victim_page->pin_count_ = 1;     // 刚读进来，说明有 1 个人在用
    replacer_->pin(victim_frame_id); // 从 LRU 淘汰名单里划掉

    // 5. 返回获取到的页面
    return victim_page;
    // 1.     从page_table_中搜寻目标页
    // 1.1    若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    // 1.2    否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    // 2.     若获得的可用frame存储的为dirty page，则须调用updata_page将page写回到磁盘
    // 3.     调用disk_manager_的read_page读取目标页到frame
    // 4.     固定目标页，更新pin_count_
    // 5.     返回目标页
    return nullptr;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    // 1. 尝试在page_table_中搜寻page_id对应的页P
    auto it = page_table_.find(page_id);
    // 1.1 P在页表中不存在 return false
    if (it == page_table_.end()) {
        return false;
    }
    // 1.2 P在页表中存在，获取其 frame 和 page
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    // 2.1 若pin_count_已经等于0，则返回false (说明出现了严重的逻辑错误，没借出去怎么会有人还？)
    if (page->pin_count_ <= 0) {
        return false;
    }
    // 2.2 若pin_count_大于0，则pin_count_自减一 (结账走了一位客人)
    page->pin_count_--;
    // 2.2.1 若自减后等于0，则调用replacer_的Unpin (所有客人都走了，把桌子扔进 LRU 待淘汰名单)
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }
    // 3 根据参数is_dirty，更改P的is_dirty_
    // 💡 防御性编程：只管把它弄脏，千万别把它洗干净！(洗干净是 flush_page 的工作)
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    // 0. lock latch
    // 1. 尝试在page_table_中搜寻page_id对应的页P
    // 1.1 P在页表中不存在 return false
    // 1.2 P在页表中存在，获取其pin_count_
    // 2.1 若pin_count_已经等于0，则返回false
    // 2.2 若pin_count_大于0，则pin_count_自减一
    // 2.2.1 若自减后等于0，则调用replacer_的Unpin
    // 3 根据参数is_dirty，更改P的is_dirty_
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
std::scoped_lock lock{latch_};

    // 防御性检查：如果是无效页面，直接拒绝
    if (page_id.page_no == INVALID_PAGE_ID) {
        return false;
    }

    // 1. 查找页表,尝试获取目标页P
    auto it = page_table_.find(page_id);
    
    // 1.1 目标页P没有被page_table_记录 ，返回false
    if (it == page_table_.end()) {
        return false;
    }

    // 拿到物理格子
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 2. 无论P是否为脏都将其写回磁盘。
    // (调用 DiskManager，组合拳再现：解引用 page_id 的 fd 和 page_no)
    disk_manager_->write_page(page->get_page_id().fd, 
                              page->get_page_id().page_no, 
                              page->get_data(), 
                              PAGE_SIZE);

    // 3. 更新P的is_dirty_ (既然已经安全存进硬盘了，就可以洗白了)
    page->is_dirty_ = false;
    // 0. lock latch
    // 1. 查找页表,尝试获取目标页P
    // 1.1 目标页P没有被page_table_记录 ，返回false
    // 2. 无论P是否为脏都将其写回磁盘。
    // 3. 更新P的is_dirty_
   
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    // 1. 获得一个可用的frame，若无法获得则返回nullptr
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr; // 内存全满且都被占用，无法腾出位置
    }
    // 2. 在fd对应的文件分配一个新的page_id
    // 上层传进来的 page_id 指针里，已经带了目标文件的 fd（文件描述符）。
    // 我们调用后厨（disk_manager）去申请一个新的页面编号（page_no），并填回指针里。
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
    // 拿到刚刚申请到的空闲物理格子
    Page* page = &pages_[frame_id];
    // 3. 将frame的数据写回磁盘 (并重置状态)
    // 这一步直接使唤我们的黄金帮手 update_page！
    // 它会自动检查这个格子以前装的旧页面是不是脏页（如果是就写回磁盘），
    // 并在页表（page_table_）中解绑旧客人、登记新客人，最后重置内存。
    update_page(page, *page_id, frame_id);
    // 4. 固定frame，更新pin_count_
    replacer_->pin(frame_id); 
    page->pin_count_ = 1;    
    return page;
    // 1.   获得一个可用的frame，若无法获得则返回nullptr
    // 2.   在fd对应的文件分配一个新的page_id
    // 3.   将frame的数据写回磁盘
    // 4.   固定frame，更新pin_count_
    // 5.   返回获得的page
   return nullptr;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
std::scoped_lock lock{latch_};

    // 1. 在page_table_中查找目标页，若不存在返回true
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true; // 根本不在内存里，相当于已经"被删除"了，直接成功
    }

    // 拿到对应的物理格子
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 2. 若目标页的pin_count不为0，则返回false
    // （说明现在正有线程在读写它，绝对不能强行删除，否则系统直接崩溃）
    if (page->pin_count_ > 0) {
        return false;
    }

    // 3. 将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    
    // 3.1 按照注释要求，如果有脏数据先写回磁盘 (有些架构删除时直接丢弃，但这里严格遵循注释)
    if (page->is_dirty_) {
        disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
    }

    // 3.2 从大堂名册 (page_table_) 中彻底抹除记录
    page_table_.erase(page_id);

    // 3.3 极其关键：告诉 LRU 赶客经理，这个桌子我收回了，不要再让它待在"淘汰名单"里了！
    replacer_->pin(frame_id);

    // 3.4 重置这块内存的元数据，洗得干干净净
    page->id_.page_no = INVALID_PAGE_ID; 
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    page->reset_memory(); // 清空吃剩的残骸

    // 3.5 变回了"新桌子"，重新加入空闲列表的尾部备用
    free_list_.push_back(frame_id);
    // 1.   在page_table_中查找目标页，若不存在返回true
    // 2.   若目标页的pin_count不为0，则返回false
    // 3.   将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    
    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
// 0. 只要动内存池，就必须上锁
    std::scoped_lock lock{latch_};

    // 1. 暴力遍历整个内存池的所有格子 (假设你的大管家容量变量名叫 pool_size_)
    for (size_t i = 0; i < pool_size_; i++) {
        Page* page = &pages_[i];
        
        // 2. 检查这块内存：里面装了页面 且 这个页面属于我们要找的文件 fd
        if (page->get_page_id().page_no != INVALID_PAGE_ID && page->get_page_id().fd == fd) {
            
            // 3. 强制写回磁盘（不管是不是脏页，保险起见全写）
            disk_manager_->write_page(page->get_page_id().fd, 
                                      page->get_page_id().page_no, 
                                      page->get_data(), 
                                      PAGE_SIZE);
            
            // 4. 写完之后，彻底洗白脏标记
            page->is_dirty_ = false;
        }
    }
}