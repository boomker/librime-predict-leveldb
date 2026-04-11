#include "rule_trigger_engine.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstdio>
#include <rime/config.h>
#include <rime/resource.h>
#include <rime/service.h>

namespace rime {

namespace {

constexpr int kTriggerRulesVersion = 3;

const vector<TriggerRule>& builtin_rules() {
  static const vector<TriggerRule> kRules = {
      // ========== Time-based rules ==========
      {"今天", 6, 12, -1, "", "早上", 10, false},
      {"今天", 6, 12, -1, "", "上午", 9, false},
      {"今天", 12, 14, -1, "", "中午", 10, false},
      {"今天", 12, 14, -1, "", "午饭", 9, false},
      {"今天", 14, 18, -1, "", "下午", 10, false},
      {"今天", 18, 24, -1, "", "晚上", 10, false},
      // ========== Holiday rules ==========
      {"今天是", -1, -1, -1, "chinese_solar", "清明节", 10, false},
      {"今天是", -1, -1, -1, "chinese_solar", "冬至", 10, false},
      {"今天是", -1, -1, -1, "holiday", "五一", 10, false},
      {"今天是", -1, -1, 0, "weekday_0", "星期天", 9, false},
      {"今天是", -1, -1, 6, "weekday_6", "星期六", 9, false},
      // ========== Scene: Office ==========
      {"请查收", -1, -1, -1, "scene_office", "附件", 10, false},
      {"请查收", -1, -1, -1, "scene_office", "谢谢", 9, false},
      {"麻烦你", -1, -1, -1, "scene_office", "确认一下", 10, false},
      {"麻烦你", -1, -1, -1, "scene_office", "尽快处理", 9, false},
      // ========== Scene: Chat ==========
      {"哈哈", -1, -1, -1, "scene_chat", "确实", 10, false},
      {"哈哈", -1, -1, -1, "scene_chat", "太有意思了", 9, false},
      {"晚安", -1, -1, -1, "scene_chat", "好梦", 10, false},
      {"晚安", -1, -1, -1, "scene_chat", "早点休息", 9, false},
      // ========== Scene: Programming ==========
      {"这个函数", -1, -1, -1, "scene_programming", "返回值", 9, false},
      {"这个函数", -1, -1, -1, "scene_programming", "需要重构", 10, false},
      {"这个接口", -1, -1, -1, "scene_programming", "需要联调", 10, false},
      {"这个接口", -1, -1, -1, "scene_programming", "返回的数据", 9, false},
      // ========== Sentence Pattern: 希望你 X ==========
      {"希望你", -1, -1, -1, "", "一切顺利", 10, false},
      {"希望你", -1, -1, -1, "", "身体健康", 9, false},
      {"希望你", -1, -1, -1, "", "开心快乐", 8, false},
      {"希望你", -1, -1, -1, "", "早日康复", 7, false},
      // ========== Sentence Pattern: 不好意思 X ==========
      {"不好意思", -1, -1, -1, "", "打扰了", 10, false},
      {"不好意思", -1, -1, -1, "", "麻烦你", 9, false},
      {"不好意思", -1, -1, -1, "", "迟到了", 8, false},
      {"不好意思", -1, -1, -1, "", "占用了您的时间", 7, false},
      // ========== Sentence Pattern: 请问 X ==========
      {"请问", -1, -1, -1, "", "一下", 10, false},
      {"请问", -1, -1, -1, "", "能否", 9, false},
      {"请问", -1, -1, -1, "", "是否", 8, false},
      {"请问", -1, -1, -1, "", "贵姓", 7, false},
      // ========== Sentence Pattern: 需要 X ==========
      {"需要", -1, -1, -1, "", "帮忙", 10, false},
      {"需要", -1, -1, -1, "", "确认", 9, false},
      {"需要", -1, -1, -1, "", "提交", 8, false},
      {"需要", -1, -1, -1, "", "审批", 7, false},
      // ========== Semantic: Person/Relation ==========
      {"妈妈", -1, -1, -1, "", "节日快乐", 9, false},
      {"妈妈", -1, -1, -1, "", "身体健康", 8, false},
      {"爸爸", -1, -1, -1, "", "辛苦了", 10, false},
      {"爸爸", -1, -1, -1, "", "注意身体", 9, false},
      {"老师", -1, -1, -1, "", "您辛苦了", 10, false},
      // ========== Semantic: Place/Travel ==========
      {"北京", -1, -1, -1, "", "出差", 10, false},
      {"北京", -1, -1, -1, "", "旅游", 9, false},
      // ========== Semantic: Number + Measure Word ==========
      {"一个", -1, -1, -1, "", "月", 10, false},
      {"三个", -1, -1, -1, "", "人", 8, false},
      {"三个", -1, -1, -1, "", "小时", 10, false},
      {"一个", -1, -1, -1, "", "星期", 9, false},
      {"三个", -1, -1, -1, "", "方案", 7, false},
      {"五个", -1, -1, -1, "", "工作日", 10, false},
      // ========== Dialogue Structure: Question Endings ==========
      // {"吗", -1, -1, -1, "", "是的", 10, false},
      // {"吗", -1, -1, -1, "", "对", 9, false},
      // {"吗", -1, -1, -1, "", "没有", 8, false},
      // {"吗", -1, -1, -1, "", "不一定", 7, false},
      // {"吗", -1, -1, -1, "", "可以", 6, false},
      // {"呢", -1, -1, -1, "", "是的", 10, false},
      // {"呢", -1, -1, -1, "", "还好", 9, false},
      // {"呢", -1, -1, -1, "", "在呢", 8, false},
      // {"吧", -1, -1, -1, "", "应该是吧", 10, false},
      // {"吧", -1, -1, -1, "", "可能吧", 9, false},
      // ========== Dialogue Structure: Exclamation/Complaint ==========
      {"真的", -1, -1, -1, "", "吗", 10, false},
      {"真的", -1, -1, -1, "", "啊", 9, false},
      {"太", -1, -1, -1, "", "夸张了", 9, false},
      {"好烦", -1, -1, -1, "", "啊", 10, false},
      {"好烦", -1, -1, -1, "", "呀", 9, false},
      // ========== Dialogue Structure: List Patterns ==========
      // {"第一", -1, -1, -1, "", "第二", 10, false},
      // {"第一", -1, -1, -1, "", "其次", 9, false},
      // {"首先", -1, -1, -1, "", "其次", 10, false},
      // {"首先", -1, -1, -1, "", "再次", 9, false},
      // {"一是", -1, -1, -1, "", "二是", 10, false},
      // {"一是", -1, -1, -1, "", "另外", 9, false},
      // ========== Dialogue Structure: Transition Words ==========
      // {"但是", -1, -1, -1, "", "还是", 10, false},
      // {"但是", -1, -1, -1, "", "仍然", 9, false},
      // {"不过", -1, -1, -1, "", "还是", 10, false},
      // {"不过", -1, -1, -1, "", "然而", 9, false},
      // {"然而", -1, -1, -1, "", "依然", 10, false},
      // {"虽然", -1, -1, -1, "", "但是", 10, false},
      // ========== Geographic/Weather Context ==========
      {"天气", -1, -1, -1, "", "晴", 10, false},
      {"天气", -1, -1, -1, "", "很好", 9, false},
      {"天气", -1, -1, -1, "", "很糟糕", 8, false},
      {"温度", -1, -1, -1, "", "太高了", 9, false},
      {"出门", -1, -1, -1, "", "要带伞", 10, false},
      {"出门", -1, -1, -1, "", "穿外套", 9, false},
      // ========== Format: Date Patterns ==========
      // {"月", -1, -1, -1, "", "日", 10, false},
      // {"月", -1, -1, -1, "", "号", 9, false},
      // {"第", -1, -1, -1, "", "章", 10, false},
      // {"第", -1, -1, -1, "", "节", 9, false},
      // {"第", -1, -1, -1, "", "条", 8, false},
      // {"第", -1, -1, -1, "", "页", 7, false},
      // ========== More Sentence Patterns ==========
      {"谢谢", -1, -1, -1, "", "辛苦了", 8, false},
      {"好的", -1, -1, -1, "", "没问题", 9, false},
      {"好的", -1, -1, -1, "", "收到", 8, false},
      {"没问题", -1, -1, -1, "", "好的", 9, false},
      // ========== More Scene-based Rules ==========
      // {"早上好", -1, -1, -1, "scene_chat", "早上好", 10, false},
      // {"早上好", -1, -1, -1, "scene_office", "早", 10, false},
      // {"明天见", -1, -1, -1, "scene_chat", "明天见", 10, false},
      // {"明天见", -1, -1, -1, "scene_office", "好的", 9, false},
      // {"收到", -1, -1, -1, "scene_office", "好的", 10, false},
      // {"收到", -1, -1, -1, "scene_office", "明白", 9, false},
  };
  return kRules;
}

const map<string, vector<string>>& builtin_holidays() {
  static const map<string, vector<string>> kHolidays = {
      // ========== Chinese Holidays ==========
      {"2026-01-01", {"元旦", "新年"}},
      {"2026-02-16", {"除夕", "春节"}},
      {"2026-02-17", {"春节", "新年", "假期"}},
      {"2026-02-18", {"春节", "新年", "假期"}},
      {"2026-04-04", {"清明节", "清明", "假期"}},
      {"2026-04-05", {"清明节", "清明", "假期"}},
      {"2026-04-06", {"清明节", "清明", "假期"}},
      {"2026-05-01", {"劳动节", "五一", "假期"}},
      {"2026-05-02", {"劳动节", "五一", "假期"}},
      {"2026-05-03", {"劳动节", "五一", "假期"}},
      {"2026-05-04", {"劳动节", "五一", "假期"}},
      {"2026-05-05", {"劳动节", "五一", "假期"}},
      {"2026-06-19", {"端午节", "端午"}},
      {"2026-09-25", {"中秋节", "中秋", "团圆"}},
      {"2026-10-01", {"国庆节", "十一", "假期"}},
      {"2026-10-02", {"国庆节", "十一", "假期"}},
      {"2026-10-03", {"国庆节", "十一", "假期"}},
      {"2026-10-04", {"国庆节", "十一", "假期"}},
      {"2026-10-05", {"国庆节", "十一", "假期"}},
      {"2026-10-06", {"国庆节", "十一", "假期"}},
      {"2026-10-07", {"国庆节", "十一", "假期"}},
      // ========== International Holidays ==========
      // Valentine's Day / 情人节 (Feb 14)
      {"2026-02-14", {"情人节", "爱", "浪漫"}},
      // April Fool's Day / 愚人节 (Apr 1)
      {"2026-04-01", {"愚人节", "玩笑", "整蛊"}},
      // Easter / 复活节 (varies, approximate Apr 5-11, 2026)
      {"2026-04-05", {"复活节", " Easter"}},
      {"2026-04-06", {"复活节", " Easter"}},
      // Mother's Day / 母亲节 (May 10, second Sunday of May)
      {"2026-05-10", {"母亲节", "妈妈", "感恩"}},
      // Father's Day / 父亲节 (Jun 21, third Sunday of June)
      {"2026-06-21", {"父亲节", "爸爸", "感恩"}},
      // Halloween / 万圣节 (Oct 31)
      {"2026-10-31", {"万圣节", "不给糖就捣蛋", "鬼节"}},
      // Thanksgiving / 感恩节 (Nov 26, fourth Thursday of November)
      {"2026-11-26", {"感恩节", "火鸡", "团圆"}},
      // Christmas / 圣诞节 (Dec 25)
      {"2026-12-25", {"圣诞节", "圣诞", "圣诞快乐"}},
      // New Year's Eve / 跨年夜 (Dec 31)
      {"2026-12-31", {"跨年夜", "新年", "除夕"}},
  };
  return kHolidays;
}

const map<string, string>& builtin_solar_terms() {
  static const map<string, string> kSolarTerms = {
      {"02-04", "立春"}, {"02-19", "雨水"}, {"03-06", "惊蛰"},
      {"03-20", "春分"}, {"04-04", "清明"}, {"04-05", "清明"},
      {"04-20", "谷雨"}, {"05-06", "立夏"}, {"05-21", "小满"},
      {"06-06", "芒种"}, {"06-21", "夏至"}, {"07-07", "小暑"},
      {"07-23", "大暑"}, {"08-07", "立秋"}, {"08-23", "处暑"},
      {"09-08", "白露"}, {"09-23", "秋分"}, {"10-08", "寒露"},
      {"10-23", "霜降"}, {"11-07", "立冬"}, {"11-22", "小雪"},
      {"12-07", "大雪"}, {"12-22", "冬至"},
  };
  return kSolarTerms;
}

string ColumnText(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  return text ? reinterpret_cast<const char*>(text) : string();
}

int ReadVersion(sqlite3* db, const char* key) {
  sqlite3_stmt* stmt = nullptr;
  std::string sql =
      std::string("SELECT value FROM meta WHERE key='") + key + "'";
  int version = 0;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      version = std::atoi(ColumnText(stmt, 0).c_str());
    }
  }
  sqlite3_finalize(stmt);
  return version;
}

