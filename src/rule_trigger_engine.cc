#include "rule_trigger_engine.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstdio>
#include <rime/config.h>

namespace rime {

namespace {

constexpr int kTriggerRulesVersion = 1;

const vector<TriggerRule>& builtin_rules() {
  static const vector<TriggerRule> kRules = {
      {"今天", 6, 12, -1, "", "早上", 10, false},
      {"今天", 6, 12, -1, "", "上午", 9, false},
      {"今天", 12, 14, -1, "", "中午", 10, false},
      {"今天", 12, 14, -1, "", "午饭", 9, false},
      {"今天", 14, 18, -1, "", "下午", 10, false},
      {"今天", 18, 24, -1, "", "晚上", 10, false},
      {"今天是", -1, -1, -1, "chinese_solar", "清明节", 10, false},
      {"今天是", -1, -1, -1, "chinese_solar", "冬至", 10, false},
      {"今天是", -1, -1, -1, "holiday", "五一", 10, false},
      {"今天是", -1, -1, 0, "weekday_0", "星期天", 9, false},
      {"今天是", -1, -1, 6, "weekday_6", "星期六", 9, false},
      {"请查收", -1, -1, -1, "scene_office", "附件", 10, false},
      {"请查收", -1, -1, -1, "scene_office", "谢谢", 9, false},
      {"麻烦你", -1, -1, -1, "scene_office", "确认一下", 10, false},
      {"麻烦你", -1, -1, -1, "scene_office", "尽快处理", 9, false},
      {"哈哈", -1, -1, -1, "scene_chat", "确实", 10, false},
      {"哈哈", -1, -1, -1, "scene_chat", "太有意思了", 9, false},
      {"晚安", -1, -1, -1, "scene_chat", "好梦", 10, false},
      {"晚安", -1, -1, -1, "scene_chat", "早点休息", 9, false},
      {"这个函数", -1, -1, -1, "scene_programming", "需要重构", 10, false},
      {"这个函数", -1, -1, -1, "scene_programming", "返回值", 9, false},
      {"这个接口", -1, -1, -1, "scene_programming", "需要联调", 10, false},
      {"这个接口", -1, -1, -1, "scene_programming", "返回数据", 9, false},
      {"希望你", -1, -1, -1, "", "一切顺利", 10, false},
      {"希望你", -1, -1, -1, "", "身体健康", 9, false},
      {"不好意思", -1, -1, -1, "", "打扰了", 10, false},
      {"不好意思", -1, -1, -1, "", "麻烦你", 9, false},
  };
  return kRules;
}

const map<string, vector<string>>& builtin_holidays() {
  static const map<string, vector<string>> kHolidays = {
      {"2026-01-01", {"元旦"}},           {"2026-02-16", {"除夕", "春节"}},
      {"2026-02-17", {"春节"}},           {"2026-02-18", {"春节"}},
      {"2026-04-04", {"清明节", "假期"}}, {"2026-04-05", {"清明节", "假期"}},
      {"2026-04-06", {"清明节", "假期"}}, {"2026-05-01", {"劳动节", "五一"}},
      {"2026-05-02", {"劳动节", "五一"}}, {"2026-05-03", {"劳动节", "五一"}},
      {"2026-05-04", {"劳动节", "五一"}}, {"2026-05-05", {"劳动节", "五一"}},
      {"2026-06-19", {"端午节"}},         {"2026-09-25", {"中秋节"}},
      {"2026-10-01", {"国庆节", "假期"}}, {"2026-10-02", {"国庆节", "假期"}},
      {"2026-10-03", {"国庆节", "假期"}}, {"2026-10-04", {"国庆节", "假期"}},
      {"2026-10-05", {"国庆节", "假期"}}, {"2026-10-06", {"国庆节", "假期"}},
      {"2026-10-07", {"国庆节", "假期"}},
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

void RuleTriggerEngine::LoadFromConfig(Config* config) {
  if (!config)
    return;
  auto rule_list = config->GetList("predict_trigger_rules");
  if (!rule_list)
    return;

  for (size_t i = 0; i < rule_list->size(); ++i) {
    auto rule_map = As<ConfigMap>(rule_list->GetAt(i));
    if (!rule_map)
      continue;
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
      rules_.push_back(std::move(rule));
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
                                        const string& scene) const {
  if (query.empty() || rules_.empty()) {
    return {};
  }

  std::time_t now_time = std::time(nullptr);
  std::tm now = *std::localtime(&now_time);
  set<string> tags = GetTodayTags(scene, now);
  vector<string> results;
  set<string> seen;
  for (const auto& rule : rules_) {
    if (!MatchRule(rule, query, tags, now))
      continue;
    if (!seen.insert(rule.candidate).second)
      continue;
    results.push_back(rule.candidate);
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
  if (ReadVersion(db, "version") >= kTriggerRulesVersion) {
    return;
  }
  sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

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
  constexpr int kHolidaysVersion = 1;
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

bool RuleTriggerEngine::MatchRule(const TriggerRule& rule,
                                  const string& query,
                                  const set<string>& tags,
                                  const std::tm& now) const {
  if (rule.trigger != query) {
    return false;
  }
  if (rule.weekday >= 0 && rule.weekday != now.tm_wday) {
    return false;
  }
  if (rule.hour_min >= 0 && now.tm_hour < rule.hour_min) {
    return false;
  }
  if (rule.hour_max >= 0 && now.tm_hour >= rule.hour_max) {
    return false;
  }
  if (!rule.tag.empty() && tags.find(rule.tag) == tags.end()) {
    return false;
  }
  return true;
}

}  // namespace rime