// Copyright 2010, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Keymap utils of Mozc interface.

#include "session/internal/keymap.h"

#include <string>
#include <vector>
#include <sstream>

#include "base/file_stream.h"
#include "base/config_file_stream.h"
#include "base/util.h"
#include "session/config.pb.h"
#include "session/config_handler.h"
#include "session/commands.pb.h"
#include "session/internal/keymap-inl.h"
#include "session/key_event_normalizer.h"
#include "session/key_parser.h"

namespace mozc {
namespace keymap {
namespace {
static const char kMSIMEKeyMapFile[] = "system://ms-ime.tsv";
static const char kATOKKeyMapFile[] = "system://atok.tsv";
static const char kKotoeriKeyMapFile[] = "system://kotoeri.tsv";
static const char kCustomKeyMapFile[] = "user://keymap.tsv";

uint32 GetModifiers(const commands::KeyEvent &key_event) {
  uint32 modifiers = 0;
  if (key_event.has_modifiers()) {
    modifiers = key_event.modifiers();
  } else {
    for (int i = 0; i < key_event.modifier_keys_size(); ++i) {
      modifiers |= key_event.modifier_keys(i);
    }
  }
  return modifiers;
}
}  // anonymous namespace

bool GetKey(const commands::KeyEvent &key_event, Key *key) {
  // Key is an alias of uint64.
  Key modifier_keys = GetModifiers(key_event);
  Key special_key = key_event.has_special_key() ?
      key_event.special_key() : commands::KeyEvent::NO_SPECIALKEY;
  Key key_code = key_event.has_key_code() ? key_event.key_code() : 0;

  // Make sure the translation from the obsolete spesification.
  // key_code should no longer contain control characters.
  if (0 < key_code && key_code <= 32) {
    return false;
  }

  // Key = |Modifiers(16bit)|SpecialKey(16bit)|Unicode(32bit)|.
  *key = (modifier_keys << 48) + (special_key << 32) + key_code;
  return true;
}

// Return a fallback keyevent generated from key_event.  In the
// current implementation, if the input key_event does not contains
// any special keys or modifier keys, that printable key will be
// replaced the ASCII special key.
bool MaybeGetKeyStub(const commands::KeyEvent &key_event, Key *key) {
  // If any modifier keys were pressed, this function does nothing.
  if (GetModifiers(key_event) != 0) {
    return false;
  }

  // No stub rule is supported for special keys yet.
  if (key_event.has_special_key()) {
    return false;
  }

  if (!key_event.has_key_code() || key_event.key_code() <= 32) {
    return false;
  }

  commands::KeyEvent stub_key_event;
  stub_key_event.set_special_key(commands::KeyEvent::ASCII);
  if (!GetKey(stub_key_event, key)) {
    return false;
  }

  return true;
}


KeyMapManager::KeyMapManager()
    : keymap_(config::Config::NONE) {
  InitCommandData();
  Reload();
}

KeyMapManager::~KeyMapManager() {}

void KeyMapManager::CheckIMEOnOffKeymap() {
  uint64 key_on = 0, key_off = 0, key_eisu = 0;
  {
    commands::KeyEvent key_event;
    KeyParser::ParseKey("ON", &key_event);
    KeyEventNormalizer::ToUint64(key_event, &key_on);
  }
  {
    commands::KeyEvent key_event;
    KeyParser::ParseKey("OFF", &key_event);
    KeyEventNormalizer::ToUint64(key_event, &key_off);
  }
  {
    commands::KeyEvent key_event;
    KeyParser::ParseKey("EISU", &key_event);
    KeyEventNormalizer::ToUint64(key_event, &key_eisu);
  }

  if (key_on == 0 || key_off == 0 || key_eisu == 0) {
    // One of KeyEventNormalizer fails: do nothing to avoid unexpected errors.
    return;
  }

  bool need_to_be_migrated = true;
  for (set<uint64>::const_iterator itr = ime_on_off_keys_.begin();
       itr != ime_on_off_keys_.end(); ++itr) {
    if (*itr != key_on && *itr != key_off && *itr != key_eisu) {
      // This seems to have own settings.
      need_to_be_migrated = false;
      break;
    }
  }

  if (need_to_be_migrated) {
    // Add rules
    commands::KeyEvent key_event_hankaku;
    commands::KeyEvent key_event_kanji;
    KeyParser::ParseKey("Hankaku/Zenkaku", &key_event_hankaku);
    KeyParser::ParseKey("Kanji", &key_event_kanji);
    keymap_direct_.AddRule(key_event_hankaku, DirectInputState::IME_ON);
    keymap_precomposition_.AddRule(key_event_hankaku, PrecompositionState::IME_OFF);
    keymap_composition_.AddRule(key_event_hankaku, CompositionState::IME_OFF);
    keymap_conversion_.AddRule(key_event_hankaku, ConversionState::IME_OFF);
    keymap_direct_.AddRule(key_event_kanji, DirectInputState::IME_ON);
    keymap_precomposition_.AddRule(key_event_kanji, PrecompositionState::IME_OFF);
    keymap_composition_.AddRule(key_event_kanji, CompositionState::IME_OFF);
    keymap_conversion_.AddRule(key_event_kanji, ConversionState::IME_OFF);

    // Write settings
    config::Config config;
    config.CopyFrom(config::ConfigHandler::GetConfig());
    ostringstream oss(config.custom_keymap_table());
    oss << endl;
    oss << "DirectInput\tHankaku/Zenkaku\tIMEOn" << endl;
    oss << "DirectInput\tKanji\tIMEOn" << endl;
    oss << "Conversion\tHankaku/Zenkaku\tIMEOff" << endl;
    oss << "Conversion\tKanji\tIMEOff" << endl;
    oss << "Precomposition\tHankaku/Zenkaku\tIMEOff" << endl;
    oss << "Precomposition\tKanji\tIMEOff" << endl;
    oss << "Composition\tHankaku/Zenkaku\tIMEOff" << endl;
    oss << "Composition\tKanji\tIMEOff" << endl;
    config.set_custom_keymap_table(oss.str());
    config::ConfigHandler::SetConfig(config);
  }
}

bool KeyMapManager::Reload() {
  return ReloadWithKeymap(GET_CONFIG(session_keymap));
}

bool KeyMapManager::ReloadWithKeymap(
    const config::Config::SessionKeymap new_keymap) {
  // If the current keymap is the same with the new keymap and not
  // CUSTOM, do nothing.
  if (new_keymap == keymap_ && new_keymap != config::Config::CUSTOM) {
    return true;
  }

  keymap_ = new_keymap;
  const char *keymap_file = GetKeyMapFileName(new_keymap);

  // Clear the previous keymaps.
  keymap_direct_.Clear();
  keymap_precomposition_.Clear();
  keymap_composition_.Clear();
  keymap_conversion_.Clear();
  keymap_suggestion_.Clear();
  keymap_prediction_.Clear();

  ime_on_off_keys_.clear();

  if (new_keymap == config::Config::CUSTOM) {
    const string &custom_keymap_table = GET_CONFIG(custom_keymap_table);
    if (custom_keymap_table.empty()) {
      LOG(WARNING) << "custom_keymap_table is empty. use default setting";
      const char *default_keymapfile = GetKeyMapFileName(GetDefaultKeyMap());
      return LoadFile(default_keymapfile);
    }
#ifndef NO_LOGGING
    // make a copy of keymap file just for debugging
    const string filename = ConfigFileStream::GetFileName(keymap_file);
    OutputFileStream ofs(filename.c_str());
    if (ofs) {
      ofs << "# This is a copy of keymap table for debugging." << endl;
      ofs << "# Nothing happens when you edit this file manually." << endl;
      ofs << custom_keymap_table;
    }
#endif
    istringstream ifs(custom_keymap_table);
    const bool result = LoadStream(&ifs);
    CheckIMEOnOffKeymap();
    return result;
  }

  if (keymap_file != NULL && LoadFile(keymap_file)) {
    return true;
  }

  const char *default_keymapfile = GetKeyMapFileName(GetDefaultKeyMap());
  return LoadFile(default_keymapfile);
}

// static
const char *KeyMapManager::GetKeyMapFileName(
    const config::Config::SessionKeymap keymap) {
  switch(keymap) {
    case config::Config::ATOK:
      return kATOKKeyMapFile;
    case config::Config::MSIME:
      return kMSIMEKeyMapFile;
    case config::Config::KOTOERI:
      return kKotoeriKeyMapFile;
    case config::Config::CUSTOM:
      return kCustomKeyMapFile;
    case config::Config::NONE:
    default:
      // should not appear here.
      LOG(ERROR) << "Keymap type: " << keymap
                 << " appeared at key map initialization.";
      const config::Config::SessionKeymap default_keymap = GetDefaultKeyMap();
      DCHECK(default_keymap == config::Config::ATOK ||
             default_keymap == config::Config::MSIME ||
             default_keymap == config::Config::KOTOERI ||
             default_keymap == config::Config::CUSTOM);
      // should never make loop.
      return GetKeyMapFileName(default_keymap);
  }
}

// static
config::Config::SessionKeymap KeyMapManager::GetDefaultKeyMap() {
#ifdef OS_MACOSX
  return config::Config::KOTOERI;
#else  // OS_MACOSX
  return config::Config::MSIME;
#endif  // OS_MACOSX
}

bool KeyMapManager::LoadFile(const char *filename) {
  scoped_ptr<istream> ifs(ConfigFileStream::Open(filename));
  if (ifs.get() == NULL) {
    LOG(WARNING) << "cannot load keymap table: " << filename;
    return false;
  }
  return LoadStream(ifs.get());
}

bool KeyMapManager::LoadStream(istream *ifs) {
  vector<string> errors;
  return LoadStreamWithErrors(ifs, &errors);
}

bool KeyMapManager::LoadStreamWithErrors(istream *ifs, vector<string> *errors) {
  string line;
  getline(*ifs, line);  // Skip the first line.
  while (!ifs->eof()) {
    getline(*ifs, line);
    Util::ChopReturns(&line);

    if (line.empty() || line[0] == '#') {  // Skip empty or comment line.
      continue;
    }

    vector<string> rules;
    Util::SplitStringUsing(line, "\t", &rules);
    if (rules.size() != 3) {
      LOG(ERROR) << "Invalid format: " << line;
      continue;
    }

#ifdef NO_LOGGING  // means RELEASE BUILD
    // On the release build, we do not support the Abort and ReportBug
    // commands.
    if (rules[2] == "Abort" || rules[2] == "ReportBug") {
      continue;
    }
#endif  // NO_LOGGING

#ifndef _DEBUG
    // Only debug build supports the Abort command.
    if (rules[2] == "Abort") {
      continue;
    }
#endif  // NO_LOGGING

    commands::KeyEvent key_event;
    KeyParser::ParseKey(rules[1], &key_event);

    // Migration code:
    // check key events for IME ON/OFF
    {
      if (rules[2] == "IMEOn" ||
          rules[2] == "IMEOff") {
        uint64 key;
        if (KeyEventNormalizer::ToUint64(key_event, &key)) {
          ime_on_off_keys_.insert(key);
        }
      }
    }

    if (rules[0] == "DirectInput" || rules[0] == "Direct") {
      DirectInputState::Commands command;
      if (ParseCommandDirect(rules[2], &command)) {
        keymap_direct_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    } else if (rules[0] == "Precomposition") {
      PrecompositionState::Commands command;
      if (ParseCommandPrecomposition(rules[2], &command)) {
        keymap_precomposition_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    } else if (rules[0] == "Composition") {
      CompositionState::Commands command;
      if (ParseCommandComposition(rules[2], &command)) {
        keymap_composition_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    } else if (rules[0] == "Conversion") {
      ConversionState::Commands command;
      if (ParseCommandConversion(rules[2], &command)) {
        keymap_conversion_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    } else if (rules[0] == "Suggestion") {
      CompositionState::Commands command;
      if (ParseCommandComposition(rules[2], &command)) {
        keymap_suggestion_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    } else if (rules[0] == "Prediction") {
      ConversionState::Commands command;
      if (ParseCommandConversion(rules[2], &command)) {
        keymap_prediction_.AddRule(key_event, command);
      } else {
        LOG(ERROR) << "Unknown command: " << line;
        errors->push_back(line);
      }
    }
  }

  commands::KeyEvent key_event;
  KeyParser::ParseKey("ASCII", &key_event);
  keymap_precomposition_.AddRule(key_event,
                                 PrecompositionState::INSERT_CHARACTER);
  keymap_composition_.AddRule(key_event, CompositionState::INSERT_CHARACTER);
  keymap_conversion_.AddRule(key_event, ConversionState::INSERT_CHARACTER);

  key_event.Clear();
  KeyParser::ParseKey("Shift", &key_event);
  keymap_composition_.AddRule(key_event, CompositionState::INSERT_CHARACTER);
  return true;
}

namespace {
template<typename T> bool GetNameInternal(
    const map<T, string> &reverse_command_map, T command, string *name) {
  DCHECK(name);
  typename map<T, string>::const_iterator itr =
      reverse_command_map.find(command);
  if (itr == reverse_command_map.end()) {
    return false;
  } else {
    *name = itr->second;
    return true;
  }
}
}  // namespace

bool KeyMapManager::GetNameFromCommandDirect(
    DirectInputState::Commands command, string *name) const {
  return GetNameInternal<DirectInputState::Commands>(
      reverse_command_direct_map_, command, name);
}

bool KeyMapManager::GetNameFromCommandPrecomposition(
    PrecompositionState::Commands command, string *name) const {
  return GetNameInternal<PrecompositionState::Commands>(
      reverse_command_precomposition_map_, command, name);
}

bool KeyMapManager::GetNameFromCommandComposition(
    CompositionState::Commands command, string *name) const {
  return GetNameInternal<CompositionState::Commands>(
      reverse_command_composition_map_, command, name);
}

bool KeyMapManager::GetNameFromCommandConversion(
    ConversionState::Commands command, string *name) const {
  return GetNameInternal<ConversionState::Commands>(
      reverse_command_conversion_map_, command, name);
}

void KeyMapManager::RegisterDirectCommand(
    const string &command_string, DirectInputState::Commands command) {
  command_direct_map_[command_string] = command;
  reverse_command_direct_map_[command] = command_string;
}

void KeyMapManager::RegisterPrecompositionCommand(
    const string &command_string, PrecompositionState::Commands command) {
  command_precomposition_map_[command_string] = command;
  reverse_command_precomposition_map_[command] = command_string;
}

void KeyMapManager::RegisterCompositionCommand(
    const string &command_string, CompositionState::Commands command) {
  command_composition_map_[command_string] = command;
  reverse_command_composition_map_[command] = command_string;
}

void KeyMapManager::RegisterConversionCommand(
    const string &command_string, ConversionState::Commands command) {
  command_conversion_map_[command_string] = command;
  reverse_command_conversion_map_[command] = command_string;
}

void KeyMapManager::InitCommandData() {
  RegisterDirectCommand("IMEOn", DirectInputState::IME_ON);
  // Support InputMode command only on Windows for now.
  // TODO(toshiyuki): delete #ifdef when we support them on Mac, and
  // activate SessionTest.InputModeConsumedForTestSendKey.
#ifdef OS_WINDOWS
  RegisterDirectCommand("InputModeHiragana",
                        DirectInputState::INPUT_MODE_HIRAGANA);
  RegisterDirectCommand("InputModeFullKatakana",
                        DirectInputState::INPUT_MODE_FULL_KATAKANA);
  RegisterDirectCommand("InputModeHalfKatakana",
                        DirectInputState::INPUT_MODE_HALF_KATAKANA);
  RegisterDirectCommand("InputModeFullAlphanumeric",
                        DirectInputState::INPUT_MODE_FULL_ALPHANUMERIC);
  RegisterDirectCommand("InputModeHalfAlphanumeric",
                        DirectInputState::INPUT_MODE_HALF_ALPHANUMERIC);
#endif  // OS_WINDOWS

  // Precomposition
  RegisterPrecompositionCommand("IMEOff", PrecompositionState::IME_OFF);
  RegisterPrecompositionCommand("IMEOn", PrecompositionState::IME_ON);
  RegisterPrecompositionCommand("InsertCharacter",
                                PrecompositionState::INSERT_CHARACTER);
  RegisterPrecompositionCommand("InsertSpace",
                                PrecompositionState::INSERT_SPACE);
  RegisterPrecompositionCommand("InsertAlternateSpace",
                                PrecompositionState::INSERT_ALTERNATE_SPACE);
  RegisterPrecompositionCommand("InsertHalfSpace",
                                PrecompositionState::INSERT_HALF_SPACE);
  RegisterPrecompositionCommand("InsertFullSpace",
                                PrecompositionState::INSERT_FULL_SPACE);
  RegisterPrecompositionCommand("ToggleAlphanumericMode",
                                PrecompositionState::TOGGLE_ALPHANUMERIC_MODE);
#ifdef OS_WINDOWS
  RegisterPrecompositionCommand("InputModeHiragana",
                                PrecompositionState::INPUT_MODE_HIRAGANA);
  RegisterPrecompositionCommand("InputModeFullKatakana",
                                PrecompositionState::INPUT_MODE_FULL_KATAKANA);
  RegisterPrecompositionCommand("InputModeHalfKatakana",
                                PrecompositionState::INPUT_MODE_HALF_KATAKANA);
  RegisterPrecompositionCommand(
      "InputModeFullAlphanumeric",
      PrecompositionState::INPUT_MODE_FULL_ALPHANUMERIC);
  RegisterPrecompositionCommand(
      "InputModeHalfAlphanumeric",
      PrecompositionState::INPUT_MODE_HALF_ALPHANUMERIC);
#endif  // OS_WINDOWS

  RegisterPrecompositionCommand("LaunchConfigDialog",
                                PrecompositionState::LAUNCH_CONFIG_DIALOG);
  RegisterPrecompositionCommand("LaunchDictionaryTool",
                                PrecompositionState::LAUNCH_DICTIONARY_TOOL);
  RegisterPrecompositionCommand("LaunchWordRegisterDialog",
                                PrecompositionState::LAUNCH_WORD_REGISTER_DIALOG);

  RegisterPrecompositionCommand("Revert", PrecompositionState::REVERT);
  RegisterPrecompositionCommand("Undo", PrecompositionState::UNDO);

#ifdef _DEBUG  // only for debugging
  RegisterPrecompositionCommand("Abort", PrecompositionState::ABORT);
#endif  // _DEBUG

  // Composition
  RegisterCompositionCommand("IMEOff", CompositionState::IME_OFF);
  RegisterCompositionCommand("IMEOn", CompositionState::IME_ON);
  RegisterCompositionCommand("InsertCharacter",
                             CompositionState::INSERT_CHARACTER);
  RegisterCompositionCommand("Delete", CompositionState::DEL);
  RegisterCompositionCommand("Backspace", CompositionState::BACKSPACE);
  RegisterCompositionCommand("InsertHalfSpace",
                             CompositionState::INSERT_HALF_SPACE);
  RegisterCompositionCommand("InsertFullSpace",
                             CompositionState::INSERT_FULL_SPACE);
  RegisterCompositionCommand("Cancel", CompositionState::CANCEL);
  RegisterCompositionCommand("MoveCursorLeft",
                             CompositionState::MOVE_CURSOR_LEFT);
  RegisterCompositionCommand("MoveCursorRight",
                             CompositionState::MOVE_CURSOR_RIGHT);
  RegisterCompositionCommand("MoveCursorToBeginning",
                             CompositionState::MOVE_CURSOR_TO_BEGINNING);
  RegisterCompositionCommand("MoveCursorToEnd",
                             CompositionState::MOVE_MOVE_CURSOR_TO_END);
  RegisterCompositionCommand("Commit", CompositionState::COMMIT);
  RegisterCompositionCommand("CommitFirstSuggestion",
                             CompositionState::COMMIT_FIRST_SUGGESTION);
  RegisterCompositionCommand("Convert", CompositionState::CONVERT);
  RegisterCompositionCommand("ConvertWithoutHistory",
                             CompositionState::CONVERT_WITHOUT_HISTORY);
  RegisterCompositionCommand("PredictAndConvert",
                             CompositionState::PREDICT_AND_CONVERT);
  RegisterCompositionCommand("ConvertToHiragana",
                             CompositionState::CONVERT_TO_HIRAGANA);
  RegisterCompositionCommand("ConvertToFullKatakana",
                             CompositionState::CONVERT_TO_FULL_KATAKANA);
  RegisterCompositionCommand("ConvertToHalfKatakana",
                             CompositionState::CONVERT_TO_HALF_KATAKANA);
  RegisterCompositionCommand("ConvertToHalfWidth",
                             CompositionState::CONVERT_TO_HALF_WIDTH);
  RegisterCompositionCommand("ConvertToFullAlphanumeric",
                             CompositionState::CONVERT_TO_FULL_ALPHANUMERIC);
  RegisterCompositionCommand("ConvertToHalfAlphanumeric",
                             CompositionState::CONVERT_TO_HALF_ALPHANUMERIC);
  RegisterCompositionCommand("SwitchKanaType",
                             CompositionState::SWITCH_KANA_TYPE);
  RegisterCompositionCommand("DisplayAsHiragana",
                             CompositionState::DISPLAY_AS_HIRAGANA);
  RegisterCompositionCommand("DisplayAsFullKatakana",
                             CompositionState::DISPLAY_AS_FULL_KATAKANA);
  RegisterCompositionCommand("DisplayAsHalfKatakana",
                             CompositionState::DISPLAY_AS_HALF_KATAKANA);
  RegisterCompositionCommand("DisplayAsHalfWidth",
                             CompositionState::TRANSLATE_HALF_WIDTH);
  RegisterCompositionCommand("DisplayAsFullAlphanumeric",
                             CompositionState::TRANSLATE_FULL_ASCII);
  RegisterCompositionCommand("DisplayAsHalfAlphanumeric",
                             CompositionState::TRANSLATE_HALF_ASCII);
  RegisterCompositionCommand("ToggleAlphanumericMode",
                             CompositionState::TOGGLE_ALPHANUMERIC_MODE);
#ifdef OS_WINDOWS
  RegisterCompositionCommand("InputModeHiragana",
                             CompositionState::INPUT_MODE_HIRAGANA);
  RegisterCompositionCommand("InputModeFullKatakana",
                             CompositionState::INPUT_MODE_FULL_KATAKANA);
  RegisterCompositionCommand("InputModeHalfKatakana",
                             CompositionState::INPUT_MODE_HALF_KATAKANA);
  RegisterCompositionCommand("InputModeFullAlphanumeric",
                             CompositionState::INPUT_MODE_FULL_ALPHANUMERIC);
  RegisterCompositionCommand("InputModeHalfAlphanumeric",
                             CompositionState::INPUT_MODE_HALF_ALPHANUMERIC);
#endif  // OS_WINDOWS
#ifdef _DEBUG  // only for debugging
  RegisterCompositionCommand("Abort", CompositionState::ABORT);
#endif  // _DEBUG

  // Conversion
  RegisterConversionCommand("IMEOff", ConversionState::IME_OFF);
  RegisterConversionCommand("IMEOn", ConversionState::IME_ON);
  RegisterConversionCommand("InsertCharacter",
                            ConversionState::INSERT_CHARACTER);
  RegisterConversionCommand("InsertHalfSpace",
                            ConversionState::INSERT_HALF_SPACE);
  RegisterConversionCommand("InsertFullSpace",
                            ConversionState::INSERT_FULL_SPACE);
  RegisterConversionCommand("Cancel", ConversionState::CANCEL);
  RegisterConversionCommand("SegmentFocusLeft",
                            ConversionState::SEGMENT_FOCUS_LEFT);
  RegisterConversionCommand("SegmentFocusRightOrCommit",
                            ConversionState::SEGMENT_FOCUS_RIGHT_OR_COMMIT);
  RegisterConversionCommand("SegmentFocusRight",
                            ConversionState::SEGMENT_FOCUS_RIGHT);
  RegisterConversionCommand("SegmentFocusFirst",
                            ConversionState::SEGMENT_FOCUS_FIRST);
  RegisterConversionCommand("SegmentFocusLast",
                            ConversionState::SEGMENT_FOCUS_LAST);
  RegisterConversionCommand("SegmentWidthExpand",
                            ConversionState::SEGMENT_WIDTH_EXPAND);
  RegisterConversionCommand("SegmentWidthShrink",
                            ConversionState::SEGMENT_WIDTH_SHRINK);
  RegisterConversionCommand("ConvertNext", ConversionState::CONVERT_NEXT);
  RegisterConversionCommand("ConvertPrev", ConversionState::CONVERT_PREV);
  RegisterConversionCommand("ConvertNextPage",
                            ConversionState::CONVERT_NEXT_PAGE);
  RegisterConversionCommand("ConvertPrevPage",
                            ConversionState::CONVERT_PREV_PAGE);
  RegisterConversionCommand("PredictAndConvert",
                            ConversionState::PREDICT_AND_CONVERT);
  RegisterConversionCommand("Commit", ConversionState::COMMIT);
  RegisterConversionCommand("CommitOnlyFirstSegment",
                            ConversionState::COMMIT_SEGMENT);
  RegisterConversionCommand("ConvertToHiragana",
                            ConversionState::CONVERT_TO_HIRAGANA);
  RegisterConversionCommand("ConvertToFullKatakana",
                            ConversionState::CONVERT_TO_FULL_KATAKANA);
  RegisterConversionCommand("ConvertToHalfKatakana",
                            ConversionState::CONVERT_TO_HALF_KATAKANA);
  RegisterConversionCommand("ConvertToHalfWidth",
                            ConversionState::CONVERT_TO_HALF_WIDTH);
  RegisterConversionCommand("ConvertToFullAlphanumeric",
                            ConversionState::CONVERT_TO_FULL_ALPHANUMERIC);
  RegisterConversionCommand("ConvertToHalfAlphanumeric",
                            ConversionState::CONVERT_TO_HALF_ALPHANUMERIC);
  RegisterConversionCommand("SwitchKanaType",
                            ConversionState::SWITCH_KANA_TYPE);
  RegisterConversionCommand("ToggleAlphanumericMode",
                            ConversionState::TOGGLE_ALPHANUMERIC_MODE);
  RegisterConversionCommand("DisplayAsHiragana",
                            ConversionState::DISPLAY_AS_HIRAGANA);
  RegisterConversionCommand("DisplayAsFullKatakana",
                            ConversionState::DISPLAY_AS_FULL_KATAKANA);
  RegisterConversionCommand("DisplayAsHalfKatakana",
                            ConversionState::DISPLAY_AS_HALF_KATAKANA);
  RegisterConversionCommand("DisplayAsHalfWidth",
                            ConversionState::TRANSLATE_HALF_WIDTH);
  RegisterConversionCommand("DisplayAsFullAlphanumeric",
                            ConversionState::TRANSLATE_FULL_ASCII);
  RegisterConversionCommand("DisplayAsHalfAlphanumeric",
                            ConversionState::TRANSLATE_HALF_ASCII);
#ifdef OS_WINDOWS
  RegisterConversionCommand("InputModeHiragana",
                            ConversionState::INPUT_MODE_HIRAGANA);
  RegisterConversionCommand("InputModeFullKatakana",
                            ConversionState::INPUT_MODE_FULL_KATAKANA);
  RegisterConversionCommand("InputModeHalfKatakana",
                            ConversionState::INPUT_MODE_HALF_KATAKANA);
  RegisterConversionCommand("InputModeFullAlphanumeric",
                            ConversionState::INPUT_MODE_FULL_ALPHANUMERIC);
  RegisterConversionCommand("InputModeHalfAlphanumeric",
                            ConversionState::INPUT_MODE_HALF_ALPHANUMERIC);
#endif  // OS_WINDOWS
#ifndef NO_LOGGING  // means NOT RELEASE build
  RegisterConversionCommand("ReportBug", ConversionState::REPORT_BUG);
#endif  // NO_LOGGING
#ifdef _DEBUG  // only for dubugging
  RegisterConversionCommand("Abort", ConversionState::ABORT);
#endif  // _DEBUG
}

#undef ADD_TO_COMMAND_MAP

bool KeyMapManager::GetCommandDirect(
    const commands::KeyEvent &key_event,
    DirectInputState::Commands *command) const {
  return keymap_direct_.GetCommand(key_event, command);
}

bool KeyMapManager::GetCommandPrecomposition(
    const commands::KeyEvent &key_event,
    PrecompositionState::Commands *command) const {
  return keymap_precomposition_.GetCommand(key_event, command);
}

bool KeyMapManager::GetCommandComposition(
    const commands::KeyEvent &key_event,
    CompositionState::Commands *command) const {
  return keymap_composition_.GetCommand(key_event, command);
}

bool KeyMapManager::GetCommandSuggestion(
    const commands::KeyEvent &key_event,
    CompositionState::Commands *command) const {
  // try suggestion rule first
  if (keymap_suggestion_.GetCommand(key_event, command)) {
    return true;
  }
  // use preedit rule
  return keymap_composition_.GetCommand(key_event, command);
}

bool KeyMapManager::GetCommandConversion(
    const commands::KeyEvent &key_event,
    ConversionState::Commands *command) const {
  return keymap_conversion_.GetCommand(key_event, command);
}

bool KeyMapManager::GetCommandPrediction(
    const commands::KeyEvent &key_event,
    ConversionState::Commands *command) const {
  // try prediction rule first
  if (keymap_prediction_.GetCommand(key_event, command)) {
    return true;
  }
  // use conversion rule
  return keymap_conversion_.GetCommand(key_event, command);
}

bool KeyMapManager::ParseCommandDirect(
    const string &command_string,
    DirectInputState::Commands *command) const {
  if (command_direct_map_.count(command_string) == 0) {
    return false;
  }
  *command = command_direct_map_.find(command_string)->second;
  return true;
}

// This should be in KeyMap instead of KeyMapManager.
bool KeyMapManager::ParseCommandPrecomposition(
    const string &command_string,
    PrecompositionState::Commands *command) const {
  if (command_precomposition_map_.count(command_string) == 0) {
    return false;
  }
  *command = command_precomposition_map_.find(command_string)->second;
  return true;
}


bool KeyMapManager::ParseCommandComposition(
    const string &command_string,
    CompositionState::Commands *command) const {
  if (command_composition_map_.count(command_string) == 0) {
    return false;
  }
  *command = command_composition_map_.find(command_string)->second;
  return true;
}

bool KeyMapManager::ParseCommandConversion(
    const string &command_string,
    ConversionState::Commands *command) const {
  if (command_conversion_map_.count(command_string) == 0) {
    return false;
  }
  *command = command_conversion_map_.find(command_string)->second;
  return true;
}

void KeyMapManager::GetAvailableCommandNameDirect(
    set<string> *command_names) const {
  DCHECK(command_names);
  for (map<string, DirectInputState::Commands>::const_iterator itr
           = command_direct_map_.begin();
       itr != command_direct_map_.end(); ++itr) {
    command_names->insert(itr->first);
  }
}

void KeyMapManager::GetAvailableCommandNamePrecomposition(
    set<string> *command_names) const {
  DCHECK(command_names);
  for (map<string, PrecompositionState::Commands>::const_iterator itr
           = command_precomposition_map_.begin();
       itr != command_precomposition_map_.end(); ++itr) {
    command_names->insert(itr->first);
  }
}

void KeyMapManager::GetAvailableCommandNameComposition(
    set<string> *command_names) const {
  DCHECK(command_names);
  for (map<string, CompositionState::Commands>::const_iterator itr
           = command_composition_map_.begin();
       itr != command_composition_map_.end(); ++itr) {
    command_names->insert(itr->first);
  }
}

void KeyMapManager::GetAvailableCommandNameConversion(
    set<string> *command_names) const {
  DCHECK(command_names);
  for (map<string, ConversionState::Commands>::const_iterator itr
           = command_conversion_map_.begin();
       itr != command_conversion_map_.end(); ++itr) {
    command_names->insert(itr->first);
  }
}

void KeyMapManager::GetAvailableCommandNameSuggestion(
    set<string> *command_names) const {
  GetAvailableCommandNameComposition(command_names);
}

void KeyMapManager::GetAvailableCommandNamePrediction(
    set<string> *command_names) const {
  GetAvailableCommandNameConversion(command_names);
}

}  // namespace keymap
}  // namespace mozc