int ReadInt(const an<ConfigMap>& map, const string& key, int default_value) {
  if (!map)
    return default_value;
  auto item = map->GetValue(key);
  if (!item)
    return default_value;
  int value = default_value;
  return item->GetInt(&value) ? value : default_value;
}

string ReadString(const an<ConfigMap>& map, const string& key) {
  if (!map)
    return string();
  auto item = map->GetValue(key);
  return item ? item->str() : string();
}

string ReadString(const an<ConfigMap>& map,
                  const string& key,
                  const string& default_value) {
  if (!map)
    return default_value;
  auto item = map->GetValue(key);
  return item ? item->str() : default_value;
}

vector<string> ReadCandidateList(const an<ConfigMap>& map) {
  vector<string> candidates;
  if (!map)
    return candidates;
  if (auto value = map->GetValue("candidate")) {
    if (!value->str().empty()) {
      candidates.push_back(value->str());
    }
  }
  auto list = As<ConfigList>(map->Get("candidates"));
  if (!list)
    return candidates;
  for (size_t i = 0; i < list->size(); ++i) {
    auto value = list->GetValueAt(i);
    if (value && !value->str().empty()) {
      candidates.push_back(value->str());
    }
  }
  return candidates;
}

}  // namespace

RuleTriggerEngine::RuleTriggerEngine() {
  LoadBuiltinCalendarDefaults();
  RegisterTemplates();
}

