#pragma once
#include <hive/chain/hive_fwd.hpp>
#include <hive/plugins/chain/chain_plugin.hpp>
#include <hive/plugins/p2p/p2p_plugin.hpp>
#include <hive/plugins/witness/block_producer.hpp>

#include <appbase/application.hpp>

#include <hive/protocol/testnet_blockchain_configuration.hpp>

#define HIVE_WITNESS_PLUGIN_NAME "witness"

#ifdef USE_ALTERNATE_CHAIN_ID
#define WITNESS_CUSTOM_OP_BLOCK_LIMIT (configuration_data.witness_custom_op_block_limit)
#else
#define WITNESS_CUSTOM_OP_BLOCK_LIMIT 5
#endif

namespace hive { namespace plugins { namespace witness {

namespace detail { class witness_plugin_impl; }

enum class block_production_condition
{
  produced = 0,
  not_synced = 1,
  not_my_turn = 2,
  not_time_yet = 3,
  no_private_key = 4,
  low_participation = 5,
  lag = 6,
  wait_for_genesis = 7,
  exception_producing_block = 8
};

struct produce_block_data_t
{
  produce_block_data_t() :
    next_slot( 0 ),
    next_slot_time( HIVE_GENESIS_TIME + HIVE_BLOCK_INTERVAL ),
    scheduled_witness( "" ),
    scheduled_private_key{},
    condition( block_production_condition::not_my_turn ),
    produce_in_next_slot( false )
  {}

  uint32_t next_slot;
  fc::time_point_sec next_slot_time;
  chain::account_name_type scheduled_witness;
  fc::ecc::private_key scheduled_private_key;
  block_production_condition condition;
  bool produce_in_next_slot;
};

class witness_plugin : public appbase::plugin< witness_plugin >
{
public:
  APPBASE_PLUGIN_REQUIRES(
    (hive::plugins::chain::chain_plugin)
    (hive::plugins::p2p::p2p_plugin)
  )

  witness_plugin();
  virtual ~witness_plugin();

  void enable_fast_confirm();
  void disable_fast_confirm();
  bool is_fast_confirm_enabled() const;

  void update_production_data( fc::time_point_sec time );
  const produce_block_data_t& get_production_data() const;

  typedef std::map< hive::protocol::public_key_type, fc::ecc::private_key > t_signing_keys;
  void add_signing_key( const fc::ecc::private_key& key );
  const t_signing_keys& get_signing_keys() const;

  void enable_queen_mode(); // turned on when queen_plugin is active

  typedef std::set< hive::protocol::account_name_type > t_witnesses;
  // for unit tests - overrides list of represented witnesses
  void set_witnesses( const t_witnesses& witnesses );

  static const std::string& name() { static std::string name = HIVE_WITNESS_PLUGIN_NAME; return name; }

  virtual void set_program_options(
    boost::program_options::options_description &command_line_options,
    boost::program_options::options_description &config_file_options
    ) override;

  virtual void plugin_initialize(const boost::program_options::variables_map& options) override;
  virtual void plugin_startup() override;
  virtual void plugin_shutdown() override;

private:
  std::unique_ptr< detail::witness_plugin_impl > my;
};

} } } // hive::plugins::witness
