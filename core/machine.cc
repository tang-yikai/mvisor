/* 
 * MVisor - Virtual Machine Controller
 * KVM API reference: https://www.kernel.org/doc/html/latest/virt/kvm/api.html
 * Copyright (C) 2021 Terrence <terrence@tenclass.com>
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


#include "machine.h"

#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <cpuid.h>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include <cstring>

#include <filesystem>

#include <yaml-cpp/yaml.h>

#include "logger.h"
#include "disk_image.h"
#include "device_interface.h"
#include "migration.h"


Machine::Machine(std::string config_path, std::string vm_name, std::string vm_uuid) :
  vm_name_(vm_name), vm_uuid_(vm_uuid) {
  /* Load the configuration and set values of num_vcpus & ram_size */
  config_ = new Configuration(this);
  if (!config_->Load(config_path)) {
    MV_PANIC("failed to load config file: %s", config_path.c_str());
  }

  InitializeKvm();

  /* Initialize system RAM and BIOS ROM */
  memory_manager_ = new MemoryManager(this);
  /* Initialize Vfio container */
  vfio_manager_ = new VfioManager(this);

  /* Currently, an i440fx / q35 chipset motherboard is implemented */
  Device* root = dynamic_cast<Device*>(LookupObjectByClass("SystemRoot"));
  if (!root) {
    MV_PANIC("failed to find system-root device");
  }
  /* Initialize IO thread before devices */
  io_thread_ = new IoThread(this);
  /* Initialize device manager, connect and reset all devices */
  device_manager_ = new DeviceManager(this, root);

  /* Create vcpu objects */
  for (int i = 0; i < num_vcpus_; ++i) {
    vcpus_.push_back(new Vcpu(this, i));
  }

  /* Start threads and wait to resume */
  paused_ = true;
  for (auto vcpu: vcpus_) {
    vcpu->Start();
  }
  io_thread_->Start();

  /* Reset devices after vCPU created and paused */
  device_manager_->ResetDevices();
}

/* Free VM resources */
Machine::~Machine() {
  valid_ = false;

  delete vfio_manager_;
  delete device_manager_;
  delete memory_manager_;
  delete io_thread_;

  // Join all vcpu threads and free resources
  for (auto vcpu: vcpus_) {
    delete vcpu;
  }

  // delete objects created by configuration
  for (auto it = objects_.begin(); it != objects_.end(); it++) {
    delete it->second;
  }

  safe_close(&vm_fd_);
  safe_close(&kvm_fd_);
  delete config_;
  
  if (network_writer_) {
    delete network_writer_;
  }
}

/* Create KVM instance */
void Machine::InitializeKvm() {
  kvm_fd_ = open("/dev/kvm", O_RDWR);
  MV_ASSERT(kvm_fd_ > 0);

  int api_version = ioctl(kvm_fd_, KVM_GET_API_VERSION, 0);
  if (api_version != KVM_API_VERSION) {
    MV_PANIC("kvm api verison %d, expected: %d", api_version, KVM_API_VERSION);
  }

  // Get the vcpu information block size. Vcpu uses this value
  kvm_vcpu_mmap_size_ = ioctl(kvm_fd_, KVM_GET_VCPU_MMAP_SIZE, 0);
  MV_ASSERT(kvm_vcpu_mmap_size_ > 0);

  // Create vm so that we can map userspace memory
  vm_fd_ = ioctl(kvm_fd_, KVM_CREATE_VM, 0);
  MV_ASSERT(vm_fd_ > 0);
}

/* Maybe there are lots of things to do before quiting a VM */
void Machine::Quit() {
  if (!valid_)
    return;
  
  /* Pause all threads and flush disk cache as well */
  if (!paused_) {
    Pause();
  }
  valid_ = false;

  /* If paused, threads are waiting to resume */
  io_thread_->Kick();
  for (auto vcpu: vcpus_) {
    vcpu->Kick();
  }

  /* Safe to quit main thread now */
  wait_to_quit_condition_.notify_all();
}

