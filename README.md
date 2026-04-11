# librime-predict-leveldb

中文说明 | [English](README.en.md)

基于提交历史预测下一个词的 librime 插件。

这是 `rime/librime-predict` 的一个改版，使用 `predict.userdb` 作为可学习用户库，并支持 `predict.db` 作为只读后备预测库。

## 用法

1. 在 `*.schema.yaml` 中，把 `predictor` 加到 `engine/processors` 里并放在 `key_binder` 之前，把 `predict_translator` 加到 `engine/translators` 里

也可以直接用 patch：

```yaml
patch:
  'engine/processors/@before 0': predictor
  'engine/translators/@before 0': predict_translator
```

2. 添加 `prediction` 开关：

```yaml
switches:
  - name: prediction
    states: [ 关闭预测, 开启预测 ]
    reset: 1
```

3. 配置预测器：

```yaml
predictor:
  trigger: 'jj' # 达到 max_iterations 后，用于续触发下一轮预测的按键序列
  cancel_key: '/' # 额外的关闭预测菜单按键
  db: predict.userdb # 用户预测库目录，位于 userdata 目录下. 默认值为 'predict.userdb'
  fallback_db: predict.db # 静态后备预测库，位于 userdata 目录下  默认值为 'predict.db' 先查 db，再查 fallback_db
  rule_db: predict_rule.db # 规则触发库，位于 user data 目录下  默认值为 'predict_rule.db'
  user_rule_priority: auto # 用户自定义规则候选的优先级策略，可选 high / auto / low，默认 auto
  max_candidates: 5 # 每次最多显示多少个预测候选  默认值为 0，表示显示全部
  max_iterations: 1 # 最多连续预测多少次 默认值为 0，表示不限制
  min_candidates: 2 # 当用户库候选少于该值时，继续从 fallback_db 补足候选
  max_context_commits: 2 # 参与上下文学习的历史提交条数 当前实现会优先学习最近两条提交形成的链式上下文
  garbage_expire_days: 90 # 已删除记录在导出到 predict.userdb.txt 时的保留天数 默认值为 0，表示永不清理
```

补充说明：

- `db` 是可学习的用户库，会记录用户提交和删除行为。
- `fallback_db` 是只读静态库；当 `db` 中查不到当前 key，或 `db` 的候选数少于 `min_candidates` 时，会作为后备数据源补充候选。
- `garbage_expire_days` 只影响导出到 `predict.userdb.txt` 时的已删除记录清理，不会直接从本地 LevelDB 中删除数据。
- `min_candidates` 只在命中用户库时生效；用户库候选会排在前面，只读库中不重复的候选会追加在后面。
- `trigger` 开启后，仅在自动预测连续达到 `max_iterations` 上限时生效；此时输入该序列会基于最近一次提交结果接续下一轮预测。
- `rule_db` 是 SQLite 规则库，插件会自动建表并灌入内置规则（节气、节假日等标签数据），可与 `predict_trigger_rules` 一起工作。
- `max_context_commits` 控制上下文联想强度；当前实现主要使用最近两条提交形成链式学习 key。
- `user_rule_priority` 控制用户自定义规则候选（通过 `predict_trigger_rules` 配置的规则）在预测菜单中的位置：
  - `high`：始终排在所有候选最前面，不受学习记录影响
  - `auto`（默认）：无近期学习记录时排在最前面；有近期学习记录时，学习候选优先，用户规则候选随后
  - `low`：初始排在第 5 候选位置；被选中提交后，该词进入学习库，下次预测时会依据学习权重自然上升

4. 日历数据配置（节气 / 节假日）

日历数据通过独立 YAML 文件提供，在 `predict_trigger_rules` 中用 `calendar_data` 条目引用：

```yaml
predict_trigger_rules:
  calendar_data: custom_calendar.yaml  # 从用户目录加载日历数据
  ...
```

5. `predict_trigger_rules` 完整示例

`predict_trigger_rules` 是一个 map，包含以下两个子键：

- **`calendar_data`**：日历数据文件名（`.yaml` 后缀），从用户目录加载节气 / 节假日数据
- **`rules`**：规则列表，每项可以是字符串（引用规则文件）或内联规则 map

```yaml
# 在 *.custom.yaml 中配置
patch:
  predict_trigger_rules:
    calendar_data: custom_calendar.yaml  # 加载日历数据（节气/节假日）
    rules:
      - my_work_rules.yaml               # 从用户目录加载规则文件
      - my_chat_rules.yaml
      - trigger: 今天                    # 内联规则，与文件引用可混用
        hour_min: 6
        hour_max: 12
        candidates: [早上, 上午]
```

外部规则文件格式（新格式，推荐）：

```yaml
# my_work_rules.yaml（位于 Rime 用户目录）
patch:
  rules/+:
    - trigger: 请查收
      candidates: [附件, 邮件]
    - trigger: 麻烦你
      candidates: [确认一下, 尽快处理]
```

可用标签说明：

- `weekday_0` ~ `weekday_6`：星期日到星期六（0=周日，6=周六）
- `scene_chat`：聊天场景
- `scene_office`：办公场景
- `scene_programming`：编程场景
- `chinese_solar`：节气标签（当天为节气时自动添加）
- `holiday`：节假日标签（当天为节假日时自动添加）

6. 重新部署后即可使用。
