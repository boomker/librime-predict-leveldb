# librime-predict-leveldb

中文说明 | [English](README.en.md)

基于提交历史预测下一个词的 librime 插件。

这是 `rime/librime-predict` 的一个改版，使用 `predict.userdb` 作为可学习用户库，并支持 `predict.db` 作为只读后备预测库。

## 用法

1. 在 `*.schema.yaml` 中，把 `predictor` 加到 `engine/processors` 里并放在 `key_binder` 之前，把 `predict_translator` 加到 `engine/translators` 里。

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
  # 用户预测库目录，位于 user data 目录下
  # 默认值为 'predict.userdb'
  db: predict.userdb
  # 静态后备预测库，位于 user/shared data 目录下
  # 默认值为 'predict.db'
  # 查询顺序：先查 db，再查 fallback_db
  fallback_db: predict.db
  # 每次最多显示多少个预测候选
  # 默认值为 0，表示显示全部
  max_candidates: 5
  # 最多连续预测多少次
  # 默认值为 0，表示不限制
  max_iterations: 1
  # 当用户库候选少于该值时，继续从 fallback_db 补足候选
  # 默认值为 3
  min_candidates: 3
  # 已删除记录在导出到 predict.userdb.txt 时的保留天数
  # 默认值为 0，表示永不清理
  deleted_record_expire_days: 90
  # 达到 max_iterations 后，用于续触发下一轮预测的按键序列
  trigger: 'jj'
  # 额外的关闭预测菜单按键
  cancel_key: '/'
  # 是否开启连续预测，所有平台都生效
  # false: 受 max_iterations 限制
  # true: 尽可能持续预测，直到没有候选
  # 默认值为 false
  continuous_prediction: false
  # 规则触发库，位于 user data 目录下
  # 默认值为 'predict_rules.db'
  rules_db: predict_rules.db
  # 参与上下文学习的历史提交条数
  # 当前实现会优先学习最近两条提交形成的链式上下文
  # 默认值为 2
  max_context_commits: 2
```

补充说明：

- `db` 是可学习的用户库，会记录用户提交和删除行为。
- `fallback_db` 是只读静态库；当 `db` 中查不到当前 key，或 `db` 的候选数少于 `min_candidates` 时，会作为后备数据源补充候选。
- `deleted_record_expire_days` 只影响导出到 `predict.userdb.txt` 时的已删除记录清理，不会直接从本地 LevelDB 中删除数据。
- `min_candidates` 只在命中用户库时生效；用户库候选会排在前面，只读库中不重复的候选会追加在后面。
- `trigger` 开启后，仅在自动预测连续达到 `max_iterations` 上限时生效；此时输入该序列会基于最近一次提交结果接续下一轮预测。
- `continuous_prediction` 开启后会忽略 `max_iterations`，尽可能持续预测，直到没有候选可提供；关闭时则按 `max_iterations` 处理。
- `rules_db` 是 SQLite 规则库，插件会自动建表并灌入内置规则（节气、节假日等标签数据），可与 `predict_trigger_rules` 一起工作。
- `max_context_commits` 控制上下文联想强度；当前实现主要使用最近两条提交形成链式学习 key。

4. `predict_calendar.yaml` 完整示例（节气节假日配置）

独立文件，路径为 Rime 用户目录下的 `predict_calendar.yaml`：

```yaml
# predict_calendar.yaml
# solar_terms: key 为 "MM-DD"，value 为节气名
# holidays: key 为 "YYYY-MM-DD"，value 为节假日名称列表
solar_terms:
  "02-04": 立春
  "02-19": 雨水
  "03-06": 惊蛰
  "03-20": 春分
  "04-04": 清明
  "04-05": 清明
  "04-20": 谷雨
  "05-06": 立夏
  "05-21": 小满
  "06-06": 芒种
  "06-21": 夏至
  "07-07": 小暑
  "07-23": 大暑
  "08-07": 立秋
  "08-23": 处暑
  "09-08": 白露
  "09-23": 秋分
  "10-08": 寒露
  "10-23": 霜降
  "11-07": 立冬
  "11-22": 小雪
  "12-07": 大雪
  "12-22": 冬至