bool RuleTriggerEngine::LoadCalendar(const path& calendar_path) {
  Config config;
  if (!config.LoadFromFile(calendar_path)) {
    return false;
  }

  if (auto solar_terms = config.GetMap("solar_terms")) {
    for (auto it = solar_terms->begin(); it != solar_terms->end(); ++it) {
      if (auto value = As<ConfigValue>(it->second)) {
        solar_terms_[it->first] = value->str();
      }
    }
  }

  if (auto holidays = config.GetMap("holidays")) {
    for (auto it = holidays->begin(); it != holidays->end(); ++it) {
      vector<string> tags;
      auto list = As<ConfigList>(it->second);
      if (!list)
        continue;
      for (size_t i = 0; i < list->size(); ++i) {
        if (auto value = list->GetValueAt(i)) {
          tags.push_back(value->str());
        }
      }
      holidays_[it->first] = std::move(tags);
    }
  }
  return true;
}

bool RuleTriggerEngine::LoadFromDB(const path& db_path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return false;
  }
  InitSchema(db);
  SeedBuiltinRules(db);
  SeedBuiltinHolidays(db);
  ReloadRules(db);
  LoadHolidaysFromDB(db);
  sqlite3_close(db);
  return true;
}

void RuleTriggerEngine::LoadRuleList(const an<ConfigList>& rule_list,
                                     const path& rules_root) {
  if (!rule_list)
    return;

  for (size_t i = 0; i < rule_list->size(); ++i) {
    // 检查是否为外部文件引用（字符串条目，如 "my_rules.yaml"）
    if (auto value = rule_list->GetValueAt(i)) {
      const string& file_name = value->str();
      if (!file_name.empty()) {
        path file_path = rules_root / file_name;
        Config file_config;
        if (!file_config.LoadFromFile(file_path)) {
          LOG(WARNING) << "predict: failed to load trigger rules file: "
                       << file_path;
          continue;
        }
        // 外部规则文件支持两种格式：
        // 1. patch: rules/+: 列表（推荐）
        // 2. 顶层 predict_trigger_rules 列表（旧格式，向后兼容）
        an<ConfigList> nested_list;
        if (auto patch_map = file_config.GetMap("patch")) {
          // key 字面含斜杠，直接用 ConfigMap::Get 不做路径分割
          nested_list = As<ConfigList>(patch_map->Get("rules/+"));
        }
        if (!nested_list) {
          nested_list = file_config.GetList("predict_trigger_rules");
        }
        if (nested_list) {
          LoadRuleList(nested_list, file_path.parent_path());
        }
      }
      continue;
    }

    auto rule_map = As<ConfigMap>(rule_list->GetAt(i));
    if (!rule_map)
      continue;

    // 检查是否为模板规则
    if (rule_map->Get("template")) {
      auto expanded = ExpandTemplate(rule_map);
      rules_.insert(rules_.end(), expanded.begin(), expanded.end());
      continue;
    }

    // 普通规则处理（原有逻辑）
    string trigger = ReadString(rule_map, "trigger");
    if (trigger.empty())
      continue;
    vector<string> candidates = ReadCandidateList(rule_map);
    if (candidates.empty())
      continue;

    int hour_min = ReadInt(rule_map, "hour_min", -1);
    int hour_max = ReadInt(rule_map, "hour_max", -1);
    int weekday = ReadInt(rule_map, "weekday", -1);
    int priority = ReadInt(rule_map, "priority", 100);
    string tag = ReadString(rule_map, "tag");

    string match_type_str = ReadString(rule_map, "match_type");
    MatchType match_type = MatchType::Exact;
    if (match_type_str == "prefix") {
      match_type = MatchType::Prefix;
    } else if (match_type_str == "suffix") {
      match_type = MatchType::Suffix;
    } else if (match_type_str == "contains") {
      match_type = MatchType::Contains;
    }

    vector<string> scenes;
    auto scenes_list = As<ConfigList>(rule_map->Get("scenes"));
    if (scenes_list) {
      for (size_t j = 0; j < scenes_list->size(); ++j) {
        auto value = scenes_list->GetValueAt(j);
        if (value && !value->str().empty()) {
          scenes.push_back(value->str());
        }
      }
    }

    string month_day_start = ReadString(rule_map, "month_day_start");
    string month_day_end = ReadString(rule_map, "month_day_end");

    for (const auto& candidate : candidates) {
      TriggerRule rule;
      rule.trigger = trigger;
      rule.hour_min = hour_min;
      rule.hour_max = hour_max;
      rule.weekday = weekday;
      rule.tag = tag;
      rule.candidate = candidate;
      rule.priority = priority--;
      rule.is_user = true;
      rule.match_type = match_type;
      rule.scenes = scenes;
      rule.month_day_start = month_day_start;
      rule.month_day_end = month_day_end;
      rules_.push_back(std::move(rule));
    }
  }
}

