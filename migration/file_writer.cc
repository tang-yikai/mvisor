/* 
 * MVisor
 * Copyright (C) 2022 cair <rui.cai@tenclass.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <filesystem>

#include "logger.h"
#include "machine.h"
#include "migration.h"


MigrationFileWriter::MigrationFileWriter(std::string base_path) {
  /* Everything is written into a temporary directory next to the target and
   * only Commit() moves it into place, so an interrupted save can never
   * destroy an existing snapshot. */
  base_path_ = base_path;
  temp_path_ = base_path + ".tmp";

  std::error_code ec;
  std::filesystem::remove_all(temp_path_, ec);
  std::filesystem::create_directories(temp_path_, ec);
  MV_ASSERT(!ec);
}

MigrationFileWriter::~MigrationFileWriter() {
  if (!committed_) {
    /* The save failed or was abandoned: drop the partial image and leave any
     * previous snapshot in place. */
    std::error_code ec;
    std::filesystem::remove_all(temp_path_, ec);
  }
}

void MigrationFileWriter::Commit() {
  if (committed_) {
    return;
  }
  std::error_code ec;
  /* Move the old snapshot aside before putting the new one in place, so the
   * final path only ever holds a complete snapshot. */
  if (std::filesystem::exists(base_path_)) {
    std::filesystem::rename(base_path_, base_path_ + ".old", ec);
    if (ec) {
      std::filesystem::remove_all(base_path_);
    }
  }
  std::filesystem::rename(temp_path_, base_path_);
  std::filesystem::remove_all(base_path_ + ".old", ec);
  committed_ = true;
}

void MigrationFileWriter::SetPrefix(std::string prefix) {
  prefix_ = prefix;
}

bool MigrationFileWriter::WriteRaw(std::string tag, void* data, size_t size) {
  BeginWrite(tag);
  auto ptr = (uint8_t*)data;
  while (size > 0) {
    auto ret = write(fd_, ptr, size);
    if (ret <= 0) {
      MV_PANIC("failed to write fd=%d ret=%ld", fd_, ret);
    }
    ptr += ret;
    size -= ret;
  }
  EndWrite(tag);
  return true;
}

bool MigrationFileWriter::WriteProtobuf(std::string tag, const Message& message) {
  BeginWrite(tag);
  message.SerializePartialToFileDescriptor(fd_);
  EndWrite(tag);
  return true;
}

bool MigrationFileWriter::WriteMemoryPages(std::string tag, void* pages, size_t size) {
  /* Write RAM to sparse file */
  BeginWrite(tag);
  MV_ASSERT(ftruncate(fd_, size) == 0);

  auto ptr = (uint8_t*)pages;
  for (size_t pos = 0; pos < size; pos += PAGE_SIZE) {
    if (!test_zero(ptr, PAGE_SIZE)) {
      MV_ASSERT(pwrite(fd_, ptr, PAGE_SIZE, pos) == PAGE_SIZE);
    }
    ptr += PAGE_SIZE;
  }
  
  EndWrite(tag);
  return true;
}

int MigrationFileWriter::BeginWrite(std::string tag) {
  MV_ASSERT(fd_ == -1);
  auto full_path = std::filesystem::path(temp_path_) / prefix_;
  if (!std::filesystem::exists(full_path)) {
    std::filesystem::create_directories(full_path);
  }
  full_path /= tag;

  /* Removing a file is faster than truncating, why ?? */
  if (exists(full_path)) {
    std::filesystem::remove(full_path);
  }

  fd_ = open(full_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  MV_ASSERT(fd_ != -1);
  return fd_;
}

void MigrationFileWriter::EndWrite(std::string tag) {
  MV_UNUSED(tag);
  safe_close(&fd_);
}