/* Recover BIOS data and reset all vCPU */
void Machine::Reset() {
  if (!valid_) {
    MV_WARN("machine is invalid, reset is ignored");
    return;
  }
  auto previous_paused = paused_;
  if (!paused_) {
    Pause();
  }

  memory_manager_->Reset();
  device_manager_->ResetDevices();

  MV_LOG("Resettings vCPUs");
  for (auto vcpu : vcpus_) {
    vcpu->Reset();
  }

  if (!previous_paused) {
    Resume();
  }
}

/* Find the first object with matching name */
Object* Machine::LookupObjectByName(std::string name) {
  auto it = objects_.find(name);
  if (it == objects_.end()) {
    return nullptr;
  }
  return it->second;
}

/* Find the first object with matching name */
Object* Machine::LookupObjectByClass(std::string name) {
  for (auto it = objects_.begin(); it != objects_.end(); it++) {
    if (name == it->second->classname()) {
      return it->second;
    }
  }
  return nullptr;
}

/* Find all objects that compare function returns true */
std::vector<Object*> Machine::LookupObjects(std::function<bool (Object*)> compare) {
  std::vector<Object*> result;
  for (auto it = objects_.begin(); it != objects_.end(); it++) {
    if (compare(it->second)) {
      result.push_back(it->second);
    }
  }
  return result;
}

/* Power button is pressed */
void Machine::Shutdown() {
  io_thread_->FlushDiskImages();
  for (auto o : LookupObjects([](auto o) { return dynamic_cast<PowerDownInterface*>(o); })) {
    auto interface = dynamic_cast<PowerDownInterface*>(o);
    interface->PowerDown();
  }
}

/* Resume from paused state */
void Machine::Resume() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!paused_) {
    MV_LOG("unable to resume a running machine");
    return;
  }
  paused_ = false;

  /* Before running, broadcast messages */
  for (auto& listener : state_change_listeners_) {
    listener();
  }
  
  /* Resume threads */
  io_thread_->Kick();
  for (auto vcpu : vcpus_) {
    vcpu->Kick();
  }
}

/* Currently this method can only be called from UI threads */
void Machine::Pause() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!valid_ || paused_)
    return;

  /* Make sure no vcpu thread is running now */
  VcpuRunLockGuard guard(vcpus_);

  /* Wait for iothread to stop */
  io_thread_->FlushDiskImages();
  IoThreadLockGuard io_guard(io_thread_);

  // Now all threads are paused, set machine state to paused
  paused_ = true;

  /* Here all vcpu are stopped, broadcast messages */
  for (auto& listener : state_change_listeners_) {
    listener();
  }
}

/* Main thread call this method to sleep */
void Machine::WaitToQuit() {
  std::unique_lock<std::mutex> lock(mutex_);
  wait_to_quit_condition_.wait(lock, [this]() {
    return !valid_;
  });
}

/* Listeners are called after Pause / Resume */
std::list<VoidCallback>::iterator Machine::RegisterStateChangeListener(VoidCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_change_listeners_.emplace(state_change_listeners_.end(), callback);
}

void Machine::UnregisterStateChangeListener(std::list<VoidCallback>::iterator it) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_change_listeners_.erase(it);
}

bool Machine::PrepareForSaving() {
  bool ret = true;
  bool paused = IsPaused();
  if (!paused) {
    Pause();
  }

  // create snapshot for current disk images
  ret = io_thread_->CreateQcow2ImageSnapshot();
  if (!ret) {
    goto end;
  }

  // start tracking dirty memory before sending memory
  memory_manager_->StartTrackingDirtyMemory();

end:
  if (!paused) {
    Resume();
  }
  return ret;
}

/* Save through network */
bool Machine::Save(const std::string ip, const uint16_t port) {
  if (saving_) {
    MV_ERROR("machine is busy migrating");
    return false;
  }

  MV_LOG("start saving");
  saving_ = true;

  bool ret = false;
  if (network_writer_) {
    MV_LOG("retry save machine, close last connection");
    delete network_writer_;
  }

  /* Make connection to target machine */
  network_writer_ = new MigrationNetworkWriter();
  if (!network_writer_->Connect(ip, port)) {
    MV_ERROR("failed to connect target machine");
    goto end;
  }

  if (!PrepareForSaving()) {
    MV_ERROR("failed to prepare for saving");
    goto end;
  }

  /* Save backing disk images */
  if(!io_thread_->SaveBackingDiskImage(network_writer_)) {
    MV_ERROR("failed to save backing disk images");
    goto end;
  }
  
  if (!network_writer_->WaitForSignal(kMigrateBackingImageComplete)) {
    goto end;
  }

  /* Save system RAM */
  if (!memory_manager_->SaveState(network_writer_)) {
    MV_ERROR("failed to save memory");
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateRamComplete)) {
    goto end;
  }
  ret = true;

