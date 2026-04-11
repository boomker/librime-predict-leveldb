#include "predict_engine.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <vector>
#include <utf8.h>
#include <boost/algorithm/string.hpp>
#include <rime_api.h>
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/schema.h>

namespace rime {

static const ResourceType kPredictDbPredictDbResourceType = {"level_predict_db",
                                                             "", ""};
static const ResourceType kTriggerRuleDbResourceType = {"userdb", "", ""};

// 旧版本的 Prediction 结构（只有 word 和 count）
struct LegacyPrediction {
  std::string word;
  double count;
  MSGPACK_DEFINE(word, count);
};

constexpr char kSceneKeyPrefix[] = "__scene__:";
constexpr char kChainKeyPrefix[] = "__chain__:";
constexpr char kDefaultScene[] = "general";

enum class ContextSnapshotType {
  kSceneQuery,
  kContextChain,
  kSceneContextChain,
};

struct ContextSnapshotEntry {
  ContextSnapshotType type = ContextSnapshotType::kSceneQuery;
  string scene;
  string query;
  vector<string> context_chain;
  string word;
  int commits = 0;
  double dee = 0.0;
  uint64_t tick = 0;
  double count = 0.0;
};

struct RankedLearningCandidate {
  string word;
  int commits = 0;
  uint64_t tick = 0;
};

static bool ReadDbTextValue(leveldb::DB* db,
                            const string& key,
                            const string& fallback,
                            string* value);
static void SortPredictions(std::vector<Prediction>& predict);

static bool ContainsAny(const string& text,
                        std::initializer_list<const char*> keywords) {
  for (const char* keyword : keywords) {
    if (text.find(keyword) != string::npos) {
      return true;
    }
  }
  return false;
}

static string ToLowerCopy(string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  return text;
}

static bool LooksLikeProgramming(const string& text) {
  string lower = ToLowerCopy(text);
  if (ContainsAny(lower, {"::",       "->",      "()",      "{}",      "[]",
                          "#include", "null",    "nullptr", "todo",    "fixme",
                          "return",   "const ",  "class ",  "struct ", "bool ",
                          "int ",     "string ", "vector<", "map<",    "if (",
                          "for (",    "while (", "json",    "api",     "sql",
                          "http",     "cpp",     "python"})) {
    return true;
  }
  size_t ascii_count = 0;
  for (unsigned char ch : text) {
    if (std::isalnum(ch) || ch == '_' || ch == '/' || ch == '.') {
      ++ascii_count;
    }
  }
  return ascii_count >= 6 && ascii_count * 2 >= text.size();
}

static bool LooksLikeOffice(const string& text) {
  return ContainsAny(
      text, {"会议", "审批", "请查收", "附件", "邮件", "汇报", "方案", "计划",
             "进度", "排期", "合同", "报表", "客户", "确认一下", "尽快处理",
             "知悉", "烦请", "辛苦了"});
}

static bool LooksLikeChat(const string& text) {
  return ContainsAny(text, {"哈哈", "晚安", "早安", "在吗", "收到啦", "么么哒",
                            "谢谢你", "想你", "吃饭", "睡觉", "周末", "好呀",
                            "表情", "开心", "哭", "呀", "呢", "嘛", "吧"});
}

static bool DecodePredictions(const string& value,
                              std::vector<Prediction>* predict) {
  if (!predict) {
    return false;
  }
  predict->clear();

  // 尝试解码为新格式（5 字段）
  try {
    msgpack::unpacked unpacked;
    msgpack::unpack(unpacked, value.data(), value.size());
    unpacked.get().convert(*predict);
    return true;
  } catch (const std::exception& ex) {
    // 解码失败，尝试旧格式
  } catch (...) {
    // 解码失败，尝试旧格式
  }

  // 尝试解码为旧格式（2 字段）msgpack
  try {
    msgpack::unpacked unpacked;
    msgpack::unpack(unpacked, value.data(), value.size());
    std::vector<LegacyPrediction> legacy_predict;
    unpacked.get().convert(legacy_predict);

    // 转换为新格式，使用默认值填充新字段
    for (const auto& legacy : legacy_predict) {
      predict->push_back({legacy.word, legacy.count, 0, 0.0, 0});
    }

    if (!predict->empty()) {
      return true;
    }
  } catch (const std::exception& ex) {
    // 解码失败，尝试文本格式
  } catch (...) {
    // 解码失败，尝试文本格式
  }

  // 最后尝试旧的文本格式
  std::istringstream iss(value);
  string word;
  double count = 0.0;
  while (std::getline(iss, word, '\0')) {
    if (!std::getline(iss, word, '\t')) {
      break;
    }
    string count_text;
    if (!std::getline(iss, count_text, '\n')) {
      break;
    }
    try {
      count = std::stod(count_text);
    } catch (const std::exception&) {
      continue;
    }
    predict->push_back({word, count, 0, 0.0, 0});
  }

  if (!predict->empty()) {
    return true;
  }
  return false;
}

static string NormalizeSnapshotHeader(string header) {
  if (!header.empty() && static_cast<unsigned char>(header[0]) == 0xEF &&
      header.size() >= 3 && static_cast<unsigned char>(header[1]) == 0xBB &&
      static_cast<unsigned char>(header[2]) == 0xBF) {
    header.erase(0, 3);
  }
  if (!header.empty() && header.back() == '\r') {
    header.pop_back();
  }
  return header;
}

static bool IsChineseCodePoint(uint32_t code_point) {
  return (code_point >= 0x3400 && code_point <= 0x4DBF) ||  // CJK Ext A
         (code_point >= 0x4E00 && code_point <= 0x9FFF) ||  // CJK Unified
         (code_point >= 0xF900 && code_point <= 0xFAFF) ||  // CJK Compatibility
         (code_point >= 0x20000 && code_point <= 0x2A6DF) ||  // CJK Ext B
         (code_point >= 0x2A700 && code_point <= 0x2B73F) ||  // CJK Ext C
         (code_point >= 0x2B740 && code_point <= 0x2B81F) ||  // CJK Ext D
         (code_point >= 0x2B820 && code_point <= 0x2CEAF) ||  // CJK Ext E-F
         (code_point >= 0x2CEB0 && code_point <= 0x2EBEF) ||  // CJK Ext G-I
         (code_point >= 0x30000 && code_point <= 0x3134F);    // CJK Ext G
}

static bool IsChinesePunctuationCodePoint(uint32_t code_point) {
  return (code_point >= 0x3000 && code_point <= 0x303F) ||
         (code_point >= 0xFF01 && code_point <= 0xFF0F) ||
         (code_point >= 0xFF1A && code_point <= 0xFF20) ||
         (code_point >= 0xFF3B && code_point <= 0xFF40) ||
         (code_point >= 0xFF5B && code_point <= 0xFF65);
}

static bool IsPunctOnly(const string& text) {
  if (text.empty())
    return true;

  const char* p = text.c_str();
  const char* end = p + text.length();

  try {
    while (p < end) {
      uint32_t code_point = utf8::next(p, end);

      // 如果包含中文字符，不是纯标点
      if (IsChineseCodePoint(code_point))
        return false;

      // 如果包含字母数字，不是纯标点
      if ((code_point >= '0' && code_point <= '9') ||
          (code_point >= 'A' && code_point <= 'Z') ||
          (code_point >= 'a' && code_point <= 'z'))
        return false;
    }
  } catch (const utf8::exception&) {
    // UTF-8 解码失败，认为不是纯标点
    DLOG(WARNING) << "invalid UTF-8 sequence in text: " << text;
    return false;
  }

  return true;
}

static bool ContainsContextPunctuationOrDelimiter(const string& text) {
  if (text.empty() || text.find_first_of("\t\r\n") != string::npos) {
    return true;
  }

  const char* p = text.c_str();
  const char* end = p + text.length();
  try {
    while (p < end) {
      uint32_t code_point = utf8::next(p, end);
      if ((code_point < 128 &&
           std::ispunct(static_cast<unsigned char>(code_point))) ||
          IsChinesePunctuationCodePoint(code_point)) {
        return true;
      }
    }
  } catch (const utf8::exception&) {
    return true;
  }
  return false;
}

static bool IsContextSnapshotSafeText(const string& text) {
  return !text.empty() && !ContainsContextPunctuationOrDelimiter(text);
}

static bool AreContextSnapshotSafeTexts(const vector<string>& texts) {
  return std::all_of(texts.begin(), texts.end(), [](const string& text) {
    return IsContextSnapshotSafeText(text);
  });
}

static const char* ContextSnapshotTypeName(ContextSnapshotType type) {
  switch (type) {
    case ContextSnapshotType::kSceneQuery:
      return "scene_query";
    case ContextSnapshotType::kContextChain:
      return "context_chain";
    case ContextSnapshotType::kSceneContextChain:
      return "scene_context_chain";
  }
  return "scene_query";
}

static bool ParseContextSnapshotType(const string& type,
                                     ContextSnapshotType* result) {
  if (!result) {
    return false;
  }
  if (type == "scene_query") {
    *result = ContextSnapshotType::kSceneQuery;
    return true;
  }
  if (type == "context_chain") {
    *result = ContextSnapshotType::kContextChain;
    return true;
  }
  if (type == "scene_context_chain") {
    *result = ContextSnapshotType::kSceneContextChain;
    return true;
  }
  return false;
}

static bool SplitScenePayload(const string& payload,
                              string* scene,
                              string* nested_key) {
  if (!scene || !nested_key) {
    return false;
  }
  size_t separator = payload.find('|');
  if (separator == string::npos || separator == 0 ||
      separator + 1 >= payload.size()) {
    return false;
  }
  *scene = payload.substr(0, separator);
  *nested_key = payload.substr(separator + 1);
  return !scene->empty() && !nested_key->empty();
}

static bool SplitContextChainPayload(const string& payload,
                                     vector<string>* context_chain) {
  if (!context_chain || payload.empty()) {
    return false;
  }
  context_chain->clear();
  boost::split(*context_chain, payload, boost::is_any_of("\n"),
               boost::token_compress_off);
  if (context_chain->empty()) {
    return false;
  }
  return std::all_of(context_chain->begin(), context_chain->end(),
                     [](const string& item) { return !item.empty(); });
}

static bool ParseContextSnapshotKey(const string& key,
                                    ContextSnapshotEntry* entry) {
  if (!entry) {
    return false;
  }
  entry->scene.clear();
  entry->query.clear();
  entry->context_chain.clear();
  if (boost::algorithm::starts_with(key, kSceneKeyPrefix)) {
    string scene;
    string nested_key;
    if (!SplitScenePayload(key.substr(sizeof(kSceneKeyPrefix) - 1), &scene,
                           &nested_key)) {
      return false;
    }
    entry->scene = scene;
    if (boost::algorithm::starts_with(nested_key, kChainKeyPrefix)) {
      entry->type = ContextSnapshotType::kSceneContextChain;
      return SplitContextChainPayload(
          nested_key.substr(sizeof(kChainKeyPrefix) - 1),
          &entry->context_chain);
    }
    entry->type = ContextSnapshotType::kSceneQuery;
    entry->query = nested_key;
    return true;
  }
  if (boost::algorithm::starts_with(key, kChainKeyPrefix)) {
    entry->type = ContextSnapshotType::kContextChain;
    return SplitContextChainPayload(key.substr(sizeof(kChainKeyPrefix) - 1),
                                    &entry->context_chain);
  }
  return false;
}

static bool WriteSnapshotHeader(leveldb::DB* db,
                                const path& snapshot_file,
                                const string& title,
                                std::ofstream* out) {
  if (!out) {
    return false;
  }
  string db_name = snapshot_file.stem().u8string();
  string db_type = "userdb";
  string rime_version = RIME_VERSION;
  string tick = std::to_string(static_cast<uint64_t>(std::time(nullptr)));
  string user_id = snapshot_file.parent_path().filename().u8string();

  ReadDbTextValue(db, "\x01/db_name", db_name, &db_name);
  ReadDbTextValue(db, "\x01/db_type", db_type, &db_type);
  ReadDbTextValue(db, "\x01/rime_version", rime_version, &rime_version);
  ReadDbTextValue(db, "\x01/user_id", user_id, &user_id);

  *out << title << "\n";
  *out << "#@/db_name\t" << db_name << "\n";
  *out << "#@/db_type\t" << db_type << "\n";
  *out << "#@/rime_version\t" << rime_version << "\n";
  *out << "#@/tick\t" << tick << "\n";
  *out << "#@/user_id\t" << user_id << "\n";
  return true;
}

static bool ParsePredictionMetadata(const string& metadata_str,
                                    int* commits,
                                    double* dee,
                                    uint64_t* tick,
                                    double* count) {
  if (!commits || !dee || !tick || !count) {
    return false;
  }
  *commits = 0;
  *dee = 0.0;
  *tick = 0;
  *count = 0.0;

  if (metadata_str.find("c=") != string::npos) {
    vector<string> kv;
    boost::split(kv, metadata_str, boost::is_any_of(" "));
    for (const string& k_eq_v : kv) {
      size_t eq = k_eq_v.find('=');
      if (eq == string::npos) {
        continue;
      }
      string key = k_eq_v.substr(0, eq);
      string value = k_eq_v.substr(eq + 1);
      try {
        if (key == "c") {
          *commits = std::stoi(value);
        } else if (key == "d") {
          *dee = std::stod(value);
        } else if (key == "t") {
          *tick = std::stoull(value);
        }
      } catch (...) {
        LOG(WARNING) << "failed parsing metadata in predict snapshot: "
                     << k_eq_v;
      }
    }
    *count = *dee;
    return true;
  }

  try {
    *count = std::stod(metadata_str);
    if (*count < 1.0) {
      *commits = 1;
    } else if (*count < 1.5) {
      *commits = 2;
    } else if (*count <= 2.0) {
      *commits = 3;
    } else {
      *commits = static_cast<int>(std::ceil(*count));
    }
    *dee = *count;
    *tick = 1;
    return true;
  } catch (const std::exception& ex) {
    LOG(WARNING) << "skipping malformed predict snapshot row: " << ex.what();
    return false;
  }
}

static bool UpsertRestoredPrediction(leveldb::DB* db,
                                     const string& key,
                                     const string& word,
                                     double count,
                                     int commits,
                                     double dee,
                                     uint64_t tick) {
  if (!db) {
    return false;
  }
  std::vector<Prediction> predict;
  string value;
  leveldb::Status status = db->Get(leveldb::ReadOptions(), key, &value);
  if (status.ok() && !DecodePredictions(value, &predict)) {
    LOG(WARNING) << "failed to decode existing prediction list for key: " << key
                 << "; recreating it.";
    predict.clear();
  }

  bool found = false;
  for (auto& p : predict) {
    if (p.word == word) {
      p.count = std::max(p.count, count);
      p.commits = std::max(p.commits, commits);
      p.dee = std::max(p.dee, dee);
      p.tick = std::max(p.tick, tick);
      found = true;
      break;
    }
  }
  if (!found) {
    predict.push_back({word, count, commits, dee, tick});
  }
  SortPredictions(predict);
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, predict);
  status = db->Put(leveldb::WriteOptions(), key,
                   leveldb::Slice(sbuf.data(), sbuf.size()));
  if (!status.ok()) {
    LOG(ERROR) << "failed writing restored prediction for key '" << key
               << "': " << status.ToString();
    return false;
  }
  return true;
}

