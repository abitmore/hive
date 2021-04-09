#pragma once

// std
#include <utility>
#include <cstdint>

// fc
#include <fc/crypto/elliptic.hpp>

// project includes
#include <hive/protocol/asset_symbol.hpp>
#include <hive/protocol/hardfork.hpp>
#include <hive/protocol/fixed_string.hpp>

namespace hive
{
  namespace protocol
  {
    struct asset;
    typedef fixed_string<16> account_name_type; // this is pretty bad

    namespace blockchain_configuration
    {

      template <typename DataType>
      class configuration
      {
        DataType m_data;

      public:
        template <typename... U>
        explicit configuration(U &&...u) : m_data{std::forward<U>(u)...} {}

        const DataType &get() const { return m_data; }
        void set(const DataType &val) { m_data = val; }
      };

      using configuration_str_t = configuration<std::string>;
      using configuration_hash_t = configuration<fc::sha256>;
      using configuration_pv_key_t = configuration<fc::ecc::private_key>;
      using configuration_version_t = configuration<version>;
      using configuration_hardfork_version_t = configuration<hardfork_version>;
      using configuration_int_t = configuration<int32_t>;
      using configuration_uint_t = configuration<uint32_t>;
      using configuration_bigint_t = configuration<int64_t>;
      using configuration_ubigint_t = configuration<uint64_t>;
      using configuration_hugeint_t = configuration<fc::uint128_t>;
      using configuration_asset_t = configuration<asset>;
      using configuration_asset_symbol_t = configuration<asset_symbol_type>;
      using configuration_time_t = configuration<fc::microseconds>;
      using configuration_timepoint_t = configuration<fc::time_point_sec>;
      using configuration_accountname_t = configuration<account_name_type>;

