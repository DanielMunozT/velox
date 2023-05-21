/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fmt/format.h>

#include "velox/dwio/common/BufferedInput.h"

DEFINE_bool(wsVRLoad, false, "Use WS VRead API to load");

namespace facebook::velox::dwio::common {

void BufferedInput::load(const LogType logType) {
  // no regions to load
  if (regions_.size() == 0) {
    return;
  }

  offsets_.clear();
  buffers_.clear();
  allocPool_->clear();

  // sorting the regions from low to high
  std::sort(regions_.begin(), regions_.end());

  mergeRegions();

  offsets_.reserve(regions_.size());
  buffers_.reserve(regions_.size());

  if (wsVRLoad_) {
    std::vector<void*> buffers;
    std::vector<Region> regions;
    uint64_t sizeToRead = 0;
    loadWithAction(
        logType,
        [&buffers, &regions, &sizeToRead](
            void* buf, uint64_t length, uint64_t offset, LogType) {
          buffers.push_back(buf);
          regions.emplace_back(offset, length);
          sizeToRead += length;
        });

    // Now we have all buffers and regions, load it in parallel
    input_->vread(buffers, regions, logType);
  } else {
    loadWithAction(
        logType,
        [this](void* buf, uint64_t length, uint64_t offset, LogType type) {
          input_->read(buf, length, offset, type);
        });
  }

  // clear the loaded regions
  regions_.clear();
}

std::unique_ptr<SeekableInputStream> BufferedInput::enqueue(
    Region region,
    const dwio::common::StreamIdentifier* /*si*/) {
  if (region.length == 0) {
    return std::make_unique<SeekableArrayInputStream>(
        static_cast<const char*>(nullptr), 0);
  }

  // if the region is already in buffer - such as metadata
  auto ret = readBuffer(region.offset, region.length);
  if (ret) {
    return ret;
  }

  // push to region pool and give the caller the callback
  regions_.push_back(region);
  return std::make_unique<SeekableArrayInputStream>(
      [region, this]() { return readInternal(region.offset, region.length); });
}

void BufferedInput::mergeRegions() {
  auto& r = regions_;
  size_t ia = 0;

  DWIO_ENSURE(!r.empty(), "Assumes that there's at least one region");
  DWIO_ENSURE_GT(r[ia].length, 0, "invalid region");

  for (size_t ib = 1; ib < r.size(); ++ib) {
    DWIO_ENSURE_GT(r[ib].length, 0, "invalid region");
    if (!tryMerge(r[ia], r[ib])) {
      r[++ia] = r[ib];
    }
  }
  r.resize(ia + 1);
}

void BufferedInput::loadWithAction(
    const LogType logType,
    std::function<void(void*, uint64_t, uint64_t, LogType)> action) {
  for (const auto& region : regions_) {
    readRegion(region, logType, action);
  }
}

bool BufferedInput::tryMerge(Region& first, const Region& second) {
  DWIO_ENSURE_GE(second.offset, first.offset, "regions should be sorted.");
  int64_t gap = second.offset - first.offset - first.length;

  // compare with 0 since it's comparison in different types
  if (gap < 0 || gap <= maxMergeDistance_) {
    // ensure try merge will handle duplicate regions (extension==0)
    int64_t extension = gap + second.length;

    // the second region is inside first one if extension is negative
    if (extension > 0) {
      first.length += extension;
      if ((input_->getStats() != nullptr) && gap > 0) {
        input_->getStats()->incRawOverreadBytes(gap);
      }
    }

    return true;
  }

  return false;
}

std::unique_ptr<SeekableInputStream> BufferedInput::readBuffer(
    uint64_t offset,
    uint64_t length) const {
  const auto result = readInternal(offset, length);

  auto size = std::get<1>(result);
  if (size == MAX_UINT64) {
    return {};
  }

  return std::make_unique<SeekableArrayInputStream>(std::get<0>(result), size);
}

std::tuple<const char*, uint64_t> BufferedInput::readInternal(
    uint64_t offset,
    uint64_t length) const {
  // return dummy one for zero length stream
  if (length == 0) {
    return std::make_tuple(nullptr, 0);
  }

  // Binary search to get the first fileOffset for which: offset < fileOffset
  auto it = std::upper_bound(offsets_.cbegin(), offsets_.cend(), offset);

  // If the first element was already greater than the target offset we don't
  // have it
  if (it != offsets_.cbegin()) {
    const uint64_t index = std::distance(offsets_.cbegin(), it) - 1;
    const uint64_t bufferOffset = offsets_[index];
    const auto& buffer = buffers_[index];
    if (bufferOffset + buffer.size() >= offset + length) {
      DWIO_ENSURE_LE(bufferOffset, offset, "Invalid offset for readInternal");
      DWIO_ENSURE_LE(
          (offset - bufferOffset) + length,
          buffer.size(),
          "Invalid readOffset for read Internal ",
          fmt::format(
              "{} {} {} {}", offset, bufferOffset, length, buffer.size()));

      return std::make_tuple(buffer.data() + (offset - bufferOffset), length);
    }
  }

  return std::make_tuple(nullptr, MAX_UINT64);
}

} // namespace facebook::velox::dwio::common