static bool ReadDbTextValue(leveldb::DB* db,
                            const string& key,
                            const string& fallback,
                            string* value) {
  if (!value) {
    return false;
  }
  *value = fallback;
  if (!db) {
    return false;
  }
  string db_value;
  leveldb::Status status = db->Get(leveldb::ReadOptions(), key, &db_value);
  if (!status.ok() || db_value.empty()) {
    return false;
  }
  *value = db_value;
  return true;
}

static int RecencyTier(uint64_t tick, uint64_t now) {
  // Legacy tick values (pre-timestamp, before ~Sep 2001) treated as ancient
  if (tick < 1000000000 || tick > now)
    return 0;
  uint64_t age = now - tick;
  if (age <= 1800)
    return 6;  // 30 min
  if (age <= 3600)
    return 5;  // 1 hour
  if (age <= 7200)
    return 4;  // 2 hours
  if (age <= 14400)
    return 3;  // 4 hours
  if (age <= 28800)
    return 2;  // 8 hours
  if (age <= 86400)
    return 1;  // 24 hours
  return 0;
}

static bool IsRecentLearningPrediction(const Prediction& prediction,
                                       uint64_t now,
                                       int max_age_seconds) {
  if (prediction.commits <= 0) {
    return false;
  }
  if (prediction.tick < 1000000000 || prediction.tick > now) {
    return false;
  }
  return now - prediction.tick <= static_cast<uint64_t>(max_age_seconds);
}