end:
  if (!ret) {
    delete network_writer_;
    network_writer_ = nullptr;
    memory_manager_->StopTrackingDirtyMemory();
  }

  saving_ = false;
  MV_LOG("done saving");
  return ret;
}

/* Save through network */
bool Machine::PostSave() {
  if (saving_) {
    MV_ERROR("machine is busy migrating");
    return false;
  }

  if (!network_writer_) {
    MV_ERROR("Save must be called successfully before PostSave");
    return false;
  }

  if (!IsPaused()) {
    Pause();
  }

  MV_LOG("start post-saving");
  saving_ = true;
  bool ret = false;

  if(!io_thread_->SaveDiskImage(network_writer_)) {
    MV_ERROR("failed to save disk images");
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateImageComplete)) {
    goto end;
  }

  if (!memory_manager_->SaveDirtyMemory(network_writer_, kDirtyMemoryTypeKvm)) {
    MV_ERROR("failed to save dirty memory from kvm");
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateDirtyMemoryFromKvmComplete)) {
    goto end;
  }

  if (!memory_manager_->SaveDirtyMemory(network_writer_, kDirtyMemoryTypeListener)) {
    MV_ERROR("failed to save dirty memory from listener");
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateDirtyMemoryFromListenerComplete)) {
    goto end;
  }

  /* Target machine need to get all memory before load device state,
   * so we send device state and dirty memory from dma together. */
  if (!device_manager_->SaveState(network_writer_)) {
    MV_ERROR("failed to save device states");
    goto end;
  }

  /* Save dirty memory from dma,
   * this step must be put after saving device state completely. */
  if (!memory_manager_->SaveDirtyMemory(network_writer_, kDirtyMemoryTypeDma)) {
    MV_ERROR("failed to save dirty memory from dma");
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateDirtyMemoryFromDmaComplete)) {
    goto end;
  }

  if (!network_writer_->WaitForSignal(kMigrateDeviceComplete)) {
    goto end;
  }

  /* Save vcpu states */
  for (auto vcpu : vcpus_) {
    if (!vcpu->SaveState(network_writer_)) {
      MV_ERROR("failed to save vcpu=%d states", vcpu->vcpu_id());
      goto end;
    }
  }

  if (!network_writer_->WaitForSignal(kMigrateVcpuComplete)) {
    goto end;
  }

  /* Wait for a finish signal */
  if (!network_writer_->WaitForSignal(kMigrateComplete)) {
    goto end;
  }
  ret = true;

end:
  delete network_writer_;
  network_writer_ = nullptr;

  saving_ = false;
  memory_manager_->StopTrackingDirtyMemory();
  MV_LOG("done post-saving");
  return ret;
}