holidays:
  # 以下为2026年，每年更新此文件即可
  "2026-01-01": [元旦]
  "2026-01-28": [春节, 除夕]
  "2026-01-29": [春节]
  "2026-02-17": [春节]
  "2026-02-18": [春节]
  "2026-04-04": [清明节, 假期]
  "2026-04-05": [清明节, 假期]
  "2026-04-06": [清明节, 假期]
  "2026-05-01": [劳动节, 五一]
  "2026-05-02": [劳动节, 五一]
  "2026-05-03": [劳动节, 五一]
  "2026-05-04": [劳动节, 五一]
  "2026-05-05": [劳动节, 五一]
  "2026-06-19": [端午节]
  "2026-09-25": [中秋节]
  "2026-10-01": [国庆节, 假期]
  "2026-10-02": [国庆节, 假期]
  "2026-10-03": [国庆节, 假期]
  "2026-10-04": [国庆节, 假期]
  "2026-10-05": [国庆节, 假期]
  "2026-10-06": [国庆节, 假期]
  "2026-10-07": [国庆节, 假期]
```

5. `predict_trigger_rules` 完整示例

在 `*.schema.yaml` 或 `*.custom.yaml` 中配置触发规则：

```yaml
patch:
  predictor/trigger: 'jj'
  predictor/cancel_key: '/'
  predictor/db: predict.userdb
  predictor/fallback_db: predict.db
  predictor/rules_db: predict_rules.db
  predictor/max_iterations: 1
  predictor/min_candidates: 3
  predictor/max_candidates: 5
  predictor/max_context_commits: 2
  predictor/continuous_prediction: false
  predict_trigger_rules:
    # 时段规则
    - trigger: 今天
      hour_min: 6
      hour_max: 12
      candidates:
        - 早上
        - 上午
        - 早餐
    - trigger: 今天
      hour_min: 12
      hour_max: 14
      candidates:
        - 中午
        - 午饭
    - trigger: 今天
      hour_min: 14
      hour_max: 18
      candidates:
        - 下午
    - trigger: 今天
      hour_min: 18
      hour_max: 24
      candidates:
        - 晚上
        - 晚饭
        - 夜里
    # 节气规则（tag: chinese_solar）
    - trigger: 今天是
      tag: chinese_solar
      candidates:
        - 清明节
        - 假期
    - trigger: 今天要
      tag: chinese_solar
      candidates:
        - 扫墓
    - trigger: 今天是
      tag: chinese_solar
      candidates:
        - 冬至
        - 吃汤圆
    # 节假日规则（tag: holiday）
    - trigger: 今天是
      tag: holiday
      candidates:
        - 国庆节
        - 假期
    - trigger: 今天要
      tag: holiday
      candidates:
        - 出行
        - 旅游
    # 星期规则（tag: weekday_0 ~ weekday_6，0=周日）
    - trigger: 今天是
      tag: weekday_0
      candidates:
        - 星期天
        - 周末
    - trigger: 今天是
      tag: weekday_6
      candidates:
        - 星期六
        - 周末
    # 场景规则
    - trigger: 请查收
      tag: scene_office
      candidates:
        - 附件
        - 邮件
        - 谢谢
    - trigger: 麻烦你
      tag: scene_office
      candidates:
        - 确认一下
        - 尽快处理
        - 跟进一下
    - trigger: 哈哈
      tag: scene_chat
      candidates:
        - 确实
        - 太有意思了
        - 我也觉得
    - trigger: 晚安
      tag: scene_chat
      candidates:
        - 好梦
        - 早点休息
        - 明天聊
    - trigger: 这个函数
      tag: scene_programming
      candidates:
        - 需要重构
        - 返回值
        - 还要补测试
    - trigger: 这个接口
      tag: scene_programming
      candidates:
        - 需要联调
        - 返回数据
        - 还没兼容错误码
```

可用标签说明：

- `weekday_0` ~ `weekday_6`：星期日到星期六（0=周日，6=周六）
- `scene_chat`：聊天场景
- `scene_office`：办公场景
- `scene_programming`：编程场景
- `chinese_solar`：节气标签（当天为节气时自动添加）
- `holiday`：节假日标签（当天为节假日时自动添加）

6. 重新部署后即可使用。