static bool IsBetterRankedLearningCandidate(
    const RankedLearningCandidate& lhs,
    const RankedLearningCandidate& rhs) {
  if (lhs.tick != rhs.tick) {
    return lhs.tick > rhs.tick;
  }
  return lhs.commits > rhs.commits;
}

static void SortPredictions(std::vector<Prediction>& predict) {
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  bool has_recent = std::any_of(
      predict.begin(), predict.end(),
      [now](const Prediction& p) { return RecencyTier(p.tick, now) > 0; });
  std::sort(predict.begin(), predict.end(),
            [now, has_recent](const Prediction& a, const Prediction& b) {
              if (has_recent) {
                int tier_a = RecencyTier(a.tick, now);
                int tier_b = RecencyTier(b.tick, now);
                if (tier_a != tier_b)
                  return tier_a > tier_b;
              }
              if (a.commits != b.commits)
                return a.commits > b.commits;
              return a.tick > b.tick;
            });
}

PredictDbManager& PredictDbManager::instance() {
  static PredictDbManager instance;
  return instance;
}

an<PredictDb> PredictDbManager::GetPredictDb(const path& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = db_cache_.find(file_path.string());
  if (found != db_cache_.end()) {
    if (auto db = found->second.lock()) {
      return db;
    } else {
      db_cache_.erase(found);
    }
  }
  an<PredictDb> new_db = std::make_shared<PredictDb>(file_path);
  if (new_db->valid()) {
    db_cache_[file_path.string()] = new_db;
    return new_db;
  } else {
    LOG(ERROR) << "Failed to create PredictDb for: " << file_path;
    return nullptr;
  }
}