/* Load through network */
void Machine::Load(uint16_t port) {
  MV_ASSERT(!loading_);
  if (!IsPaused()) {
    Pause();
  }

  MV_LOG("start loading");
  loading_ = true;
  
  // Bind port to wait connection
  MigrationNetworkReader reader;
  if (!reader.WaitForConnection(port)) {
    MV_PANIC("failed to setup connection");
  }
  
  /* Load backing disk image */
  if (!io_thread_->LoadBackingDiskImage(&reader)) {
    MV_PANIC("failed to load backing disk image");
  }
  reader.SendSignal(kMigrateBackingImageComplete);

  /* Load system RAM */
  if (!memory_manager_->LoadState(&reader)) {
    MV_PANIC("failed to load system ram");
  }
  reader.SendSignal(kMigrateRamComplete);

  /* Load disk image, LoadDiskImage must be called before virtio-block loadstate */
  if (!io_thread_->LoadDiskImage(&reader)) {
    MV_PANIC("failed to load disk image");
  }
  reader.SendSignal(kMigrateImageComplete);

  /* Load dirty memory from kvm */
  if (!memory_manager_->LoadDirtyMemory(&reader, kDirtyMemoryTypeKvm)) {
    MV_PANIC("failed to load dirty memory from kvm");
  }
  reader.SendSignal(kMigrateDirtyMemoryFromKvmComplete);

  /* Load dirty memory from listener */
  if (!memory_manager_->LoadDirtyMemory(&reader, kDirtyMemoryTypeListener)) {
    MV_PANIC("failed to load dirty memory from listener");
  }
  reader.SendSignal(kMigrateDirtyMemoryFromListenerComplete);

  /* Load dirty memory from Dma */
  if (!memory_manager_->LoadDirtyMemory(&reader, kDirtyMemoryTypeDma)) {
    MV_PANIC("failed to load dirty memory from dma");
  }
  reader.SendSignal(kMigrateDirtyMemoryFromDmaComplete);

  /* Load device states */
  if (!device_manager_->LoadState(&reader)) {
    MV_PANIC("failed to load device states");
  }
  reader.SendSignal(kMigrateDeviceComplete);

  /* Load vcpu states */
  for (auto vcpu : vcpus_) {
    if (!vcpu->LoadState(&reader)) {
      MV_PANIC("failed to load %s", vcpu->name());
    }
  }
  reader.SendSignal(kMigrateVcpuComplete);

  /* Make a finish signal to notice the source vm */
  reader.SendSignal(kMigrateComplete);

  loading_ = false;
  MV_LOG("done loading");
}

/* Should be called by UI thread */
/* Decode the host CPU signature the same way CPU_VERSION() encodes it. */
static void GetHostSignature(std::string& vendor, uint32_t& family, uint32_t& model, uint32_t& stepping) {
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  __cpuid(0, eax, ebx, ecx, edx);
  char v[13] = {};
  memcpy(&v[0], &ebx, 4);
  memcpy(&v[4], &edx, 4);
  memcpy(&v[8], &ecx, 4);
  vendor = v;

  __cpuid(1, eax, ebx, ecx, edx);
  uint32_t base_family = (eax >> 8) & 0xF;
  uint32_t base_model = (eax >> 4) & 0xF;
  uint32_t ext_family = (eax >> 20) & 0xFF;
  uint32_t ext_model = (eax >> 16) & 0xF;
  family = (base_family == 0xF) ? base_family + ext_family : base_family;
  model = (base_family == 0x6 || base_family == 0xF) ? ((ext_model << 4) | base_model) : base_model;
  stepping = eax & 0xF;
}

/* A snapshot is usable once both the RAM image and the saved configuration
 * are present. */
bool Machine::IsSnapshotUsable(const std::string& path) {
  for (auto name : { "/memory/BIOS", "/memory/RAM", "/configuration.yaml" }) {
    if (!std::filesystem::exists(path + name)) {
      MV_LOG("snapshot %s is incomplete (missing %s), starting fresh", path.c_str(), name);
      return false;
    }
  }
  return true;
}

void Machine::SaveHostFingerprint(const std::string& path) {
  std::string vendor;
  uint32_t family, model, stepping;
  GetHostSignature(vendor, family, model, stepping);

  YAML::Node root;
  root["cpu"]["vendor"] = vendor;
  root["cpu"]["family"] = family;
  root["cpu"]["model"] = model;
  root["cpu"]["stepping"] = stepping;
  root["machine"]["ram_size"] = ram_size_;
  root["machine"]["vcpus"] = num_vcpus_;

  std::ofstream ofs(path + "/host.yaml", std::ios::out);
  if (!ofs.is_open()) {
    MV_WARN("failed to write the host fingerprint into %s", path.c_str());
    return;
  }
  ofs << root << std::endl;
}

