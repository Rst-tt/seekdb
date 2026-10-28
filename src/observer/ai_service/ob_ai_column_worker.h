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

#pragma once

#include <atomic>
#include <thread>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
}
}
namespace observer
{

// Background worker that evaluates pending async AI columns and publishes
// results back to the data table.
class ObAiColumnWorker
{
public:
  ObAiColumnWorker();
  ~ObAiColumnWorker();
  int init();
  int start();
  void stop();
  void wait();
  void destroy();

private:
  void run_();
  int process_once_();
  int process_table_(const share::schema::ObTableSchema &table_schema);

private:
  bool is_inited_;
  std::atomic<bool> stop_;
  std::thread thread_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // namespace observer
} // namespace oceanbase