PredictEngine::PredictEngine(an<PredictDb> level_db,
                             an<LegacyPredictDb> fallback_db,
                             an<RuleTriggerEngine> rule_engine,
                             int max_iterations,
                             int min_candidates,
                             int max_candidates,
                             int deleted_record_expire_days,
                             bool enable_rule_prediction,
                             bool enable_scene_learning,
                             int max_context_commits,
                             UserRulePriority user_rule_priority)
    : level_db_(level_db),
      fallback_db_(fallback_db),
      rule_engine_(rule_engine),
      max_iterations_(max_iterations),
      min_candidates_(min_candidates),
      max_candidates_(max_candidates),
      deleted_record_expire_days_(deleted_record_expire_days),
      enable_rule_prediction_(enable_rule_prediction),
      enable_scene_learning_(enable_scene_learning),
      max_context_commits_(std::max(1, max_context_commits)),
      user_rule_priority_(user_rule_priority) {}

PredictEngine::~PredictEngine() {}

bool PredictEngine::Predict(Context* ctx, const string& context_query) {
  if (!level_db_ && !fallback_db_ && !rule_engine_) {
    return false;
  }
  query_ = context_query;
  vector<string> rule_candidates;
  vector<string> user_rule_candidates;
  vector<string> all_learned_candidates;
  vector<string> top_contextual_candidates;
  vector<string> merged;
  set<string> seen_all_learned;
  set<string> seen;

  // Collect rule-based candidates first
  if (enable_rule_prediction_ && rule_engine_) {
    const string rule_scene = DetectScene(ctx);
    user_rule_candidates =
        rule_engine_->Match(context_query, rule_scene, /*user_only=*/true);
    rule_candidates = rule_engine_->Match(context_query, rule_scene);
  }

  string best_recent_query_candidate;
  int best_recent_query_commits = -1;
  uint64_t best_recent_query_tick = 0;
  std::map<string, RankedLearningCandidate> contextual_candidate_scores;
  if (level_db_) {
    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    const string scene = DetectScene(ctx);
    vector<std::pair<string, bool>> lookup_keys;
    vector<string> recent = CollectRecentCommits(ctx, max_context_commits_);
    if (recent.size() >= 2) {
      vector<string> context_chain = {recent[recent.size() - 2], recent.back()};
      const string context_chain_key = BuildChainKey(context_chain);
      lookup_keys.emplace_back(BuildSceneKey(scene, context_chain_key), true);
      lookup_keys.emplace_back(context_chain_key, true);
    }
    lookup_keys.emplace_back(BuildSceneKey(scene, context_query), false);
    lookup_keys.emplace_back(context_query, false);

    for (const auto& lookup_key : lookup_keys) {
      const string& key = lookup_key.first;
      const bool is_contextual = lookup_key.second;
      std::vector<Prediction> predictions;
      if (!level_db_->LookupPredictions(key, &predictions)) {
        continue;
      }
      vector<string> learned_candidates;
      for (const auto& prediction : predictions) {
        if (prediction.commits <= 0) {
          continue;
        }
        learned_candidates.push_back(prediction.word);
      }
      if (learned_candidates.empty()) {
        continue;
      }
      AppendCandidates(learned_candidates, &all_learned_candidates,
                       &seen_all_learned);

      if (is_contextual) {
        for (const auto& prediction : predictions) {
          if (!IsRecentLearningPrediction(prediction, now, 2 * 3600)) {
            continue;
          }
          RankedLearningCandidate candidate = {
              prediction.word, prediction.commits, prediction.tick};
          auto found = contextual_candidate_scores.find(prediction.word);
          if (found == contextual_candidate_scores.end() ||
              IsBetterRankedLearningCandidate(candidate, found->second)) {
            contextual_candidate_scores[prediction.word] = candidate;
          }
        }
        continue;
      }

      for (const auto& prediction : predictions) {
        if (!IsRecentLearningPrediction(prediction, now, 1800)) {
          continue;
        }
        if (best_recent_query_candidate.empty() ||
            prediction.tick > best_recent_query_tick ||
            (prediction.tick == best_recent_query_tick &&
             prediction.commits > best_recent_query_commits)) {
          best_recent_query_candidate = prediction.word;
          best_recent_query_commits = prediction.commits;
          best_recent_query_tick = prediction.tick;
        }
      }
    }
  }

  if (!contextual_candidate_scores.empty()) {
    vector<RankedLearningCandidate> ranked_contextual_candidates;
    ranked_contextual_candidates.reserve(contextual_candidate_scores.size());
    for (const auto& item : contextual_candidate_scores) {
      ranked_contextual_candidates.push_back(item.second);
    }
    std::sort(ranked_contextual_candidates.begin(),
              ranked_contextual_candidates.end(),
              [](const RankedLearningCandidate& lhs,
                 const RankedLearningCandidate& rhs) {
                return IsBetterRankedLearningCandidate(lhs, rhs);
              });
    const size_t limit =
        std::min<size_t>(2, ranked_contextual_candidates.size());
    for (size_t i = 0; i < limit; ++i) {
      top_contextual_candidates.push_back(ranked_contextual_candidates[i].word);
    }
  }

  // Promote rule-based candidates only when no recent learning candidates exist
  // in either the direct-query (30 min) or contextual (2 hour) windows.
  const bool has_recent_learning = !best_recent_query_candidate.empty() ||
                                   !top_contextual_candidates.empty();

  // 根据 user_rule_priority 决定用户规则候选的位置
  if (user_rule_priority_ == UserRulePriority::High) {
    // high：用户规则候选始终在最前面，不受学习项影响
    AppendCandidates(user_rule_candidates, &merged, &seen);
    if (!has_recent_learning) {
      AppendCandidates(rule_candidates, &merged, &seen);
    } else {
      if (!best_recent_query_candidate.empty()) {
        vector<string> best_recent_query = {best_recent_query_candidate};
        AppendCandidates(best_recent_query, &merged, &seen);
      }
      AppendCandidates(top_contextual_candidates, &merged, &seen);
      AppendCandidates(rule_candidates, &merged, &seen);
    }
    AppendCandidates(all_learned_candidates, &merged, &seen);
  } else if (user_rule_priority_ == UserRulePriority::Auto) {
    // auto：无新学习项时用户规则在前，有新学习项则学习项顶替到前面
    if (!has_recent_learning) {
      AppendCandidates(rule_candidates, &merged, &seen);
    } else {
      if (!best_recent_query_candidate.empty()) {
        vector<string> best_recent_query = {best_recent_query_candidate};
        AppendCandidates(best_recent_query, &merged, &seen);
      }
      AppendCandidates(top_contextual_candidates, &merged, &seen);
      AppendCandidates(rule_candidates, &merged, &seen);
    }
    AppendCandidates(all_learned_candidates, &merged, &seen);
  } else {
    // low：先不插入用户规则候选，收集其余候选后插入到第5位（index 4）
    // 构造排除用户规则候选的 builtin rule 候选集
    set<string> user_rule_set(user_rule_candidates.begin(),
                              user_rule_candidates.end());
    vector<string> builtin_rule_candidates;
    for (const auto& c : rule_candidates) {
      if (!user_rule_set.count(c)) {
        builtin_rule_candidates.push_back(c);
      }
    }
    if (!has_recent_learning) {
      AppendCandidates(builtin_rule_candidates, &merged, &seen);
    } else {
      if (!best_recent_query_candidate.empty()) {
        vector<string> best_recent_query = {best_recent_query_candidate};
        AppendCandidates(best_recent_query, &merged, &seen);
      }
      AppendCandidates(top_contextual_candidates, &merged, &seen);
      AppendCandidates(builtin_rule_candidates, &merged, &seen);
    }
    AppendCandidates(all_learned_candidates, &merged, &seen);
    // 将用户规则候选插入到第5候选位置（index 4），重复项跳过
    static constexpr size_t kLowInsertPos = 4;
    size_t insert_pos = std::min(kLowInsertPos, merged.size());
    for (const auto& c : user_rule_candidates) {
      if (seen.insert(c).second) {
        merged.insert(merged.begin() + insert_pos, c);
        ++insert_pos;
      }
    }
  }

  if (fallback_db_ &&
      (merged.empty() || (min_candidates_ > 0 &&
                          static_cast<int>(merged.size()) < min_candidates_))) {
    vector<string> fallback_candidates;
    if (fallback_db_->Lookup(context_query, &fallback_candidates)) {
      AppendCandidates(fallback_candidates, &merged, &seen);
    }
  }

  if (merged.empty()) {
    Clear();
    return false;
  }
  candidates_ = std::move(merged);
  return true;
}

