//
// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_consumer/vabc_partition_writer.h"

#include <memory>
#include <vector>

#include <libsnapshot/cow_writer.h>

#include "update_engine/common/cow_operation_convert.h"
#include "update_engine/common/utils.h"
#include "update_engine/payload_consumer/extent_writer.h"
#include "update_engine/payload_consumer/file_descriptor.h"
#include "update_engine/payload_consumer/install_plan.h"
#include "update_engine/payload_consumer/partition_writer.h"
#include "update_engine/payload_consumer/snapshot_extent_writer.h"

namespace chromeos_update_engine {
// Expected layout of COW file:
// === Beginning of Cow Image ===
// All Source Copy Operations
// ========== Label 0 ==========
// Operation 0 in PartitionUpdate
// ========== Label 1 ==========
// Operation 1 in PartitionUpdate
// ========== label 2 ==========
// Operation 2 in PartitionUpdate
// ========== label 3 ==========
// .
// .
// .

// When resuming, pass |kPrefsUpdateStatePartitionNextOperation| as label to
// |InitializeWithAppend|.
// For example, suppose we finished writing SOURCE_COPY, and we finished writing
// operation 2 completely. Update is suspended when we are half way through
// operation 3.
// |kPrefsUpdateStatePartitionNextOperation| would be 3, so we pass 3 as
// label to |InitializeWithAppend|. The CowWriter will retain all data before
// label 3, Which contains all operation 2's data, but none of operation 3's
// data.

bool VABCPartitionWriter::Init(const InstallPlan* install_plan,
                               bool source_may_exist) {
  TEST_AND_RETURN_FALSE(install_plan != nullptr);
  TEST_AND_RETURN_FALSE(PartitionWriter::Init(install_plan, source_may_exist));
  cow_writer_ = dynamic_control_->OpenCowWriter(
      install_part_.name, install_part_.source_path, install_plan->is_resume);
  TEST_AND_RETURN_FALSE(cow_writer_ != nullptr);

  // Emit a label before writing SOURCE_COPY. When resuming,
  // use pref or CowWriter::GetLastLabel to determine if the SOURCE_COPY ops are
  // written. No need to handle SOURCE_COPY operations when resuming.

  // ===== Resume case handling code goes here ====
  if (install_plan->is_resume) {
    int64_t next_op = 0;
    if (!prefs_->GetInt64(kPrefsUpdateStatePartitionNextOperation, &next_op)) {
      LOG(ERROR)
          << "Resuming an update but can't fetch |next_op| from saved prefs.";
      return false;
    }
    if (next_op < 0) {
      TEST_AND_RETURN_FALSE(cow_writer_->Initialize());
    } else {
      TEST_AND_RETURN_FALSE(cow_writer_->InitializeAppend(next_op));
      return true;
    }
  } else {
    TEST_AND_RETURN_FALSE(cow_writer_->Initialize());
  }

  // ==============================================
  TEST_AND_RETURN_FALSE(
      prefs_->SetInt64(kPrefsUpdateStatePartitionNextOperation, -1));

  // TODO(zhangkelvin) Rewrite this in C++20 coroutine once that's available.
  auto converted = ConvertToCowOperations(partition_update_.operations(),
                                          partition_update_.merge_operations());

  WriteAllCowOps(block_size_, converted, cow_writer_.get(), source_fd_);
  // Emit label 0 to mark end of all SOURCE_COPY operations
  cow_writer_->AddLabel(0);
  TEST_AND_RETURN_FALSE(
      prefs_->SetInt64(kPrefsUpdateStatePartitionNextOperation, 0));
  return true;
}

bool VABCPartitionWriter::WriteAllCowOps(
    size_t block_size,
    const std::vector<CowOperation>& converted,
    android::snapshot::ICowWriter* cow_writer,
    FileDescriptorPtr source_fd) {
  std::vector<uint8_t> buffer(block_size);

  for (const auto& cow_op : converted) {
    switch (cow_op.op) {
      case CowOperation::CowCopy:
        TEST_AND_RETURN_FALSE(
            cow_writer->AddCopy(cow_op.dst_block, cow_op.src_block));
        break;
      case CowOperation::CowReplace:
        ssize_t bytes_read = 0;
        TEST_AND_RETURN_FALSE(utils::PReadAll(source_fd,
                                              buffer.data(),
                                              block_size,
                                              cow_op.src_block * block_size,
                                              &bytes_read));
        if (bytes_read <= 0 || static_cast<size_t>(bytes_read) != block_size) {
          LOG(ERROR) << "source_fd->Read failed: " << bytes_read;
          return false;
        }
        TEST_AND_RETURN_FALSE(cow_writer->AddRawBlocks(
            cow_op.dst_block, buffer.data(), block_size));
        break;
    }
  }
  return true;
}

std::unique_ptr<ExtentWriter> VABCPartitionWriter::CreateBaseExtentWriter() {
  return std::make_unique<SnapshotExtentWriter>(cow_writer_.get());
}

[[nodiscard]] bool VABCPartitionWriter::PerformZeroOrDiscardOperation(
    const InstallOperation& operation) {
  for (const auto& extent : operation.dst_extents()) {
    TEST_AND_RETURN_FALSE(
        cow_writer_->AddZeroBlocks(extent.start_block(), extent.num_blocks()));
  }
  return true;
}

[[nodiscard]] bool VABCPartitionWriter::PerformSourceCopyOperation(
    const InstallOperation& operation, ErrorCode* error) {
  // TODO(zhangkelvin) Probably just ignore SOURCE_COPY? They should be taken
  // care of during Init();
  return true;
}

bool VABCPartitionWriter::Flush() {
  // No need to call fsync/sync, as CowWriter flushes after a label is added
  // added.
  int64_t next_op = 0;
  // |kPrefsUpdateStatePartitionNextOperation| will be maintained and set by
  // CheckpointUpdateProgress()
  TEST_AND_RETURN_FALSE(
      prefs_->GetInt64(kPrefsUpdateStatePartitionNextOperation, &next_op));
  // +1 because label 0 is reserved for SOURCE_COPY. See beginning of this
  // file for explanation for cow format.
  cow_writer_->AddLabel(next_op + 1);
  return true;
}

void VABCPartitionWriter::CheckpointUpdateProgress(size_t next_op_index) {
  prefs_->SetInt64(kPrefsUpdateStatePartitionNextOperation, next_op_index);
}

VABCPartitionWriter::~VABCPartitionWriter() {
  // Reset |kPrefsUpdateStatePartitionNextOperation| once we finished a
  // partition.
  prefs_->SetInt64(kPrefsUpdateStatePartitionNextOperation, -1);
  cow_writer_->Finalize();
}

}  // namespace chromeos_update_engine
