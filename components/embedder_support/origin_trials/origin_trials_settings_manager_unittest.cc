// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/embedder_support/origin_trials/origin_trials_settings_manager.h"
#include <cstdint>
#include <string>
#include <vector>
#include "base/i18n/number_formatting.h"
#include "base/memory/ptr_util.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace embedder_support {

// Matches kOriginMaxDisabledTrialTokens in origin_trials_settings_manager.cc
const unsigned long kOriginMaxDisabledTrialTokens = 11;
class OriginTrialsSettingsManagerTest : public testing::Test {
 protected:
  OriginTrialsSettingsManagerTest()
      : manager_(base::WrapUnique(new OriginTrialsSettingsManager())) {}

  OriginTrialsSettingsManager* manager() { return manager_.get(); }

  base::Value::List CreateMaxTokensList(uint16_t initial_number) {
    base::Value::List max_tokens_list =
        base::Value::List::with_capacity(kOriginMaxDisabledTrialTokens);
    for (uint16_t i = initial_number;
         i < kOriginMaxDisabledTrialTokens + initial_number; i++) {
      max_tokens_list.Append(base::FormatNumber(i));
    }
    return max_tokens_list;
  }

 private:
  std::unique_ptr<OriginTrialsSettingsManager> manager_;
};

TEST_F(OriginTrialsSettingsManagerTest, EnsureEmptyValidSettingsOnCreation) {
  EXPECT_EQ(manager()->GetSettings().is_null(), false);
  EXPECT_EQ(manager()->GetSettings()->disabled_tokens.size(), 0UL);
}

TEST_F(OriginTrialsSettingsManagerTest, DisabledTokensPopulatedInSettings) {
  manager()->PopulateFromOriginTrialsConfig(
      base::Value::List().Append("token 1"));
  EXPECT_EQ(manager()->GetSettings()->disabled_tokens,
            std::vector<std::string>({"token 1"}));
}

TEST_F(OriginTrialsSettingsManagerTest,
       PopulateEmptyConfigResetsExistingConfig) {
  manager()->PopulateFromOriginTrialsConfig(
      base::Value::List().Append("token 1"));
  EXPECT_EQ(manager()->GetSettings()->disabled_tokens,
            std::vector<std::string>({"token 1"}));
  manager()->PopulateFromOriginTrialsConfig(base::Value::List());
  EXPECT_EQ(manager()->GetSettings()->disabled_tokens,
            std::vector<std::string>({}));
}

TEST_F(OriginTrialsSettingsManagerTest, CloneDoesNotAffectedSettingsInManager) {
  manager()->PopulateFromOriginTrialsConfig(
      base::Value::List().Append("token 1"));
  // Get the clone of the initial settings
  blink::mojom::OriginTrialsSettingsPtr settings1 = manager()->GetSettings();
  // Add new settings
  manager()->PopulateFromOriginTrialsConfig(
      base::Value::List().Append("token a"));
  // Get the clone of the new settings
  blink::mojom::OriginTrialsSettingsPtr settings2 = manager()->GetSettings();
  // Settings should be different
  EXPECT_EQ(settings1->disabled_tokens, std::vector<std::string>({"token 1"}));
  EXPECT_EQ(settings2->disabled_tokens, std::vector<std::string>({"token a"}));
}

TEST_F(OriginTrialsSettingsManagerTest, MaxDisabledTokensExistInSettings) {
  manager()->PopulateFromOriginTrialsConfig(CreateMaxTokensList(0));
  EXPECT_EQ(manager()->GetSettings()->disabled_tokens.size(),
            kOriginMaxDisabledTrialTokens);
}

TEST_F(OriginTrialsSettingsManagerTest,
       PopulateWithOverMaxDisabledTokensDoesNotChangeSettings) {
  base::Value::List disabled_tokens_list_within_limit = CreateMaxTokensList(0);
  manager()->PopulateFromOriginTrialsConfig(disabled_tokens_list_within_limit);
  std::vector<std::string> initial_disabled_tokens =
      manager()->GetSettings()->disabled_tokens;

  // Populate with a new list of disabled tokens. This list has
  // |kOriginMaxDisabledTrialTokens| + 1 items.
  base::Value::List disabled_tokens_list_over_limit =
      CreateMaxTokensList(kOriginMaxDisabledTrialTokens);
  disabled_tokens_list_over_limit.Append("-1");
  EXPECT_NE(disabled_tokens_list_within_limit, disabled_tokens_list_over_limit);
  manager()->PopulateFromOriginTrialsConfig(disabled_tokens_list_over_limit);

  std::vector<std::string> disabled_tokens_after_repopulate =
      manager()->GetSettings()->disabled_tokens;
  EXPECT_EQ(initial_disabled_tokens, disabled_tokens_after_repopulate);
  EXPECT_EQ(initial_disabled_tokens.size(), kOriginMaxDisabledTrialTokens);
}
}  // namespace embedder_support