void PredictEngine::Clear() {
  VLOG(3) << "PredictEngine::Clear";
  query_.clear();
  vector<string>().swap(candidates_);
}

void PredictEngine::CreatePredictSegment(Context* ctx) const {
  VLOG(3) << "PredictEngine::CreatePredictSegment";
  int end = int(ctx->input().length());
  Segment segment(end, end);
  segment.tags.insert("prediction");
  segment.tags.insert("placeholder");
  ctx->composition().AddSegment(segment);
  ctx->composition().back().tags.erase("raw");
  VLOG(3) << "segments: " << ctx->composition();
}

an<Translation> PredictEngine::Translate(const Segment& segment) const {
  VLOG(3) << "PredictEngine::Translate";
  auto translation = New<FifoTranslation>();
  size_t end = segment.end;
  int i = 0;
  for (auto predict : candidates_) {
    translation->Append(New<SimpleCandidate>("prediction", end, end, predict));
    i++;
    if (max_candidates_ > 0 && i >= max_candidates_)
      break;
  }
  return translation;
}

void PredictEngine::UpdatePredict(Context* ctx,
                                  const string& key,
                                  const string& word,
                                  bool todelete) {
  if (!level_db_) {
    return;
  }
  UpdatePredict(key, word, todelete);
  if (!enable_scene_learning_ || !ctx) {
    return;
  }
  if (!IsContextSnapshotSafeText(key) || !IsContextSnapshotSafeText(word)) {
    return;
  }

  const string scene = DetectScene(ctx);
  level_db_->UpdatePredict(BuildSceneKey(scene, key), word, todelete);

  if (ctx->commit_history().size() < 3) {
    return;
  }
  const auto last = ctx->commit_history().back();
  const auto middle = *std::prev(ctx->commit_history().end(), 2);
  const auto first = *std::prev(ctx->commit_history().end(), 3);
  if (middle.text != key || last.text != word || !IsContextualRecord(first) ||
      !IsContextualRecord(middle) || !IsContextualRecord(last)) {
    return;
  }
  vector<string> context_chain = {first.text, middle.text};
  const string context_chain_key = BuildChainKey(context_chain);
  level_db_->UpdatePredict(context_chain_key, word, todelete);
  level_db_->UpdatePredict(BuildSceneKey(scene, context_chain_key), word,
                           todelete);
}

void PredictEngine::AppendCandidates(const vector<string>& source,
                                     vector<string>* merged,
                                     set<string>* seen) const {
  if (!merged || !seen) {
    return;
  }
  for (const auto& candidate : source) {
    if (candidate.empty() || !seen->insert(candidate).second) {
      continue;
    }
    merged->push_back(candidate);
  }
}

vector<string> PredictEngine::BuildLookupKeys(Context* ctx,
                                              const string& query) const {
  vector<string> keys;
  if (query.empty()) {
    return keys;
  }
  const string scene = DetectScene(ctx);
  vector<string> recent = CollectRecentCommits(ctx, max_context_commits_);
  if (recent.size() >= 2) {
    vector<string> context_chain = {recent[recent.size() - 2], recent.back()};
    const string context_chain_key = BuildChainKey(context_chain);
    keys.push_back(BuildSceneKey(scene, context_chain_key));
    keys.push_back(context_chain_key);
  }
  if (IsContextSnapshotSafeText(query)) {
    keys.push_back(BuildSceneKey(scene, query));
  }
  keys.push_back(query);
  return keys;
}

vector<string> PredictEngine::CollectRecentCommits(Context* ctx,
                                                   size_t limit) const {
  vector<string> commits;
  if (!ctx || limit == 0) {
    return commits;
  }
  for (auto it = ctx->commit_history().rbegin();
       it != ctx->commit_history().rend() && commits.size() < limit; ++it) {
    if (!IsContextualRecord(*it)) {
      continue;
    }
    commits.push_back(it->text);
  }
  std::reverse(commits.begin(), commits.end());
  return commits;
}

