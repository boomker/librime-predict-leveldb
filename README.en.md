# librime-predict-leveldb

[中文说明](README.md) | English

A librime plugin that predicts the next word from commit history.

This is a modified version of `rime/librime-predict`. It uses `predict.userdb` as a learnable user database and supports `predict.db` as a read-only fallback prediction database.

## Usage

1. In `*.schema.yaml`, add `predictor` to `engine/processors` before `key_binder`, and add `predict_translator` to `engine/translators`.

You can also patch the schema directly:

```yaml
patch:
  'engine/processors/@before 0': predictor
  'engine/translators/@before 0': predict_translator
```

2. Add the `prediction` switch:

```yaml
switches:
  - name: prediction
    states: [ Disable Prediction, Enable Prediction ]
    reset: 1
```

3. Configure the predictor:

```yaml
predictor:
  # User prediction database in the user data directory.
  # Default: 'predict.userdb'
  db: predict.userdb
  # Static fallback prediction database in the user/shared data directory.
  # Default: 'predict.db'
  # Query order: db first, fallback_db second.
  fallback_db: predict.db
  # Maximum number of prediction candidates shown each time.
  # Default: 0, which means show all candidates.
  max_candidates: 5
  # Maximum number of continuous prediction steps.
  # Default: 0, which means unlimited.
  max_iterations: 1
  # When user-db candidates are fewer than this value,
  # continue filling from fallback_db.
  # Default: 3
  min_candidates: 3
  # Retention period in days for deleted records when exporting
  # to predict.userdb.txt. Default: 0, which means never prune.
  deleted_record_expire_days: 90
  # Key sequence used to continue prediction after max_iterations is reached.
  trigger: 'jj'
  # Extra key used to close the prediction menu.
  cancel_key: '/'
```

Notes:

- `db` is the learnable user database. It records user commits and deletions.
- `fallback_db` is a read-only static database. It is used when the current key is not found in `db`, or when `db` returns fewer than `min_candidates` candidates.
- `deleted_record_expire_days` only affects pruning deleted records during export to `predict.userdb.txt`; it does not remove data directly from the local LevelDB.
- Existing legacy `predict.db` data can be reused directly as `fallback_db`.
- `min_candidates` only applies after a user-db hit; user-db candidates stay first, and non-duplicate fallback candidates are appended after them.
- `trigger` no longer starts the first prediction round manually. When enabled, it only takes effect after automatic prediction has already hit `max_iterations`; typing the sequence continues the next round from the most recent commit without requiring an intermediate normal commit.

4. Deploy and use it.