void RuleTriggerEngine::LoadFromConfig(Config* config) {
  if (!config)
    return;

  path rules_root;
  the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
      {"userdb", "", ""}));
  rules_root = resolver->ResolvePath("");

  // Try production path first: predictor/predict_trigger_rules (ConfigMap)
  auto trigger_map = config->GetMap("predictor/predict_trigger_rules");
  if (!trigger_map) {
    // Fallback 1: top-level predict_trigger_rules as ConfigMap
    trigger_map = config->GetMap("predict_trigger_rules");
  }

  if (trigger_map) {
    // calendar_data: 日历数据文件（节气/节假日）
    if (auto cal_value = As<ConfigValue>(trigger_map->Get("calendar_data"))) {
      const string& cal_file = cal_value->str();
      if (!cal_file.empty()) {
        LoadCalendar(rules_root / cal_file);
      }
    }

    // rules: 规则列表（字符串文件引用 或 内联规则 map）
    auto rule_list = As<ConfigList>(trigger_map->Get("rules"));
    if (rule_list) {
      LoadRuleList(rule_list, rules_root);
    }
  } else {
    // Fallback 2: top-level predict_trigger_rules as ConfigList directly
    // (used in unit tests that set rules inline without a wrapping map)
    auto rule_list = config->GetList("predict_trigger_rules");
    if (rule_list) {
      LoadRuleList(rule_list, rules_root);
    }
  }

  std::stable_sort(rules_.begin(), rules_.end(),
                   [](const TriggerRule& lhs, const TriggerRule& rhs) {
                     if (lhs.is_user != rhs.is_user)
                       return lhs.is_user > rhs.is_user;
                     return lhs.priority > rhs.priority;
                   });
}

