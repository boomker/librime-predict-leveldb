#ifndef RIME_RULE_TRIGGER_ENGINE_H_
#define RIME_RULE_TRIGGER_ENGINE_H_

#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <rime/common.h>

namespace rime {

class Config;

struct TriggerRule {
  string trigger;
  int hour_min = -1;
  int hour_max = -1;
  int weekday = -1;
  string tag;
  string candidate;
  int priority = 0;
  bool is_user = false;
};

class RuleTriggerEngine {
 public:
  RuleTriggerEngine();

  bool LoadCalendar(const path& calendar_path);
  bool LoadFromDB(const path& db_path);
  void LoadFromConfig(Config* config);

  vector<string> Match(const string& query, const string& scene) const;

 private:
  void LoadBuiltinCalendarDefaults();
  void InitSchema(sqlite3* db);
  void SeedBuiltinRules(sqlite3* db);
  void SeedBuiltinHolidays(sqlite3* db);
  void LoadHolidaysFromDB(sqlite3* db);
  void ReloadRules(sqlite3* db);
  set<string> GetTodayTags(const string& scene, const std::tm& now) const;
  bool MatchRule(const TriggerRule& rule,
                 const string& query,
                 const set<string>& tags,
                 const std::tm& now) const;

  map<string, string> solar_terms_;
  map<string, vector<string>> holidays_;
  vector<TriggerRule> rules_;
};

}  // namespace rime

#endif  // RIME_RULE_TRIGGER_ENGINE_H_