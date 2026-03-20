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
```

补充说明：

- `db` 是可学习的用户库，会记录用户提交和删除行为。
- `fallback_db` 是只读静态库；当 `db` 中查不到当前 key，或 `db` 的候选数少于 `min_candidates` 时，会作为后备数据源补充候选。
- `deleted_record_expire_days` 只影响导出到 `predict.userdb.txt` 时的已删除记录清理，不会直接从本地 LevelDB 中删除数据。
- 如果你已有旧版 `predict.db` 数据，可以直接作为 `fallback_db` 使用。
- `min_candidates` 只在命中用户库时生效；用户库候选会排在前面，只读库中不重复的候选会追加在后面。
- `trigger` 不再用于手动拉起首轮预测。开启后，仅在自动预测连续达到 `max_iterations` 上限时生效；此时输入该序列会基于最近一次提交结果接续下一轮预测，无需先提交一个普通候选。

4. 重新部署后即可使用。