vector<string> RuleTriggerEngine::Match(const string& query,
                                        const string& scene,
                                        bool user_only) const {
  if (query.empty() || rules_.empty()) {
    return {};
  }

  std::time_t now_time = std::time(nullptr);
  std::tm now = *std::localtime(&now_time);
  set<string> tags = GetTodayTags(scene, now);
  vector<string> results;
  set<string> seen;

  // First pass: collect all matching candidates
  vector<string> all_candidates;
  for (const auto& rule : rules_) {
    if (user_only && !rule.is_user)
      continue;
    if (!MatchRule(rule, query, scene, tags, now))
      continue;
    if (seen.insert(rule.candidate).second) {
      all_candidates.push_back(rule.candidate);
    }
  }

  // Second pass: if any candidate equals the query, return only those
  for (const auto& candidate : all_candidates) {
    if (candidate == query) {
      results.push_back(candidate);
    }
  }

  // If no exact match, return all candidates
  if (results.empty()) {
    results = std::move(all_candidates);
  }

  return results;
}

void RuleTriggerEngine::LoadBuiltinCalendarDefaults() {
  solar_terms_ = builtin_solar_terms();
  holidays_.clear();
}

void RuleTriggerEngine::InitSchema(sqlite3* db) {
  static const char* kSql =
      "CREATE TABLE IF NOT EXISTS trigger_rules ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "trigger TEXT NOT NULL,"
      "hour_min INTEGER NOT NULL DEFAULT -1,"
      "hour_max INTEGER NOT NULL DEFAULT -1,"
      "weekday INTEGER NOT NULL DEFAULT -1,"
      "tag TEXT NOT NULL DEFAULT '',"
      "candidate TEXT NOT NULL,"
      "priority INTEGER NOT NULL DEFAULT 0,"
      "is_user INTEGER NOT NULL DEFAULT 0);"
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_trigger_rules_uniq "
      "ON trigger_rules(trigger, hour_min, hour_max, weekday, tag, candidate);"
      "CREATE TABLE IF NOT EXISTS holidays ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "date TEXT NOT NULL,"
      "tag TEXT NOT NULL,"
      "UNIQUE(date, tag));"
      "CREATE TABLE IF NOT EXISTS meta ("
      "key TEXT PRIMARY KEY,"
      "value TEXT NOT NULL);"
      "INSERT OR IGNORE INTO meta(key, value) VALUES('version', '0');"
      "INSERT OR IGNORE INTO meta(key, value) VALUES('holidays_version', '0');";
  sqlite3_exec(db, kSql, nullptr, nullptr, nullptr);
}