string PredictEngine::DetectScene(Context* ctx) const {
  if (!ctx) {
    return kDefaultScene;
  }
  string explicit_scene = ctx->get_property("predict_scene");
  if (explicit_scene == "chat" || explicit_scene == "office" ||
      explicit_scene == "programming" || explicit_scene == "general") {
    return explicit_scene;
  }

  int programming_score = 0;
  int office_score = 0;
  int chat_score = 0;
  for (const auto& text : CollectRecentCommits(ctx, 6)) {
    programming_score += LooksLikeProgramming(text) ? 2 : 0;
    office_score += LooksLikeOffice(text) ? 2 : 0;
    chat_score += LooksLikeChat(text) ? 2 : 0;
  }
  if (ctx->get_option("ascii_mode")) {
    ++programming_score;
  }
  if (programming_score >= office_score && programming_score >= chat_score &&
      programming_score > 0) {
    return "programming";
  }
  if (office_score >= chat_score && office_score > 0) {
    return "office";
  }
  if (chat_score > 0) {
    return "chat";
  }
  return kDefaultScene;
}

string PredictEngine::BuildSceneKey(const string& scene,
                                    const string& query) const {
  return string(kSceneKeyPrefix) +
         (scene.empty() ? string(kDefaultScene) : scene) + "|" + query;
}

string PredictEngine::BuildChainKey(const vector<string>& context_chain) const {
  return string(kChainKeyPrefix) + boost::algorithm::join(context_chain, "\n");
}

bool PredictEngine::IsContextualRecord(const CommitRecord& record) const {
  return record.type != "punct" && record.type != "raw" &&
         record.type != "thru" && IsContextSnapshotSafeText(record.text);
}

PredictEngineComponent::PredictEngineComponent() {}

PredictEngineComponent::~PredictEngineComponent() {}

PredictEngine* PredictEngineComponent::Create(const Ticket& ticket) {
  string level_db_name = "predict.userdb";
  string fallback_db_name = "predict.db";
  string rules_db_name = "predict_rule.db";
  string db_name;
  int min_candidates = 3;
  int max_candidates = 0;
  int max_iterations = 0;
  int deleted_record_expire_days = 0;
  int max_context_commits = 2;
  const bool enable_rule_prediction = true;
  const bool enable_scene_learning = true;
  UserRulePriority user_rule_priority = UserRulePriority::Auto;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    if (!config->GetString("predictor/predictdb", &level_db_name)) {
      if (config->GetString("predictor/db", &db_name)) {
        if (boost::ends_with(db_name, ".userdb")) {
          level_db_name = db_name;
        } else {
          fallback_db_name = db_name;
        }
      }
    } else {
      config->GetString("predictor/db", &db_name);
      if (!db_name.empty() && !boost::ends_with(db_name, ".userdb")) {
        fallback_db_name = db_name;
      }
    }
    config->GetString("predictor/fallback_db", &fallback_db_name);
    config->GetInt("predictor/min_candidates", &min_candidates);
    config->GetInt("predictor/max_candidates", &max_candidates);
    config->GetInt("predictor/max_iterations", &max_iterations);
    config->GetInt("predictor/garbage_expire_days",
                   &deleted_record_expire_days);
    config->GetString("predictor/rule_db", &rules_db_name);
    config->GetInt("predictor/max_context_commits", &max_context_commits);
    string user_rule_priority_str;
    if (config->GetString("predictor/user_rule_priority",
                          &user_rule_priority_str)) {
      if (user_rule_priority_str == "high") {
        user_rule_priority = UserRulePriority::High;
      } else if (user_rule_priority_str == "low") {
        user_rule_priority = UserRulePriority::Low;
      }
      // "auto" or unrecognized → keep default Auto
    }
  }

  the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
      kPredictDbPredictDbResourceType));
  auto file_path = resolver->ResolvePath(level_db_name);
  an<PredictDb> level_db = PredictDbManager::instance().GetPredictDb(file_path);
  an<LegacyPredictDb> fallback_db;
  if (!fallback_db_name.empty()) {
    static const ResourceType kPredictDbResourceType = {"predict_db", "", ""};
    the<ResourceResolver> fallback_resolver(
        Service::instance().CreateResourceResolver(kPredictDbResourceType));
    auto fallback_path = fallback_resolver->ResolvePath(fallback_db_name);
    fallback_db =
        LegacyPredictDbManager::instance().GetPredictDb(fallback_path);
  }

  an<RuleTriggerEngine> rule_engine = New<RuleTriggerEngine>();
  the<ResourceResolver> trigger_resolver(
      Service::instance().CreateResourceResolver(kTriggerRuleDbResourceType));
  rule_engine->LoadFromDB(trigger_resolver->ResolvePath(rules_db_name));
  if (ticket.schema && ticket.schema->config()) {
    rule_engine->LoadFromConfig(ticket.schema->config());
  }

  if (level_db && level_db->valid()) {
    return new PredictEngine(level_db, fallback_db, rule_engine, max_iterations,
                             min_candidates, max_candidates,
                             deleted_record_expire_days, enable_rule_prediction,
                             enable_scene_learning, max_context_commits,
                             user_rule_priority);
  }
  if (fallback_db && fallback_db->valid()) {
    return new PredictEngine(level_db, fallback_db, rule_engine, max_iterations,
                             min_candidates, max_candidates,
                             deleted_record_expire_days, enable_rule_prediction,
                             enable_scene_learning, max_context_commits,
                             user_rule_priority);
  }
  {
    LOG(ERROR) << "failed to load predict db: " << level_db_name;
  }

  return nullptr;
}

an<PredictEngine> PredictEngineComponent::GetInstance(const Ticket& ticket) {
  if (Schema* schema = ticket.schema) {
    std::ostringstream key_builder;
    key_builder << schema->schema_id() << "@" << ticket.engine;
    const string engine_key = key_builder.str();
    auto found = predict_engine_by_schema_id.find(engine_key);
    if (found != predict_engine_by_schema_id.end()) {
      if (auto instance = found->second.lock()) {
        return instance;
      }
    }
    an<PredictEngine> new_instance{Create(ticket)};
    if (new_instance) {
      predict_engine_by_schema_id[engine_key] = new_instance;
      return new_instance;
    }
  }
  return nullptr;
}

PredictDb::PredictDb(const path& file_path) {
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::Status status = leveldb::DB::Open(options, file_path.string(), &db_);
  if (!status.ok()) {
    LOG(ERROR) << "failed to open leveldb database: " << file_path;
    db_ = nullptr;
  }
  Clear();
}

