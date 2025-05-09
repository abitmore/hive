#if defined IS_TEST_NET

#include "condenser_api_fixture.hpp"

#include <hive/plugins/account_history_api/account_history_api_plugin.hpp>
#include <hive/plugins/account_history_api/account_history_api.hpp>
#include <hive/plugins/database_api/database_api_plugin.hpp>
#include <hive/plugins/condenser_api/condenser_api_plugin.hpp>
#include <hive/plugins/condenser_api/condenser_api.hpp>

#include <fc/io/json.hpp>

#include <boost/test/unit_test.hpp>

// account history API -> where it's used in condenser API implementation
//  get_ops_in_block -> get_ops_in_block
//  get_transaction -> ditto get_transaction
//  get_account_history -> ditto get_account_history
//  enum_virtual_ops -> not used

/// Account history pattern goes first in the pair, condenser version pattern follows.
typedef std::vector< std::pair< std::string, std::string > > expected_t;

BOOST_FIXTURE_TEST_SUITE( condenser_get_ops_in_block_tests, condenser_api_fixture );

void compare_get_ops_in_block_results(const condenser_api::get_ops_in_block_return& block_ops,
                                      const account_history::get_ops_in_block_return& ah_block_ops,
                                      uint32_t block_num,
                                      const expected_t& expected_operations )
{
  ilog("block #${num}, ${op} operations from condenser, ${ah} operations from account history",
    ("num", block_num)("op", block_ops.size())("ah", ah_block_ops.ops.size()));
  BOOST_REQUIRE_EQUAL( block_ops.size(), ah_block_ops.ops.size() );
  BOOST_REQUIRE_EQUAL( expected_operations.size(), ah_block_ops.ops.size() );

  auto i_condenser = block_ops.begin();
  auto i_ah = ah_block_ops.ops.begin();
  for (size_t index = 0; i_condenser != block_ops.end(); ++i_condenser, ++i_ah, ++index )
  {
    ilog("result ah is ${result}", ("result", fc::json::to_string(*i_ah)));
    ilog("result condenser is ${result}", ("result", fc::json::to_string(*i_condenser)));

    // Compare operations in their serialized form with expected patterns:
    const auto expected = expected_operations[index];
    BOOST_REQUIRE_EQUAL( expected.first, fc::json::to_string(*i_ah) );
    BOOST_REQUIRE_EQUAL( expected.second, fc::json::to_string(*i_condenser) );
  }
}

void test_get_ops_in_block( condenser_api_fixture& caf, const expected_t& expected_operations,
  const expected_t& expected_virtual_operations, uint32_t block_num )
{
  // Call condenser get_ops_in_block and verify results with result of account history variant.
  // Note that condenser variant calls ah's one with default value of include_reversible = false.
  // Two arguments, second set to false.
  auto block_ops = caf.condenser_api->get_ops_in_block({block_num, false /*only_virtual*/});
  auto ah_block_ops = caf.account_history_api->get_ops_in_block({block_num, false /*only_virtual*/, false /*include_reversible*/});
  compare_get_ops_in_block_results( block_ops, ah_block_ops, block_num, expected_operations );
  // Two arguments, second set to true.
  block_ops = caf.condenser_api->get_ops_in_block({block_num, true /*only_virtual*/});
  ah_block_ops = caf.account_history_api->get_ops_in_block({block_num, true /*only_virtual*/});
  compare_get_ops_in_block_results( block_ops, ah_block_ops, block_num, expected_virtual_operations );
  // Single argument
  block_ops = caf.condenser_api->get_ops_in_block({block_num});
  ah_block_ops = caf.account_history_api->get_ops_in_block({block_num});
  compare_get_ops_in_block_results( block_ops, ah_block_ops, block_num, expected_operations );

  // Too few arguments
  BOOST_REQUIRE_THROW( caf.condenser_api->get_ops_in_block({}), fc::assert_exception );
  // Too many arguments
  BOOST_REQUIRE_THROW( caf.condenser_api->get_ops_in_block({block_num, false /*only_virtual*/, 0 /*redundant arg*/}), fc::assert_exception );
}

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf1 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block on operations of HF1" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 2 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    // Let's check operation that happens only on first hardfork:
    expected_t expected_operations = { { // producer_reward_operation / goes to initminer (in vests)
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":["producer_reward",{"producer":"initminer","vesting_shares":"1.000 TESTS"}]})~"
    }, { // hardfork_operation / HF1
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":{"type":"hardfork_operation","value":{"hardfork_id":1}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":["hardfork",{"hardfork_id":1}]})~"
    }, { // vesting_shares_split_operation / splitting producer reward
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":{"type":"vesting_shares_split_operation","value":{"owner":"initminer","vesting_shares_before_split":{"amount":"1000000","precision":6,"nai":"@@000000037"},"vesting_shares_after_split":{"amount":"1000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":["vesting_shares_split",{"owner":"initminer","vesting_shares_before_split":"1.000000 VESTS","vesting_shares_after_split":"1000000.000000 VESTS"}]})~"
    } }; 
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 2 );

    generate_until_irreversible_block( 21 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    // In block 21 maximum block size is being changed:
    expected_operations = { { // system_warning_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":21,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:03","op":{"type":"system_warning_operation","value":{"message":"Changing maximum block size from 2097152 to 131072"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":21,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:03","op":["system_warning",{"message":"Changing maximum block size from 2097152 to 131072"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":21,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:03","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":21,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:03","op":["producer_reward",{"producer":"initminer","vesting_shares":"1.000 TESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 21 );
  };

  hf1_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf8 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block on operations of HF8" );
  BOOST_TEST_MESSAGE( (db->liquidity_rewards_enabled ? std::string("true") : std::string("false")) );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 65 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    // Let's check operations resulting from matched orders: interest_operation & fill_order_operation
    expected_t expected_operations = { { // limit_order_create_operation
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:03:15","op":{"type":"limit_order_create_operation","value":{"owner":"hf8ben","orderid":1,"amount_to_sell":{"amount":"650","precision":3,"nai":"@@000000013"},"min_to_receive":{"amount":"400","precision":3,"nai":"@@000000021"},"fill_or_kill":false,"expiration":"2016-01-29T00:03:12"}},"operation_id":0})~",
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:03:15","op":["limit_order_create",{"owner":"hf8ben","orderid":1,"amount_to_sell":"0.650 TBD","min_to_receive":"0.400 TESTS","fill_or_kill":false,"expiration":"2016-01-29T00:03:12"}]})~"
      }, { // interest_operation
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"interest_operation","value":{"owner":"hf8ben","interest":{"amount":"1","precision":3,"nai":"@@000000013"},"is_saved_into_hbd_balance":true}},"operation_id":0})~",
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["interest",{"owner":"hf8ben","interest":"0.001 TBD","is_saved_into_hbd_balance":true}]})~"
      }, { // fill_order_operation
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"fill_order_operation","value":{"current_owner":"hf8ben","current_orderid":1,"current_pays":{"amount":"650","precision":3,"nai":"@@000000013"},"open_owner":"hf8alice","open_orderid":3,"open_pays":{"amount":"625","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"d5253238722878b027edb688590275ab3432bacf","block":65,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["fill_order",{"current_owner":"hf8ben","current_orderid":1,"current_pays":"0.650 TBD","open_owner":"hf8alice","open_orderid":3,"open_pays":"0.625 TESTS"}]})~"
      }, { // limit_order_create_operation
      R"~({"trx_id":"0640f0e2b2c18de66407fbcd2f163814520ed923","block":65,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:03:15","op":{"type":"limit_order_create_operation","value":{"owner":"hf8ben","orderid":3,"amount_to_sell":{"amount":"11650","precision":3,"nai":"@@000000021"},"min_to_receive":{"amount":"11400","precision":3,"nai":"@@000000013"},"fill_or_kill":false,"expiration":"2016-01-29T00:03:12"}},"operation_id":0})~",
      R"~({"trx_id":"0640f0e2b2c18de66407fbcd2f163814520ed923","block":65,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:03:15","op":["limit_order_create",{"owner":"hf8ben","orderid":3,"amount_to_sell":"11.650 TESTS","min_to_receive":"11.400 TBD","fill_or_kill":false,"expiration":"2016-01-29T00:03:12"}]})~"
      }, { // fill_order_operation
      R"~({"trx_id":"0640f0e2b2c18de66407fbcd2f163814520ed923","block":65,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"fill_order_operation","value":{"current_owner":"hf8ben","current_orderid":3,"current_pays":{"amount":"11650","precision":3,"nai":"@@000000021"},"open_owner":"hf8alice","open_orderid":4,"open_pays":{"amount":"11923","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0640f0e2b2c18de66407fbcd2f163814520ed923","block":65,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["fill_order",{"current_owner":"hf8ben","current_orderid":3,"current_pays":"11.650 TESTS","open_owner":"hf8alice","open_orderid":4,"open_pays":"11.923 TBD"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"1000000.000000 VESTS"}]})~"
    } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[2], expected_operations[4], expected_operations[5] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 65 );

    generate_until_irreversible_block( 1200 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    // In block 1200 (see HIVE_LIQUIDITY_REWARD_BLOCKS) liquidity_reward_operation appears.
    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1200,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T01:00:00","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1200,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T01:00:00","op":["producer_reward",{"producer":"initminer","vesting_shares":"1000000.000000 VESTS"}]})~"
      }, { // liquidity_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1200,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T01:00:00","op":{"type":"liquidity_reward_operation","value":{"owner":"hf8alice","payout":{"amount":"1200000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1200,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T01:00:00","op":["liquidity_reward",{"owner":"hf8alice","payout":"1200.000 TESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 1200 );
  };

  hf8_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf12 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with hf12_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 3 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );
    // Check the operations spawned by pow (3rd block).
    expected_t expected_operations = { { // pow_operation / creating carol0ah account
#ifdef HIVE_ENABLE_SMT //SMTs have different patterns due to different nonces in pow/pow2
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_operation","value":{"worker_account":"carol0ah","block_id":"0000000206e295c82de8cf026aa103784d5cc90f","nonce":5,"work":{"worker":"STM5Mwq5o6BruTVbCcxkqVKL4eeRm3Jrs5fjRGHshvGUj29FPh7Yr","input":"36336c7dabfb4cc28a6c1a1b012c838629b8b3358b11c7526cfaae0a34f83fcf","signature":"1f7c98f0f679ae20a3a49c8c4799dd4f59f613fa4b7c657581ffa2225adffb5bf042fba3c5ce2911ebd7d00ff19baa99d706175a2cc1574b8a297441c8861b1ead","work":"069554289ab10f11a9bc9e9086e17453f1b528a46fad3269e6d32b4117ac62e3"},"props":{"account_creation_fee":{"amount":"0","precision":3,"nai":"@@000000021"},"maximum_block_size":131072,"hbd_interest_rate":1000}}},"operation_id":0})~",
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["pow",{"worker_account":"carol0ah","block_id":"0000000206e295c82de8cf026aa103784d5cc90f","nonce":5,"work":{"worker":"STM5Mwq5o6BruTVbCcxkqVKL4eeRm3Jrs5fjRGHshvGUj29FPh7Yr","input":"36336c7dabfb4cc28a6c1a1b012c838629b8b3358b11c7526cfaae0a34f83fcf","signature":"1f7c98f0f679ae20a3a49c8c4799dd4f59f613fa4b7c657581ffa2225adffb5bf042fba3c5ce2911ebd7d00ff19baa99d706175a2cc1574b8a297441c8861b1ead","work":"069554289ab10f11a9bc9e9086e17453f1b528a46fad3269e6d32b4117ac62e3"},"props":{"account_creation_fee":"0.000 TESTS","maximum_block_size":131072,"hbd_interest_rate":1000}}]})~"
    }, { // account_created_operation
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"carol0ah","creator":"carol0ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"carol0ah","creator":"carol0ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
    }, { // pow_reward_operation / direct result of pow_operation
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_reward_operation","value":{"worker":"initminer","reward":{"amount":"21000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"3641a2d95c30fb4805dee279399788add889657a","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["pow_reward",{"worker":"initminer","reward":"21.000 TESTS"}]})~"
#else
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_operation","value":{"worker_account":"carol0ah","block_id":"000000029182c87b08eb70059f4bdc22352cbfdb","nonce":40,"work":{"worker":"STM5Mwq5o6BruTVbCcxkqVKL4eeRm3Jrs5fjRGHshvGUj29FPh7Yr","input":"4e4ee151ae1b5317e0fa9835b308163b5fce6ba4b836ecd7dac90acbae5d477a","signature":"208e93e1810b5716c9725fb7e487c271d1eb7bd5674cc5bfb79c01a52b581b269777195d5e3d59750cdf3c10353d77373bfcf6393541cfa1411aa196097cdf90ed","work":"000bae328972f541f9fb4b8a07c52fe15005117434c597c7b37e320397036586"},"props":{"account_creation_fee":{"amount":"0","precision":3,"nai":"@@000000021"},"maximum_block_size":131072,"hbd_interest_rate":1000}}},"operation_id":0})~",
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["pow",{"worker_account":"carol0ah","block_id":"000000029182c87b08eb70059f4bdc22352cbfdb","nonce":40,"work":{"worker":"STM5Mwq5o6BruTVbCcxkqVKL4eeRm3Jrs5fjRGHshvGUj29FPh7Yr","input":"4e4ee151ae1b5317e0fa9835b308163b5fce6ba4b836ecd7dac90acbae5d477a","signature":"208e93e1810b5716c9725fb7e487c271d1eb7bd5674cc5bfb79c01a52b581b269777195d5e3d59750cdf3c10353d77373bfcf6393541cfa1411aa196097cdf90ed","work":"000bae328972f541f9fb4b8a07c52fe15005117434c597c7b37e320397036586"},"props":{"account_creation_fee":"0.000 TESTS","maximum_block_size":131072,"hbd_interest_rate":1000}}]})~"
    }, { // account_created_operation
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"carol0ah","creator":"carol0ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"carol0ah","creator":"carol0ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
    }, { // pow_reward_operation / direct result of pow_operation
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_reward_operation","value":{"worker":"initminer","reward":{"amount":"21000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"70cd2d23ff4c590cd4bc8361cf4e93373fbd9935","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["pow_reward",{"worker":"initminer","reward":"21.000 TESTS"}]})~"
#endif
    }, { // producer_reward_operation
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["producer_reward",{"producer":"initminer","vesting_shares":"1.000 TESTS"}]})~"
    } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[2], expected_operations[3] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 3 );
  };

  hf12_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf13 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with hf13_scenario" );

  auto check_point_1_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 3 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // pow2_operation / first obsolete operation tested here.