void RuleTriggerEngine::SeedBuiltinRules(sqlite3* db) {
  const int current_version = ReadVersion(db, "version");
  if (current_version >= kTriggerRulesVersion) {
    return;
  }
  sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

  if (current_version < 3) {
    sqlite3_exec(db,
                 "DELETE FROM trigger_rules "
                 "WHERE trigger='这个接口' "
                 "AND hour_min=-1 "
                 "AND hour_max=-1 "
                 "AND weekday=-1 "
                 "AND tag='scene_programming' "
                 "AND candidate='返回数据' "
                 "AND is_user=0",
                 nullptr, nullptr, nullptr);
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "INSERT OR IGNORE INTO trigger_rules "
          "(trigger, hour_min, hour_max, weekday, tag, candidate, priority, "
          "is_user) VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
          -1, &stmt, nullptr) == SQLITE_OK) {
    for (const auto& rule : builtin_rules()) {
      sqlite3_bind_text(stmt, 1, rule.trigger.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, rule.hour_min);
      sqlite3_bind_int(stmt, 3, rule.hour_max);
      sqlite3_bind_int(stmt, 4, rule.weekday);
      sqlite3_bind_text(stmt, 5, rule.tag.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6, rule.candidate.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 7, rule.priority);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }
  }
  sqlite3_finalize(stmt);

  stmt = nullptr;
  if (sqlite3_prepare_v2(db, "UPDATE meta SET value=? WHERE key='version'", -1,
                         &stmt, nullptr) == SQLITE_OK) {
    string version = std::to_string(kTriggerRulesVersion);
    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

void RuleTriggerEngine::SeedBuiltinHolidays(sqlite3* db) {
  constexpr int kHolidaysVersion = 2;
  if (ReadVersion(db, "holidays_version") >= kHolidaysVersion) {
    return;
  }
  sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db, "INSERT OR IGNORE INTO holidays (date, tag) VALUES (?, ?)", -1,
          &stmt, nullptr) == SQLITE_OK) {
    for (const auto& holiday : builtin_holidays()) {
      for (const auto& tag : holiday.second) {
        sqlite3_bind_text(stmt, 1, holiday.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
      }
    }
  }
  sqlite3_finalize(stmt);

  stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "UPDATE meta SET value=? WHERE key='holidays_version'",
                         -1, &stmt, nullptr) == SQLITE_OK) {
    string version = std::to_string(kHolidaysVersion);
    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

void RuleTriggerEngine::LoadHolidaysFromDB(sqlite3* db) {
  holidays_.clear();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT date, tag FROM holidays ORDER BY date", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    string date = ColumnText(stmt, 0);
    string tag = ColumnText(stmt, 1);
    holidays_[date].push_back(tag);
  }
  sqlite3_finalize(stmt);
}

void RuleTriggerEngine::ReloadRules(sqlite3* db) {
  rules_.clear();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT trigger, hour_min, hour_max, weekday, tag, candidate, "
          "priority, is_user "
          "FROM trigger_rules ORDER BY is_user DESC, priority DESC",
          -1, &stmt, nullptr) != SQLITE_OK) {
    return;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TriggerRule rule;
    rule.trigger = ColumnText(stmt, 0);
    rule.hour_min = sqlite3_column_int(stmt, 1);
    rule.hour_max = sqlite3_column_int(stmt, 2);
    rule.weekday = sqlite3_column_int(stmt, 3);
    rule.tag = ColumnText(stmt, 4);
    rule.candidate = ColumnText(stmt, 5);
    rule.priority = sqlite3_column_int(stmt, 6);
    rule.is_user = sqlite3_column_int(stmt, 7) != 0;
    rules_.push_back(std::move(rule));
  }
  sqlite3_finalize(stmt);
}

