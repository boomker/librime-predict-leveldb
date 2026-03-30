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
                             int max_context_commits)
    : level_db_(level_db),
      fallback_db_(fallback_db),
      rule_engine_(rule_engine),
      max_iterations_(max_iterations),
      min_candidates_(min_candidates),
      max_candidates_(max_candidates),
      deleted_record_expire_days_(deleted_record_expire_days),
      enable_rule_prediction_(enable_rule_prediction),
      enable_scene_learning_(enable_scene_learning),
      max_context_commits_(std::max(1, max_context_commits)) {}

PredictEngine::~PredictEngine() {}

bool PredictEngine::Predict(Context* ctx, const string& context_query) {
  if (!level_db_ && !fallback_db_ && !rule_engine_) {
    return false;
  }
  query_ = context_query;
  vector<string> merged;
  set<string> seen;

  if (level_db_) {
    for (const auto& key : BuildLookupKeys(ctx, context_query)) {
      vector<string> learned_candidates;
      if (level_db_->Lookup(key, &learned_candidates)) {
        AppendCandidates(learned_candidates, &merged, &seen);
      }
    }
  }

  if (enable_rule_prediction_ && rule_engine_) {
    AppendCandidates(rule_engine_->Match(context_query, DetectScene(ctx)),
                     &merged, &seen);
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
  vector<string> chain = {first.text, middle.text};
  const string chain_key = BuildChainKey(chain);
  level_db_->UpdatePredict(chain_key, word, todelete);
  level_db_->UpdatePredict(BuildSceneKey(scene, chain_key), word, todelete);
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
    vector<string> chain = {recent[recent.size() - 2], recent.back()};
    const string chain_key = BuildChainKey(chain);
    keys.push_back(BuildSceneKey(scene, chain_key));
    keys.push_back(chain_key);
  }
  keys.push_back(BuildSceneKey(scene, query));
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

string PredictEngine::BuildChainKey(const vector<string>& commits) const {
  return string(kChainKeyPrefix) + boost::algorithm::join(commits, "\n");
}

bool PredictEngine::IsContextualRecord(const CommitRecord& record) const {
  return record.type != "punct" && record.type != "raw" &&
         record.type != "thru" && !record.text.empty();
}

PredictEngineComponent::PredictEngineComponent() {}

PredictEngineComponent::~PredictEngineComponent() {}

PredictEngine* PredictEngineComponent::Create(const Ticket& ticket) {
  string level_db_name = "predict.userdb";
  string fallback_db_name = "predict.db";
  string rules_db_name = "predict_rules.db";
  string db_name;
  int min_candidates = 3;
  int max_candidates = 0;
  int max_iterations = 0;
  int deleted_record_expire_days = 0;
  int max_context_commits = 2;
  const bool enable_rule_prediction = true;
  const bool enable_scene_learning = true;
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
    config->GetInt("predictor/deleted_record_expire_days",
                   &deleted_record_expire_days);
    config->GetString("predictor/rules_db", &rules_db_name);
    config->GetInt("predictor/max_context_commits", &max_context_commits);
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
                             enable_scene_learning, max_context_commits);
  }
  if (fallback_db && fallback_db->valid()) {
    return new PredictEngine(level_db, fallback_db, rule_engine, max_iterations,
                             min_candidates, max_candidates,
                             deleted_record_expire_days, enable_rule_prediction,
                             enable_scene_learning, max_context_commits);
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
      for (auto& entry : predict) {
        if (entry.word == word) {
          entry.commits = std::min(-1, -std::abs(entry.commits));
          entry.dee = 0.0;
          entry.tick = current_tick;
          found = true;
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

  string db_name = snapshot_file.stem().u8string();
  string db_type = "userdb";
  string rime_version = RIME_VERSION;
  uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));
  string tick = std::to_string(current_time);  // 使用当前时间戳
  string user_id = snapshot_file.parent_path().filename().u8string();

  ReadDbTextValue(db_, "\x01/db_name", db_name, &db_name);
  ReadDbTextValue(db_, "\x01/db_type", db_type, &db_type);
  ReadDbTextValue(db_, "\x01/rime_version", rime_version, &rime_version);
  ReadDbTextValue(db_, "\x01/user_id", user_id, &user_id);

  out << "# Rime user dictionary\n";
  out << "#@/db_name\t" << db_name << "\n";
  out << "#@/db_type\t" << db_type << "\n";
  out << "#@/rime_version\t" << rime_version << "\n";
  out << "#@/tick\t" << tick << "\n";
  out << "#@/user_id\t" << user_id << "\n";

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

    if (metadata_str.find("c=") != string::npos) {
      vector<string> kv;
      boost::split(kv, metadata_str, boost::is_any_of(" "));
      for (const string& k_eq_v : kv) {
        size_t eq = k_eq_v.find('=');
        if (eq == string::npos)
          continue;
        string k(k_eq_v.substr(0, eq));
        string v(k_eq_v.substr(eq + 1));
        try {
          if (k == "c") {
            commits = std::stoi(v);
          } else if (k == "d") {
            dee = std::stod(v);
          } else if (k == "t") {
            tick = std::stoull(v);
          }
        } catch (...) {
          LOG(WARNING) << "failed parsing metadata in predict snapshot: "
                       << k_eq_v;
        }
      }
      count = dee;
    } else {
      try {
        count = std::stod(metadata_str);
        // 根据旧格式的权重值估算提交次数
        if (count < 1.0) {
          commits = 1;
        } else if (count < 1.5) {
          commits = 2;
        } else if (count <= 2.0) {
          commits = 3;
        } else {
          commits = static_cast<int>(std::ceil(count));
        }
        dee = count;
        tick = 1;
      } catch (const std::exception& ex) {
        LOG(WARNING) << "skipping malformed predict snapshot row in "
                     << snapshot_file << ": " << ex.what();
        continue;
      }
    }

    std::vector<Prediction> predict;
    string value;
    leveldb::Status status = db_->Get(leveldb::ReadOptions(), key, &value);
    if (status.ok() && !DecodePredictions(value, &predict)) {
      LOG(WARNING) << "failed to decode existing prediction list for key: "
                   << key << "; recreating it.";
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
    status = db_->Put(leveldb::WriteOptions(), key,
                      leveldb::Slice(sbuf.data(), sbuf.size()));
    if (!status.ok()) {
      LOG(ERROR) << "failed writing restored prediction for key '" << key
                 << "': " << status.ToString();
    }
  }
  in.close();
  return true;
}

}  // namespace rime