#ifdef HIVE_ENABLE_SMT //SMTs have different patterns due to different nonces in pow/pow2
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow2_operation","value":{"work":{"type":"pow2","value":{"input":{"worker_account":"dan0ah","prev_block":"000000024912184de38f50b4c83440750981ccbd","nonce":24},"pow_summary":4196506260}},"new_owner_key":"STM7YJmUoKbPQkrMrZbrgPxDMYJA3uD3utaN3WYRwaFGKYbQ9ftKV","props":{"account_creation_fee":{"amount":"0","precision":3,"nai":"@@000000021"},"maximum_block_size":131072,"hbd_interest_rate":1000}}},"operation_id":0})~",
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["pow2",{"work":[0,{"input":{"worker_account":"dan0ah","prev_block":"000000024912184de38f50b4c83440750981ccbd","nonce":24},"pow_summary":4196506260}],"new_owner_key":"STM7YJmUoKbPQkrMrZbrgPxDMYJA3uD3utaN3WYRwaFGKYbQ9ftKV","props":{"account_creation_fee":"0.000 TESTS","maximum_block_size":131072,"hbd_interest_rate":1000}}]})~"
    }, { // account_created_operation
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"dan0ah","creator":"dan0ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"dan0ah","creator":"dan0ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
    }, { // pow_reward_operation
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_reward_operation","value":{"worker":"initminer","reward":{"amount":"1000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"47fae6df13c7117800ec407bafb1afc62f918cd3","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["pow_reward",{"worker":"initminer","reward":"1000000.000000 VESTS"}]})~"
#else
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow2_operation","value":{"work":{"type":"pow2","value":{"input":{"worker_account":"dan0ah","prev_block":"00000002a8c7664246009c0f84f09687dfb9ec55","nonce":8},"pow_summary":4211230234}},"new_owner_key":"STM7YJmUoKbPQkrMrZbrgPxDMYJA3uD3utaN3WYRwaFGKYbQ9ftKV","props":{"account_creation_fee":{"amount":"0","precision":3,"nai":"@@000000021"},"maximum_block_size":131072,"hbd_interest_rate":1000}}},"operation_id":0})~",
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["pow2",{"work":[0,{"input":{"worker_account":"dan0ah","prev_block":"00000002a8c7664246009c0f84f09687dfb9ec55","nonce":8},"pow_summary":4211230234}],"new_owner_key":"STM7YJmUoKbPQkrMrZbrgPxDMYJA3uD3utaN3WYRwaFGKYbQ9ftKV","props":{"account_creation_fee":"0.000 TESTS","maximum_block_size":131072,"hbd_interest_rate":1000}}]})~"
    }, { // account_created_operation
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"dan0ah","creator":"dan0ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"dan0ah","creator":"dan0ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
    }, { // pow_reward_operation
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"pow_reward_operation","value":{"worker":"initminer","reward":{"amount":"1000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"ff3ec48842cf338b6a5905e817daea3cd12b0dd4","block":3,"trx_in_block":0,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["pow_reward",{"worker":"initminer","reward":"1000000.000000 VESTS"}]})~"
#endif
    }, { // account_create_with_delegation_operation / second obsolete operation tested here.
    R"~({"trx_id":"14509bc4811afed8b9d5a277ca17223d3e9f8c87","block":3,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_create_with_delegation_operation","value":{"fee":{"amount":"0","precision":3,"nai":"@@000000021"},"delegation":{"amount":"100000000000000","precision":6,"nai":"@@000000037"},"creator":"initminer","new_account_name":"edgar0ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8R8maxJxeBMR3JYmap1n3Pypm886oEUjLYdsetzcnPDFpiq3pZ",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8R8maxJxeBMR3JYmap1n3Pypm886oEUjLYdsetzcnPDFpiq3pZ",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8ZCsvwKqttXivgPyJ1MYS4q1r3fBZJh3g1SaBxVbfsqNcmnvD3",1]]},"memo_key":"STM8ZCsvwKqttXivgPyJ1MYS4q1r3fBZJh3g1SaBxVbfsqNcmnvD3","json_metadata":"","extensions":[]}},"operation_id":0})~",
    R"~({"trx_id":"14509bc4811afed8b9d5a277ca17223d3e9f8c87","block":3,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["account_create_with_delegation",{"fee":"0.000 TESTS","delegation":"100000000.000000 VESTS","creator":"initminer","new_account_name":"edgar0ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8R8maxJxeBMR3JYmap1n3Pypm886oEUjLYdsetzcnPDFpiq3pZ",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8R8maxJxeBMR3JYmap1n3Pypm886oEUjLYdsetzcnPDFpiq3pZ",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM8ZCsvwKqttXivgPyJ1MYS4q1r3fBZJh3g1SaBxVbfsqNcmnvD3",1]]},"memo_key":"STM8ZCsvwKqttXivgPyJ1MYS4q1r3fBZJh3g1SaBxVbfsqNcmnvD3","json_metadata":"","extensions":[]}]})~"
    }, { // account_created_operation
    R"~({"trx_id":"14509bc4811afed8b9d5a277ca17223d3e9f8c87","block":3,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"edgar0ah","creator":"initminer","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"100000000000000","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
    R"~({"trx_id":"14509bc4811afed8b9d5a277ca17223d3e9f8c87","block":3,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"edgar0ah","creator":"initminer","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"100000000.000000 VESTS"}]})~"
    }, { // comment_operation
    R"~({"trx_id":"63807c110bc33695772793f61972ac9d29d7689a","block":3,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"comment_operation","value":{"parent_author":"","parent_permlink":"parentpermlink1","author":"edgar0ah","permlink":"permlink1","title":"Title 1","body":"Body 1","json_metadata":""}},"operation_id":0})~",
    R"~({"trx_id":"63807c110bc33695772793f61972ac9d29d7689a","block":3,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["comment",{"parent_author":"","parent_permlink":"parentpermlink1","author":"edgar0ah","permlink":"permlink1","title":"Title 1","body":"Body 1","json_metadata":""}]})~"
    }, { // comment_options_operation"
    R"~({"trx_id":"21b2ce49cfbace306321fabf5783163893e6f841","block":3,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"comment_options_operation","value":{"author":"edgar0ah","permlink":"permlink1","max_accepted_payout":{"amount":"50010","precision":3,"nai":"@@000000013"},"percent_hbd":5100,"allow_votes":true,"allow_curation_rewards":false,"extensions":[]}},"operation_id":0})~",
    R"~({"trx_id":"21b2ce49cfbace306321fabf5783163893e6f841","block":3,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["comment_options",{"author":"edgar0ah","permlink":"permlink1","max_accepted_payout":"50.010 TBD","percent_hbd":5100,"allow_votes":true,"allow_curation_rewards":false,"extensions":[]}]})~"
    }, { // producer_reward_operation
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["producer_reward",{"producer":"initminer","vesting_shares":"1.000 TESTS"}]})~"
    } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[2], expected_operations[4], expected_operations[7] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 3 );
  };

  auto check_point_2_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 25 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // vote_operation
    R"~({"trx_id":"a9fcfc9ce8dabd6e47e7f2e0ce0b24ab03aa1611","block":25,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:15","op":{"type":"vote_operation","value":{"voter":"dan0ah","author":"edgar0ah","permlink":"permlink1","weight":10000}},"operation_id":0})~",
    R"~({"trx_id":"a9fcfc9ce8dabd6e47e7f2e0ce0b24ab03aa1611","block":25,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:15","op":["vote",{"voter":"dan0ah","author":"edgar0ah","permlink":"permlink1","weight":10000}]})~"
    }, { // effective_comment_vote_operation
    R"~({"trx_id":"a9fcfc9ce8dabd6e47e7f2e0ce0b24ab03aa1611","block":25,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":{"type":"effective_comment_vote_operation","value":{"voter":"dan0ah","author":"edgar0ah","permlink":"permlink1","weight":0,"rshares":5100000000,"total_vote_weight":0,"pending_payout":{"amount":"48000","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
    R"~({"trx_id":"a9fcfc9ce8dabd6e47e7f2e0ce0b24ab03aa1611","block":25,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":["effective_comment_vote",{"voter":"dan0ah","author":"edgar0ah","permlink":"permlink1","weight":0,"rshares":5100000000,"total_vote_weight":0,"pending_payout":"48.000 TBD"}]})~"
    }, { // delete_comment_operation
    R"~({"trx_id":"8e3e87e3e1adc6a946973834e4c8b79ee4750585","block":25,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:15","op":{"type":"delete_comment_operation","value":{"author":"edgar0ah","permlink":"permlink1"}},"operation_id":0})~",
    R"~({"trx_id":"8e3e87e3e1adc6a946973834e4c8b79ee4750585","block":25,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:15","op":["delete_comment",{"author":"edgar0ah","permlink":"permlink1"}]})~"
    }, { // ineffective_delete_comment_operation / third and last obsolete operation tested here.
    R"~({"trx_id":"8e3e87e3e1adc6a946973834e4c8b79ee4750585","block":25,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":{"type":"ineffective_delete_comment_operation","value":{"author":"edgar0ah","permlink":"permlink1"}},"operation_id":0})~",
    R"~({"trx_id":"8e3e87e3e1adc6a946973834e4c8b79ee4750585","block":25,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":["ineffective_delete_comment",{"author":"edgar0ah","permlink":"permlink1"}]})~"
    }, { // producer_reward_operation
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":25,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":{"type":"producer_reward_operation","value":{"producer":"dan0ah","vesting_shares":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
    R"~({"trx_id":"0000000000000000000000000000000000000000","block":25,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:15","op":["producer_reward",{"producer":"dan0ah","vesting_shares":"1.000 TESTS"}]})~"
    } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[3], expected_operations[4] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 25 );
  };

  hf13_scenario( check_point_1_tester, check_point_2_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf19 )
{ try {
  BOOST_TEST_MESSAGE( "testing get_ops_in_block with hf19_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    // Make the block with desired vops irreversible.
    generate_until_irreversible_block( 27 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // shutdown_witness_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":{"type":"shutdown_witness_operation","value":{"owner":"ben19ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":["shutdown_witness",{"owner":"ben19ah"}]})~"
      }, { // producer_missed_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":{"type":"producer_missed_operation","value":{"producer":"ben19ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":["producer_missed",{"producer":"ben19ah"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"61314308868","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":27,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":["producer_reward",{"producer":"initminer","vesting_shares":"61314.308868 VESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 27 );
  };
  
  hf19_scenario( check_point_tester );
  
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_hf23 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with hf23_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 6 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // transfer_operation
      R"~({"trx_id":"fc9573c39c9d474b812e4f44beea6cc4d02b981e","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_operation","value":{"from":"steemflower","to":"null","amount":{"amount":"12","precision":3,"nai":"@@000000013"},"memo":"For flowers :)"}},"operation_id":0})~",
      R"~({"trx_id":"fc9573c39c9d474b812e4f44beea6cc4d02b981e","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer",{"from":"steemflower","to":"null","amount":"0.012 TBD","memo":"For flowers :)"}]})~"
      }, { // clear_null_account_balance_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"clear_null_account_balance_operation","value":{"total_cleared":[{"amount":"12","precision":3,"nai":"@@000000013"}]}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["clear_null_account_balance",{"total_cleared":["0.012 TBD"]}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"78022546158","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"78022.546158 VESTS"}]})~"
      }, { // hardfork_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"hardfork_operation","value":{"hardfork_id":23}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["hardfork",{"hardfork_id":23}]})~"
      }, { // hardfork_hive_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"hardfork_hive_operation","value":{"account":"steem","treasury":"steem.dao","other_affected_accounts":[],"hbd_transferred":{"amount":"0","precision":3,"nai":"@@000000013"},"hive_transferred":{"amount":"0","precision":3,"nai":"@@000000021"},"vests_converted":{"amount":"0","precision":6,"nai":"@@000000037"},"total_hive_from_vests":{"amount":"0","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["hardfork_hive",{"account":"steem","treasury":"steem.dao","other_affected_accounts":[],"hbd_transferred":"0.000 TBD","hive_transferred":"0.000 TESTS","vests_converted":"0.000000 VESTS","total_hive_from_vests":"0.000 TESTS"}]})~"
      }, { // hardfork_hive_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"hardfork_hive_operation","value":{"account":"steemflower","treasury":"steem.dao","other_affected_accounts":[],"hbd_transferred":{"amount":"123456789000","precision":3,"nai":"@@000000013"},"hive_transferred":{"amount":"0","precision":3,"nai":"@@000000021"},"vests_converted":{"amount":"98716683119","precision":6,"nai":"@@000000037"},"total_hive_from_vests":{"amount":"132","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["hardfork_hive",{"account":"steemflower","treasury":"steem.dao","other_affected_accounts":[],"hbd_transferred":"123456789.000 TBD","hive_transferred":"0.000 TESTS","vests_converted":"98716.683119 VESTS","total_hive_from_vests":"0.132 TESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[2], expected_operations[3], expected_operations[4], expected_operations[5] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"69832394911","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["producer_reward",{"producer":"initminer","vesting_shares":"69832.394911 VESTS"}]})~"
      }, { // hardfork_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"hardfork_operation","value":{"hardfork_id":24}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["hardfork",{"hardfork_id":24}]})~"
      }, { // hardfork_hive_restore_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"hardfork_hive_restore_operation","value":{"account":"steemflower","treasury":"steem.dao","hbd_transferred":{"amount":"123456789000","precision":3,"nai":"@@000000013"},"hive_transferred":{"amount":"0","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["hardfork_hive_restore",{"account":"steemflower","treasury":"steem.dao","hbd_transferred":"123456789.000 TBD","hive_transferred":"0.000 TESTS"}]})~"
      }, { // consolidate_treasury_balance_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"consolidate_treasury_balance_operation","value":{"total_moved":[{"amount":"132","precision":3,"nai":"@@000000021"},{"amount":"390","precision":3,"nai":"@@000000013"}]}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["consolidate_treasury_balance",{"total_moved":["0.132 TESTS","0.390 TBD"]}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 6 );
  };

  hf23_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_comment_and_reward )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with comment_and_reward_scenario" );

  // Check virtual operations resulting from scenario's 1st set of actions some blocks later:
  auto check_point_1_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 6 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8525584098","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["producer_reward",{"producer":"initminer","vesting_shares":"8525.584098 VESTS"}]})~"
      }, { // curation_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"curation_reward_operation","value":{"curator":"dan0ah","reward":{"amount":"1089380190306","precision":6,"nai":"@@000000037"},"author":"edgar0ah","permlink":"permlink1","payout_must_be_claimed":true}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["curation_reward",{"curator":"dan0ah","reward":"1089380.190306 VESTS","author":"edgar0ah","permlink":"permlink1","payout_must_be_claimed":true}]})~"
      }, { // comment_benefactor_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"comment_benefactor_reward_operation","value":{"benefactor":"dan0ah","author":"edgar0ah","permlink":"permlink1","hbd_payout":{"amount":"287","precision":3,"nai":"@@000000013"},"hive_payout":{"amount":"0","precision":3,"nai":"@@000000021"},"vesting_payout":{"amount":"272818691137","precision":6,"nai":"@@000000037"},"payout_must_be_claimed":true}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["comment_benefactor_reward",{"benefactor":"dan0ah","author":"edgar0ah","permlink":"permlink1","hbd_payout":"0.287 TBD","hive_payout":"0.000 TESTS","vesting_payout":"272818.691137 VESTS","payout_must_be_claimed":true}]})~"
      }, { // author_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"author_reward_operation","value":{"author":"edgar0ah","permlink":"permlink1","hbd_payout":{"amount":"287","precision":3,"nai":"@@000000013"},"hive_payout":{"amount":"0","precision":3,"nai":"@@000000021"},"vesting_payout":{"amount":"272818691137","precision":6,"nai":"@@000000037"},"curators_vesting_payout":{"amount":"1089380190305","precision":6,"nai":"@@000000037"},"payout_must_be_claimed":true}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["author_reward",{"author":"edgar0ah","permlink":"permlink1","hbd_payout":"0.287 TBD","hive_payout":"0.000 TESTS","vesting_payout":"272818.691137 VESTS","curators_vesting_payout":"1089380.190305 VESTS","payout_must_be_claimed":true}]})~"
      }, { // comment_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"comment_reward_operation","value":{"author":"edgar0ah","permlink":"permlink1","payout":{"amount":"2300","precision":3,"nai":"@@000000013"},"author_rewards":575,"total_payout_value":{"amount":"575","precision":3,"nai":"@@000000013"},"curator_payout_value":{"amount":"1150","precision":3,"nai":"@@000000013"},"beneficiary_payout_value":{"amount":"575","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["comment_reward",{"author":"edgar0ah","permlink":"permlink1","payout":"2.300 TBD","author_rewards":575,"total_payout_value":"0.575 TBD","curator_payout_value":"1.150 TBD","beneficiary_payout_value":"0.575 TBD"}]})~"
      }, { // comment_payout_update_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":6,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":{"type":"comment_payout_update_operation","value":{"author":"edgar0ah","permlink":"permlink1"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":6,"trx_in_block":4294967295,"op_in_trx":6,"virtual_op":true,"timestamp":"2016-01-01T00:00:18","op":["comment_payout_update",{"author":"edgar0ah","permlink":"permlink1"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 6 );
  };

  // Check operations resulting from 2nd set of actions:
  auto check_point_2_tester = [ this ]( uint32_t generate_no_further_than )
  { 
    generate_until_irreversible_block( 28 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // claim_reward_balance_operation
      R"~({"trx_id":"daa9aa439e8af76a93cf4b539c3337b1bc303466","block":28,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:24","op":{"type":"claim_reward_balance_operation","value":{"account":"edgar0ah","reward_hive":{"amount":"0","precision":3,"nai":"@@000000021"},"reward_hbd":{"amount":"1","precision":3,"nai":"@@000000013"},"reward_vests":{"amount":"1","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"daa9aa439e8af76a93cf4b539c3337b1bc303466","block":28,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:01:24","op":["claim_reward_balance",{"account":"edgar0ah","reward_hive":"0.000 TESTS","reward_hbd":"0.001 TBD","reward_vests":"0.000001 VESTS"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":28,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"7076153007","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":28,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:24","op":["producer_reward",{"producer":"initminer","vesting_shares":"7076.153007 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[1] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 28 );
  };

  comment_and_reward_scenario( check_point_1_tester, check_point_2_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_convert_and_limit_order )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with convert_and_limit_order_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 5 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // convert_operation
      R"~({"trx_id":"6a79f548e62e1013e6fcbc7442f215cd7879f431","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"convert_operation","value":{"owner":"edgar3ah","requestid":0,"amount":{"amount":"11201","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"6a79f548e62e1013e6fcbc7442f215cd7879f431","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["convert",{"owner":"edgar3ah","requestid":0,"amount":"11.201 TBD"}]})~"
      }, { // collateralized_convert_operation
      R"~({"trx_id":"9f3d234471e6b053be812e180f8f63a4811f462d","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"collateralized_convert_operation","value":{"owner":"carol3ah","requestid":0,"amount":{"amount":"22102","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"9f3d234471e6b053be812e180f8f63a4811f462d","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["collateralized_convert",{"owner":"carol3ah","requestid":0,"amount":"22.102 TESTS"}]})~"
      }, { // collateralized_convert_immediate_conversion_operation
      R"~({"trx_id":"9f3d234471e6b053be812e180f8f63a4811f462d","block":5,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"collateralized_convert_immediate_conversion_operation","value":{"owner":"carol3ah","requestid":0,"hbd_out":{"amount":"10524","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"9f3d234471e6b053be812e180f8f63a4811f462d","block":5,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["collateralized_convert_immediate_conversion",{"owner":"carol3ah","requestid":0,"hbd_out":"10.524 TBD"}]})~"
      }, { // limit_order_create_operation
      R"~({"trx_id":"ad19c50dc64931096ca6bd82574f03330c37c7d9","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"limit_order_create_operation","value":{"owner":"carol3ah","orderid":1,"amount_to_sell":{"amount":"11400","precision":3,"nai":"@@000000021"},"min_to_receive":{"amount":"11650","precision":3,"nai":"@@000000013"},"fill_or_kill":false,"expiration":"2016-01-29T00:00:12"}},"operation_id":0})~",
      R"~({"trx_id":"ad19c50dc64931096ca6bd82574f03330c37c7d9","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["limit_order_create",{"owner":"carol3ah","orderid":1,"amount_to_sell":"11.400 TESTS","min_to_receive":"11.650 TBD","fill_or_kill":false,"expiration":"2016-01-29T00:00:12"}]})~"
      }, { // limit_order_create2_operation
      R"~({"trx_id":"5381fe94856170a97821cf6c1a3518d279111d05","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"limit_order_create2_operation","value":{"owner":"carol3ah","orderid":2,"amount_to_sell":{"amount":"22075","precision":3,"nai":"@@000000021"},"exchange_rate":{"base":{"amount":"10","precision":3,"nai":"@@000000021"},"quote":{"amount":"10","precision":3,"nai":"@@000000013"}},"fill_or_kill":false,"expiration":"2016-01-29T00:00:12"}},"operation_id":0})~",
      R"~({"trx_id":"5381fe94856170a97821cf6c1a3518d279111d05","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["limit_order_create2",{"owner":"carol3ah","orderid":2,"amount_to_sell":"22.075 TESTS","exchange_rate":{"base":"0.010 TESTS","quote":"0.010 TBD"},"fill_or_kill":false,"expiration":"2016-01-29T00:00:12"}]})~"
      }, { // limit_order_create2_operation
      R"~({"trx_id":"1d61550283c545a8c827556348236a260f958c0c","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"limit_order_create2_operation","value":{"owner":"edgar3ah","orderid":3,"amount_to_sell":{"amount":"22075","precision":3,"nai":"@@000000013"},"exchange_rate":{"base":{"amount":"10","precision":3,"nai":"@@000000013"},"quote":{"amount":"10","precision":3,"nai":"@@000000021"}},"fill_or_kill":true,"expiration":"2016-01-29T00:00:12"}},"operation_id":0})~",
      R"~({"trx_id":"1d61550283c545a8c827556348236a260f958c0c","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["limit_order_create2",{"owner":"edgar3ah","orderid":3,"amount_to_sell":"22.075 TBD","exchange_rate":{"base":"0.010 TBD","quote":"0.010 TESTS"},"fill_or_kill":true,"expiration":"2016-01-29T00:00:12"}]})~"
      }, { // fill_order_operation
      R"~({"trx_id":"1d61550283c545a8c827556348236a260f958c0c","block":5,"trx_in_block":4,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"fill_order_operation","value":{"current_owner":"edgar3ah","current_orderid":3,"current_pays":{"amount":"22075","precision":3,"nai":"@@000000013"},"open_owner":"carol3ah","open_orderid":2,"open_pays":{"amount":"22075","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"1d61550283c545a8c827556348236a260f958c0c","block":5,"trx_in_block":4,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["fill_order",{"current_owner":"edgar3ah","current_orderid":3,"current_pays":"22.075 TBD","open_owner":"carol3ah","open_orderid":2,"open_pays":"22.075 TESTS"}]})~"
      }, { // limit_order_cancel_operation
      R"~({"trx_id":"8c57b6cc735c077c0f0e41974e3f74dafab89319","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"limit_order_cancel_operation","value":{"owner":"carol3ah","orderid":1}},"operation_id":0})~",
      R"~({"trx_id":"8c57b6cc735c077c0f0e41974e3f74dafab89319","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["limit_order_cancel",{"owner":"carol3ah","orderid":1}]})~"
      }, { // limit_order_cancelled_operation
      R"~({"trx_id":"8c57b6cc735c077c0f0e41974e3f74dafab89319","block":5,"trx_in_block":5,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"limit_order_cancelled_operation","value":{"seller":"carol3ah","orderid":1,"amount_back":{"amount":"11400","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"8c57b6cc735c077c0f0e41974e3f74dafab89319","block":5,"trx_in_block":5,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["limit_order_cancelled",{"seller":"carol3ah","orderid":1,"amount_back":"11.400 TESTS"}]})~"
      },{ // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8611634248","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"8611.634248 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[2], expected_operations[6], expected_operations[8], expected_operations[9] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    generate_until_irreversible_block( 88 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    // Check virtual operations spawned by the ones obove:
    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"150192950111","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":["producer_reward",{"producer":"initminer","vesting_shares":"150192.950111 VESTS"}]})~"
      }, { // fill_convert_request_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":{"type":"fill_convert_request_operation","value":{"owner":"edgar3ah","requestid":0,"amount_in":{"amount":"11201","precision":3,"nai":"@@000000013"},"amount_out":{"amount":"11201","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":["fill_convert_request",{"owner":"edgar3ah","requestid":0,"amount_in":"11.201 TBD","amount_out":"11.201 TESTS"}]})~"
      }, { // fill_collateralized_convert_request_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":{"type":"fill_collateralized_convert_request_operation","value":{"owner":"carol3ah","requestid":0,"amount_in":{"amount":"11050","precision":3,"nai":"@@000000021"},"amount_out":{"amount":"10524","precision":3,"nai":"@@000000013"},"excess_collateral":{"amount":"11052","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":88,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:04:24","op":["fill_collateralized_convert_request",{"owner":"carol3ah","requestid":0,"amount_in":"11.050 TESTS","amount_out":"10.524 TBD","excess_collateral":"11.052 TESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 88 );
  };

  convert_and_limit_order_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_vesting )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with vesting_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 9 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // transfer_to_vesting_operation
      R"~({"trx_id":"0ace3d6e57043dfde1daf6c00d5d7a6c1e556126","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_to_vesting_operation","value":{"from":"alice4ah","to":"alice4ah","amount":{"amount":"2000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0ace3d6e57043dfde1daf6c00d5d7a6c1e556126","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer_to_vesting",{"from":"alice4ah","to":"alice4ah","amount":"2.000 TESTS"}]})~"
      }, { // transfer_to_vesting_completed_operation
      R"~({"trx_id":"0ace3d6e57043dfde1daf6c00d5d7a6c1e556126","block":5,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_to_vesting_completed_operation","value":{"from_account":"alice4ah","to_account":"alice4ah","hive_vested":{"amount":"2000","precision":3,"nai":"@@000000021"},"vesting_shares_received":{"amount":"1936378093693","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0ace3d6e57043dfde1daf6c00d5d7a6c1e556126","block":5,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["transfer_to_vesting_completed",{"from_account":"alice4ah","to_account":"alice4ah","hive_vested":"2.000 TESTS","vesting_shares_received":"1936378.093693 VESTS"}]})~"
      }, { // set_withdraw_vesting_route_operation
      R"~({"trx_id":"f836d85674c299b307b8168bd3c18d9018de4bb6","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"set_withdraw_vesting_route_operation","value":{"from_account":"alice4ah","to_account":"ben4ah","percent":5000,"auto_vest":true}},"operation_id":0})~",
      R"~({"trx_id":"f836d85674c299b307b8168bd3c18d9018de4bb6","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["set_withdraw_vesting_route",{"from_account":"alice4ah","to_account":"ben4ah","percent":5000,"auto_vest":true}]})~"
      }, { // delegate_vesting_shares_operation
      R"~({"trx_id":"64261f6d3e997f3e9c7846712bde0b5d7da983fb","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"delegate_vesting_shares_operation","value":{"delegator":"alice4ah","delegatee":"carol4ah","vesting_shares":{"amount":"3","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"64261f6d3e997f3e9c7846712bde0b5d7da983fb","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["delegate_vesting_shares",{"delegator":"alice4ah","delegatee":"carol4ah","vesting_shares":"0.000003 VESTS"}]})~"
      }, { // withdraw_vesting_operation
      R"~({"trx_id":"f5db21d976c5d7281e3c09838c6b14a8d78e9913","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"withdraw_vesting_operation","value":{"account":"alice4ah","vesting_shares":{"amount":"123","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"f5db21d976c5d7281e3c09838c6b14a8d78e9913","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["withdraw_vesting",{"account":"alice4ah","vesting_shares":"0.000123 VESTS"}]})~"
      }, { // delegate_vesting_shares_operation
      R"~({"trx_id":"650e7569d7c96f825886f04b4b53168e27c5707a","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"delegate_vesting_shares_operation","value":{"delegator":"alice4ah","delegatee":"carol4ah","vesting_shares":{"amount":"2","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"650e7569d7c96f825886f04b4b53168e27c5707a","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["delegate_vesting_shares",{"delegator":"alice4ah","delegatee":"carol4ah","vesting_shares":"0.000002 VESTS"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8680177266","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"8680.177266 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[6] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8581651955","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["producer_reward",{"producer":"initminer","vesting_shares":"8581.651955 VESTS"}]})~"
      }, { // fill_vesting_withdraw_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"fill_vesting_withdraw_operation","value":{"from_account":"alice4ah","to_account":"ben4ah","withdrawn":{"amount":"4","precision":6,"nai":"@@000000037"},"deposited":{"amount":"4","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["fill_vesting_withdraw",{"from_account":"alice4ah","to_account":"ben4ah","withdrawn":"0.000004 VESTS","deposited":"0.000004 VESTS"}]})~"
      }, { // fill_vesting_withdraw_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"fill_vesting_withdraw_operation","value":{"from_account":"alice4ah","to_account":"alice4ah","withdrawn":{"amount":"5","precision":6,"nai":"@@000000037"},"deposited":{"amount":"0","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["fill_vesting_withdraw",{"from_account":"alice4ah","to_account":"alice4ah","withdrawn":"0.000005 VESTS","deposited":"0.000 TESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 8 );

    expected_operations = { { // return_vesting_delegation_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":9,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:27","op":{"type":"return_vesting_delegation_operation","value":{"account":"alice4ah","vesting_shares":{"amount":"1","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":9,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:27","op":["return_vesting_delegation",{"account":"alice4ah","vesting_shares":"0.000001 VESTS"}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":9,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:27","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8549473854","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":9,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:27","op":["producer_reward",{"producer":"initminer","vesting_shares":"8549.473854 VESTS"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 9 );
  };

  vesting_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_witness )
{ try {
  // We need to set HIVE_GENESIS_TIME to very low value to force updating governance_vote_expiration_ts to low value.
  // (see the conditional instruction in update_governance_vote_expiration_ts that we don't want to trigger)
  configuration::hardfork_schedule_t ancient_hf25_schedule = { hardfork_schedule_item_t{ 25, 1 } };
  configuration_data.set_hardfork_schedule( fc::time_point_sec( 0 ), ancient_hf25_schedule );
  // Additionally shorten governance voting expiration period to acquire expired_account_notification_operation in block 8.
  configuration_data.set_governance_voting_related_values( 60*60*24*1, 15 );

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with witness_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 8 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // witness_update_operation
      R"~({"trx_id":"68dc811e40bbf5298cc49906ac0b07f2b8d91d50","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"witness_update_operation","value":{"owner":"alice5ah","url":"foo.bar","block_signing_key":"STM5x2Aroso4sW7B7wp191Zvzs4mV7vH12g9yccbb2rV7kVUwTC5D","props":{"account_creation_fee":{"amount":"0","precision":3,"nai":"@@000000021"},"maximum_block_size":131072,"hbd_interest_rate":1000},"fee":{"amount":"1000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"68dc811e40bbf5298cc49906ac0b07f2b8d91d50","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["witness_update",{"owner":"alice5ah","url":"foo.bar","block_signing_key":"STM5x2Aroso4sW7B7wp191Zvzs4mV7vH12g9yccbb2rV7kVUwTC5D","props":{"account_creation_fee":"0.000 TESTS","maximum_block_size":131072,"hbd_interest_rate":1000},"fee":"1.000 TESTS"}]})~"
      }, { // feed_publish_operation
      R"~({"trx_id":"515f87df80219a8bc5aee9950e3c0f9db74262a8","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"feed_publish_operation","value":{"publisher":"alice5ah","exchange_rate":{"base":{"amount":"1000","precision":3,"nai":"@@000000013"},"quote":{"amount":"1000","precision":3,"nai":"@@000000021"}}}},"operation_id":0})~",
      R"~({"trx_id":"515f87df80219a8bc5aee9950e3c0f9db74262a8","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["feed_publish",{"publisher":"alice5ah","exchange_rate":{"base":"1.000 TBD","quote":"1.000 TESTS"}}]})~"
      }, { // account_witness_proxy_operation
      R"~({"trx_id":"e3fe9799b02e6ed6bee030ab7e1315f2a6e6a6d4","block":4,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"account_witness_proxy_operation","value":{"account":"ben5ah","proxy":"carol5ah"}},"operation_id":0})~",
      R"~({"trx_id":"e3fe9799b02e6ed6bee030ab7e1315f2a6e6a6d4","block":4,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["account_witness_proxy",{"account":"ben5ah","proxy":"carol5ah"}]})~"
      }, { // account_witness_vote_operation
      R"~({"trx_id":"a0d6ca658067cfeefb822933ae04a1687c5d235d","block":4,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"account_witness_vote_operation","value":{"account":"carol5ah","witness":"alice5ah","approve":true}},"operation_id":0})~",
      R"~({"trx_id":"a0d6ca658067cfeefb822933ae04a1687c5d235d","block":4,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["account_witness_vote",{"account":"carol5ah","witness":"alice5ah","approve":true}]})~"
      }, { // account_witness_vote_operation
      R"~({"trx_id":"c755a1dac4036f634d4677d696f8dff9d19c0053","block":4,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"account_witness_vote_operation","value":{"account":"carol5ah","witness":"alice5ah","approve":false}},"operation_id":0})~",
      R"~({"trx_id":"c755a1dac4036f634d4677d696f8dff9d19c0053","block":4,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["account_witness_vote",{"account":"carol5ah","witness":"alice5ah","approve":false}]})~"
      }, { // witness_set_properties_operation
      R"~({"trx_id":"3e7106641de9e1cc93a415ad73169dc62b3fa89c","block":4,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"witness_set_properties_operation","value":{"owner":"alice5ah","props":[["hbd_interest_rate","00000000"],["key","028bb6e3bfd8633279430bd6026a1178e8e311fe4700902f647856a9f32ae82a8b"]],"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"3e7106641de9e1cc93a415ad73169dc62b3fa89c","block":4,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["witness_set_properties",{"owner":"alice5ah","props":[["hbd_interest_rate","00000000"],["key","028bb6e3bfd8633279430bd6026a1178e8e311fe4700902f647856a9f32ae82a8b"]],"extensions":[]}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8713701421","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":["producer_reward",{"producer":"initminer","vesting_shares":"8713.701421 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[6] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 4 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8397109606","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["producer_reward",{"producer":"initminer","vesting_shares":"8397.109606 VESTS"}]})~"
      }, { // proxy_cleared_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"proxy_cleared_operation","value":{"account":"ben5ah","proxy":"carol5ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["proxy_cleared",{"account":"ben5ah","proxy":"carol5ah"}]})~"
      }, { // expired_account_notification_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"expired_account_notification_operation","value":{"account":"ben5ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["expired_account_notification",{"account":"ben5ah"}]})~"
      }, { // expired_account_notification_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":{"type":"expired_account_notification_operation","value":{"account":"carol5ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":8,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:00:24","op":["expired_account_notification",{"account":"carol5ah"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 8 );
  };
  
  witness_scenario( check_point_tester );

  configuration_data.reset_hardfork_schedule();
  configuration_data.reset_governance_voting_values();

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_escrow_and_savings )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with escrow_and_savings_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 5 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // escrow_transfer_operation
      R"~({"trx_id":"d991732e509c73b78ef79873cded63346f6c0201","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_transfer_operation","value":{"from":"alice6ah","to":"ben6ah","hbd_amount":{"amount":"0","precision":3,"nai":"@@000000013"},"hive_amount":{"amount":"71","precision":3,"nai":"@@000000021"},"escrow_id":30,"agent":"carol6ah","fee":{"amount":"1","precision":3,"nai":"@@000000021"},"json_meta":"","ratification_deadline":"2016-01-01T00:00:42","escrow_expiration":"2016-01-01T00:01:12"}},"operation_id":0})~",
      R"~({"trx_id":"d991732e509c73b78ef79873cded63346f6c0201","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_transfer",{"from":"alice6ah","to":"ben6ah","hbd_amount":"0.000 TBD","hive_amount":"0.071 TESTS","escrow_id":30,"agent":"carol6ah","fee":"0.001 TESTS","json_meta":"","ratification_deadline":"2016-01-01T00:00:42","escrow_expiration":"2016-01-01T00:01:12"}]})~"
      }, { // escrow_transfer_operation
      R"~({"trx_id":"d0500bd052ea89116eaf186fc39af892d178a608","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_transfer_operation","value":{"from":"alice6ah","to":"ben6ah","hbd_amount":{"amount":"7","precision":3,"nai":"@@000000013"},"hive_amount":{"amount":"0","precision":3,"nai":"@@000000021"},"escrow_id":31,"agent":"carol6ah","fee":{"amount":"1","precision":3,"nai":"@@000000021"},"json_meta":"{\"go\":\"now\"}","ratification_deadline":"2016-01-01T00:00:42","escrow_expiration":"2016-01-01T00:01:12"}},"operation_id":0})~",
      R"~({"trx_id":"d0500bd052ea89116eaf186fc39af892d178a608","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_transfer",{"from":"alice6ah","to":"ben6ah","hbd_amount":"0.007 TBD","hive_amount":"0.000 TESTS","escrow_id":31,"agent":"carol6ah","fee":"0.001 TESTS","json_meta":"{\"go\":\"now\"}","ratification_deadline":"2016-01-01T00:00:42","escrow_expiration":"2016-01-01T00:01:12"}]})~"
      }, { // escrow_approve_operation
      R"~({"trx_id":"b10e747da05fe4b9c28106e965bb48c152f13e6b","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_approve_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"carol6ah","escrow_id":30,"approve":true}},"operation_id":0})~",
      R"~({"trx_id":"b10e747da05fe4b9c28106e965bb48c152f13e6b","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_approve",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"carol6ah","escrow_id":30,"approve":true}]})~"
      }, { // escrow_approve_operation
      R"~({"trx_id":"5697a8fe6d4e3e2b367443d0e4cc2e0df60306c5","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_approve_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"ben6ah","escrow_id":30,"approve":true}},"operation_id":0})~",
      R"~({"trx_id":"5697a8fe6d4e3e2b367443d0e4cc2e0df60306c5","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_approve",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"ben6ah","escrow_id":30,"approve":true}]})~"
      }, { // escrow_approved_operation
      R"~({"trx_id":"5697a8fe6d4e3e2b367443d0e4cc2e0df60306c5","block":5,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_approved_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","escrow_id":30,"fee":{"amount":"1","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"5697a8fe6d4e3e2b367443d0e4cc2e0df60306c5","block":5,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["escrow_approved",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","escrow_id":30,"fee":"0.001 TESTS"}]})~"
      }, { // escrow_release_operation
      R"~({"trx_id":"b151c38de8ba594a5a53a648ce03d49890ce4cf9","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_release_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"alice6ah","receiver":"ben6ah","escrow_id":30,"hbd_amount":{"amount":"0","precision":3,"nai":"@@000000013"},"hive_amount":{"amount":"13","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"b151c38de8ba594a5a53a648ce03d49890ce4cf9","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_release",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"alice6ah","receiver":"ben6ah","escrow_id":30,"hbd_amount":"0.000 TBD","hive_amount":"0.013 TESTS"}]})~"
      }, { // escrow_dispute_operation
      R"~({"trx_id":"0e98dacbf33b7454b6928abe85638fac6ee86e00","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_dispute_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"ben6ah","escrow_id":30}},"operation_id":0})~",
      R"~({"trx_id":"0e98dacbf33b7454b6928abe85638fac6ee86e00","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_dispute",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"ben6ah","escrow_id":30}]})~"
      }, { // escrow_approve_operation
      R"~({"trx_id":"25f39679ea1a87ab8cc879ecdb46ea253252cdb6","block":5,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_approve_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"carol6ah","escrow_id":31,"approve":false}},"operation_id":0})~",
      R"~({"trx_id":"25f39679ea1a87ab8cc879ecdb46ea253252cdb6","block":5,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["escrow_approve",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","who":"carol6ah","escrow_id":31,"approve":false}]})~"
      }, { // escrow_rejected_operation
      R"~({"trx_id":"25f39679ea1a87ab8cc879ecdb46ea253252cdb6","block":5,"trx_in_block":6,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"escrow_rejected_operation","value":{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","escrow_id":31,"hbd_amount":{"amount":"7","precision":3,"nai":"@@000000013"},"hive_amount":{"amount":"0","precision":3,"nai":"@@000000021"},"fee":{"amount":"1","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"25f39679ea1a87ab8cc879ecdb46ea253252cdb6","block":5,"trx_in_block":6,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["escrow_rejected",{"from":"alice6ah","to":"ben6ah","agent":"carol6ah","escrow_id":31,"hbd_amount":"0.007 TBD","hive_amount":"0.000 TESTS","fee":"0.001 TESTS"}]})~"
      }, { // transfer_to_savings_operation
      R"~({"trx_id":"3182360e45d5898072aefe5899dca6f0993b7975","block":5,"trx_in_block":7,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_to_savings_operation","value":{"from":"alice6ah","to":"ben6ah","amount":{"amount":"9","precision":3,"nai":"@@000000021"},"memo":"ah savings"}},"operation_id":0})~",
      R"~({"trx_id":"3182360e45d5898072aefe5899dca6f0993b7975","block":5,"trx_in_block":7,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer_to_savings",{"from":"alice6ah","to":"ben6ah","amount":"0.009 TESTS","memo":"ah savings"}]})~"
      }, { // transfer_from_savings_operation
      R"~({"trx_id":"1fe983436e2de26bd357d704b3c2f5efe3dd32a3","block":5,"trx_in_block":8,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_from_savings_operation","value":{"from":"ben6ah","request_id":0,"to":"alice6ah","amount":{"amount":"6","precision":3,"nai":"@@000000021"},"memo":""}},"operation_id":0})~",
      R"~({"trx_id":"1fe983436e2de26bd357d704b3c2f5efe3dd32a3","block":5,"trx_in_block":8,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer_from_savings",{"from":"ben6ah","request_id":0,"to":"alice6ah","amount":"0.006 TESTS","memo":""}]})~"
      }, { // transfer_from_savings_operation
      R"~({"trx_id":"e70a95aca9c0b1401a6ee887f0f1ecbfd82c81d2","block":5,"trx_in_block":9,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_from_savings_operation","value":{"from":"ben6ah","request_id":1,"to":"carol6ah","amount":{"amount":"3","precision":3,"nai":"@@000000021"},"memo":""}},"operation_id":0})~",
      R"~({"trx_id":"e70a95aca9c0b1401a6ee887f0f1ecbfd82c81d2","block":5,"trx_in_block":9,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer_from_savings",{"from":"ben6ah","request_id":1,"to":"carol6ah","amount":"0.003 TESTS","memo":""}]})~"
      }, { // cancel_transfer_from_savings_operation
      R"~({"trx_id":"3cd627f3872f583d17cef4ab90c37850a4af7b57","block":5,"trx_in_block":10,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"cancel_transfer_from_savings_operation","value":{"from":"ben6ah","request_id":0}},"operation_id":0})~",
      R"~({"trx_id":"3cd627f3872f583d17cef4ab90c37850a4af7b57","block":5,"trx_in_block":10,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["cancel_transfer_from_savings",{"from":"ben6ah","request_id":0}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8631556303","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"8631.556303 VESTS"}]})~"
    } };
    expected_t expected_virtual_operations = { expected_operations[4], expected_operations[8], expected_operations[13] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    generate_until_irreversible_block( 11 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":11,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:33","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8179054903","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":11,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:33","op":["producer_reward",{"producer":"initminer","vesting_shares":"8179.054903 VESTS"}]})~"
      }, { // fill_transfer_from_savings_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":11,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:33","op":{"type":"fill_transfer_from_savings_operation","value":{"from":"ben6ah","to":"carol6ah","amount":{"amount":"3","precision":3,"nai":"@@000000021"},"request_id":1,"memo":""}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":11,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:33","op":["fill_transfer_from_savings",{"from":"ben6ah","to":"carol6ah","amount":"0.003 TESTS","request_id":1,"memo":""}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 11 );
  };

  escrow_and_savings_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_CASE( get_ops_in_block_proposal )
{ try {
  configuration_data.set_governance_voting_related_values( 90, 60*60*24*5 );
  configuration_data.set_proposal_related_values( 30 );

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with proposal_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 42 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8884501480","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":["producer_reward",{"producer":"initminer","vesting_shares":"8884.501480 VESTS"}]})~"
      }, { // dhf_funding_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":{"type":"dhf_funding_operation","value":{"treasury":"hive.fund","additional_funds":{"amount":"9","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":2,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:06","op":["dhf_funding",{"treasury":"hive.fund","additional_funds":"0.009 TBD"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 2 );

    expected_operations = { { // transfer_operation
      R"~({"trx_id":"5c9221cfc5835c86be71425d55cdee6e334d6967","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"transfer_operation","value":{"from":"carol7ah","to":"hive.fund","amount":{"amount":"30000333","precision":3,"nai":"@@000000021"},"memo":""}},"operation_id":0})~",
      R"~({"trx_id":"5c9221cfc5835c86be71425d55cdee6e334d6967","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["transfer",{"from":"carol7ah","to":"hive.fund","amount":"30000.333 TESTS","memo":""}]})~"
      }, { // dhf_conversion_operation
      R"~({"trx_id":"5c9221cfc5835c86be71425d55cdee6e334d6967","block":5,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"dhf_conversion_operation","value":{"treasury":"hive.fund","hive_amount_in":{"amount":"30000333","precision":3,"nai":"@@000000021"},"hbd_amount_out":{"amount":"30000333","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"5c9221cfc5835c86be71425d55cdee6e334d6967","block":5,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["dhf_conversion",{"treasury":"hive.fund","hive_amount_in":"30000.333 TESTS","hbd_amount_out":"30000.333 TBD"}]})~"
      }, { // comment_operation
      R"~({"trx_id":"3a20708685a9510a1a03a58499cc2a0cd42985d8","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"comment_operation","value":{"parent_author":"","parent_permlink":"test","author":"alice7ah","permlink":"permlink0","title":"title","body":"body","json_metadata":""}},"operation_id":0})~",
      R"~({"trx_id":"3a20708685a9510a1a03a58499cc2a0cd42985d8","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["comment",{"parent_author":"","parent_permlink":"test","author":"alice7ah","permlink":"permlink0","title":"title","body":"body","json_metadata":""}]})~"
      }, { // create_proposal_operation
      R"~({"trx_id":"3984630fffc862924860434b613595a3ef6f4925","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"create_proposal_operation","value":{"creator":"alice7ah","receiver":"ben7ah","start_date":"2015-12-31T00:00:12","end_date":"2016-01-03T00:00:12","daily_pay":{"amount":"100","precision":3,"nai":"@@000000013"},"subject":"0","permlink":"permlink0","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"3984630fffc862924860434b613595a3ef6f4925","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["create_proposal",{"creator":"alice7ah","receiver":"ben7ah","start_date":"2015-12-31T00:00:12","end_date":"2016-01-03T00:00:12","daily_pay":"0.100 TBD","subject":"0","permlink":"permlink0","extensions":[]}]})~"
      }, { // proposal_fee_operation
      R"~({"trx_id":"3984630fffc862924860434b613595a3ef6f4925","block":5,"trx_in_block":2,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"proposal_fee_operation","value":{"creator":"alice7ah","treasury":"hive.fund","proposal_id":0,"fee":{"amount":"10000","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"3984630fffc862924860434b613595a3ef6f4925","block":5,"trx_in_block":2,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["proposal_fee",{"creator":"alice7ah","treasury":"hive.fund","proposal_id":0,"fee":"10.000 TBD"}]})~"
      }, { // update_proposal_operation
      R"~({"trx_id":"4aa8d1fc867bfaf9c50f3272f5899a2b153d4500","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"update_proposal_operation","value":{"proposal_id":0,"creator":"alice7ah","daily_pay":{"amount":"80","precision":3,"nai":"@@000000013"},"subject":"new subject","permlink":"permlink0","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"4aa8d1fc867bfaf9c50f3272f5899a2b153d4500","block":5,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["update_proposal",{"proposal_id":0,"creator":"alice7ah","daily_pay":"0.080 TBD","subject":"new subject","permlink":"permlink0","extensions":[]}]})~"
      }, { // update_proposal_votes_operation
      R"~({"trx_id":"7407474904e92ad5c9e27c8b611defc7900bc8a7","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"update_proposal_votes_operation","value":{"voter":"carol7ah","proposal_ids":[0],"approve":false,"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"7407474904e92ad5c9e27c8b611defc7900bc8a7","block":5,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["update_proposal_votes",{"voter":"carol7ah","proposal_ids":[0],"approve":false,"extensions":[]}]})~"
      }, { // remove_proposal_operation
      R"~({"trx_id":"e7a3be6db61976dcb884009a0aad7b01ae5c5221","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"remove_proposal_operation","value":{"proposal_owner":"alice7ah","proposal_ids":[0],"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"e7a3be6db61976dcb884009a0aad7b01ae5c5221","block":5,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["remove_proposal",{"proposal_owner":"alice7ah","proposal_ids":[0],"extensions":[]}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8631556303","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"8631.556303 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[4], expected_operations[8] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"6989046794","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":["producer_reward",{"producer":"initminer","vesting_shares":"6989.046794 VESTS"}]})~"
      }, { // dhf_funding_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":{"type":"dhf_funding_operation","value":{"treasury":"hive.fund","additional_funds":{"amount":"90","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":["dhf_funding",{"treasury":"hive.fund","additional_funds":"0.090 TBD"}]})~"
      }, { // delayed_voting_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":{"type":"delayed_voting_operation","value":{"voter":"alice7ah","votes":98716683119}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":["delayed_voting",{"voter":"alice7ah","votes":98716683119}]})~"
      }, { // delayed_voting_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":{"type":"delayed_voting_operation","value":{"voter":"ben7ah","votes":98716683119}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":4,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":["delayed_voting",{"voter":"ben7ah","votes":98716683119}]})~"
      }, { // delayed_voting_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":{"type":"delayed_voting_operation","value":{"voter":"carol7ah","votes":98716683119}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":32,"trx_in_block":4294967295,"op_in_trx":5,"virtual_op":true,"timestamp":"2016-01-01T00:01:36","op":["delayed_voting",{"voter":"carol7ah","votes":98716683119}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 32 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"168545291237","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":["producer_reward",{"producer":"initminer","vesting_shares":"168545.291237 VESTS"}]})~"
      }, { // dhf_funding_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":{"type":"dhf_funding_operation","value":{"treasury":"hive.fund","additional_funds":{"amount":"90","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":["dhf_funding",{"treasury":"hive.fund","additional_funds":"0.090 TBD"}]})~"
      }, { // proposal_pay_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":{"type":"proposal_pay_operation","value":{"proposal_id":1,"receiver":"ben7ah","payer":"hive.fund","payment":{"amount":"0","precision":3,"nai":"@@000000013"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":42,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:02:06","op":["proposal_pay",{"proposal_id":1,"receiver":"ben7ah","payer":"hive.fund","payment":"0.000 TBD"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 42 );
  };

  proposal_scenario( check_point_tester );

  configuration_data.reset_governance_voting_values();
  configuration_data.reset_proposal_values();
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_account )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with account_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 65 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // account_update_operation
      R"~({"trx_id":"c585a5c5811c8a0a9bbbdcccaf9fdd25d2cf4f2d","block":46,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"account_update_operation","value":{"account":"alice8ah","memo_key":"STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx","json_metadata":"\"{\"position\":\"top\"}\""}},"operation_id":0})~",
      R"~({"trx_id":"c585a5c5811c8a0a9bbbdcccaf9fdd25d2cf4f2d","block":46,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["account_update",{"account":"alice8ah","memo_key":"STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx","json_metadata":"\"{\"position\":\"top\"}\""}]})~"
      }, { // claim_account_operation
      R"~({"trx_id":"3e760e26dd8837a42b37f79b1e91ad015b20cf5e","block":46,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"claim_account_operation","value":{"creator":"alice8ah","fee":{"amount":"0","precision":3,"nai":"@@000000021"},"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"3e760e26dd8837a42b37f79b1e91ad015b20cf5e","block":46,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["claim_account",{"creator":"alice8ah","fee":"0.000 TESTS","extensions":[]}]})~"
      }, { // create_claimed_account_operation
      R"~({"trx_id":"06fbb6ab0784afedb45a06b70f41acef7203d210","block":46,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"create_claimed_account_operation","value":{"creator":"alice8ah","new_account_name":"ben8ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7F7N2n8RYwoBkS3rCtwDkaTdnbkctCm3V3fn2cDvdx988XMNZv",1]]},"memo_key":"STM7F7N2n8RYwoBkS3rCtwDkaTdnbkctCm3V3fn2cDvdx988XMNZv","json_metadata":"\"{\"go\":\"now\"}\"","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"06fbb6ab0784afedb45a06b70f41acef7203d210","block":46,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["create_claimed_account",{"creator":"alice8ah","new_account_name":"ben8ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7F7N2n8RYwoBkS3rCtwDkaTdnbkctCm3V3fn2cDvdx988XMNZv",1]]},"memo_key":"STM7F7N2n8RYwoBkS3rCtwDkaTdnbkctCm3V3fn2cDvdx988XMNZv","json_metadata":"\"{\"go\":\"now\"}\"","extensions":[]}]})~"
      }, { // account_created_operation
      R"~({"trx_id":"06fbb6ab0784afedb45a06b70f41acef7203d210","block":46,"trx_in_block":2,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":{"type":"account_created_operation","value":{"new_account_name":"ben8ah","creator":"alice8ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"06fbb6ab0784afedb45a06b70f41acef7203d210","block":46,"trx_in_block":2,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":["account_created",{"new_account_name":"ben8ah","creator":"alice8ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
      }, { // transfer_to_vesting_operation
      R"~({"trx_id":"58912caa28fd404f51c087d19700847341f98314","block":46,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"transfer_to_vesting_operation","value":{"from":"initminer","to":"ben8ah","amount":{"amount":"1000000","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"58912caa28fd404f51c087d19700847341f98314","block":46,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["transfer_to_vesting",{"from":"initminer","to":"ben8ah","amount":"1000.000 TESTS"}]})~"
      }, { // transfer_to_vesting_completed_operation
      R"~({"trx_id":"58912caa28fd404f51c087d19700847341f98314","block":46,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":{"type":"transfer_to_vesting_completed_operation","value":{"from_account":"initminer","to_account":"ben8ah","hive_vested":{"amount":"1000000","precision":3,"nai":"@@000000021"},"vesting_shares_received":{"amount":"908200580279667","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"58912caa28fd404f51c087d19700847341f98314","block":46,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":["transfer_to_vesting_completed",{"from_account":"initminer","to_account":"ben8ah","hive_vested":"1000.000 TESTS","vesting_shares_received":"908200580.279667 VESTS"}]})~"
      }, { // change_recovery_account_operation
      R"~({"trx_id":"049b20d348ac2a9aa04c58db40de22269b04cce5","block":46,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"change_recovery_account_operation","value":{"account_to_recover":"ben8ah","new_recovery_account":"initminer","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"049b20d348ac2a9aa04c58db40de22269b04cce5","block":46,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["change_recovery_account",{"account_to_recover":"ben8ah","new_recovery_account":"initminer","extensions":[]}]})~"
      }, { // account_update2_operation
      R"~({"trx_id":"2e71c57370eab954d4522311e2eb591b807e2c5c","block":46,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"account_update2_operation","value":{"account":"ben8ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5Wteiod1TC7Wraux73AZvMsjrA5b3E1LTsv1dZa3CB9V4LhXTN",1]]},"memo_key":"STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S","json_metadata":"\"{\"success\":true}\"","posting_json_metadata":"\"{\"winner\":\"me\"}\"","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"2e71c57370eab954d4522311e2eb591b807e2c5c","block":46,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["account_update2",{"account":"ben8ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5Wteiod1TC7Wraux73AZvMsjrA5b3E1LTsv1dZa3CB9V4LhXTN",1]]},"memo_key":"STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S","json_metadata":"\"{\"success\":true}\"","posting_json_metadata":"\"{\"winner\":\"me\"}\"","extensions":[]}]})~"
      }, { // request_account_recovery_operation
      R"~({"trx_id":"2a1f4e331c8d3451837dd4445e5310ce37a4a2b0","block":46,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"request_account_recovery_operation","value":{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx",1]]},"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"2a1f4e331c8d3451837dd4445e5310ce37a4a2b0","block":46,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["request_account_recovery",{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx",1]]},"extensions":[]}]})~"
      }, { // recover_account_operation
      R"~({"trx_id":"2b9456a25d3e86df2cde6cfd3c77f445358b143b","block":46,"trx_in_block":7,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"recover_account_operation","value":{"account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx",1]]},"recent_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"2b9456a25d3e86df2cde6cfd3c77f445358b143b","block":46,"trx_in_block":7,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["recover_account",{"account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM6gpma8a74jnePfFaR5AeAncusvfEkKLQ6webzKdRLrxcuEsDDx",1]]},"recent_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"extensions":[]}]})~"
      }, { // request_account_recovery_operation
      R"~({"trx_id":"b0f230b70f895565bc8d2b6094882c7270ebd642","block":46,"trx_in_block":8,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"request_account_recovery_operation","value":{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"b0f230b70f895565bc8d2b6094882c7270ebd642","block":46,"trx_in_block":8,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["request_account_recovery",{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",1]]},"extensions":[]}]})~"
      }, { // request_account_recovery_operation
      R"~({"trx_id":"97f6adb04b45f75ea0014a497c3fae00707f3ab5","block":46,"trx_in_block":9,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"request_account_recovery_operation","value":{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":0,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",0]]},"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"97f6adb04b45f75ea0014a497c3fae00707f3ab5","block":46,"trx_in_block":9,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["request_account_recovery",{"recovery_account":"alice8ah","account_to_recover":"ben8ah","new_owner_authority":{"weight_threshold":0,"account_auths":[],"key_auths":[["STM7NVJSvcpYMSVkt1mzJ7uo8Ema7uwsuSypk9wjNjEK9cDyN6v3S",0]]},"extensions":[]}]})~"
      }, { // change_recovery_account_operation
      R"~({"trx_id":"5cdf72f7546c7d90a8c45c9603ab8f72957dcd43","block":46,"trx_in_block":10,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"change_recovery_account_operation","value":{"account_to_recover":"alice8ah","new_recovery_account":"ben8ah","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"5cdf72f7546c7d90a8c45c9603ab8f72957dcd43","block":46,"trx_in_block":10,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["change_recovery_account",{"account_to_recover":"alice8ah","new_recovery_account":"ben8ah","extensions":[]}]})~"
      }, { // change_recovery_account_operation
      R"~({"trx_id":"7baf470297e5416eaa25f2bf18309f829c3ea283","block":46,"trx_in_block":11,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":{"type":"change_recovery_account_operation","value":{"account_to_recover":"alice8ah","new_recovery_account":"initminer","extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"7baf470297e5416eaa25f2bf18309f829c3ea283","block":46,"trx_in_block":11,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:02:18","op":["change_recovery_account",{"account_to_recover":"alice8ah","new_recovery_account":"initminer","extensions":[]}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":46,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"209791628548","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":46,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:02:18","op":["producer_reward",{"producer":"initminer","vesting_shares":"209791.628548 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[3], expected_operations[5], expected_operations[14] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 46 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"209740354761","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"209740.354761 VESTS"}]})~"
      }, { // changed_recovery_account_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":{"type":"changed_recovery_account_operation","value":{"account":"ben8ah","old_recovery_account":"alice8ah","new_recovery_account":"initminer"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":65,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:03:15","op":["changed_recovery_account",{"account":"ben8ah","old_recovery_account":"alice8ah","new_recovery_account":"initminer"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 65 );
  };

  account_scenario( check_point_tester);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_custom )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with custom_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 4 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // custom_operation
      R"~({"trx_id":"3e44a79c011431391ccb98dff11a0025ee25ecbf","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"custom_operation","value":{"required_auths":["alice9ah"],"id":7,"data":"44415441"}},"operation_id":0})~",
      R"~({"trx_id":"3e44a79c011431391ccb98dff11a0025ee25ecbf","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["custom",{"required_auths":["alice9ah"],"id":7,"data":"44415441"}]})~"
      }, { // custom_json_operation
      R"~({"trx_id":"79b98c81b10d32c2e284cb4b0d62ff609a14ed90","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"custom_json_operation","value":{"required_auths":[],"required_posting_auths":["alice9ah"],"id":"7id","json":"\"{\"type\": \"json\"}\""}},"operation_id":0})~",
      R"~({"trx_id":"79b98c81b10d32c2e284cb4b0d62ff609a14ed90","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["custom_json",{"required_auths":[],"required_posting_auths":["alice9ah"],"id":"7id","json":"\"{\"type\": \"json\"}\""}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8684058191","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":["producer_reward",{"producer":"initminer","vesting_shares":"8684.058191 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[2] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 4 );
  };

  custom_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_recurrent_transfer )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with recurrent_transfer_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 1204 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // recurrent_transfer_operation
      R"~({"trx_id":"93e35087c9401f6fa910684a849008cca1f85124","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"recurrent_transfer_operation","value":{"from":"alice10ah","to":"ben10ah","amount":{"amount":"37","precision":3,"nai":"@@000000021"},"memo":"With love","recurrence":1,"executions":2,"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"93e35087c9401f6fa910684a849008cca1f85124","block":5,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["recurrent_transfer",{"from":"alice10ah","to":"ben10ah","amount":"0.037 TESTS","memo":"With love","recurrence":1,"executions":2,"extensions":[]}]})~"
      }, { // recurrent_transfer_operation
      R"~({"trx_id":"58b39536800983e5b7228a3d8482515c6bc8ec1c","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"recurrent_transfer_operation","value":{"from":"ben10ah","to":"alice10ah","amount":{"amount":"7713","precision":3,"nai":"@@000000013"},"memo":"","recurrence":2,"executions":4,"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"58b39536800983e5b7228a3d8482515c6bc8ec1c","block":5,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["recurrent_transfer",{"from":"ben10ah","to":"alice10ah","amount":"7.713 TBD","memo":"","recurrence":2,"executions":4,"extensions":[]}]})~"
      }, { // recurrent_transfer_operation
      R"~({"trx_id":"2a7846469ffa8901e37b64ce65f6edbfe160aa7d","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":{"type":"recurrent_transfer_operation","value":{"from":"ben10ah","to":"alice10ah","amount":{"amount":"0","precision":3,"nai":"@@000000013"},"memo":"","recurrence":3,"executions":7,"extensions":[]}},"operation_id":0})~",
      R"~({"trx_id":"2a7846469ffa8901e37b64ce65f6edbfe160aa7d","block":5,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:15","op":["recurrent_transfer",{"from":"ben10ah","to":"alice10ah","amount":"0.000 TBD","memo":"","recurrence":3,"executions":7,"extensions":[]}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8611634248","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["producer_reward",{"producer":"initminer","vesting_shares":"8611.634248 VESTS"}]})~"
      }, { // fill_recurrent_transfer_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":{"type":"fill_recurrent_transfer_operation","value":{"from":"alice10ah","to":"ben10ah","amount":{"amount":"37","precision":3,"nai":"@@000000021"},"memo":"With love","remaining_executions":1}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":5,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:00:15","op":["fill_recurrent_transfer",{"from":"alice10ah","to":"ben10ah","amount":"0.037 TESTS","memo":"With love","remaining_executions":1}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[3], expected_operations[4] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 5 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1204,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T01:00:12","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"127616241278","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1204,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T01:00:12","op":["producer_reward",{"producer":"initminer","vesting_shares":"127616.241278 VESTS"}]})~"
      }, { // failed_recurrent_transfer_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1204,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T01:00:12","op":{"type":"failed_recurrent_transfer_operation","value":{"from":"alice10ah","to":"ben10ah","amount":{"amount":"37","precision":3,"nai":"@@000000021"},"memo":"With love","consecutive_failures":1,"remaining_executions":0,"deleted":false}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":1204,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T01:00:12","op":["failed_recurrent_transfer",{"from":"alice10ah","to":"ben10ah","amount":"0.037 TESTS","memo":"With love","consecutive_failures":1,"remaining_executions":0,"deleted":false}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 1204 );
  };

  recurrent_transfer_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( get_ops_in_block_decline_voting_rights )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with decline_voting_rights_scenario" );

  auto check_point_tester = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 23 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // account_witness_proxy_operation
      R"~({"trx_id":"60ebbe2fd041e117a5f24e07767d1686cee6c49a","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"account_witness_proxy_operation","value":{"account":"alice11ah","proxy":"ben11ah"}},"operation_id":0})~",
      R"~({"trx_id":"60ebbe2fd041e117a5f24e07767d1686cee6c49a","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["account_witness_proxy",{"account":"alice11ah","proxy":"ben11ah"}]})~"
      }, { // decline_voting_rights_operation
      R"~({"trx_id":"b725c37b755848a67161d30050eda164da5bd2a9","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"decline_voting_rights_operation","value":{"account":"alice11ah","decline":true}},"operation_id":0})~",
      R"~({"trx_id":"b725c37b755848a67161d30050eda164da5bd2a9","block":4,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["decline_voting_rights",{"account":"alice11ah","decline":true}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8700063351","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":["producer_reward",{"producer":"initminer","vesting_shares":"8700.063351 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[2] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 4 );

    expected_operations = { { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"7346451563","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":["producer_reward",{"producer":"initminer","vesting_shares":"7346.451563 VESTS"}]})~"
      }, { // proxy_cleared_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":{"type":"proxy_cleared_operation","value":{"account":"alice11ah","proxy":"ben11ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":2,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":["proxy_cleared",{"account":"alice11ah","proxy":"ben11ah"}]})~"
      }, { // declined_voting_rights_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":{"type":"declined_voting_rights_operation","value":{"account":"alice11ah"}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":23,"trx_in_block":4294967295,"op_in_trx":3,"virtual_op":true,"timestamp":"2016-01-01T00:01:09","op":["declined_voting_rights",{"account":"alice11ah"}]})~"
      } };
    // Note that all operations of this block are virtual, hence we can reuse the same expected container here.
    test_get_ops_in_block( *this, expected_operations, expected_operations, 23 );
  };

  decline_voting_rights_scenario( check_point_tester );

} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_CASE( get_ops_in_block_combo_1 )
{ try {

  BOOST_TEST_MESSAGE( "testing get_ops_in_block with combo_1_scenario" );

  auto check_point_tester1 = [ this ]( uint32_t generate_no_further_than )
  {
    generate_block();
  };

  auto check_point_tester2 = [ this ]( uint32_t generate_no_further_than )
  {
    generate_until_irreversible_block( 4 );
    BOOST_REQUIRE( db->head_block_num() <= generate_no_further_than );

    expected_t expected_operations = { { // account_witness_proxy_operation
      R"~({"trx_id":"1abb51849339ddedd0668a95c7384ec546fd4f30","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_create_operation","value":{"fee":{"amount":"0","precision":3,"nai":"@@000000021"},"creator":"initminer","new_account_name":"alice12ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"memo_key":"STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C","json_metadata":""}},"operation_id":0})~",
      R"~({"trx_id":"1abb51849339ddedd0668a95c7384ec546fd4f30","block":3,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["account_create",{"fee":"0.000 TESTS","creator":"initminer","new_account_name":"alice12ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C",1]]},"memo_key":"STM5GohjoP5Uu8yxgxR1BWmbDCNU8zL9Zqr4Ff5CThM7kfH59EX2C","json_metadata":""}]})~"
      }, { // account_created_operation
      R"~({"trx_id":"1abb51849339ddedd0668a95c7384ec546fd4f30","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"alice12ah","creator":"initminer","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"1abb51849339ddedd0668a95c7384ec546fd4f30","block":3,"trx_in_block":0,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"alice12ah","creator":"initminer","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
      }, { // transfer_to_vesting_operation
      R"~({"trx_id":"a76a08018b7fcb0b7a64dd6603cec79c104581fc","block":3,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"transfer_to_vesting_operation","value":{"from":"initminer","to":"alice12ah","amount":{"amount":"100","precision":3,"nai":"@@000000021"}}},"operation_id":0})~",
      R"~({"trx_id":"a76a08018b7fcb0b7a64dd6603cec79c104581fc","block":3,"trx_in_block":1,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["transfer_to_vesting",{"from":"initminer","to":"alice12ah","amount":"0.100 TESTS"}]})~"
      }, { // transfer_to_vesting_completed_operation
      R"~({"trx_id":"a76a08018b7fcb0b7a64dd6603cec79c104581fc","block":3,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"transfer_to_vesting_completed_operation","value":{"from_account":"initminer","to_account":"alice12ah","hive_vested":{"amount":"100","precision":3,"nai":"@@000000021"},"vesting_shares_received":{"amount":"98716683119","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"a76a08018b7fcb0b7a64dd6603cec79c104581fc","block":3,"trx_in_block":1,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["transfer_to_vesting_completed",{"from_account":"initminer","to_account":"alice12ah","hive_vested":"0.100 TESTS","vesting_shares_received":"98716.683119 VESTS"}]})~"
      }, { // comment_operation
      R"~({"trx_id":"70f6b11367e582cb7bb7265e86c964f7c04657be","block":3,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"comment_operation","value":{"parent_author":"","parent_permlink":"parentpermlink12","author":"alice12ah","permlink":"permlink12-1","title":"Title 12-1","body":"Body 12-1","json_metadata":""}},"operation_id":0})~",
      R"~({"trx_id":"70f6b11367e582cb7bb7265e86c964f7c04657be","block":3,"trx_in_block":2,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["comment",{"parent_author":"","parent_permlink":"parentpermlink12","author":"alice12ah","permlink":"permlink12-1","title":"Title 12-1","body":"Body 12-1","json_metadata":""}]})~"
      }, { // account_create_operation
      R"~({"trx_id":"2f3b134af6270fda2b156b0f44a107ea4a5e6fb9","block":3,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_create_operation","value":{"fee":{"amount":"0","precision":3,"nai":"@@000000021"},"creator":"alice12ah","new_account_name":"ben12ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"memo_key":"STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3","json_metadata":"{\"relation\":\"sibling\"}"}},"operation_id":0})~",
      R"~({"trx_id":"2f3b134af6270fda2b156b0f44a107ea4a5e6fb9","block":3,"trx_in_block":3,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["account_create",{"fee":"0.000 TESTS","creator":"alice12ah","new_account_name":"ben12ah","owner":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"active":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"posting":{"weight_threshold":1,"account_auths":[],"key_auths":[["STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3",1]]},"memo_key":"STM5MSvdqbA7scw5RTBx5UGpDAUKxpF53VjBBP1MkAMMfTMw9tLp3","json_metadata":"{\"relation\":\"sibling\"}"}]})~"
      }, { // account_created_operation
      R"~({"trx_id":"2f3b134af6270fda2b156b0f44a107ea4a5e6fb9","block":3,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"account_created_operation","value":{"new_account_name":"ben12ah","creator":"alice12ah","initial_vesting_shares":{"amount":"0","precision":6,"nai":"@@000000037"},"initial_delegation":{"amount":"0","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"2f3b134af6270fda2b156b0f44a107ea4a5e6fb9","block":3,"trx_in_block":3,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["account_created",{"new_account_name":"ben12ah","creator":"alice12ah","initial_vesting_shares":"0.000000 VESTS","initial_delegation":"0.000000 VESTS"}]})~"
      }, { // delegate_vesting_shares_operation
      R"~({"trx_id":"accc5c098490b71d16022ea6b7d43c0ed8830f08","block":3,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"delegate_vesting_shares_operation","value":{"delegator":"alice12ah","delegatee":"ben12ah","vesting_shares":{"amount":"3003","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"accc5c098490b71d16022ea6b7d43c0ed8830f08","block":3,"trx_in_block":4,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["delegate_vesting_shares",{"delegator":"alice12ah","delegatee":"ben12ah","vesting_shares":"0.003003 VESTS"}]})~"
      }, { // comment_operation
      R"~({"trx_id":"a286bf7a924da9fb33597fa3dacb31237ebf6210","block":3,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"comment_operation","value":{"parent_author":"alice12ah","parent_permlink":"permlink12-1","author":"ben12ah","permlink":"permlink12-2","title":"Title 12-1","body":"Body 12-1","json_metadata":""}},"operation_id":0})~",
      R"~({"trx_id":"a286bf7a924da9fb33597fa3dacb31237ebf6210","block":3,"trx_in_block":5,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["comment",{"parent_author":"alice12ah","parent_permlink":"permlink12-1","author":"ben12ah","permlink":"permlink12-2","title":"Title 12-1","body":"Body 12-1","json_metadata":""}]})~"
      }, { // decline_voting_rights_operation
      R"~({"trx_id":"0fef2c9a880d02c893c2b750a1d0238f478ee0e5","block":3,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":{"type":"decline_voting_rights_operation","value":{"account":"alice12ah","decline":true}},"operation_id":0})~",
      R"~({"trx_id":"0fef2c9a880d02c893c2b750a1d0238f478ee0e5","block":3,"trx_in_block":6,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:09","op":["decline_voting_rights",{"account":"alice12ah","decline":true}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8782740670","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":3,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:09","op":["producer_reward",{"producer":"initminer","vesting_shares":"8782.740670 VESTS"}]})~"
      } };
    expected_t expected_virtual_operations = { expected_operations[1], expected_operations[3], expected_operations[6], expected_operations[10] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 3 );

    expected_operations = { { // decline_voting_rights_operation
      R"~({"trx_id":"2250a4003a3b895b75bb51fca0f6b31cdb5a5950","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":{"type":"decline_voting_rights_operation","value":{"account":"alice12ah","decline":false}},"operation_id":0})~",
      R"~({"trx_id":"2250a4003a3b895b75bb51fca0f6b31cdb5a5950","block":4,"trx_in_block":0,"op_in_trx":0,"virtual_op":false,"timestamp":"2016-01-01T00:00:12","op":["decline_voting_rights",{"account":"alice12ah","decline":false}]})~"
      }, { // producer_reward_operation
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":{"type":"producer_reward_operation","value":{"producer":"initminer","vesting_shares":{"amount":"8684058191","precision":6,"nai":"@@000000037"}}},"operation_id":0})~",
      R"~({"trx_id":"0000000000000000000000000000000000000000","block":4,"trx_in_block":4294967295,"op_in_trx":1,"virtual_op":true,"timestamp":"2016-01-01T00:00:12","op":["producer_reward",{"producer":"initminer","vesting_shares":"8684.058191 VESTS"}]})~"
      } };
    expected_virtual_operations = { expected_operations[1] };
    test_get_ops_in_block( *this, expected_operations, expected_virtual_operations, 4 );
  };

  combo_1_scenario( check_point_tester1, check_point_tester2 );

} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_SUITE_END() // condenser_get_ops_in_block_tests

#endif