bool Machine::CheckHostFingerprint(const std::string& path) {
  auto file = path + "/host.yaml";
  if (!std::filesystem::exists(file)) {
    MV_WARN("snapshot %s has no host fingerprint, skipping the compatibility check", path.c_str());
    return true;
  }

  std::string vendor;
  uint32_t family, model, stepping;
  GetHostSignature(vendor, family, model, stepping);

  YAML::Node root = YAML::LoadFile(file);
  auto saved_vendor = root["cpu"]["vendor"].as<std::string>();
  auto saved_family = root["cpu"]["family"].as<uint32_t>();
  auto saved_model = root["cpu"]["model"].as<uint32_t>();
  auto saved_ram = root["machine"]["ram_size"].as<uint64_t>();
  auto saved_vcpus = root["machine"]["vcpus"].as<int>();

  /* A memory image carries MSRs and FPU state that do not move across vendors,
   * so a vendor change is fatal. Everything else is only a warning: the guest
   * may well come up fine. */
  if (saved_vendor != vendor) {
    MV_ERROR("snapshot was taken on a '%s' host but this host is '%s'; refusing to restore it",
      saved_vendor.c_str(), vendor.c_str());
    return false;
  }
  if (saved_family != family || saved_model != model) {
    MV_WARN("snapshot CPU signature %u/%u differs from this host's %u/%u; continuing",
      saved_family, saved_model, family, model);
  }
  if (saved_ram != ram_size_) {
    MV_WARN("snapshot was taken with %lu bytes of RAM, this machine has %lu; continuing",
      (unsigned long)saved_ram, (unsigned long)ram_size_);
  }
  if (saved_vcpus != num_vcpus_) {
    MV_WARN("snapshot was taken with %d vcpus, this machine has %d; continuing",
      saved_vcpus, num_vcpus_);
  }
  return true;
}

bool Machine::Save(const std::string path) {
  bool ok = false;
  MV_ASSERT(!saving_);
  /* Make sure the machine is paused */
  if (!IsPaused()) {
    Pause();
  }
  saving_ = true;
  MV_LOG("start saving");

  MigrationFileWriter writer(path);
  /* Everything below goes into the writer's temporary directory; Commit()
   * publishes it only once the whole image is complete, so an interrupted save
   * cannot damage an existing snapshot. */
  /* Save device states */
  if (!device_manager_->SaveState(&writer)) {
    MV_ERROR("failed to save device states");
    goto end;
  }
  /* Save vcpu states */
  for (auto vcpu : vcpus_) {
    vcpu->SaveState(&writer);
  }
  /* Save system RAM */
  if (!memory_manager_->SaveState(&writer)) {
    MV_ERROR("failed to save RAM");
    goto end;
  }
  /* Save disk images */
  if (!io_thread_->SaveDiskImage(&writer)) {
    MV_ERROR("failed to sync disk images");
    goto end;
  }
  /* Save configuration after saving disk images (paths might changed) */
  if (!config_->Save(writer.base_path() + "/configuration.yaml")) {
    MV_ERROR("failed to save configuration yaml");
    goto end;
  }
  /* Record which host produced this image, so restoring it elsewhere can be
   * validated, then publish the snapshot. */
  SaveHostFingerprint(writer.base_path());
  writer.Commit();
  ok = true;

end:
  saving_ = false;
  MV_LOG("done saving");
  return ok;
}

/* Should be called by UI thread */
void Machine::Load(const std::string path) {
  MV_ASSERT(!loading_);
  /* Make sure the machine is paused */
  if (!IsPaused()) {
    Pause();
  }
  loading_ = true;
  MV_LOG("start loading");

  MigrationFileReader reader(path);
  /* Load system RAM */
  if (!memory_manager_->LoadState(&reader)) {
    MV_PANIC("failed to load RAM");
  }
  /* Load device states */
  if (!device_manager_->LoadState(&reader)) {
    MV_PANIC("failed to load device states");
  }
  /* Load vcpu states */
  for (auto vcpu : vcpus_) {
    if (!vcpu->LoadState(&reader)) {
      MV_PANIC("failed to load %s", vcpu->name());
    }
  }

  loading_ = false;
  MV_LOG("done loading");
}

const char* Machine::GetStatus() {
  if (!valid_) {
    return "invalid";
  }

  if (saving_) {
    return "saving";
  }
  if (loading_) {
    return "loading";
  }
  if (!paused_) {
    return "running";
  }

  /* otherwise return paused */
  return "paused";
}
