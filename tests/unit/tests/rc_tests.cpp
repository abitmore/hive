#ifdef IS_TEST_NET
#include <boost/test/unit_test.hpp>

#include <hive/chain/rc/rc_objects.hpp>
#include <hive/chain/database_exceptions.hpp>
#include <hive/protocol/hive_custom_operations.hpp>

#include "../db_fixture/clean_database_fixture.hpp"

#include <fc/log/appender.hpp>

#include <chrono>

using namespace hive::chain;
using namespace hive::protocol;
using namespace hive::plugins;

int64_t regenerate_rc_mana( debug_node::debug_node_plugin* db_plugin, const account_object& acc )
{
  db_plugin->debug_update( [&]( database& db )
  {
    db.modify( acc, [&]( account_object& account )
    {
      auto max_rc = account.get_maximum_rc();
      hive::chain::util::manabar_params manabar_params( max_rc.value, HIVE_RC_REGEN_TIME );
      account.rc_manabar.regenerate_mana( manabar_params, db.head_block_time() );
    } );
  } );
  return acc.rc_manabar.current_mana;
}

void clear_mana( debug_node::debug_node_plugin* db_plugin, const account_object& acc )
{
  db_plugin->debug_update( [&]( database& db )
  {
    db.modify( acc, [&]( account_object& account )
    {
      account.rc_manabar.current_mana = 0;
      account.rc_manabar.last_update_time = db.head_block_time().sec_since_epoch();
    } );
  } );
}

BOOST_FIXTURE_TEST_SUITE( rc_tests, genesis_database_fixture )