bool PredictDb::Lookup(const string& query) {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);  // 读锁
  std::vector<Prediction> predict;
  if (!LookupPredictions(query, &predict)) {
    return false;
  }
  Clear();
  for (const auto& entry : predict) {
    if (entry.commits < 0) {
      continue;
    }
    candidates_.push_back(entry.word);
  }
  return true;
}

bool PredictDb::Lookup(const string& query, vector<string>* candidates) const {
  if (!candidates) {
    return false;
  }
  candidates->clear();
  std::vector<Prediction> predict;
  if (!LookupPredictions(query, &predict)) {
    return false;
  }
  for (const auto& entry : predict) {
    if (entry.commits < 0) {
      continue;
    }
    candidates->push_back(entry.word);
  }
  return !candidates->empty();
}

bool PredictDb::LookupPredictions(const string& query,
                                  std::vector<Prediction>* predict) const {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);
  if (!db_ || !predict) {
    return false;
  }
  string value;
  leveldb::Status status = db_->Get(leveldb::ReadOptions(), query, &value);
  if (!status.ok() || !DecodePredictions(value, predict)) {
    return false;
  }
  SortPredictions(*predict);
  return true;
}

bool PredictDb::HasRecentPrediction(const string& query,
                                    int max_age_seconds) const {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);
  if (!db_) {
    return false;
  }
  string value;
  leveldb::Status status = db_->Get(leveldb::ReadOptions(), query, &value);
  if (!status.ok() || value.empty()) {
    return false;
  }
  std::vector<Prediction> predict;
  if (!DecodePredictions(value, &predict)) {
    return false;
  }
  if (predict.empty()) {
    return false;
  }
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  for (const auto& entry : predict) {
    if (entry.commits <= 0) {
      continue;
    }
    if (entry.tick < 1000000000 || entry.tick > now) {
      continue;
    }
    uint64_t age = now - entry.tick;
    if (age <= static_cast<uint64_t>(max_age_seconds)) {
      return true;
    }
  }
  return false;
}

void PredictDb::UpdatePredict(const string& key,
                              const string& word,
                              bool todelete) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);  // 写锁
  if (!db_) {
    return;
  }
  if (IsPunctOnly(key)) {
    return;
  }
  string value;
  leveldb::Status status = db_->Get(leveldb::ReadOptions(), key, &value);
  std::vector<Prediction> predict;

  uint64_t current_tick = static_cast<uint64_t>(std::time(nullptr));

  if (status.ok()) {
    if (!DecodePredictions(value, &predict)) {
      LOG(WARNING) << "failed to decode existing prediction list for key: "
                   << key << "; recreating it.";
      predict.clear();
    }

    bool found = false;
    double total_count = 0.0;

    for (const auto& entry : predict) {
      total_count += entry.count;
    }

    if (todelete) {
      // 标记为已删除：将 commits 设为负值（与 schema 级用户词行为一致）
      // 标记所有匹配的重复记录
      for (auto& entry : predict) {
        if (entry.word == word) {
          entry.commits = std::min(-1, -std::abs(entry.commits));
          entry.dee = 0.0;
          entry.tick = current_tick;
          found = true;
          // 不 break，继续标记所有重复的记录
        }
      }
    } else {
      for (auto& entry : predict) {
        if (entry.word == word) {
          // 如果是已删除的记录（commits < 0），恢复它
          if (entry.commits < 0) {
            entry.commits = -entry.commits;
          }
          entry.count += 1.0 / (total_count + 1.0);
          entry.commits += 1;
          entry.dee = entry.count;
          entry.tick = current_tick;
          found = true;
          break;
        }
      }
    }
    if (!found && !todelete) {
      predict.push_back({word, 1.0 / (total_count + 1.0), 1,
                         1.0 / (total_count + 1.0), current_tick});
    }
    SortPredictions(predict);

  } else {
    if (!todelete) {
      predict.push_back({word, 1.0, 1, 1.0, current_tick});
    }
  }

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, predict);

  status = db_->Put(leveldb::WriteOptions(), key,
                    leveldb::Slice(sbuf.data(), sbuf.size()));
  if (!status.ok()) {
    LOG(ERROR) << "Error updating or inserting prediction: "
               << status.ToString();
  }
}

bool PredictDb::Backup(const path& snapshot_file,
                       int deleted_record_expire_days) {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);  // 读锁
  LOG(INFO) << "backing up predict db to " << snapshot_file;
  std::ofstream out(snapshot_file.string());
  if (!out) {
    LOG(ERROR) << "failed to open backup file: " << snapshot_file;
    return false;
  }

  uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));
  WriteSnapshotHeader(db_, snapshot_file, "# Rime user dictionary", &out);

  // 用于计算已删除记录的过期时间
  const uint64_t expire_seconds =
      static_cast<uint64_t>(deleted_record_expire_days) * 24 * 3600;
  const bool enable_expire = (deleted_record_expire_days > 0);

  leveldb::Iterator* it = db_->NewIterator(leveldb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    string key = it->key().ToString();
    if (key.empty() || key[0] == '\x01' || key[0] == '/') {
      continue;
    }
    if (IsPunctOnly(key)) {
      continue;
    }
    // 排除场景和链式预测数据，只备份用户直接输入学习的数据
    if (key.find(kSceneKeyPrefix) == 0 || key.find(kChainKeyPrefix) == 0) {
      continue;
    }

    string value = it->value().ToString();
    std::vector<Prediction> predict;
    if (!DecodePredictions(value, &predict)) {
      continue;
    }
    for (const auto& p : predict) {
      // 如果启用过期清理，过滤掉已删除且超过指定天数的记录
      if (enable_expire && p.commits < 0 && p.tick > 0 &&
          current_time > p.tick && (current_time - p.tick) >= expire_seconds) {
        continue;
      }
      out << key << "\t" << p.word << "\tc=" << p.commits << " d=" << p.dee
          << " t=" << p.tick << "\n";
    }
  }
  delete it;
  out.close();
  return true;
}

