#include "predict_legacy_db.h"

#include <algorithm>
#include <cstring>
#include <boost/algorithm/string.hpp>

namespace rime {

namespace {

const string kPredictFormat = "Rime::Predict/1.0";
const string kPredictFormatPrefix = "Rime::Predict/";

}  // namespace

LegacyPredictDb::LegacyPredictDb(const path& file_path)
    : MappedFile(file_path),
      key_trie_(new Darts::DoubleArray),
      value_trie_(new StringTable) {}

LegacyPredictDbManager& LegacyPredictDbManager::instance() {
  static LegacyPredictDbManager instance;
  return instance;
}

an<LegacyPredictDb> LegacyPredictDbManager::GetPredictDb(
    const path& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = db_cache_.find(file_path.string());
  if (found != db_cache_.end()) {
    if (auto db = found->second.lock()) {
      return db;
    }
    db_cache_.erase(found);
  }

  an<LegacyPredictDb> new_db = std::make_shared<LegacyPredictDb>(file_path);
  if (new_db->Load()) {
    db_cache_[file_path.string()] = new_db;
    return new_db;
  }

  return nullptr;
}

bool LegacyPredictDb::Load() {
  if (IsOpen()) {
    Close();
  }

  loaded_ = false;
  metadata_ = nullptr;
  key_trie_.reset(new Darts::DoubleArray);
  value_trie_.reset(new StringTable);

  if (!OpenReadOnly()) {
    return false;
  }

  metadata_ = Find<predict_legacy::Metadata>(0);
  if (!metadata_) {
    Close();
    return false;
  }

  if (!boost::starts_with(string(metadata_->format), kPredictFormatPrefix)) {
    Close();
    return false;
  }

  if (!metadata_->key_trie || !metadata_->value_trie) {
    Close();
    return false;
  }

  key_trie_->set_array(metadata_->key_trie.get(), metadata_->key_trie_size);
  value_trie_ = std::make_unique<StringTable>(metadata_->value_trie.get(),
                                              metadata_->value_trie_size);
  loaded_ = true;
  return true;
}

bool LegacyPredictDb::Save() {
  if (!key_trie_->total_size()) {
    return false;
  }
  return ShrinkToFit();
}

int LegacyPredictDb::WriteCandidates(
    const vector<predict_legacy::RawEntry>& candidates,
    const table::Entry* entry) {
  auto* array = CreateArray<table::Entry>(candidates.size());
  auto* next = array->begin();
  for (size_t i = 0; i < candidates.size(); ++i) {
    *next++ = *entry++;
  }
  auto offset = reinterpret_cast<char*>(array) - address();
  return int(offset);
}

bool LegacyPredictDb::Build(const predict_legacy::RawData& data) {
  const int data_size = data.size();
  size_t entry_count = 0;
  for (const auto& kv : data) {
    entry_count += kv.second.size();
  }

  StringTableBuilder string_table;
  vector<table::Entry> entries(entry_count);
  vector<const char*> keys;
  keys.reserve(data_size);

  int i = 0;
  for (const auto& kv : data) {
    if (kv.second.empty()) {
      continue;
    }
    for (const auto& candidate : kv.second) {
      string_table.Add(candidate.text, candidate.weight,
                       &entries[i].text.str_id());
      entries[i].weight = float(candidate.weight);
      ++i;
    }
    keys.push_back(kv.first.c_str());
  }

  string_table.Build();
  size_t value_trie_image_size = string_table.BinarySize();
  if (!Create(value_trie_image_size)) {
    return false;
  }
  if (!Allocate<predict_legacy::Metadata>()) {
    return false;
  }

  const table::Entry* available_entries =
      entries.empty() ? nullptr : &entries[0];
  vector<int> values;
  values.reserve(data_size);
  for (const auto& kv : data) {
    if (kv.second.empty()) {
      continue;
    }
    values.push_back(WriteCandidates(kv.second, available_entries));
    available_entries += kv.second.size();
  }

  if (0 != key_trie_->build(data_size, keys.data(), nullptr, values.data())) {
    return false;
  }

  size_t key_trie_image_size = key_trie_->total_size();
  char* key_trie_image = Allocate<char>(key_trie_image_size);
  if (!key_trie_image) {
    return false;
  }
  std::memcpy(key_trie_image, key_trie_->array(), key_trie_image_size);

  metadata_ = reinterpret_cast<predict_legacy::Metadata*>(address());
  metadata_->key_trie = key_trie_image;
  metadata_->key_trie_size = key_trie_->size();

  char* value_trie_image = Allocate<char>(value_trie_image_size);
  if (!value_trie_image) {
    return false;
  }
  string_table.Dump(value_trie_image, value_trie_image_size);

  metadata_ = reinterpret_cast<predict_legacy::Metadata*>(address());
  metadata_->value_trie = value_trie_image;
  metadata_->value_trie_size = value_trie_image_size;
  value_trie_ =
      std::make_unique<StringTable>(value_trie_image, value_trie_image_size);
  std::strncpy(metadata_->format, kPredictFormat.c_str(),
               kPredictFormat.length());
  loaded_ = true;
  return true;
}

bool LegacyPredictDb::Lookup(const string& query, vector<string>* candidates) {
  if (!loaded_ || !candidates) {
    return false;
  }

  candidates->clear();
  int result = key_trie_->exactMatchSearch<int>(query.c_str());
  if (result == -1) {
    return false;
  }

  const auto* found = Find<predict_legacy::Candidates>(result);
  if (!found) {
    return false;
  }

  for (const auto& entry : *found) {
    candidates->push_back(GetEntryText(entry));
  }
  return !candidates->empty();
}

string LegacyPredictDb::GetEntryText(const ::rime::table::Entry& entry) const {
  return value_trie_->GetString(entry.text.str_id());
}

}  // namespace rime
