/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    // 从第一个页面开始找第一条记录
    int num_pages = file_handle_->file_hdr_.num_pages;
    int num_records_per_page = file_handle_->file_hdr_.num_records_per_page;

    rid_ = {-1, -1};

    for (int page_no = RM_FIRST_RECORD_PAGE; page_no < num_pages; page_no++) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        int slot_no = Bitmap::first_bit(true, page_handle.bitmap, num_records_per_page);
        if (slot_no < num_records_per_page) {
            // 找到记录
            rid_ = {page_no, slot_no};
            return;
        }
    }
    // 没有找到任何记录，设置为结束
    rid_ = {RM_NO_PAGE, -1};
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    int num_pages = file_handle_->file_hdr_.num_pages;
    int num_records_per_page = file_handle_->file_hdr_.num_records_per_page;

    // 从当前rid的下一个位置开始找
    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;

    while (page_no < num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        // 找下一个bit为1的slot
        slot_no = Bitmap::next_bit(true, page_handle.bitmap, num_records_per_page, slot_no);
        if (slot_no < num_records_per_page) {
            // 在当前页找到下一个记录
            rid_ = {page_no, slot_no};
            return;
        }
        // 当前页没有更多记录，移到下一页
        page_no++;
        slot_no = -1;
    }
    // 到达文件末尾
    rid_ = {RM_NO_PAGE, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 判断是否到达文件末尾
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}