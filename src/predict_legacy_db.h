#ifndef RIME_PREDICT_LEGACY_DB_H_
#define RIME_PREDICT_LEGACY_DB_H_

#include <darts.h>
#include <mutex>
#include <rime/common.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/string_table.h>
#include <rime/dict/table.h>

namespace rime {

namespace predict_legacy {

struct Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t db_checksum;
  OffsetPtr<char> key_trie;
  uint32_t key_trie_size;
  OffsetPtr<char> value_trie;
  uint32_t value_trie_size;
};

using Candidates = ::rime::Array<::rime::table::Entry>;

struct RawEntry {
  string text;
  double weight;
};

using RawData = map<string, vector<RawEntry>>;

}  // namespace predict_legacy

class LegacyPredictDb : public MappedFile {
 public:
  explicit LegacyPredictDb(const path& file_path);

  bool Load();
  bool Save();
  bool Build(const predict_legacy::RawData& data);
  bool Lookup(const string& query, vector<string>* candidates);
  bool valid() const { return loaded_; }

 private:
  int WriteCandidates(const vector<predict_legacy::RawEntry>& candidates,
                      const table::Entry* entry);
  string GetEntryText(const ::rime::table::Entry& entry) const;

  predict_legacy::Metadata* metadata_ = nullptr;
  the<Darts::DoubleArray> key_trie_;
  the<StringTable> value_trie_;
  bool loaded_ = false;
};

class LegacyPredictDbManager {
 public:
  static LegacyPredictDbManager& instance();
  an<LegacyPredictDb> GetPredictDb(const path& file_path);

 private:
  LegacyPredictDbManager() = default;
  ~LegacyPredictDbManager() = default;
  LegacyPredictDbManager(const LegacyPredictDbManager&) = delete;

  std::mutex mutex_;
  std::map<string, weak<LegacyPredictDb>> db_cache_;
};

}  // namespace rime

#endif  // RIME_PREDICT_LEGACY_DB_H_
