// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <atomic>
#include "rootserver/ob_server_thread_helper.h"

namespace oceanbase { namespace common { class ObMySQLProxy; }
namespace share { namespace schema { class ObMultiVersionSchemaService; } }
namespace observer {

class AiColumnService final : public rootserver::ObServerThreadHelper {
public:
  AiColumnService() : proxy_(nullptr), schema_service_(nullptr), cancel_(true), started_(false) {}
  ~AiColumnService() { stop(); wait(); }
  int start(common::ObMySQLProxy &proxy, share::schema::ObMultiVersionSchemaService &schema_service);
  void stop();
  void wait();
  void do_work() override;
  void deactivate() override { stop(); }
  int activate() override { return common::OB_SUCCESS; }
private:
  int execute_one();
  int publish_one();
  int backfill_one();
  common::ObMySQLProxy *proxy_;
  share::schema::ObMultiVersionSchemaService *schema_service_;
  std::atomic<bool> cancel_;
  bool started_;
};
} } // namespace oceanbase::observer