      // has to be const, because they are used as template parameters, or dependencies of them
      constexpr uint32_t hive_100_percent{10'000u};
      constexpr uint32_t hive_1_percent{hive_100_percent / 100};
      constexpr uint32_t hive_1_basis_point{hive_100_percent / 10'000};
      constexpr int hive_block_interval{3};
      constexpr int hive_blocks_per_year{365 * 24 * 60 * 60 / hive_block_interval};
      constexpr uint32_t hive_blocks_per_day{24u * 60 * 60 / hive_block_interval};
      constexpr int hive_max_witnesses{21};
      constexpr int hive_max_voted_witnesses_hf0{19};
      constexpr int hive_max_miner_witnesses_hf0{1};
      constexpr int hive_max_runner_witnesses_hf0{1};
      constexpr int hive_max_voted_witnesses_hf17{20};
      constexpr int hive_max_miner_witnesses_hf17{0};
      constexpr int hive_max_runner_witnesses_hf17{1};
      constexpr int hive_max_proxy_recursion_depth{4};
      constexpr int hive_voting_mana_regeneration_seconds{5 * 60 * 60 * 24};
      constexpr int hive_liquidity_reward_period_sec{60 * 60};
      constexpr uint64_t hive_apr_percent_multiply_per_block{(uint64_t(0x5ccc) << 0x20) | (uint64_t(0xe802) << 0x10) | (uint64_t(0xde5f))};
      constexpr int hive_apr_percent_shift_per_block{87};
      constexpr uint64_t hive_apr_percent_multiply_per_round{(uint64_t(0x79cc) << 0x20) | (uint64_t(0xf5c7) << 0x10) | (uint64_t(0x3480))};
      constexpr int hive_apr_percent_shift_per_round{83};
      constexpr uint64_t hive_apr_percent_multiply_per_hour{(uint64_t(0x6cc1) << 0x20) | (uint64_t(0x39a1) << 0x10) | (uint64_t(0x5cbd))};
      constexpr int hive_apr_percent_shift_per_hour{77};
      constexpr int hive_curate_apr_percent{3875};
      constexpr int hive_content_apr_percent{3875};
      constexpr int hive_liquidity_apr_percent{750};
      constexpr int hive_producer_apr_percent{750};
      constexpr int hive_pow_apr_percent{750};
      constexpr uint32_t hive_min_account_name_length{3u};
      constexpr int hive_rd_min_decay_bits{6};
      constexpr uint32_t hive_irreversible_threshold{75u * hive_1_percent};
      constexpr uint32_t hive_rd_decay_denom_shift{36u};
      constexpr int hive_rd_max_pool_bits{64};
      constexpr uint64_t hive_rd_max_budget_1{(uint64_t(1) << (hive_rd_max_pool_bits + hive_rd_min_decay_bits - hive_rd_decay_denom_shift)) - 1};
      constexpr uint64_t hive_rd_max_budget_2{(uint64_t(1) << (64 - hive_rd_decay_denom_shift)) - 1};
      constexpr uint64_t hive_rd_max_budget_3{uint64_t(std::numeric_limits<int32_t>::max())};
      constexpr int hive_rd_max_budget{int32_t(std::min({hive_rd_max_budget_1, hive_rd_max_budget_2, hive_rd_max_budget_3}))};

#ifdef IS_TEST_NET
      extern configuration_pv_key_t hive_init_private_key;
      extern configuration_uint_t testnet_block_limit;
#endif
      // theese varries in case of testnet
      extern configuration_version_t hive_blockchain_version;
      extern configuration_str_t hive_init_public_key_str;
      extern configuration_hash_t steem_chain_id;
      extern configuration_hash_t def_hive_chain_id;
      extern configuration_str_t hive_address_prefix;
      extern configuration_timepoint_t hive_genesis_time;
      extern configuration_timepoint_t hive_mining_time;
      extern configuration_int_t hive_cashout_window_seconds_pre_hf12;
      extern configuration_int_t hive_cashout_window_seconds_pre_hf17;
      extern configuration_int_t hive_cashout_window_seconds;
      extern configuration_int_t hive_second_cashout_window;
      extern configuration_int_t hive_max_cashout_window_seconds;
      extern configuration_time_t hive_upvote_lockout_hf7;
      extern configuration_uint_t hive_upvote_lockout_seconds;
      extern configuration_time_t hive_upvote_lockout_hf17;
      extern configuration_int_t hive_min_account_creation_fee;
      extern configuration_bigint_t hive_max_account_creation_fee;
      extern configuration_time_t hive_owner_auth_recovery_period;
      extern configuration_time_t hive_account_recovery_request_expiration_period;
      extern configuration_time_t hive_owner_update_limit;
      extern configuration_uint_t hive_owner_auth_history_tracking_start_block_num;
      extern configuration_bigint_t hive_init_supply;
      extern configuration_bigint_t hive_hbd_init_supply;
      extern configuration_int_t hive_proposal_maintenance_period;
      extern configuration_int_t hive_proposal_maintenance_cleanup;
      extern configuration_int_t hive_daily_proposal_maintenance_period;
      extern configuration_time_t hive_governance_vote_expiration_period;
      extern configuration_int_t hive_global_remove_threshold;

      // theese are independent from testnet
      extern configuration_asset_symbol_t vests_symbol;
      extern configuration_asset_symbol_t hive_symbol;
      extern configuration_asset_symbol_t hbd_symbol;
      extern configuration_hardfork_version_t hive_blockchain_hardfork_version;
      extern configuration_uint_t hive_start_vesting_block;
      extern configuration_uint_t hive_start_miner_voting_block;
      extern configuration_str_t hive_init_miner_name;
      extern configuration_int_t hive_num_init_miners;
      extern configuration_timepoint_t hive_init_time;
      extern configuration_int_t hive_hardfork_required_witnesses;
      extern configuration_uint_t hive_max_time_until_expiration;
      extern configuration_uint_t hive_max_memo_size;
      extern configuration_int_t hive_vesting_withdraw_intervals_pre_hf_16;
      extern configuration_int_t hive_vesting_withdraw_intervals;
      extern configuration_int_t hive_vesting_withdraw_interval_seconds;
      extern configuration_int_t hive_max_withdraw_routes;
      extern configuration_int_t hive_max_pending_transfers;
      extern configuration_time_t hive_savings_withdraw_time;
      extern configuration_int_t hive_savings_withdraw_request_limit;
      extern configuration_int_t hive_max_vote_changes;
      extern configuration_int_t hive_reverse_auction_window_seconds_hf6;
      extern configuration_int_t hive_reverse_auction_window_seconds_hf20;
      extern configuration_int_t hive_reverse_auction_window_seconds_hf21;
      extern configuration_int_t hive_early_voting_seconds_hf25;
      extern configuration_int_t hive_mid_voting_seconds_hf25;
      extern configuration_int_t hive_min_vote_interval_sec;
      extern configuration_int_t hive_vote_dust_threshold;
      extern configuration_uint_t hive_downvote_pool_percent_hf21;
      extern configuration_int_t hive_delayed_voting_total_interval_seconds;
      extern configuration_int_t hive_delayed_voting_interval_seconds;
      extern configuration_time_t hive_min_root_comment_interval;
      extern configuration_time_t hive_min_reply_interval;
      extern configuration_time_t hive_min_reply_interval_hf20;
      extern configuration_time_t hive_min_comment_edit_interval;
      extern configuration_uint_t hive_post_average_window;
      extern configuration_ubigint_t hive_post_weight_constant;
      extern configuration_int_t hive_max_account_witness_votes;
      extern configuration_uint_t hive_default_hbd_interest_rate;
      extern configuration_int_t hive_inflation_rate_start_percent;
      extern configuration_int_t hive_inflation_rate_stop_percent;
      extern configuration_int_t hive_inflation_narrowing_period;
      extern configuration_uint_t hive_content_reward_percent_hf16;
      extern configuration_uint_t hive_vesting_fund_percent_hf16;
      extern configuration_int_t hive_proposal_fund_percent_hf0;
      extern configuration_uint_t hive_content_reward_percent_hf21;
      extern configuration_uint_t hive_proposal_fund_percent_hf21;
      extern configuration_hugeint_t hive_hf21_convergent_linear_recent_claims;
      extern configuration_hugeint_t hive_content_constant_hf21;
      extern configuration_uint_t hive_miner_pay_percent;
      extern configuration_int_t hive_max_ration_decay_rate;
      extern configuration_int_t hive_bandwidth_average_window_seconds;
      extern configuration_ubigint_t hive_bandwidth_precision;
      extern configuration_int_t hive_max_comment_depth_pre_hf17;
      extern configuration_int_t hive_max_comment_depth;
      extern configuration_int_t hive_soft_max_comment_depth;
      extern configuration_int_t hive_max_reserve_ratio;
      extern configuration_int_t hive_create_account_with_hive_modifier;
      extern configuration_int_t hive_create_account_delegation_ratio;
      extern configuration_time_t hive_create_account_delegation_time;
      extern configuration_asset_t hive_mining_reward;
      extern configuration_uint_t hive_equihash_n;
      extern configuration_uint_t hive_equihash_k;
      extern configuration_time_t hive_liquidity_timeout_sec;
      extern configuration_time_t hive_min_liquidity_reward_period_sec;
      extern configuration_int_t hive_liquidity_reward_blocks;
      extern configuration_asset_t hive_min_liquidity_reward;
      extern configuration_asset_t hive_min_content_reward;
      extern configuration_asset_t hive_min_curate_reward;
      extern configuration_asset_t hive_min_producer_reward;
      extern configuration_asset_t hive_min_pow_reward;
      extern configuration_asset_t hive_active_challenge_fee;
      extern configuration_asset_t hive_owner_challenge_fee;
      extern configuration_time_t hive_active_challenge_cooldown;
      extern configuration_time_t hive_owner_challenge_cooldown;
      extern configuration_str_t hive_post_reward_fund_name;
      extern configuration_str_t hive_comment_reward_fund_name;
      extern configuration_time_t hive_recent_rshares_decay_time_hf17;
      extern configuration_time_t hive_recent_rshares_decay_time_hf19;
      extern configuration_hugeint_t hive_content_constant_hf0;
      extern configuration_asset_t hive_min_payout_hbd;
      extern configuration_uint_t hive_hbd_stop_percent_hf14;
      extern configuration_uint_t hive_hbd_stop_percent_hf20;
      extern configuration_uint_t hive_hbd_start_percent_hf14;
      extern configuration_uint_t hive_hbd_start_percent_hf20;
      extern configuration_uint_t hive_max_account_name_length;
      extern configuration_uint_t hive_min_permlink_length;
      extern configuration_uint_t hive_max_permlink_length;
      extern configuration_uint_t hive_max_witness_url_length;
      extern configuration_bigint_t hive_max_share_supply;
      extern configuration_bigint_t hive_max_satoshis;
      extern configuration_int_t hive_max_sig_check_depth;
      extern configuration_int_t hive_max_sig_check_accounts;
      extern configuration_int_t hive_min_transaction_size_limit;
      extern configuration_ubigint_t hive_seconds_per_year;
      extern configuration_int_t hive_hbd_interest_compound_interval_sec;
      extern configuration_int_t hive_max_transaction_size;
      extern configuration_uint_t hive_min_block_size_limit;
      extern configuration_uint_t hive_max_block_size;
      extern configuration_uint_t hive_soft_max_block_size;
      extern configuration_uint_t hive_min_block_size;
      extern configuration_int_t hive_blocks_per_hour;
      extern configuration_int_t hive_feed_interval_blocks;
      extern configuration_int_t hive_feed_history_window_pre_hf_16;
      extern configuration_uint_t def_hive_feed_history_window;
      extern configuration_int_t hive_max_feed_age_seconds;
      extern configuration_uint_t hive_min_feeds;
      extern configuration_time_t hive_conversion_delay_pre_hf_16;
      extern configuration_time_t def_hive_conversion_delay;
      extern configuration_int_t hive_min_undo_history;
      extern configuration_uint_t hive_max_undo_history;
      extern configuration_int_t hive_min_transaction_expiration_limit;
      extern configuration_ubigint_t hive_blockchain_precision;
      extern configuration_int_t hive_blockchain_precision_digits;
      extern configuration_ubigint_t hive_max_instance_id;
      extern configuration_uint_t hive_max_authority_membership;
      extern configuration_int_t hive_max_asset_whitelist_authorities;
      extern configuration_int_t hive_max_url_length;
      extern configuration_hugeint_t hive_virtual_schedule_lap_length;
      extern configuration_hugeint_t hive_virtual_schedule_lap_length2;
      extern configuration_int_t hive_initial_vote_power_rate;
      extern configuration_int_t hive_reduced_vote_power_rate;
      extern configuration_int_t hive_max_limit_order_expiration;
      extern configuration_int_t hive_delegation_return_period_hf0;
      extern configuration_int_t hive_delegation_return_period_hf20;
      extern configuration_int_t hive_rd_max_decay_bits;
      extern configuration_uint_t hive_rd_min_decay;
      extern configuration_int_t hive_rd_min_budget;
      extern configuration_uint_t hive_rd_max_decay;
      extern configuration_uint_t hive_account_subsidy_precision;
      extern configuration_uint_t hive_witness_subsidy_budget_percent;
      extern configuration_uint_t hive_witness_subsidy_decay_percent;
      extern configuration_int_t hive_default_account_subsidy_decay;
      extern configuration_int_t hive_default_account_subsidy_budget;
      extern configuration_uint_t hive_decay_backstop_percent;
      extern configuration_uint_t hive_block_generation_postponed_tx_limit;
      extern configuration_time_t hive_pending_transaction_execution_limit;
      extern configuration_uint_t hive_custom_op_id_max_length;
      extern configuration_uint_t hive_custom_op_data_max_length;
      extern configuration_uint_t hive_beneficiary_limit;
      extern configuration_uint_t hive_comment_title_limit;
      extern configuration_int_t hive_one_day_seconds;
      extern configuration_str_t hive_miner_account;
      extern configuration_str_t hive_null_account;
      extern configuration_str_t hive_temp_account;
      extern configuration_str_t hive_proxy_to_self_account;
      extern configuration_accountname_t hive_root_post_parent;
      extern configuration_str_t obsolete_treasury_account;
      extern configuration_str_t new_hive_treasury_account;
      extern configuration_ubigint_t hive_treasury_fee;
      extern configuration_uint_t hive_proposal_subject_max_length;
      extern configuration_uint_t hive_proposal_max_ids_number;
      extern configuration_int_t hive_proposal_fee_increase_days;
      extern configuration_uint_t hive_proposal_fee_increase_days_sec;
      extern configuration_ubigint_t hive_proposal_fee_increase_amount;
      extern configuration_uint_t hive_proposal_conversion_rate;
#ifdef HIVE_ENABLE_SMT
      extern configuration_int_t smt_max_votable_assets;
      extern configuration_int_t smt_vesting_withdraw_interval_seconds;
      extern configuration_int_t smt_upvote_lockout;
      extern configuration_int_t smt_emission_min_interval_seconds;
      extern configuration_uint_t smt_emit_indefinitely;
      extern configuration_int_t smt_max_nominal_votes_per_day;
      extern configuration_uint_t smt_max_votes_per_regeneration;
      extern configuration_int_t smt_default_votes_per_regen_period;
      extern configuration_int_t smt_default_percent_curation_rewards;
#endif

    }
  }
} // hive::protocol::blockchain_configuration