BOOST_AUTO_TEST_CASE( rc_usage_buckets )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing rc usage bucket tracking" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );

    fc::int_array< std::string, HIVE_RC_NUM_RESOURCE_TYPES > rc_names;
    for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
      rc_names[i] = fc::reflector< rc_resource_types >::to_string(i);

    const auto& pools = db->get< rc_pool_object, by_id >( rc_pool_id_type() );
    const auto& bucketIdx = db->get_index< rc_usage_bucket_index >().indices().get< by_timestamp >();

    static_assert( HIVE_RC_NUM_RESOURCE_TYPES == 5, "If it fails update logging code below accordingly" );

    auto get_active_bucket = [&]() -> const rc_usage_bucket_object&
    {
      return *( bucketIdx.rbegin() );
    };
    auto check_eq = []( const resource_count_type& rc1, const resource_count_type& rc2 )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
        BOOST_CHECK_EQUAL( rc1[i], rc2[i] );
    };
    auto print = [&]( bool nonempty_buckets = false ) -> int //index of first nonempty bucket or -1 when only active
    {
      ilog( "Global usage ${b} @${t}: [${u0},${u1},${u2},${u3},${u4}]", ( "b", db->head_block_num() )( "t", db->head_block_time() )
        ( "u0", pools.get_usage(0) )( "u1", pools.get_usage(1) )( "u2", pools.get_usage(2) )
        ( "u3", pools.get_usage(3) )( "u4", pools.get_usage(4) )
      );
      ilog( "Resource weights: [${w0},${w1},${w2},${w3},${w4}] / ${sw}", ( "sw", pools.get_weight_divisor() )
        ( "w0", pools.get_weight(0) )( "w1", pools.get_weight(1) )( "w2", pools.get_weight(2) )
        ( "w3", pools.get_weight(3) )( "w4", pools.get_weight(4) )
      );
      ilog( "Relative resource weights: [${w0}bp,${w1}bp,${w2}bp,${w3}bp,${w4}bp]",
        ( "w0", pools.count_share(0) )( "w1", pools.count_share(1) )( "w2", pools.count_share(2) )
        ( "w3", pools.count_share(3) )( "w4", pools.count_share(4) )
      );
      int result = -1;
      if( nonempty_buckets )
      {
        ilog( "Nonempty buckets:" );
        int i = 0;
        for( const auto& bucket : bucketIdx )
        {
          if( bucket.get_usage(0) || bucket.get_usage(1) || bucket.get_usage(2) ||
              bucket.get_usage(3) || bucket.get_usage(4) )
          {
            if( result < 0 )
              result = i;
            ilog( "${i} @${t}: [${u0},${u1},${u2},${u3},${u4}]", ( "i", i )( "t", bucket.get_timestamp() )
              ( "u0", bucket.get_usage(0) )( "u1", bucket.get_usage(1) )( "u2", bucket.get_usage(2) )
              ( "u3", bucket.get_usage(3) )( "u4", bucket.get_usage(4) )
            );
          }
          ++i;
        }
      }
      else
      {
        const auto& bucket = *bucketIdx.rbegin();
        ilog( "Active bucket @${t}: [${u0},${u1},${u2},${u3},${u4}]", ( "t", bucket.get_timestamp() )
          ( "u0", bucket.get_usage(0) )( "u1", bucket.get_usage(1) )( "u2", bucket.get_usage(2) )
          ( "u3", bucket.get_usage(3) )( "u4", bucket.get_usage(4) )
        );
      }
      return result;
    };

    BOOST_TEST_MESSAGE( "All buckets empty" );

    BOOST_CHECK_EQUAL( print( true ), -1 );
    for( const auto& bucket : bucketIdx )
      check_eq( bucket.get_usage(), {} );

    ACTORS( (alice)(bob)(sam) )
    fund( "alice", ASSET( "1.000 TESTS" ) );
    fund( "bob", ASSET( "1.000 TESTS" ) );
    fund( "sam", ASSET( "1.000 TESTS" ) );
    generate_block();

    BOOST_TEST_MESSAGE( "Some resources consumed" );

    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 1 );
    check_eq( get_active_bucket().get_usage(), pools.get_usage() );

    generate_blocks( get_active_bucket().get_timestamp() + fc::seconds( HIVE_RC_BUCKET_TIME_LENGTH - HIVE_BLOCK_INTERVAL ) );

    BOOST_TEST_MESSAGE( "First bucket is about to switch" );

    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 1 );
    check_eq( get_active_bucket().get_usage(), pools.get_usage() );
    generate_block(); //switch bucket
    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 2 );
    check_eq( get_active_bucket().get_usage(), {} );

    auto circular_transfer = [&]( int amount )
    {
      for( int i = 0; i < amount; ++i )
      {
        resource_count_type usage = pools.get_usage();
        transfer( "alice", "bob", ASSET( "0.100 TESTS" ), "", alice_private_key );
        transfer( "bob", "sam", ASSET( "0.100 TESTS" ), "", bob_private_key );
        transfer( "sam", "alice", ASSET( "0.100 TESTS" ), "", sam_private_key );
        generate_block();
        print(); //transfers use market and execution, but transaction itself uses history and state
        BOOST_CHECK_LT( usage[ resource_history_bytes ], pools.get_usage( resource_history_bytes ) );
        BOOST_CHECK_EQUAL( usage[ resource_new_accounts ], pools.get_usage( resource_new_accounts ) );
        BOOST_CHECK_LT( usage[ resource_market_bytes ], pools.get_usage( resource_market_bytes ) );
        BOOST_CHECK_LT( usage[ resource_state_bytes ], pools.get_usage( resource_state_bytes ) );
        BOOST_CHECK_LT( usage[ resource_execution_time ], pools.get_usage( resource_execution_time ) );
      }
    };
    BOOST_TEST_MESSAGE( "Spamming transfers" );
    int market_bytes_share = pools.count_share( resource_market_bytes );
    circular_transfer( ( HIVE_RC_BUCKET_TIME_LENGTH / HIVE_BLOCK_INTERVAL ) - 1 );
    int new_market_bytes_share = pools.count_share( resource_market_bytes );
    BOOST_CHECK_LT( market_bytes_share, new_market_bytes_share ); //share of market bytes in RC inflation should've increased

    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 2 );
    generate_block(); //switch bucket
    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 3 );
    check_eq( get_active_bucket().get_usage(), {} );

    BOOST_TEST_MESSAGE( "More transfer spam within new bucket subwindow" );
    market_bytes_share = new_market_bytes_share;
    circular_transfer( 100 ); //do some more to show switching bucket did not influence resource weights in any way
    new_market_bytes_share = pools.count_share( resource_market_bytes );
    BOOST_CHECK_LT( market_bytes_share, new_market_bytes_share ); //share of market bytes in RC inflation should've increased again

    custom_operation custom;
    custom.id = 1;
    custom.data.resize( HIVE_CUSTOM_OP_DATA_MAX_LENGTH );
    auto custom_spam = [&]( int amount )
    {
      for( int i = 0; i < amount; ++i )
      {
        resource_count_type usage = pools.get_usage();
        custom.required_auths = { "alice" };
        push_transaction( custom, alice_private_key );
        custom.required_auths = { "bob" };
        push_transaction( custom, bob_private_key );
        custom.required_auths = { "sam" };
        push_transaction( custom, sam_private_key );
        generate_block();
        print(); //custom_op uses only execution, but transaction itself uses history (a lot in this case) and state
        BOOST_CHECK_LT( usage[ resource_history_bytes ], pools.get_usage( resource_history_bytes ) );
        BOOST_CHECK_EQUAL( usage[ resource_new_accounts ], pools.get_usage( resource_new_accounts ) );
        BOOST_CHECK_EQUAL( usage[ resource_market_bytes ], pools.get_usage( resource_market_bytes ) );
        BOOST_CHECK_LT( usage[ resource_state_bytes ], pools.get_usage( resource_state_bytes ) );
        BOOST_CHECK_LT( usage[ resource_execution_time ], pools.get_usage( resource_execution_time ) );
      }
    };
    BOOST_TEST_MESSAGE( "Lengthy custom op spam within the same bucket subwindow as previous transfers" );
    market_bytes_share = new_market_bytes_share;
    int history_bytes_share = pools.count_share( resource_history_bytes );
    custom_spam( ( HIVE_RC_BUCKET_TIME_LENGTH / HIVE_BLOCK_INTERVAL ) - 101 );
    new_market_bytes_share = pools.count_share( resource_market_bytes );
    int new_history_bytes_share = pools.count_share( resource_history_bytes );
    BOOST_CHECK_GT( market_bytes_share, new_market_bytes_share ); //share of market bytes in RC inflation must fall...
    BOOST_CHECK_LT( history_bytes_share, new_history_bytes_share ); //...while share of history bytes increases

    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 3 );
    generate_block(); //switch bucket
    BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - 4 );
    check_eq( get_active_bucket().get_usage(), {} );

    BOOST_TEST_MESSAGE( "Bucket switches with small amount of custom ops in each to have all buckets nonempty" );
    for( int i = 5; i <= HIVE_RC_WINDOW_BUCKET_COUNT; ++i )
    {
      custom_spam( 1 );
      generate_blocks( get_active_bucket().get_timestamp() + fc::seconds( HIVE_RC_BUCKET_TIME_LENGTH - HIVE_BLOCK_INTERVAL ) );
      generate_block(); //switch bucket
      BOOST_CHECK_EQUAL( print( true ), HIVE_RC_WINDOW_BUCKET_COUNT - i );
      check_eq( get_active_bucket().get_usage(), {} );
    }
    BOOST_TEST_MESSAGE( "Some more custom op spam" );
    history_bytes_share = pools.count_share( resource_history_bytes );
    custom_spam( 200 );
    new_history_bytes_share = pools.count_share( resource_history_bytes );
    BOOST_CHECK_LT( history_bytes_share, new_history_bytes_share ); //despite being pretty high previously, share of history bytes should still increase

    custom_json_operation custom_json;
    custom_json.json = "{}";
    auto custom_json_spam = [&]( int amount )
    {
      for( int i = 0; i < amount; ++i )
      {
        resource_count_type usage = pools.get_usage();
        custom_json.id = i&1 ? "follow" : "reblog";
        custom_json.required_auths = {};
        custom_json.required_posting_auths = { "alice" };
        push_transaction( custom_json, alice_post_key );
        custom_json.id = i&1 ? "notify" : "community";
        custom_json.required_posting_auths = { "bob" };
        push_transaction( custom_json, bob_post_key );
        custom_json.id = i&1 ? "splinterlands" : "dex";
        custom_json.required_auths = { "sam" };
        custom_json.required_posting_auths = {};
        push_transaction( custom_json, sam_private_key );
        generate_block();
        print(); //custom_json uses only execution, transaction itself uses history (not much in this case) and state
        BOOST_CHECK_LT( usage[ resource_history_bytes ], pools.get_usage( resource_history_bytes ) );
        BOOST_CHECK_EQUAL( usage[ resource_new_accounts ], pools.get_usage( resource_new_accounts ) );
        BOOST_CHECK_EQUAL( usage[ resource_market_bytes ], pools.get_usage( resource_market_bytes ) );
        BOOST_CHECK_LT( usage[ resource_state_bytes ], pools.get_usage( resource_state_bytes ) );
        BOOST_CHECK_LT( usage[ resource_execution_time ], pools.get_usage( resource_execution_time ) );
      }
    };
    BOOST_TEST_MESSAGE( "Custom json spam (including HM related) within the same bucket subwindow as previous custom ops" );
    history_bytes_share = new_history_bytes_share;
    custom_json_spam( ( HIVE_RC_BUCKET_TIME_LENGTH / HIVE_BLOCK_INTERVAL ) - 201 );
    new_history_bytes_share = pools.count_share( resource_history_bytes );
    BOOST_CHECK_GT( history_bytes_share, new_history_bytes_share ); //share of history bytes in RC inflation must fall

    BOOST_TEST_MESSAGE( "First nonempty bucket is about to expire" );
    auto bucket_usage = bucketIdx.begin()->get_usage();
    auto global_usage = pools.get_usage();

    BOOST_CHECK_EQUAL( print( true ), 0 );
    generate_block(); //switch bucket (nonempty bucket 0 expired)
    BOOST_CHECK_EQUAL( print( true ), 0 );
    check_eq( get_active_bucket().get_usage(), {} );

    BOOST_TEST_MESSAGE( "Checking if bucket was expired correctly" );
    //content of first bucket (which was nonempty for the first time in this test) should be
    //subtracted from global usage
    for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
      global_usage[i] -= bucket_usage[i];
    check_eq( global_usage, pools.get_usage() );
    
    generate_blocks( get_active_bucket().get_timestamp() + fc::seconds( HIVE_RC_BUCKET_TIME_LENGTH - HIVE_BLOCK_INTERVAL ) );

    market_bytes_share = pools.count_share( resource_market_bytes );
    BOOST_CHECK_EQUAL( print( true ), 0 );
    generate_block(); //switch bucket (another nonempty bucket 0 expired)
    BOOST_CHECK_EQUAL( print( true ), 0 );
    check_eq( get_active_bucket().get_usage(), {} );

    BOOST_TEST_MESSAGE( "Checking how bucket expiration can influence resource shares" );
    new_market_bytes_share = pools.count_share( resource_market_bytes );
    //most transfers using market bytes were produced within second bucket subwindow
    //but some were done in next bucket, so we should still be above zero
    BOOST_CHECK_GT( market_bytes_share, new_market_bytes_share );
    BOOST_CHECK_GT( new_market_bytes_share, 0 );

    generate_blocks( get_active_bucket().get_timestamp() + fc::seconds( HIVE_RC_BUCKET_TIME_LENGTH - HIVE_BLOCK_INTERVAL ) );

    BOOST_TEST_MESSAGE( "Checking bucket expiration while there is pending operation" );
    market_bytes_share = new_market_bytes_share;
    BOOST_CHECK_EQUAL( pools.get_usage( resource_new_accounts ), 0 );

    claim_account( "alice", ASSET( "0.000 TESTS" ), alice_private_key );
    generate_block(); //switch bucket
    BOOST_CHECK_EQUAL( print( true ), 0 );
    //last transaction should be registered within new bucket - it uses new account and execution, tx itself also state and history
    bucket_usage = get_active_bucket().get_usage();
    BOOST_CHECK_GT( bucket_usage[ resource_history_bytes ], 0 );
    BOOST_CHECK_GT( bucket_usage[ resource_new_accounts ], 0 );
    BOOST_CHECK_EQUAL( bucket_usage[ resource_market_bytes ], 0 );
    BOOST_CHECK_GT( bucket_usage[ resource_state_bytes ], 0 );
    BOOST_CHECK_GT( bucket_usage[ resource_execution_time ], 0 );

    //all remaining market bytes were used within expired bucket subwindow
    BOOST_CHECK_EQUAL( pools.get_usage( resource_market_bytes ), 0 );
    new_market_bytes_share = pools.count_share( resource_market_bytes );
    BOOST_CHECK_EQUAL( new_market_bytes_share, 0 );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_single_recover_account )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing rc resource cost of single recover_account_operation" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS( (agent)(victim)(thief) )
    generate_block(); 
    vest( "agent", ASSET( "1000.000 TESTS" ) );
    issue_funds( "victim", ASSET( "1.000 TESTS" ) );

    const auto& agent_rc = db->get_account( "agent" );
    const auto& victim_rc = db->get_account( "victim" );
    const auto& thief_rc = db->get_account( "thief" );

    BOOST_TEST_MESSAGE( "agent becomes recovery account for victim" );
    change_recovery_account( "victim", "agent", victim_private_key );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );

    generate_block();

    BOOST_TEST_MESSAGE( "thief steals private key of victim and sets authority to himself" );
    account_update_operation account_update;
    account_update.account = "victim";
    account_update.owner = authority( 1, "thief", 1 );
    push_transaction( account_update, victim_private_key );
    generate_blocks( db->head_block_time() + ( HIVE_OWNER_AUTH_RECOVERY_PERIOD.to_seconds() / 2 ) );

    BOOST_TEST_MESSAGE( "victim notices a problem and asks agent for recovery" );
    auto victim_new_private_key = generate_private_key( "victim2" );
    request_account_recovery( "agent", "victim", authority( 1, victim_new_private_key.get_public_key(), 1 ), agent_private_key );
    generate_block();

    BOOST_TEST_MESSAGE( "thief keeps RC of victim at zero - recovery still possible" );
    auto pre_tx_agent_mana = regenerate_rc_mana( db_plugin, agent_rc );
    clear_mana( db_plugin, victim_rc );
    auto pre_tx_thief_mana = regenerate_rc_mana( db_plugin, thief_rc );
    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    recover_account_operation recover;
    recover.account_to_recover = "victim";
    recover.new_owner_authority = authority( 1, victim_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    push_transaction( tx, {victim_private_key, victim_new_private_key} );
    tx.clear();
    //RC cost covered by the network - no RC spent on any account (it would fail if victim was charged like it used to be)
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief_mana, thief_rc.rc_manabar.current_mana );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );

    BOOST_TEST_MESSAGE( "victim wants to try to rob rc from recovery agent or network" );
    //this part tests against former and similar solutions that would be exploitable
    //first change authority to contain agent but without actual control (weight below threshold) and finalize it
    account_update.account = "victim";
    account_update.owner = authority( 3, "agent", 1, victim_new_private_key.get_public_key(), 3 );
    push_transaction( account_update, victim_new_private_key );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );
    //"steal" account also including agent as above
    auto victim_testA_private_key = generate_private_key( "victimA" );
    auto victim_active_private_key = generate_private_key( "victimActive" );
    account_update.owner = authority( 3, "agent", 1, victim_testA_private_key.get_public_key(), 3 );
    account_update.active = authority( 1, victim_active_private_key.get_public_key(), 1 );
    push_transaction( account_update, victim_new_private_key );
    generate_blocks( db->head_block_time() + ( HIVE_OWNER_AUTH_RECOVERY_PERIOD.to_seconds() / 2 ) );
    //ask agent for help to recover "stolen" account - again add agent
    auto victim_testB_private_key = generate_private_key( "victimB" );
    request_account_recovery( "agent", "victim", authority( 3, "agent", 1, victim_testB_private_key.get_public_key(), 3 ), agent_private_key );
    generate_block();
    //finally recover adding expensive operation as extra - test 1: before actual recovery
    pre_tx_agent_mana = regenerate_rc_mana( db_plugin, agent_rc );
    auto pre_tx_victim_mana = regenerate_rc_mana( db_plugin, victim_rc );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    claim_account_operation expensive;
    expensive.creator = "victim";
    expensive.fee = ASSET( "0.000 TESTS" );
    recover.account_to_recover = "victim";
    recover.new_owner_authority = authority( 3, "agent", 1, victim_testB_private_key.get_public_key(), 3 );
    recover.recent_owner_authority = authority( 3, "agent", 1, victim_new_private_key.get_public_key(), 3 );
    tx.operations.push_back( expensive );
    tx.operations.push_back( recover );
    HIVE_REQUIRE_EXCEPTION( push_transaction( tx, {victim_new_private_key, victim_active_private_key/*that key is needed for claim account*/, victim_testB_private_key} ), "has_mana", plugin_exception );
    tx.clear();
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_victim_mana, victim_rc.rc_manabar.current_mana );
    //test 2: after recovery
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    tx.operations.push_back( recover );
    tx.operations.push_back( expensive );
    HIVE_REQUIRE_EXCEPTION( push_transaction( tx, {victim_new_private_key, victim_active_private_key, victim_testB_private_key} ), "has_mana", plugin_exception );
    tx.clear();
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_victim_mana, victim_rc.rc_manabar.current_mana );
    //test 3: add something less expensive so the tx will go through
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    transfer_operation cheap;
    cheap.from = "victim";
    cheap.to = "agent";
    cheap.amount = ASSET( "0.001 TESTS" );
    cheap.memo = "Thanks for help!";
    tx.operations.push_back( recover );
    tx.operations.push_back( cheap );
    push_transaction( tx, {victim_new_private_key, victim_active_private_key, victim_testB_private_key} );
    tx.clear();
    //RC consumed from victim - recovery is not free if mixed with other operations
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_GT( pre_tx_victim_mana, victim_rc.rc_manabar.current_mana );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_many_recover_accounts )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing rc resource cost of many recover_account_operations" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS( (agent)(victim1)(victim2)(victim3)(thief1)(thief2)(thief3) )
    generate_block();
    vest( "agent", ASSET( "1000.000 TESTS" ) );
    issue_funds( "victim1", ASSET( "1.000 TESTS" ) );

    const auto& agent_rc = db->get_account( "agent" );
    const auto& victim1_rc = db->get_account( "victim1" );
    const auto& victim2_rc = db->get_account( "victim2" );
    const auto& victim3_rc = db->get_account( "victim3" );
    const auto& thief1_rc = db->get_account( "thief1" );
    const auto& thief2_rc = db->get_account( "thief2" );
    const auto& thief3_rc = db->get_account( "thief3" );

    BOOST_TEST_MESSAGE( "agent becomes recovery account for all victims" );
    change_recovery_account( "victim1", "agent", victim1_private_key );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );
    change_recovery_account( "victim2", "agent", victim2_private_key );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );
    change_recovery_account( "victim3", "agent", victim3_private_key );
    generate_blocks( db->head_block_time() + HIVE_OWNER_AUTH_RECOVERY_PERIOD );

    generate_block();

    BOOST_TEST_MESSAGE( "thiefs steal private keys of victims and set authority to their keys" );
    account_update_operation account_update;
    account_update.account = "victim1";
    account_update.owner = authority( 1, thief1_private_key.get_public_key(), 1 );
    push_transaction( account_update, victim1_private_key );
    generate_blocks( db->head_block_time() + ( HIVE_OWNER_AUTH_RECOVERY_PERIOD.to_seconds() / 5 ) );
    account_update.account = "victim2";
    account_update.owner = authority( 1, thief2_private_key.get_public_key(), 1 );
    push_transaction( account_update, victim2_private_key );
    generate_blocks( db->head_block_time() + ( HIVE_OWNER_AUTH_RECOVERY_PERIOD.to_seconds() / 5 ) );
    account_update.account = "victim3";
    account_update.owner = authority( 1, thief3_private_key.get_public_key(), 1 );
    push_transaction( account_update, victim3_private_key );
    generate_blocks( db->head_block_time() + ( HIVE_OWNER_AUTH_RECOVERY_PERIOD.to_seconds() / 5 ) );

    BOOST_TEST_MESSAGE( "victims notice a problem and ask agent for recovery" );
    request_account_recovery_operation request;
    request.account_to_recover = "victim1";
    request.recovery_account = "agent";
    auto victim1_new_private_key = generate_private_key( "victim1n" );
    request.new_owner_authority = authority( 1, victim1_new_private_key.get_public_key(), 1 );
    push_transaction( request, agent_private_key );
    request.account_to_recover = "victim2";
    auto victim2_new_private_key = generate_private_key( "victim2n" );
    request.new_owner_authority = authority( 1, victim2_new_private_key.get_public_key(), 1 );
    push_transaction( request, agent_private_key );
    request.account_to_recover = "victim3";
    auto victim3_new_private_key = generate_private_key( "victim3n" );
    request.new_owner_authority = authority( 1, victim3_new_private_key.get_public_key(), 1 );
    push_transaction( request, agent_private_key );
    generate_block();

    BOOST_TEST_MESSAGE( "thiefs keep RC of victims at zero - recovery not possible for multiple accounts in one tx..." );
    auto pre_tx_agent_mana = regenerate_rc_mana( db_plugin, agent_rc );
    clear_mana( db_plugin, victim1_rc );
    clear_mana( db_plugin, victim2_rc );
    clear_mana( db_plugin, victim3_rc );
    auto pre_tx_thief1_mana = regenerate_rc_mana( db_plugin, thief1_rc );
    auto pre_tx_thief2_mana = regenerate_rc_mana( db_plugin, thief2_rc );
    auto pre_tx_thief3_mana = regenerate_rc_mana( db_plugin, thief3_rc );
    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    recover_account_operation recover;
    recover.account_to_recover = "victim1";
    recover.new_owner_authority = authority( 1, victim1_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim1_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    recover.account_to_recover = "victim2";
    recover.new_owner_authority = authority( 1, victim2_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim2_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    recover.account_to_recover = "victim3";
    recover.new_owner_authority = authority( 1, victim3_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim3_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    transfer_operation transfer;
    transfer.from = "victim1";
    transfer.to = "agent";
    transfer.amount = ASSET( "0.001 TESTS" );
    transfer.memo = "All those accounts are actually mine";
    tx.operations.push_back( transfer );
    //oops! recovery failed when combined with transfer because it's not free then
    HIVE_REQUIRE_EXCEPTION( push_transaction( tx, {victim1_private_key, victim1_new_private_key, victim2_private_key, victim2_new_private_key, victim3_private_key, victim3_new_private_key} ), "has_mana", plugin_exception );
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim3_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief1_mana, thief1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief2_mana, thief2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief3_mana, thief3_rc.rc_manabar.current_mana );
    //remove transfer from tx
    tx.operations.pop_back();
    
    //now that transfer was removed it used to work ok despite total lack of RC mana, however
    //rc_multisig_recover_account test showed the dangers of such approach, therefore it was blocked
    //now there can be only one subsidized operation in tx and with no more than allowed limit of
    //signatures (2 in this case) for the tx to be free
    HIVE_REQUIRE_EXCEPTION( push_transaction( tx, {victim1_private_key, victim1_new_private_key, victim2_private_key, victim2_new_private_key, victim3_private_key, victim3_new_private_key} ), "has_mana", plugin_exception );
    tx.clear();
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim3_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief1_mana, thief1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief2_mana, thief2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief3_mana, thief3_rc.rc_manabar.current_mana );
    //try separate transactions
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    recover.account_to_recover = "victim1";
    recover.new_owner_authority = authority( 1, victim1_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim1_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    push_transaction( tx, {victim1_private_key, victim1_new_private_key} );
    tx.clear();
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    recover.account_to_recover = "victim2";
    recover.new_owner_authority = authority( 1, victim2_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim2_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    push_transaction( tx, {victim2_private_key, victim2_new_private_key} );
    tx.clear();
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    recover.account_to_recover = "victim3";
    recover.new_owner_authority = authority( 1, victim3_new_private_key.get_public_key(), 1 );
    recover.recent_owner_authority = authority( 1, victim3_private_key.get_public_key(), 1 );
    tx.operations.push_back( recover );
    push_transaction( tx, {victim3_private_key, victim3_new_private_key} );
    tx.clear();
    BOOST_REQUIRE_EQUAL( pre_tx_agent_mana, agent_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( 0, victim3_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief1_mana, thief1_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief2_mana, thief2_rc.rc_manabar.current_mana );
    BOOST_REQUIRE_EQUAL( pre_tx_thief3_mana, thief3_rc.rc_manabar.current_mana );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_multisig_recover_account )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing rc resource cost of recover_account_operation with complex authority" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    static_assert( HIVE_MAX_SIG_CHECK_DEPTH >= 2 );
    static_assert( HIVE_MAX_SIG_CHECK_ACCOUNTS >= 3 * HIVE_MAX_AUTHORITY_MEMBERSHIP );

    fc::flat_map< std::string, std::vector<char> > props;
    props[ "maximum_block_size" ] = fc::raw::pack_to_vector( HIVE_MAX_BLOCK_SIZE );
    set_witness_props( props ); //simple tx with maxed authority uses over 300kB
    const auto fee = db->get_witness_schedule_object().median_props.account_creation_fee;

    ACTORS( (agent)(thief) )
    generate_block();
    vest( "agent", ASSET( "1000.000 TESTS" ) );
    vest( "thief", ASSET( "1000.000 TESTS" ) );
    issue_funds( "agent", ASSET( "1000.000 TESTS" ) );

    BOOST_TEST_MESSAGE( "create signer accounts and victim account (with agent as recovery)" );

    struct signer //key signer has 40 keys, mixed signer has 2 accounts + 38 keys
    {
      account_name_type name;
      std::vector< private_key_type > keys;
      authority auth;
      bool isMixed;

      const char* build_name( char* out, int n, bool _isMixed ) const
      {
        if( _isMixed ) { out[0] = 'm'; out[1] = 'i'; out[2] = 'x'; }
        else { out[0] = 'k'; out[1] = 'e'; out[2] = 'y'; }
        out[3] = char( '0' + n / 1000 );
        out[4] = char( '0' + n % 1000 / 100 );
        out[5] = char( '0' + n % 100 / 10 );
        out[6] = char( '0' + n % 10 );
        for( int i = 7; i < 16; ++i )
          out[i] = 0;
        return out;
      }

      void create( int n, bool _isMixed, database_fixture* _this, const asset& fee )
      {
        isMixed = _isMixed;

        account_create_operation create;
        create.fee = fee;
        create.creator = "initminer";

        char _name[16];
        name = build_name( _name, n, isMixed );
        create.new_account_name = name;

        auth = authority();
        auth.weight_threshold = HIVE_MAX_AUTHORITY_MEMBERSHIP;
        for( int k = isMixed ? 2 : 0; k < HIVE_MAX_AUTHORITY_MEMBERSHIP; ++k )
        {
          _name[7] = char( '0' + k / 10 );
          _name[8] = char( '0' + k % 10 );
          keys.emplace_back( generate_private_key( _name ) );
          auth.add_authority( keys.back().get_public_key(), 1 );
        }
        if( isMixed )
        {
          auth.add_authority( build_name( _name, 2*n, false ), 1 );
          auth.add_authority( build_name( _name, 2*n+1, false ), 1 );
        }
        create.owner = auth;
        create.active = create.owner;
        create.posting = create.owner;

        _this->push_transaction( create, _this->init_account_priv_key );
        _this->generate_block();
      }

    } key_signers[ 2 * HIVE_MAX_AUTHORITY_MEMBERSHIP ],
      mixed_signers[ HIVE_MAX_AUTHORITY_MEMBERSHIP ];

    //both victim_auth and alternative are built in layers:
    //victim needs signatures of mixed signers (40)
    //mixed signers need their own signatures (38) as well as those of key signers (2)
    //each key signer has just keys (40)
    //alternative replaces last mixed signer with another account (it is needed because during
    //recovery old and new authority have to differ)
    authority victim_auth;
    authority alternative_auth;
    victim_auth.weight_threshold = HIVE_MAX_AUTHORITY_MEMBERSHIP;
    alternative_auth.weight_threshold = HIVE_MAX_AUTHORITY_MEMBERSHIP;

    int key_count = 0;
    account_create_operation create;
    create.fee = fee;
    create.creator = "agent";
    create.new_account_name = "victim";
    for( int i = 0; i < HIVE_MAX_AUTHORITY_MEMBERSHIP; ++i )
    {
      key_signers[2*i].create( 2*i, false, this, fee );
      key_count += key_signers[2*i].keys.size();
      key_signers[2*i+1].create( 2*i+1, false, this, fee );
      key_count += key_signers[2*i+1].keys.size();
      mixed_signers[i].create( i, true, this, fee );
      key_count += mixed_signers[i].keys.size();
    }
    //create one alternative account with copy of authority from last mixed signer
    {
      account_create_operation create;
      create.fee = fee;
      create.creator = "initminer";
      create.new_account_name = "alternative";
      create.owner = mixed_signers[ HIVE_MAX_AUTHORITY_MEMBERSHIP - 1 ].auth;
      create.active = create.owner;
      create.posting = create.owner;
      push_transaction( create, init_account_priv_key );
      generate_block();
    }
    for( int i = 0; i < HIVE_MAX_AUTHORITY_MEMBERSHIP-1; ++i )
    {
      victim_auth.add_authority( mixed_signers[i].name, 1 );
      alternative_auth.add_authority( mixed_signers[i].name, 1 );
    }
    victim_auth.add_authority( mixed_signers[ HIVE_MAX_AUTHORITY_MEMBERSHIP-1 ].name, 1 );
    alternative_auth.add_authority( "alternative", 1 );
    create.owner = victim_auth;
    create.active = create.owner;
    create.posting = create.owner;
    push_transaction( create, agent_private_key );

    vest( "victim", ASSET( "1000.000 TESTS" ) );
    generate_block();
    const auto& victim_rc = db->get_account( "victim" );

    //many repeats to gather average CPU consumption of recovery process
    uint64_t time = 0;
    const int ITERATIONS = 20; //slooow - note that there is 4720 keys and each signature is validated around 75us
    ilog( "Measuring time of account recovery operation with ${k} signatures - ${c} iterations",
      ( "k", key_count )( "c", ITERATIONS ) );
    for( int i = 0; i < ITERATIONS; ++i )
    {
      ilog( "iteration ${i}, RC = ${r}", ( "i", i )( "r", victim_rc.rc_manabar.current_mana ) );
      BOOST_TEST_MESSAGE( "thief steals private keys of signers and sets authority to himself" );
      signed_transaction tx;
      tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
      account_update_operation account_update;
      account_update.account = "victim";
      account_update.owner = authority( 1, "thief", 1 );
      tx.operations.push_back( account_update );
      std::vector< private_key_type >_keys;
      for( int k = 0; k < 2 * HIVE_MAX_AUTHORITY_MEMBERSHIP; ++k )
        std::copy( key_signers[k].keys.begin(), key_signers[k].keys.end(), std::back_inserter( _keys ) );
      for( int k = 0; k < HIVE_MAX_AUTHORITY_MEMBERSHIP; ++k )
        std::copy( mixed_signers[k].keys.begin(), mixed_signers[k].keys.end(), std::back_inserter( _keys ) );
      push_transaction( tx, _keys );
      tx.clear();
      generate_block();

      BOOST_TEST_MESSAGE( "victim notices a problem and asks agent for recovery" );
      request_account_recovery( "agent", "victim", alternative_auth, agent_private_key );
      generate_block();
      
      BOOST_TEST_MESSAGE( "victim gets account back" );
      tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
      recover_account_operation recover;
      recover.account_to_recover = "victim";
      recover.new_owner_authority = alternative_auth;
      recover.recent_owner_authority = victim_auth;
      std::swap( victim_auth, alternative_auth ); //change for next iteration
      tx.operations.push_back( recover );

      // sign transaction outside of time measurement
      full_transaction_ptr _ftx = full_transaction_type::create_from_signed_transaction( tx, pack_type::hf26, false );
      _ftx->sign_transaction( _keys, db->get_chain_id(), pack_type::hf26 );
      uint64_t start_time = std::chrono::duration_cast< std::chrono::nanoseconds >(
        std::chrono::system_clock::now().time_since_epoch() ).count();
      get_chain_plugin().push_transaction( _ftx, 0 );
      uint64_t stop_time = std::chrono::duration_cast< std::chrono::nanoseconds >(
        std::chrono::system_clock::now().time_since_epoch() ).count();
      time += stop_time - start_time;
      tx.clear();
      generate_block();
    }
    ilog( "Average time for recovery transaction = ${t}ms",
      ( "t", ( time + 500'000 ) / ITERATIONS / 1'000'000 ) );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_tx_order_bug )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing different transaction order in pending transactions vs actual block" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS( (alice)(bob) )
    generate_block();
    vest( "bob", ASSET( "35000.000 TESTS" ) ); //<- change that amount to tune RC cost
    issue_funds( "alice", ASSET( "1000.000 TESTS" ) );

    const auto& alice_rc = db->get_account( "alice" );

    BOOST_TEST_MESSAGE( "Clear RC on alice and wait a bit so she has enough for one operation but not two" );
    clear_mana( db_plugin, alice_rc );
    generate_block();
    generate_block();

    signed_transaction tx1, tx2;
    tx1.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    tx2.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    transfer_operation transfer;
    transfer.from = "alice";
    transfer.to = "bob";
    transfer.amount = ASSET( "10.000 TESTS" );
    transfer.memo = "First transfer";
    tx1.operations.push_back( transfer );
    push_transaction( tx1, alice_private_key ); //t1
    BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "990.000 TESTS" ) );
    BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "10.000 TESTS" ) );
    transfer.amount = ASSET( "5.000 TESTS" );
    transfer.memo = "Second transfer";
    tx2.operations.push_back( transfer );
    HIVE_REQUIRE_EXCEPTION( push_transaction( tx2, alice_private_key ), "has_mana", plugin_exception ); //t2
    BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "990.000 TESTS" ) );
    BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "10.000 TESTS" ) );
    generate_block(); //t1 becomes part of block

    BOOST_TEST_MESSAGE( "Save aside and remove head block" );
    auto block = get_block_reader().get_block_by_number( db->head_block_num() );
    BOOST_REQUIRE( block );
    db->pop_block(); //t1 becomes popped
    BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "1000.000 TESTS" ) );
    BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "0.000 TESTS" ) );

    BOOST_TEST_MESSAGE( "Reapply transaction that failed before putting it to pending" );
    push_transaction( tx2, alice_private_key, 0 ); //t2 becomes pending
    BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "995.000 TESTS" ) );
    BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "5.000 TESTS" ) );

    BOOST_TEST_MESSAGE( "Push previously popped block - pending transaction should run into lack of RC again" );
    //the only way to see if we run into problem is to observe ilog messages
    BOOST_REQUIRE( fc::logger::get( DEFAULT_LOGGER ).is_enabled( fc::log_level::info ) );
    {
      struct tcatcher : public fc::appender
      {
        virtual void log( const fc::log_message& m )
        {
          const char* PROBLEM_MSG = "Accepting transaction by alice";
          BOOST_REQUIRE_NE( std::memcmp( m.get_message().c_str(), PROBLEM_MSG, std::strlen( PROBLEM_MSG ) ), 0 );
        }
      };
      auto catcher = fc::shared_ptr<tcatcher>( new tcatcher() );
      autoscope auto_reset( [&]() { fc::logger::get( DEFAULT_LOGGER ).remove_appender( catcher ); } );
      fc::logger::get( DEFAULT_LOGGER ).add_appender( catcher );
      push_block( block );
      //t1 was applied as part of block, then popped version of t1 was skipped as duplicate and t2 was
      //applied as pending, however lack of RC caused it to be dropped; we can check that by looking at balances
      BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "990.000 TESTS" ) );
      BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "10.000 TESTS" ) );

      generate_block();
      //due to change that makes pending transactions drop from list due to lack of RC, testing fix for
      //former 'is_producing() == false' when building new block is no longer possible; nevertheless witness actually
      //'is_in_control()' when producing block, which means pending t2 would not be included during block production
      //(due to lack of RC)
      BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "990.000 TESTS" ) );
      BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "10.000 TESTS" ) );
      block = get_block_reader().get_block_by_number( db->head_block_num() );
      BOOST_REQUIRE( block );
      //check that block is indeed empty, without t2 and that tx2 waits as pending <- no longer true; tx2 was dropped above
      //BOOST_REQUIRE( block->get_block().transactions.empty() && !db->_pending_tx.empty() );

      //previously transaction was waiting in pending until alice gained enough RC or transaction expired; since now
      //it was dropped, we can send it again after couple blocks, because it won't be treated as duplicate
      generate_block();
      generate_block();
      push_transaction( tx2, alice_private_key, 0 );

      //check that t2 did not expire while RC was regenerating and got accepted when send second time
      BOOST_REQUIRE( get_balance( "alice" ) == ASSET( "985.000 TESTS" ) );
      BOOST_REQUIRE( get_balance( "bob" ) == ASSET( "15.000 TESTS" ) );
    }

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_pending_data_reset )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing if block info (formely rc_pending_data) resets properly" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS( (alice)(bob) )
    generate_block();
    fund( "alice", ASSET( "1000.000 TESTS" ) );
    generate_block();

    auto check_direction = []( const resource_cost_type& values, const std::array< int, HIVE_RC_NUM_RESOURCE_TYPES >& sign )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
      {
        switch( sign[i] )
        {
          case -1:
            BOOST_REQUIRE_LT( values[i], 0 );
            break;
          case 0:
            BOOST_REQUIRE_EQUAL( values[i], 0 );
            break;
          case 1:
            BOOST_REQUIRE_GT( values[i], 0 );
            break;
          default:
            assert( false && "Incorrect use" );
            break;
        }
      }
    };
    auto compare = [&]( const resource_count_type& v1, const resource_count_type& v2 )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
        BOOST_REQUIRE_EQUAL( v1[i], v2[i] );
    };
    auto add = [&]( resource_count_type* v1, const resource_count_type& v2 )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
        (*v1)[i] += v2[i];
    };
    resource_count_type empty;
    resource_count_type pending_usage;

    BOOST_TEST_MESSAGE( "Pending usage and pending cost are all zero at the start of block" );
      //ABW: above comment is true, but only when applying block; when checking after nonempty
      //block, we'll still have values from that block (in our case data from funding 'alice')
    generate_block(); //empty block that clears block info left from previous block
    compare( db->rc.get_block_info().usage, empty );
    compare( db->rc.get_block_info().cost, empty );
    //ABW: since we no longer keep differential usage as separate data piece, we can only check full
    //actual usage after the end of transaction (the values will stay until next transaction is processed);
    //the values won't change all that frequently (unlike RC cost) so it should be ok to do full comparison
    auto tx_usage = db->rc.get_tx_info().usage;
    //last tx was a transfer
    compare( tx_usage, { 112, 0, 1120, 128, 94165 + 6622 + 5999 } );
    //not adding transfer to pending usage since it is already part of block before current head

    BOOST_TEST_MESSAGE( "Update active key to generate some usage" );
    auto new_alice_private_key = generate_private_key( "alice_active" );
    account_update_operation update;
    update.account = "alice";
    update.active = authority( 1, new_alice_private_key.get_public_key(), 1 );
    push_transaction( update, alice_private_key );
    //transaction info for pending transactions in no longer added to block info
    compare( db->rc.get_block_info().usage, empty );
    compare( db->rc.get_block_info().cost, empty );
    tx_usage = db->rc.get_tx_info().usage;
    //account update gets discount for authorities already present, so their state consumptions cancel out
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13322 } );
    add( &pending_usage, tx_usage );

    BOOST_TEST_MESSAGE( "Make a transfer - differential usage should reset" );
    transfer_operation transfer;
    transfer.from = "alice";
    transfer.to = "bob";
    transfer.amount = ASSET( "10.000 TESTS" );
    push_transaction( transfer, new_alice_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    //another transfer (differences are just due to sizes of account names)
    compare( tx_usage, { 106, 0, 1060, 128, 94165 + 6622 + 5999 } );
    add( &pending_usage, tx_usage );

    BOOST_TEST_MESSAGE( "Update active key for second time - differential usage resets again to the same value" );
    auto new_alice_2_private_key = generate_private_key( "alice_active_2" );
    update.active = authority( 1, new_alice_2_private_key.get_public_key(), 1 );
    push_transaction( update, new_alice_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    //same as with previous account update
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13322 } );
    auto last_valid_tx_usage = tx_usage;
    add( &pending_usage, tx_usage );

    BOOST_TEST_MESSAGE( "Attempt to update active key for third time but fail - all values but tx info revert to previous" );
    update.active = authority( 1, "nonexistent", 1 );
    HIVE_REQUIRE_ASSERT( push_transaction( update, new_alice_2_private_key ), "a != nullptr" );
    tx_usage = db->rc.get_tx_info().usage;
    //since transaction was stopped mid way, usage only contains part filled prior to failure (that is, a discount on state)
    compare( tx_usage, { 0, 0, 0, -1576800, 0 } );
    //not adding failed transaction usage to future block usage since it didn't become pending transaction

    BOOST_TEST_MESSAGE( "Finalize block and move to new one - pending data and cost are reset (but not differential usage)" );
    generate_block();
    compare( db->rc.get_block_info().usage, pending_usage );
    check_direction( db->rc.get_block_info().cost, { 1, 0, 1, 1, 1 } );
    //Why two generate_block calls? To understand that we need to understand what generate_block actually
    //does. When transaction is pushed, it becomes pending (as if it was passed through API or P2P).
    //generate_block rewinds the state, then it produces block out of pending transactions (assuming
    //they actually fit, because we might've pushed too many for max size of block), rewinds state again
    //and reapplies it as a whole opening it (pre-apply block), applying all transactions from fresh
    //block and closing the block (post-apply block); tries to reapply pending transactions on top of it
    //(the ones that became part of recent block will be dropped from list as "known") and finishes.
    //The code after generate_block call will see the state after reapplication of transactions and
    //not freshly after reset. To emulate fresh reset we need to actually produce empty block.
    generate_block();
    compare( db->rc.get_block_info().usage, empty );
    compare( db->rc.get_block_info().cost, empty );
    //since last transaction that was applied during block generations above is last successful account
    //update, that is what we are seing now in tx info
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, last_valid_tx_usage );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_differential_usage_operations )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing differential RC usage for selected operations" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    generate_block();

    ACTORS( (alice)(bob)(sam) )
    generate_block();
    fund( "alice", ASSET( "1000.000 TESTS" ) );
    vest( "alice", ASSET( "10.000 TESTS" ) );
    vest( "bob", ASSET( "10.000 TESTS" ) );
    generate_block();

    auto alice_owner_key = generate_private_key( "alice_owner" );
    auto bob_owner_key = generate_private_key( "bob_owner" );

    auto alice_active_key  = generate_private_key( "alice_active" );
    auto bob_active_key  = generate_private_key( "bob_active" );

    auto alice_posting_key = generate_private_key( "alice_posting" );

    auto compare = [&]( const resource_count_type& v1, const resource_count_type& v2 )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
        BOOST_REQUIRE_EQUAL( v1[ i ], v2[ i ] );
    };
    //rc_pending_data_reset test showed that differential usage is reset by transaction but not block
    //so let's make transfer to force reset to known usage values of that transfer
    auto clean = [&]( const std::optional<fc::ecc::private_key>& current_active_key = std::optional<fc::ecc::private_key>() )
    {
      transfer_operation transfer;
      transfer.from = "alice";
      transfer.to = "bob";
      transfer.amount = ASSET( "0.001 TESTS" );
      push_transaction( transfer, current_active_key.has_value() ? current_active_key.value() : alice_active_key );
      generate_block();
      generate_block();
      auto tx_usage = db->rc.get_tx_info().usage;
      compare( tx_usage, { 106, 0, 1060, 128, 106786 } );
      const auto& block_info = db->rc.get_block_info();
      compare( block_info.usage, { 0, 0, 0, 0, 0 } );
      compare( block_info.cost, { 0, 0, 0, 0, 0 } );
    };

    BOOST_TEST_MESSAGE( "Update owner key with account_update_operation" );
    account_update_operation update;
    update.account = "alice";
    update.owner = authority( 1, alice_owner_key.get_public_key(), 1 );
    push_transaction( update, alice_private_key );
    update.owner.reset();
    auto tx_usage = db->rc.get_tx_info().usage;
    //there is extra state for 30 days of holding historical owner key
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800 + 120960, 94165 + 6622 + 13322 } );
    clean( alice_private_key );

    BOOST_TEST_MESSAGE( "Update active key with account_update_operation" );
    update.active = authority( 1, alice_active_key.get_public_key(), 1 );
    push_transaction( update, alice_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13322 } );
    update.active.reset();
    clean();

    BOOST_TEST_MESSAGE( "Update posting key with account_update_operation" );
    update.posting = authority( 1, alice_posting_key.get_public_key(), 1 );
    push_transaction( update, alice_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13322 } );
    update.posting.reset();
    clean();

    BOOST_TEST_MESSAGE( "Update memo key with account_update_operation" );
    update.memo_key = generate_private_key( "alice_memo" ).get_public_key();
    push_transaction( update, alice_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //memo key does not allocate new state
    compare( tx_usage, { 122, 0, 0, 128, 94165 + 6622 + 13322 } );
    update.memo_key = public_key_type();
    clean();

    BOOST_TEST_MESSAGE( "Update metadata with account_update_operation" );
    update.json_metadata = "{\"profile_image\":\"https://somewhere.com/myself.png\"}";
    push_transaction( update, alice_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //while metadata allocates state, it is optionally stored which means nodes that don't store it
    //would not be able to calculate differential usage; that's why metadata is not subject to
    //differential usage calculation
    compare( tx_usage, { 174, 0, 0, 128, 94165 + 6622 + 13322 } );
    update.json_metadata = "";
    clean();

    account_update2_operation update2;
    BOOST_TEST_MESSAGE( "Update owner key with account_update2_operation" );
    update2.account = "bob";
    update2.owner = authority( 1, bob_owner_key.get_public_key(), 1 );
    push_transaction( update2, bob_private_key );
    update2.owner.reset();
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 131, 0, 0, 128 - 1576800 + 1576800 + 120960, 94165 + 6622 + 13648 } );
    clean();

    BOOST_TEST_MESSAGE( "Update active key with account_update2_operation" );
    update2.active = authority( 1, bob_active_key.get_public_key(), 1 );
    push_transaction( update2, bob_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 131, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13648 } );
    update2.active.reset();
    clean();

    BOOST_TEST_MESSAGE( "Update posting key with account_update2_operation" );
    update2.posting = authority( 1, generate_private_key( "bob_posting" ).get_public_key(), 1 );
    push_transaction( update2, bob_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 131, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13648 } );
    update2.posting.reset();
    clean();

    BOOST_TEST_MESSAGE( "Update memo key with account_update2_operation" );
    update2.memo_key = generate_private_key( "bob_memo" ).get_public_key();
    push_transaction( update2, bob_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //memo key does not allocate new state
    compare( tx_usage, { 123, 0, 0, 128, 94165 + 6622 + 13648 } );
    update2.memo_key = public_key_type();
    clean();

    BOOST_TEST_MESSAGE( "Update metadata with account_update2_operation" );
    update2.json_metadata = "{\"profile_image\":\"https://somewhere.com/superman.png\"}";
    update2.posting_json_metadata = "{\"description\":\"I'm here just for test.\"}";
    push_transaction( update2, bob_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //same as in case of account_update_operation
    compare( tx_usage, { 218, 0, 0, 128, 94165 + 6622 + 13648 } );
    update2.json_metadata = "";
    update2.posting_json_metadata = "";
    clean();

    BOOST_TEST_MESSAGE( "Register witness with witness_update_operation" );
    witness_update_operation witness;
    witness.owner = "alice";
    witness.url = "https://alice.has.cat";
    witness.block_signing_key = generate_private_key( "alice_witness" ).get_public_key();
    witness.props.account_creation_fee = legacy_hive_asset::from_amount( HIVE_MIN_ACCOUNT_CREATION_FEE );
    witness.props.maximum_block_size = HIVE_MIN_BLOCK_SIZE_LIMIT * 2;
    witness.props.hbd_interest_rate = 0;
    witness.fee = asset( 0, HIVE_SYMBOL );
    push_transaction( witness, alice_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //when witness is created there is no initial state to give discount
    compare( tx_usage, { 178, 0, 0, 128 + 23827200 + 919800, 94165 + 6622 + 4237 } );
    clean();

    BOOST_TEST_MESSAGE( "Update witness with witness_update_operation" );
    witness.url = "https://alice.wonder.land/my.cat";
    push_transaction( witness, alice_active_key );
    tx_usage = db->rc.get_tx_info().usage;
    //when witness is updated differential usage kicks in
    compare( tx_usage, { 189, 0, 0, 128 - 23827200 - 919800 + 23827200 + 1401600, 94165 + 6622 + 4237 } );
    clean();

    auto alice_new_owner_key = generate_private_key( "alice_new_owner" );
    auto bob_new_owner_key = generate_private_key( "bob_new_owner" );
    auto thief_public_key = generate_private_key( "stolen" ).get_public_key();

    BOOST_TEST_MESSAGE( "Compromise account authority and request recovery with request_account_recovery_operation" );
    //we can't pretend previous change of owner was from a thief because in testnet we only have 20 blocks
    //worth of owner history and 3 blocks for recovery after request
    update.account = "alice";
    update.owner = authority( 1, thief_public_key, 1 );
    push_transaction( update, alice_owner_key );
    update.owner.reset();
    generate_block();
    request_account_recovery_operation request;
    request.account_to_recover = "alice";
    request.recovery_account = "initminer";
    request.new_owner_authority = authority( 1, alice_new_owner_key.get_public_key(), 1 );
    push_transaction( request, init_account_priv_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 137, 0, 0, 128 + 4608, 94165 + 6622 + 7947 } ); //no differential usage
    generate_block();
    generate_block();

    BOOST_TEST_MESSAGE( "Recover account with recover_account_operation - subsidized version" );
    recover_account_operation recover;
    recover.account_to_recover = "alice";
    recover.recent_owner_authority = authority( 1, alice_owner_key.get_public_key(), 1 );
    recover.new_owner_authority = authority( 1, alice_new_owner_key.get_public_key(), 1 );
    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    tx.operations.push_back( recover );
    push_transaction( tx, {alice_owner_key, alice_new_owner_key} );
    tx.clear();
    alice_owner_key = alice_new_owner_key;
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 0, 0, 0, 0, 0 } ); //this is subsidized
    clean();

    BOOST_TEST_MESSAGE( "Compromise account authority and request recovery with request_account_recovery_operation" );
    update.account = "bob";
    update.owner = authority( 1, thief_public_key, 1 );
    push_transaction( update, bob_owner_key );
    update.owner.reset();
    generate_block();
    request.account_to_recover = "bob";
    request.new_owner_authority = authority( 1, bob_new_owner_key.get_public_key(), 1, generate_private_key( "bob_new_owner2" ).get_public_key(), 1 );
    push_transaction( request, init_account_priv_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 170, 0, 0, 128 + 4608, 94165 + 6622 + 7947 } ); //no differential usage
    generate_block();
    generate_block();

    BOOST_TEST_MESSAGE( "Recover account with recover_account_operation - fully paid version" );
    recover.account_to_recover = "bob";
    recover.recent_owner_authority = authority( 1, bob_owner_key.get_public_key(), 1 );
    recover.new_owner_authority = authority( 1, bob_new_owner_key.get_public_key(), 1, generate_private_key( "bob_new_owner2" ).get_public_key(), 1 );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    tx.operations.push_back( recover );
    push_transaction( tx, {bob_owner_key, bob_new_owner_key} );
    tx.clear();
    bob_owner_key = bob_new_owner_key;
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 266, 0, 0, 128 - 1576800 + 1576800 * 2 + 120960, 94165 * 2 + 6622 + 18955 } ); //this is fully paid
    clean();

    auto calculate_cost = []( const resource_cost_type& costs ) -> int64_t
    {
      int64_t result = 0;
      for( auto& cost : costs )
        result += cost;
      return result;
    };

    BOOST_TEST_MESSAGE( "Create RC delegation with delegate_rc_operation" );
    delegate_rc_operation rc_delegation;
    rc_delegation.from = "alice";
    rc_delegation.delegatees = { "bob" };
    rc_delegation.max_rc = 10000000;
    custom_json_operation custom_json;
    custom_json.required_posting_auths.insert( "alice" );
    custom_json.id = HIVE_RC_CUSTOM_OPERATION_ID;
    custom_json.json = fc::json::to_string( hive::protocol::rc_custom_operation( rc_delegation ) );
    push_transaction( custom_json, alice_posting_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 203, 0, 0, 128 + 1927200, 94165 + 6622 + 1509 + 75000 } );
    generate_block(); //block generation is needed so the pending transaction cost is added to actual block cost
    auto first_delegation_cost = calculate_cost( db->rc.get_block_info().cost );
    clean();

    const auto diff_limit = ( first_delegation_cost + 99 ) / 100; //rounded up 1% of first delegation cost

    BOOST_TEST_MESSAGE( "Create and update RC delegation with delegate_rc_operation" );
    rc_delegation.delegatees = { "bob", "sam" };
    rc_delegation.max_rc = 5000000;
    custom_json.json = fc::json::to_string( hive::protocol::rc_custom_operation( rc_delegation ) );
    push_transaction( custom_json, alice_posting_key );
    tx_usage = db->rc.get_tx_info().usage;
    //just one new delegation like in first case
    compare( tx_usage, { 208, 0, 0, 128 - 1927200 + 1927200 * 2, 94165 + 6622 + 1509 + 75000 } );
    generate_block();
    auto second_delegation_cost = calculate_cost( db->rc.get_block_info().cost );
    //cost of first and second should be almost the same (allowing small difference)
    BOOST_REQUIRE_LT( abs( first_delegation_cost - second_delegation_cost ), diff_limit );
    clean();

    BOOST_TEST_MESSAGE( "Update RC delegations with delegate_rc_operation" );
    rc_delegation.max_rc = 7500000;
    custom_json.json = fc::json::to_string( hive::protocol::rc_custom_operation( rc_delegation ) );
    push_transaction( custom_json, alice_posting_key );
    tx_usage = db->rc.get_tx_info().usage;
    //no extra state - all delegations are updates
    compare( tx_usage, { 208, 0, 0, 128 - 1927200 * 2 + 1927200 * 2, 94165 + 6622 + 1509 + 75000 } );
    generate_block();
    auto third_delegation_cost = calculate_cost( db->rc.get_block_info().cost );
    //cost of third should be minuscule (allowing small value)
    BOOST_REQUIRE_LT( third_delegation_cost, diff_limit );
    clean();

    //for comparison we're doing the same custom json but with dummy tag, so it is not interpreted
    BOOST_TEST_MESSAGE( "Fake RC delegation update with delegate_rc_operation under dummy tag" );
    custom_json.id = "dummy";
    push_transaction( custom_json, alice_posting_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 211, 0, 0, 128, 94165 + 6622 + 1509 } ); //no differential usage
    generate_block();
    auto dummy_delegation_cost = calculate_cost( db->rc.get_block_info().cost );
    //cost should be even lower than that for third actual delegation
    BOOST_REQUIRE_LT( dummy_delegation_cost, third_delegation_cost );
    clean();

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_differential_usage_negative )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing differential RC usage with potentially negative value" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    generate_block();

    PREP_ACTOR( alice )
      //alice will initially use HIVE_MAX_AUTHORITY_MEMBERSHIP of keys with the same full authority;
    PREP_ACTOR( barry )
      //similar to alice, but will be used in discount test at the end (less keys - to tune the usage)
    PREP_ACTOR( carol )
      //carol will use two keys (if usage changes, it might end up being not enough to outweight other
      //state usage of transaction, but for now it is enough to put her in negative when she changes
      //that to single key);
    PREP_ACTOR( diana )
      //diana uses single key from the start
      //this way only one signature will still be needed, making transactions for alice, carol and diana
      //weight the same, while alice's authority definition will be way heavier and carol a bit heavier

    auto compare = [&]( const resource_count_type& v1, const resource_count_type& v2 )
    {
      for( int i = 0; i < HIVE_RC_NUM_RESOURCE_TYPES; ++i )
        BOOST_REQUIRE_EQUAL( v1[ i ], v2[ i ] );
    };

    account_create_operation create;
    create.fee = db->get_witness_schedule_object().median_props.account_creation_fee;
    create.creator = "initminer";
    create.new_account_name = "alice";
    create.owner = authority( 1, alice_public_key, 1 );
    {
      std::string name( "alice0" );
      for( int i = 1; i < HIVE_MAX_AUTHORITY_MEMBERSHIP; ++i )
      {
        name[5] = '0' + i;
        create.owner.add_authority( generate_private_key( name ).get_public_key(), 1 );
      }
    }
    create.active = create.owner;
    create.posting = authority( 1, alice_post_key.get_public_key(), 1 );
    push_transaction( create, init_account_priv_key );
    create.new_account_name = "barry";
    create.owner = authority( 1, barry_public_key, 1 );
    const int NUM_KEYS = 3; //<- use this value to tune usage so in the end we have it small but positive
    {
      std::string name( "barry0" );
      for( int i = 1; i < NUM_KEYS; ++i )
      {
        name[5] = '0' + i;
        create.owner.add_authority( generate_private_key( name ).get_public_key(), 1 );
      }
    }
    create.active = create.owner;
    create.posting = authority( 1, barry_post_key.get_public_key(), 1 );
    push_transaction( create, init_account_priv_key );
    create.new_account_name = "carol";
    create.owner = authority( 1, carol_public_key, 1, generate_private_key( "carol1" ).get_public_key(), 1 );
    create.active = create.owner;
    create.posting = authority( 1, carol_post_key.get_public_key(), 1 );
    push_transaction( create, init_account_priv_key );
    create.new_account_name = "diana";
    create.owner = authority( 1, diana_public_key, 1 );
    create.active = create.owner;
    create.posting = authority( 1, diana_post_key.get_public_key(), 1 );
    push_transaction( create, init_account_priv_key );
    generate_block();

    //add some source of RC so they can actually perform those (expensive) operations
    vest( "alice", ASSET( "10.000 TESTS" ) );
    fund( "alice", ASSET( "10.000 TESTS" ) );
    vest( "barry", ASSET( "10.000 TESTS" ) );
    fund( "barry", ASSET( "10.000 TESTS" ) );
    vest( "carol", ASSET( "10.000 TESTS" ) );
    fund( "carol", ASSET( "10.000 TESTS" ) );
    vest( "diana", ASSET( "10.000 TESTS" ) );
    fund( "diana", ASSET( "10.000 TESTS" ) );
    generate_block();
    //see explanation at the end of rc_pending_data_reset test
    //note that we don't actually have access to data on transaction but whole block, so it is
    //important for this test to always use just one transaction and fully reset before next one
    generate_block();

    BOOST_TEST_MESSAGE( "Changing authority and comparing RC usage" );

    auto alice_active_private_key = generate_private_key( "alice_active" );
    account_update_operation update;
    update.account = "alice";
    update.active = authority( 1, alice_active_private_key.get_public_key(), 1 );
    push_transaction( update, alice_private_key );
    auto tx_usage = db->rc.get_tx_info().usage;
    //state usage didn't end up being negative
    compare( tx_usage, { 163, 0, 0, std::max( 0, 128 - 1576800 * 40 + 1576800 ), 94165 + 6622 + 13322 } );
    generate_block();
    generate_block();

    update.account = "carol";
    update.active = authority( 1, generate_private_key( "carol_active" ).get_public_key(), 1 );
    push_transaction( update, carol_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 163, 0, 0, std::max( 0, 128 - 1576800 * 2 + 1576800 ), 94165 + 6622 + 13322 } );
    generate_block();
    generate_block();

    update.account = "diana";
    update.active = authority( 1, generate_private_key( "diana_active" ).get_public_key(), 1 );
    push_transaction( update, diana_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    compare( tx_usage, { 163, 0, 0, 128 - 1576800 + 1576800, 94165 + 6622 + 13322 } );
    generate_block();
    generate_block();

    BOOST_TEST_MESSAGE( "Changing authority and using its negative usage to discount other operations" );
    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    transfer_to_vesting_operation powerup;
    powerup.from = "barry";
    powerup.to = "barry";
    powerup.amount = ASSET( "10.000 TESTS" );
    tx.operations.push_back( powerup );
    //that operation on itself has small positive state usage
    delegate_vesting_shares_operation delegation;
    delegation.delegator = "barry";
    delegation.delegatee = "alice";
    delegation.vesting_shares = ASSET( "5.000000 VESTS" );
    tx.operations.push_back( delegation );
    //that operation on itself has fairly noticable positive state usage
    update.account = "barry";
    update.active = authority( 1, generate_private_key( "barry_active" ).get_public_key(), 1 );
    tx.operations.push_back( update );
    //as we've seen above such update will have significant negative state usage
    //some more ops so we can get positive state usage at the end (also NUM_KEYS can be used for tuning)
    delegation.delegatee = "carol";
    delegation.vesting_shares = ASSET( "5.000000 VESTS" );
    tx.operations.push_back( delegation );
    //delegation.delegatee = "diana";
    //delegation.vesting_shares = ASSET( "5.000000 VESTS" );
    //tx.operations.push_back( delegation );
    push_transaction( tx, barry_private_key );
    tx.clear();
    tx_usage = db->rc.get_tx_info().usage;
    // vesting + delegation + account update + delegation
    compare( tx_usage, { 250, 0, 2500,
      128 + 11520 + 1927200 + ( -1576800 * 3 + 1576800 ) + 1927200,
      94165 + 6622 + 42245 + 16727 + 13322 + 16727 } );
    auto barry_state_usage = tx_usage[ resource_state_bytes ];
    generate_block();
    generate_block();

    BOOST_TEST_MESSAGE( "Comparing state usage with delegation in separate transaction" );
    delegation.delegator = "alice";
    delegation.delegatee = "barry";
    push_transaction( delegation, alice_active_private_key );
    tx_usage = db->rc.get_tx_info().usage;
    auto alice_delegation_state_usage = tx_usage[ resource_state_bytes ];
    //check if we tuned the test so single delegation uses more state than authority reducing update mixed with couple delegations
    BOOST_REQUIRE_LT( barry_state_usage, alice_delegation_state_usage );
    generate_block();
    generate_block();

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_differential_usage_many_ops )
{
  try
  {
    BOOST_TEST_MESSAGE( "Testing differential RC usage with multiple operations" );

    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS( (alice)(carol) )
    generate_block();
    vest( "alice", ASSET( "10.000 TESTS" ) );
    vest( "carol", ASSET( "10.000 TESTS" ) );
    generate_block();
    //see explanation at the end of rc_pending_data_reset test
    //note that we don't actually have access to data on transaction but whole block, so it is
    //important for this test to always use just one transaction and fully reset before next one
    generate_block();

    BOOST_TEST_MESSAGE( "Testing when related witness does not exist before transaction" );
    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    witness_update_operation witness;
    witness.owner = "alice";
    witness.url = "https://alice.wonder.land/my.cat";
    witness.block_signing_key = generate_private_key( "alice_witness" ).get_public_key();
    witness.props.account_creation_fee = legacy_hive_asset::from_amount( HIVE_MIN_ACCOUNT_CREATION_FEE );
    witness.props.maximum_block_size = HIVE_MIN_BLOCK_SIZE_LIMIT * 2;
    witness.props.hbd_interest_rate = 0;
    witness.fee = asset( 0, HIVE_SYMBOL );
    tx.operations.push_back( witness );
    witness.url = "https://alice.has.cat";
    tx.operations.push_back( witness );
    push_transaction( tx, alice_private_key );
    tx.clear();
    auto alice_state_usage = db->rc.get_block_info().usage[ resource_state_bytes ];
    generate_block();
    generate_block();

    witness.owner = "carol";
    witness.url = "blocks@north.carolina";
    witness.block_signing_key = generate_private_key( "carol_witness" ).get_public_key();
    witness.props.account_creation_fee = legacy_hive_asset::from_amount( HIVE_MAX_ACCOUNT_CREATION_FEE / 2 );
    witness.props.maximum_block_size = HIVE_MAX_BLOCK_SIZE;
    witness.props.hbd_interest_rate = 30 * HIVE_1_PERCENT;
    witness.fee = asset( 100, HIVE_SYMBOL );
    push_transaction( witness, carol_private_key );
    auto carol_state_usage = db->rc.get_block_info().usage[ resource_state_bytes ];
    generate_block();
    generate_block();

    //both witnesses take the same state space, even though alice had hers built with two operations
    //it wouldn't calculate properly if visitor was run in pre-apply transaction instead of pre-apply
    //operation (second witness update for alice would not yet see state resulting from first update)
    BOOST_REQUIRE_EQUAL( alice_state_usage, carol_state_usage );

    validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( rc_exception_during_modify )
{
  bool expected_exception_found = false;

  try
  {
    BOOST_TEST_MESSAGE( "Testing exception throw during rc_account modify" );
    inject_hardfork( HIVE_BLOCKCHAIN_VERSION.minor_v() );
    configuration_data.allow_not_enough_rc = false;

    ACTORS((dave))
    generate_block();
    vest( "dave", ASSET( "70000.000 TESTS" ) ); //<- change that amount to tune RC cost
    issue_funds( "dave", ASSET( "1000.000 TESTS" ) );

    generate_block();

    const auto& dave_rc = db->get_account( "dave" );

    BOOST_TEST_MESSAGE( "Clear RC on dave" );
    clear_mana( db_plugin, dave_rc );

    transfer_operation transfer;
    transfer.from = "dave";
    transfer.to = "initminer";
    transfer.amount = ASSET( "0.001 TESTS" );
    transfer.memo = "test";

    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    tx.operations.push_back( transfer );

    try
    {
      BOOST_TEST_MESSAGE( "Attempting to push transaction" );
      push_transaction( tx, dave_private_key );
    }
    catch( const hive::chain::not_enough_rc_exception& e )
    {
      BOOST_TEST_MESSAGE( "Caught exception..." );
      const auto& saved_log = e.get_log();

      for( const auto& msg : saved_log )
      {
        fc::variant_object data = msg.get_data();
        if( data.contains( "tx_info" ) )
        {
          expected_exception_found = true;
          const fc::variant_object& tx_info_data = data[ "tx_info" ].get_object();
          BOOST_REQUIRE_EQUAL( tx_info_data[ "payer" ].as_string(), "dave" );
          break;
        }
      }
    }

    /// Find dave RC account once again and verify pointers to chain object (they should match)
    const auto* dave_rc2 = db->find_account( "dave" );
    BOOST_REQUIRE_EQUAL( &dave_rc, dave_rc2 );
  }
  FC_LOG_AND_RETHROW()

  BOOST_REQUIRE_EQUAL( expected_exception_found, true );
}

BOOST_AUTO_TEST_SUITE_END()

#endif
