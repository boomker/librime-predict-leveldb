#include "predict_data_sync.h"

#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <rime/service.h>
#include <rime/schema.h>
#include <rime/deployer.h>
#include <rime/switcher.h>
#include "predict_engine.h"

namespace fs = std::filesystem;

namespace rime {

static const string predict_snapshot_extension = ".txt";

static const ResourceType kPredictDbResourceType = {"level_predict_db", "", ""};

static string GetUserPredictDbName(Config* config) {
  if (!config) {
    return "predict.userdb";
  }

  string level_db_name = "predict.userdb";
  string db_name;
  if (!config->GetString("predictor/predictdb", &level_db_name)) {
    if (config->GetString("predictor/db", &db_name) &&
        boost::ends_with(db_name, ".userdb")) {
      level_db_name = db_name;
    }
  }
  return level_db_name;
}

static int GetDeletedRecordExpireDays(const string& db_name) {
  the<Config> default_config(Config::Require("config")->Create("default"));
  if (!default_config) {
    return 0;
  }

  int expire_days = 0;
  bool found = false;
  Switcher::ForEachSchemaListEntry(
      default_config.get(),
      [&db_name, &expire_days, &found](const string& schema_id) {
        Schema schema(schema_id);
        Config* config = schema.config();
        if (!config) {
          return true;
        }
        if (GetUserPredictDbName(config) != db_name) {
          return true;
        }

        int schema_expire_days = 0;
        config->GetInt("predictor/deleted_record_expire_days",
                       &schema_expire_days);
        if (!found) {
          expire_days = schema_expire_days;
          found = true;
        } else if (schema_expire_days != expire_days) {
          // Prefer the least destructive retention window when multiple
          // schemas share the same predict userdb.
          expire_days = std::max(expire_days, schema_expire_days);
        }
        return true;
      });

  return expire_days;
}

static bool SyncPredictDb(Deployer* deployer, const string& db_name) {
  LOG(INFO) << "syncing predict db: " << db_name;
  bool success = true;

  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver(kPredictDbResourceType));
  auto file_path = resolver->ResolvePath(db_name);

  if (!fs::exists(file_path)) {
    LOG(INFO) << "predict db not exists: " << file_path;
    return true;  // 不存在不算失败
  }

  auto predict_db = PredictDbManager::instance().GetPredictDb(file_path);
  if (!predict_db || !predict_db->valid()) {
    LOG(ERROR) << "failed to open predict db: " << db_name;
    return false;
  }

  path sync_dir(deployer->sync_dir);
  path backup_dir(deployer->user_data_sync_dir());
  if (!fs::exists(backup_dir)) {
    std::error_code ec;
    if (!fs::create_directories(backup_dir, ec)) {
      LOG(ERROR) << "error creating directory '" << backup_dir << "'.";
      return false;
    }
  }
  string snapshot_file = db_name + predict_snapshot_extension;
  const int deleted_record_expire_days = GetDeletedRecordExpireDays(db_name);

  // 合并旧版快照
  path legacy_snapshot = sync_dir / snapshot_file;
  if (fs::exists(legacy_snapshot) && fs::is_regular_file(legacy_snapshot)) {
    LOG(INFO) << "merging legacy predict snapshot: " << legacy_snapshot;
    if (!predict_db->Restore(legacy_snapshot)) {
      LOG(ERROR) << "failed to merge legacy predict snapshot: "
                 << legacy_snapshot;
      success = false;
    }
  }

  // 合并各设备的快照
  int merged_count = 0;
  int failed_count = 0;
  for (fs::directory_iterator it(sync_dir), end; it != end; ++it) {
    if (!fs::is_directory(it->path()))
      continue;
    path file_path = path(it->path()) / snapshot_file;
    if (fs::exists(file_path) && fs::is_regular_file(file_path)) {
      LOG(INFO) << "merging predict snapshot: " << file_path;
      if (!predict_db->Restore(file_path)) {
        LOG(ERROR) << "failed to merge predict snapshot: " << file_path;
        failed_count++;
        success = false;
      } else {
        merged_count++;
      }
    }
  }

  if (merged_count > 0) {
    LOG(INFO) << "merged " << merged_count << " predict snapshot(s)";
  }
  if (failed_count > 0) {
    LOG(WARNING) << "failed to merge " << failed_count
                 << " predict snapshot(s)";
  }

  // 备份当前状态
  path backup_path = backup_dir / snapshot_file;
  if (!predict_db->Backup(backup_path, deleted_record_expire_days)) {
    LOG(ERROR) << "backup failed: " << backup_path;
    success = false;
  } else {
    LOG(INFO) << "backed up to: " << backup_path;
  }

  return success;
}

bool PredictDataSync::Run(Deployer* deployer) {
  LOG(INFO) << "synchronizing predict data.";
  path sync_dir(deployer->sync_dir);
  if (!fs::exists(sync_dir)) {
    std::error_code ec;
    if (!fs::create_directories(sync_dir, ec)) {
      LOG(ERROR) << "error creating directory '" << sync_dir << "'.";
      return false;
    }
  }

  path user_data_dir = deployer->user_data_dir;
  if (!fs::exists(user_data_dir)) {
    LOG(WARNING) << "user data dir not exists: " << user_data_dir;
    return false;
  }

  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver(kPredictDbResourceType));

  int total_count = 0;
  int failure_count = 0;
  for (fs::directory_iterator it(user_data_dir), end; it != end; ++it) {
    string name = it->path().filename().u8string();
    if (!fs::is_directory(it->path()))
      continue;
    if (name == "predict.userdb" || boost::ends_with(name, ".predict.userdb")) {
      total_count++;
      if (!SyncPredictDb(deployer, name)) {
        failure_count++;
      }
    }
  }

  if (total_count == 0) {
    LOG(INFO) << "no predict databases found.";
    return true;
  }

  if (failure_count > 0) {
    LOG(ERROR) << "failed synchronizing " << failure_count << "/" << total_count
               << " predict database(s).";
    return false;
  }

  LOG(INFO) << "predict data sync completed successfully (" << total_count
            << " database(s)).";
  return true;
}

}  // namespace rime
