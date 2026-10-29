/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  release_pinned_dicts();
  is_inited_ = false;
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        } else if (OB_FAIL(pin_dict(desc.type_, container))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}

int ObFTDictHub::pin_dict(const ObFTDictType type, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  const int idx = static_cast<int>(type);
  if (idx <= 0 || idx >= PINNED_DICT_CNT) {
    // Not a pinnable built-in dictionary type.
  } else {
    if (OB_NOT_NULL(pinned_[idx])) {
      pinned_[idx]->~ObFTCacheRangeContainer();
      pinned_[idx] = nullptr;
    }
    ObFTCacheRangeContainer *pin = OB_NEWx(ObFTCacheRangeContainer, &pin_alloc_, pin_alloc_);
    if (OB_ISNULL(pin)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      const ObList<ObFTCacheRangeHandle *, ObIAllocator> &src_handles = container.get_handles();
      ObList<ObFTCacheRangeHandle *, ObIAllocator>::const_iterator iter = src_handles.begin();
      for (; OB_SUCC(ret) && iter != src_handles.end(); ++iter) {
        ObFTCacheRangeHandle *src = *iter;
        ObFTCacheRangeHandle *dst = nullptr;
        if (OB_FAIL(pin->fetch_info_for_dict(dst))) {
        } else if (OB_FAIL(dst->handle_.assign(src->handle_))) {
        } else {
          dst->type_ = src->type_;
          dst->key_ = src->key_;
          dst->value_ = src->value_;
        }
      }
      if (OB_FAIL(ret)) {
        pin->~ObFTCacheRangeContainer();
      } else {
        pinned_[idx] = pin;
      }
    }
  }
  return ret;
}

void ObFTDictHub::release_pinned_dicts()
{
  for (int i = 0; i < PINNED_DICT_CNT; ++i) {
    if (OB_NOT_NULL(pinned_[i])) {
      pinned_[i]->~ObFTCacheRangeContainer();
      pinned_[i] = nullptr;
    }
  }
  pin_alloc_.reset();
}

} //  namespace storage
} //  namespace oceanbase