bool PredictDb::BackupContext(const path& snapshot_file,
                              int deleted_record_expire_days) {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);  // 读锁
  LOG(INFO) << "backing up predict context db to " << snapshot_file;
  std::ofstream out(snapshot_file.string());
  if (!out) {
    LOG(ERROR) << "failed to open context backup file: " << snapshot_file;
    return false;
  }

  uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));
  WriteSnapshotHeader(db_, snapshot_file, "# Rime predict context dictionary",
                      &out);

  const uint64_t expire_seconds =
      static_cast<uint64_t>(deleted_record_expire_days) * 24 * 3600;
  const bool enable_expire = (deleted_record_expire_days > 0);

  leveldb::Iterator* it = db_->NewIterator(leveldb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    string key = it->key().ToString();
    ContextSnapshotEntry entry;
    if (!ParseContextSnapshotKey(key, &entry)) {
      continue;
    }
    if ((!entry.query.empty() && !IsContextSnapshotSafeText(entry.query)) ||
        !AreContextSnapshotSafeTexts(entry.context_chain)) {
      continue;
    }

    string value = it->value().ToString();
    std::vector<Prediction> predict;
    if (!DecodePredictions(value, &predict)) {
      continue;
    }
    for (const auto& p : predict) {
      if (enable_expire && p.commits < 0 && p.tick > 0 &&
          current_time > p.tick && (current_time - p.tick) >= expire_seconds) {
        continue;
      }
      if (!IsContextSnapshotSafeText(p.word)) {
        continue;
      }
      out << ContextSnapshotTypeName(entry.type);
      if (!entry.scene.empty()) {
        out << "\tscene=" << entry.scene;
      }
      if (!entry.query.empty()) {
        out << "\tquery=" << entry.query;
      }
      for (const auto& item : entry.context_chain) {
        out << "\tcontext_chain_item=" << item;
      }
      out << "\tword=" << p.word << "\tc=" << p.commits << " d=" << p.dee
          << " t=" << p.tick << "\n";
    }
  }
  delete it;
  out.close();
  return true;
}

bool PredictDb::Restore(const path& snapshot_file) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);  // 写锁
  LOG(INFO) << "restoring predict db from " << snapshot_file;
  std::ifstream in(snapshot_file.string());
  if (!in) {
    LOG(ERROR) << "failed to open restore file: " << snapshot_file;
    return false;
  }
  string line;
  std::getline(in, line);
  line = NormalizeSnapshotHeader(line);

  const bool is_legacy_export = (line == "Rime predict dictionary export");
  const bool is_rime_user_dict = (line == "# Rime user dictionary");
  if (!is_legacy_export && !is_rime_user_dict) {
    LOG(ERROR) << "invalid predict db backup file format";
    return false;
  }

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      continue;
    }

    size_t tab1 = line.find('\t');
    size_t tab2 = line.find('\t', tab1 == string::npos ? 0 : tab1 + 1);
    if (tab1 == string::npos || tab2 == string::npos)
      continue;

    string key = line.substr(0, tab1);
    string word = line.substr(tab1 + 1, tab2 - tab1 - 1);

    string metadata_str = line.substr(tab2 + 1);
    int commits = 0;
    double dee = 0.0;
    uint64_t tick = 0;
    double count = 0.0;
    if (!ParsePredictionMetadata(metadata_str, &commits, &dee, &tick, &count)) {
      continue;
    }
    UpsertRestoredPrediction(db_, key, word, count, commits, dee, tick);
  }
  in.close();
  return true;
}

bool PredictDb::RestoreContext(const path& snapshot_file) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);  // 写锁
  LOG(INFO) << "restoring predict context db from " << snapshot_file;
  std::ifstream in(snapshot_file.string());
  if (!in) {
    LOG(ERROR) << "failed to open context restore file: " << snapshot_file;
    return false;
  }
  string line;
  std::getline(in, line);
  line = NormalizeSnapshotHeader(line);
  if (line != "# Rime predict context dictionary") {
    LOG(ERROR) << "invalid predict context backup file format";
    return false;
  }

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }

    vector<string> fields;
    boost::split(fields, line, boost::is_any_of("\t"),
                 boost::token_compress_off);
    if (fields.empty()) {
      continue;
    }

    ContextSnapshotEntry entry;
    if (!ParseContextSnapshotType(fields.front(), &entry.type)) {
      continue;
    }
    for (size_t i = 1; i < fields.size(); ++i) {
      size_t eq = fields[i].find('=');
      if (eq == string::npos) {
        continue;
      }
      const string field = fields[i].substr(0, eq);
      const string value = fields[i].substr(eq + 1);
      try {
        if (field == "scene") {
          entry.scene = value;
        } else if (field == "query") {
          entry.query = value;
        } else if (field == "context_chain_item") {
          entry.context_chain.push_back(value);
        } else if (field == "word") {
          entry.word = value;
        } else if (field == "c") {
          entry.commits = std::stoi(value);
        } else if (field == "d") {
          entry.dee = std::stod(value);
        } else if (field == "t") {
          entry.tick = std::stoull(value);
        }
      } catch (...) {
        LOG(WARNING) << "failed parsing context snapshot field: " << fields[i];
      }
    }
    entry.count = entry.dee;
    if (!IsContextSnapshotSafeText(entry.word)) {
      continue;
    }

    string internal_key;
    switch (entry.type) {
      case ContextSnapshotType::kSceneQuery:
        if (entry.scene.empty() || !IsContextSnapshotSafeText(entry.query)) {
          continue;
        }
        internal_key =
            string(kSceneKeyPrefix) + entry.scene + "|" + entry.query;
        break;
      case ContextSnapshotType::kContextChain:
        if (entry.context_chain.empty() ||
            !AreContextSnapshotSafeTexts(entry.context_chain)) {
          continue;
        }
        internal_key = string(kChainKeyPrefix) +
                       boost::algorithm::join(entry.context_chain, "\n");
        break;
      case ContextSnapshotType::kSceneContextChain:
        if (entry.scene.empty() || entry.context_chain.empty() ||
            !AreContextSnapshotSafeTexts(entry.context_chain)) {
          continue;
        }
        internal_key = string(kSceneKeyPrefix) + entry.scene + "|" +
                       string(kChainKeyPrefix) +
                       boost::algorithm::join(entry.context_chain, "\n");
        break;
    }
    UpsertRestoredPrediction(db_, internal_key, entry.word, entry.count,
                             entry.commits, entry.dee, entry.tick);
  }
  in.close();
  return true;
}

}  // namespace rime
