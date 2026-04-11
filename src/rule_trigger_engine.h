#ifndef RIME_RULE_TRIGGER_ENGINE_H_
#define RIME_RULE_TRIGGER_ENGINE_H_

#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <rime/common.h>

namespace rime {

class Config;
class ConfigList;
class ConfigMap;

enum class MatchType {
  Exact = 0,  // 精确匹配 (默认)
  Prefix,     // 前缀匹配
  Suffix,     // 后缀匹配
  Contains    // 包含匹配
};

struct TriggerRule {
  string trigger;
  int hour_min = -1;
  int hour_max = -1;
  int weekday = -1;
  string tag;
  string candidate;
  int priority = 0;
  bool is_user = false;

  // 新增字段
  MatchType match_type = MatchType::Exact;  // 匹配类型
  vector<string> scenes;                    // 场景白名单 (空=不限制)
  string month_day_start;                   // 日期范围开始 "MM-DD" (空=不限制)
  string month_day_end;                     // 日期范围结束 "MM-DD" (空=不限制)
};

// 模板展开器：接收配置和基础优先级，返回展开后的规则列表
// 模板系统设计原则：
// - 模板不是第二套规则系统，而是普通规则的批量生成器
// - 模板只在 LoadFromConfig() 阶段展开成 TriggerRule
// - 展开后的规则与手写规则完全等价，匹配层无感知
using TemplateExpander =
    std::function<vector<TriggerRule>(const an<ConfigMap>&, int base_priority)>;

class RuleTriggerEngine {
 public:
  RuleTriggerEngine();

  bool LoadCalendar(const path& calendar_path);
  bool LoadFromDB(const path& db_path);
  void LoadFromConfig(Config* config);

  // 匹配规则并返回候选列表
  // 冲突解决语义：当规则匹配结果中存在与当前 query 完全相同的 candidate 时，
  // 仅返回这组精确候选；否则返回全部匹配候选。
  // 这确保用户已输入完整词时，不会建议无关的其他候选。
  // user_only=true 时仅返回用户自定义规则（is_user=true）的候选。
  vector<string> Match(const string& query,
                       const string& scene,
                       bool user_only = false) const;

 protected:
  void LoadBuiltinCalendarDefaults();
  void InitSchema(sqlite3* db);
  void SeedBuiltinRules(sqlite3* db);
  void SeedBuiltinHolidays(sqlite3* db);
  void LoadHolidaysFromDB(sqlite3* db);
  void ReloadRules(sqlite3* db);
  set<string> GetTodayTags(const string& scene, const std::tm& now) const;
  bool MatchRule(const TriggerRule& rule,
                 const string& query,
                 const string& scene,
                 const set<string>& tags,
                 const std::tm& now) const;

  // 从 ConfigList 加载规则（供 LoadFromConfig 和 LoadRulesFromFile 共用）
  void LoadRuleList(const an<ConfigList>& rule_list, const path& rules_root);

  // 模板系统
  void RegisterTemplates();
  vector<TriggerRule> ExpandTemplate(
      const an<ConfigMap>& template_config) const;

  // 内置模板展开器
  // time_greeting: 时间问候模板
  //   参数: items (trigger, hour_min, hour_max, candidates)
  //   base_priority: 默认 100，展开后按顺序递减
  //   注意: 不支持跨午夜时间段 (如 22:00->02:00)，需拆成两条
  static vector<TriggerRule> ExpandTimeGreeting(const an<ConfigMap>& config,
                                                int base_priority);

  // holiday_greeting: 节日问候模板
  //   参数: holidays, candidate_template (默认 "{holiday}快乐"), trigger (可选)
  //   base_priority: 默认 100
  //   trigger 语义: 为空时默认用 holiday 名，非空时对所有 holiday 复用该
  //   trigger
  static vector<TriggerRule> ExpandHolidayGreeting(const an<ConfigMap>& config,
                                                   int base_priority);

  // weekday_reminder: 工作日提醒模板
  //   参数: items (trigger, weekday, candidates)
  //   base_priority: 默认 100
  //   weekday: -1 表示不限制，0-6 表示周日到周六
  //   注意: weekday=-1 可用，但模板主要用途仍是 weekday 约束提醒
  static vector<TriggerRule> ExpandWeekdayReminder(const an<ConfigMap>& config,
                                                   int base_priority);

  map<string, string> solar_terms_;
  map<string, vector<string>> holidays_;
  vector<TriggerRule> rules_;
  map<string, TemplateExpander> template_registry_;
};

}  // namespace rime

#endif  // RIME_RULE_TRIGGER_ENGINE_H_