set<string> RuleTriggerEngine::GetTodayTags(const string& scene,
                                            const std::tm& now) const {
  set<string> tags;
  tags.insert("weekday_" + std::to_string(now.tm_wday));
  tags.insert("scene_" + (scene.empty() ? string("general") : scene));

  char month_day[6] = {0};
  std::snprintf(month_day, sizeof(month_day), "%02d-%02d", now.tm_mon + 1,
                now.tm_mday);
  auto solar_term = solar_terms_.find(month_day);
  if (solar_term != solar_terms_.end()) {
    tags.insert(solar_term->second);
    tags.insert("chinese_solar");
  }

  char year_month_day[11] = {0};
  std::snprintf(year_month_day, sizeof(year_month_day), "%04d-%02d-%02d",
                now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
  auto holiday = holidays_.find(year_month_day);
  if (holiday != holidays_.end()) {
    for (const auto& tag : holiday->second) {
      tags.insert(tag);
    }
    tags.insert("holiday");
  }
  return tags;
}

// 解析 "MM-DD" 格式日期字符串，返回 MMDD 整数，如 "01-15" -> 115
// 返回 -1 表示解析失败
int ParseMonthDay(const string& month_day) {
  if (month_day.empty()) {
    return -1;
  }
  size_t dash_pos = month_day.find('-');
  if (dash_pos == string::npos) {
    dash_pos = month_day.find('/');
  }
  if (dash_pos == string::npos) {
    return -1;
  }
  try {
    int month = std::stoi(month_day.substr(0, dash_pos));
    int day = std::stoi(month_day.substr(dash_pos + 1));
    if (month < 1 || month > 12 || day < 1 || day > 31) {
      return -1;
    }
    return month * 100 + day;
  } catch (...) {
    return -1;
  }
}

bool RuleTriggerEngine::MatchRule(const TriggerRule& rule,
                                  const string& query,
                                  const string& scene,
                                  const set<string>& tags,
                                  const std::tm& now) const {
  // 1. trigger + match_type 检查 (最基础的字符串匹配)
  bool match_trigger = false;
  switch (rule.match_type) {
    case MatchType::Exact:
      match_trigger = (rule.trigger == query);
      break;
    case MatchType::Prefix:
      match_trigger =
          (query.size() >= rule.trigger.size() &&
           query.compare(0, rule.trigger.size(), rule.trigger) == 0);
      break;
    case MatchType::Suffix:
      match_trigger = (query.size() >= rule.trigger.size() &&
                       query.compare(query.size() - rule.trigger.size(),
                                     rule.trigger.size(), rule.trigger) == 0);
      break;
    case MatchType::Contains:
      match_trigger = (query.find(rule.trigger) != string::npos);
      break;
  }
  if (!match_trigger) {
    return false;
  }

  // 2. weekday 检查
  if (rule.weekday >= 0 && rule.weekday != now.tm_wday) {
    return false;
  }

  // 3. hour range 检查
  if (rule.hour_min >= 0 && now.tm_hour < rule.hour_min) {
    return false;
  }
  if (rule.hour_max >= 0 && now.tm_hour >= rule.hour_max) {
    return false;
  }

  // 4. month-day range 检查 (支持跨年范围)
  if (!rule.month_day_start.empty() && !rule.month_day_end.empty()) {
    int current_md = (now.tm_mon + 1) * 100 + now.tm_mday;
    int start_md = ParseMonthDay(rule.month_day_start);
    int end_md = ParseMonthDay(rule.month_day_end);

    if (start_md > 0 && end_md > 0) {
      bool in_range;
      if (start_md <= end_md) {
        // 正常范围，如 01-01 到 12-31
        in_range = (current_md >= start_md && current_md <= end_md);
      } else {
        // 跨年范围，如 12-01 到 02-05
        in_range = (current_md >= start_md || current_md <= end_md);
      }
      if (!in_range) {
        return false;
      }
    }
  }

  // 5. scene allowlist 检查 (scene 是一级概念，直接判断)
  if (!rule.scenes.empty()) {
    bool scene_matched = false;
    for (const auto& allowed : rule.scenes) {
      if (scene == allowed) {
        scene_matched = true;
        break;
      }
    }
    if (!scene_matched) {
      return false;
    }
  }

  // 6. tag 检查
  if (!rule.tag.empty() && tags.find(rule.tag) == tags.end()) {
    return false;
  }

  return true;
}

// ========== 模板系统实现 ==========

void RuleTriggerEngine::RegisterTemplates() {
  template_registry_["time_greeting"] = ExpandTimeGreeting;
  template_registry_["holiday_greeting"] = ExpandHolidayGreeting;
  template_registry_["weekday_reminder"] = ExpandWeekdayReminder;
}

vector<TriggerRule> RuleTriggerEngine::ExpandTemplate(
    const an<ConfigMap>& template_config) const {
  string template_name = ReadString(template_config, "template");
  if (template_name.empty()) {
    LOG(WARNING) << "Template config missing 'template' field, skipping";
    return {};
  }

  auto it = template_registry_.find(template_name);
  if (it == template_registry_.end()) {
    LOG(WARNING) << "Unknown template: " << template_name << ", skipping";
    return {};
  }

  int base_priority = ReadInt(template_config, "base_priority", 100);
  return it->second(template_config, base_priority);
}

vector<TriggerRule> RuleTriggerEngine::ExpandTimeGreeting(
    const an<ConfigMap>& config,
    int base_priority) {
  vector<TriggerRule> rules;
  auto items = As<ConfigList>(config->Get("items"));
  if (!items) {
    LOG(WARNING) << "time_greeting template missing 'items' field, skipping";
    return rules;
  }

  int current_priority = base_priority;
  for (size_t i = 0; i < items->size(); ++i) {
    auto item = As<ConfigMap>(items->GetAt(i));
    if (!item)
      continue;

    string trigger = ReadString(item, "trigger");
    int hour_min = ReadInt(item, "hour_min", -1);
    int hour_max = ReadInt(item, "hour_max", -1);
    vector<string> candidates = ReadCandidateList(item);

    if (trigger.empty() || candidates.empty()) {
      LOG(WARNING) << "time_greeting item missing trigger or candidates, "
                      "skipping";
      continue;
    }

    // 验证时间范围
    if (hour_min >= 0 && hour_max >= 0 && hour_min >= hour_max) {
      LOG(WARNING) << "time_greeting item has invalid hour range ("
                   << hour_min << " >= " << hour_max << "), skipping";
      continue;
    }

    for (const auto& candidate : candidates) {
      TriggerRule rule;
      rule.trigger = trigger;
      rule.hour_min = hour_min;
      rule.hour_max = hour_max;
      rule.candidate = candidate;
      rule.priority = current_priority--;
      rule.is_user = true;
      rules.push_back(std::move(rule));
    }
  }

  return rules;
}

vector<TriggerRule> RuleTriggerEngine::ExpandHolidayGreeting(
    const an<ConfigMap>& config,
    int base_priority) {
  vector<TriggerRule> rules;
  auto holidays = As<ConfigList>(config->Get("holidays"));
  if (!holidays) {
    LOG(WARNING)
        << "holiday_greeting template missing 'holidays' field, skipping";
    return rules;
  }

  string candidate_template =
      ReadString(config, "candidate_template", "{holiday}快乐");
  string trigger_field = ReadString(config, "trigger", "");

  int current_priority = base_priority;
  for (size_t i = 0; i < holidays->size(); ++i) {
    auto value = holidays->GetValueAt(i);
    if (!value || value->str().empty())
      continue;

    string holiday = value->str();
    string trigger = trigger_field.empty() ? holiday : trigger_field;
    string candidate = candidate_template;

    // 简单的模板替换：{holiday} -> 实际节日名
    size_t pos = candidate.find("{holiday}");
    if (pos != string::npos) {
      candidate.replace(pos, 9, holiday);
    }

    TriggerRule rule;
    rule.trigger = trigger;
    rule.candidate = candidate;
    rule.priority = current_priority--;
    rule.is_user = true;
    rules.push_back(std::move(rule));
  }

  return rules;
}

vector<TriggerRule> RuleTriggerEngine::ExpandWeekdayReminder(
    const an<ConfigMap>& config,
    int base_priority) {
  vector<TriggerRule> rules;
  auto items = As<ConfigList>(config->Get("items"));
  if (!items) {
    LOG(WARNING)
        << "weekday_reminder template missing 'items' field, skipping";
    return rules;
  }

  int current_priority = base_priority;
  for (size_t i = 0; i < items->size(); ++i) {
    auto item = As<ConfigMap>(items->GetAt(i));
    if (!item)
      continue;

    string trigger = ReadString(item, "trigger");
    int weekday = ReadInt(item, "weekday", -1);
    vector<string> candidates = ReadCandidateList(item);

    if (trigger.empty() || candidates.empty()) {
      LOG(WARNING) << "weekday_reminder item missing required fields, "
                      "skipping";
      continue;
    }

    // 验证 weekday 范围（-1 表示不限制，0-6 表示周日到周六）
    if (weekday < -1 || weekday > 6) {
      LOG(WARNING) << "weekday_reminder item has invalid weekday (" << weekday
                   << "), skipping";
      continue;
    }

    for (const auto& candidate : candidates) {
      TriggerRule rule;
      rule.trigger = trigger;
      rule.weekday = weekday;
      rule.candidate = candidate;
      rule.priority = current_priority--;
      rule.is_user = true;
      rules.push_back(std::move(rule));
    }
  }

  return rules;
}

}  // namespace rime
