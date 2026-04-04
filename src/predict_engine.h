#ifndef RIME_PREDICT_ENGINE_H_
#define RIME_PREDICT_ENGINE_H_

#include "predict_legacy_db.h"
#include "rule_trigger_engine.h"
#include <rime/component.h>
#include <rime/commit_history.h>
#include <msgpack.hpp>
#include <leveldb/db.h>
#include <mutex>
#include <shared_mutex>
#include <rime/dict/db.h>

namespace rime {

struct Prediction {
  std::string word;
  double count;
  int commits = 0;    // commit count (c)
  double dee = 0.0;   // weight/distance (d)
  uint64_t tick = 0;  // timestamp (t)
  MSGPACK_DEFINE(word, count, commits, dee, tick);
};

class PredictDbManager {
 public:
  static PredictDbManager& instance();
  an<class PredictDb> GetPredictDb(const path& file_path);

 private:
  PredictDbManager() = default;
  ~PredictDbManager() = default;
  PredictDbManager(const PredictDbManager&) = delete;
  std::mutex mutex_;
  std::map<string, weak<class PredictDb>> db_cache_;
};

class Context;
struct Segment;
struct Ticket;
class Translation;

class PredictDb {
 public:
  PredictDb(const path& file_path);
  ~PredictDb() { delete db_; }
  bool Lookup(const string& query);
  bool Lookup(const string& query, vector<string>* candidates) const;
  bool LookupPredictions(const string& query,
                         std::vector<Prediction>* predict) const;
  // Check whether any non-deleted prediction for a key is recent.
  bool HasRecentPrediction(const string& query, int max_age_seconds = 1800) const;
  void Clear() {
    if (db_) {
      vector<string>().swap(candidates_);
    }
  }

  bool valid() { return db_ != nullptr; }
  const vector<string>& candidates() const { return candidates_; }
  void UpdatePredict(const string& key,
                     const string& word,
                     bool todelete = false);

  // Sync support
  bool Backup(const path& snapshot_file, int deleted_record_expire_days = 0);
  bool Restore(const path& snapshot_file);
  bool BackupContext(const path& snapshot_file,
                     int deleted_record_expire_days = 0);
  bool RestoreContext(const path& snapshot_file);

 private:
  leveldb::DB* db_;
  vector<string> candidates_;
  mutable std::shared_mutex rw_mutex_;  // 保护数据库并发访问
  friend class PredictDbManager;
};

class PredictEngine : public Class<PredictEngine, const Ticket&> {
 public:
  PredictEngine(an<PredictDb> level_db,
                an<LegacyPredictDb> fallback_db,
                an<RuleTriggerEngine> rule_engine,
                int max_iterations,
                int min_candidates,
                int max_candidates,
                int deleted_record_expire_days,
                bool enable_rule_prediction,
                bool enable_scene_learning,
                int max_context_commits);
  virtual ~PredictEngine();

  bool Predict(Context* ctx, const string& context_query);
  void Clear();
  void CreatePredictSegment(Context* ctx) const;
  an<Translation> Translate(const Segment& segment) const;

  int max_iterations() const { return max_iterations_; }
  int min_candidates() const { return min_candidates_; }
  int max_candidates() const { return max_candidates_; }
  int deleted_record_expire_days() const { return deleted_record_expire_days_; }
  const string& query() const { return query_; }
  int num_candidates() const { return candidates_.size(); }
  string candidates(size_t i) {
    return candidates_.size() ? candidates_.at(i) : string();
  }
  void UpdatePredict(const string& key, const string& word, bool todelete) {
    if (level_db_) {
      level_db_->UpdatePredict(key, word, todelete);
    }
  }
  void UpdatePredict(Context* ctx,
                     const string& key,
                     const string& word,
                     bool todelete);

  bool BackupData(const path& snapshot_file) {
    return level_db_ &&
           level_db_->Backup(snapshot_file, deleted_record_expire_days_);
  }
  bool RestoreData(const path& snapshot_file) {
    return level_db_ && level_db_->Restore(snapshot_file);
  }

 private:
  void AppendCandidates(const vector<string>& source,
                        vector<string>* merged,
                        set<string>* seen) const;
  vector<string> BuildLookupKeys(Context* ctx, const string& query) const;
  vector<string> CollectRecentCommits(Context* ctx, size_t limit) const;
  string DetectScene(Context* ctx) const;
  string BuildSceneKey(const string& scene, const string& query) const;
  string BuildChainKey(const vector<string>& context_chain) const;
  bool IsContextualRecord(const CommitRecord& record) const;

  an<PredictDb> level_db_;
  an<LegacyPredictDb> fallback_db_;
  an<RuleTriggerEngine> rule_engine_;
  int max_iterations_;              // prediction times limit
  int min_candidates_;              // minimum candidate count before fallback
  int max_candidates_;              // prediction candidate count limit
  int deleted_record_expire_days_;  // deleted record expire days
  bool enable_rule_prediction_;
  bool enable_scene_learning_;
  int max_context_commits_;
  string query_;  // cache last query
  vector<string> candidates_;
};

class PredictEngineComponent : public PredictEngine::Component {
 public:
  PredictEngineComponent();
  virtual ~PredictEngineComponent();

  PredictEngine* Create(const Ticket& ticket) override;

  an<PredictEngine> GetInstance(const Ticket& ticket);

 protected:
  map<string, weak<PredictEngine>> predict_engine_by_schema_id;
};

}  // namespace rime

#endif  // RIME_PREDICT_ENGINE_H_
