#include <hive/chain/hive_fwd.hpp>

#include <hive/utilities/git_revision.hpp>

#include <hive/protocol/base.hpp>
#include <hive/protocol/hardfork.hpp>
#include <hive/protocol/dhf_operations.hpp>
#include <hive/protocol/validation.hpp>
#include <hive/protocol/crypto_memo.hpp>
#include <hive/protocol/transaction_util.hpp>
#include <hive/protocol/version.hpp>
#include <hive/protocol/hive_collateral.hpp>
#include <hive/wallet/wallet.hpp>
#include <hive/wallet/api_documentation.hpp>
#include <hive/wallet/reflect_util.hpp>

#include <hive/plugins/wallet_bridge_api/wallet_bridge_api_plugin.hpp>
#include <hive/wallet/misc_utilities.hpp>
#include <hive/chain/rc/rc_objects.hpp>
#include <hive/plugins/rc_api/rc_api.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <list>

#include <boost/version.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/range/algorithm/unique.hpp>
#include <boost/range/algorithm/sort.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/hashed_index.hpp>

#include <fc/container/deque.hpp>
#include <fc/git_revision.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <fc/io/stdio.hpp>
#include <fc/macros.hpp>
#include <fc/network/http/websocket.hpp>
#include <fc/rpc/cli.hpp>
#include <fc/rpc/websocket_api.hpp>
#include <fc/crypto/aes.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/thread/mutex.hpp>
#include <fc/thread/scoped_lock.hpp>
#include <fc/exception/exception.hpp>
#include <fc/smart_ref_impl.hpp>

#ifndef WIN32
# include <sys/types.h>
# include <sys/stat.h>
#endif

constexpr bool LOCK = false;  //DECLARE_API addes lock argument to all wallet_bridge_api methods. Default value is false, so we need to pass it here.

using hive::chain::full_transaction_ptr;
using hive::chain::full_transaction_type;

namespace hive { namespace wallet {

namespace detail {

template<class T>
optional<T> maybe_id( const string& name_or_id )
{
  if( std::isdigit( name_or_id.front() ) )
  {
    try
    {
      return fc::variant(name_or_id).as<T>();
    }
    catch (const fc::exception&)
    {
    }
  }
  return optional<T>();
}

string pubkey_to_shorthash( const public_key_type& key )
{
  uint32_t x = fc::sha256::hash(key)._hash[0];
  static const char hd[] = "0123456789abcdef";
  string result;

  result += hd[(x >> 0x1c) & 0x0f];
  result += hd[(x >> 0x18) & 0x0f];
  result += hd[(x >> 0x14) & 0x0f];
  result += hd[(x >> 0x10) & 0x0f];
  result += hd[(x >> 0x0c) & 0x0f];
  result += hd[(x >> 0x08) & 0x0f];
  result += hd[(x >> 0x04) & 0x0f];
  result += hd[(x        ) & 0x0f];

  return result;
}

struct op_prototype_visitor
{
  typedef void result_type;

  int t = 0;
  flat_map< std::string, operation >& name2op;

  op_prototype_visitor(
    int _t,
    flat_map< std::string, operation >& _prototype_ops
    ):t(_t), name2op(_prototype_ops) {}

  template<typename Type>
  result_type operator()( const Type& op )const
  {
    string name = fc::get_typename<Type>::name();
    size_t p = name.rfind(':');
    if( p != string::npos )
      name = name.substr( p+1 );
    name2op[ name ] = Type();
  }
};

class wallet_api_impl
{
  public:
    api_documentation method_documentation;
  private:
    void enable_umask_protection()
    {
#ifdef __unix__
      _old_umask = umask( S_IRWXG | S_IRWXO );
#endif
    }

    void disable_umask_protection()
    {
#ifdef __unix__
      umask( _old_umask );
#endif
    }

    void init_prototype_ops()
    {
      for( int t=0; t<operation::count(); t++ )
        {
        op_prototype_visitor  visitor(t, _prototype_ops);
        operation op(t, visitor);
        }
      return;
    }

public:
  wallet_api& self;
  wallet_api_impl( wallet_api& s, const wallet_data& initial_data, const chain_id_type& hive_chain_id,
    const fc::api< hive::plugins::wallet_bridge_api::wallet_bridge_api >& remote_api, transaction_serialization_type transaction_serialization,
    const std::string& store_transaction ) : self( s ), _wallet( initial_data ), _hive_chain_id( hive_chain_id ), _chosen_transaction_serialization(transaction_serialization),
    _remote_wallet_bridge_api( remote_api ), _key_collector( *this ), _store_transaction( store_transaction )
  {
    wallet_transaction_serialization::transaction_serialization = transaction_serialization;
    serialization_mode_controller::set_pack( transaction_serialization );
    init_prototype_ops();
  }

  virtual ~wallet_api_impl()
  {}

  void require_online()const
  {
    FC_ASSERT( !_wallet.offline, "Online mode is required in order to perform this method" );
  }

  void encrypt_keys()
  {
    if( !is_locked() )
    {
      plain_keys data;
      data.keys = _keys;
      data.checksum = _checksum;
      auto plain_txt = fc::raw::pack_to_vector(data);
      _wallet.cipher_keys = fc::aes_encrypt( data.checksum, plain_txt );
    }
  }

  bool copy_wallet_file( const string& destination_filename )
  {
    fc::path src_path = get_wallet_filename();
    if( !fc::exists( src_path ) )
      return false;
    fc::path dest_path = destination_filename + _wallet_filename_extension;
    int suffix = 0;
    while( fc::exists(dest_path) )
    {
      ++suffix;
      dest_path = destination_filename + "-" + std::to_string( suffix ) + _wallet_filename_extension;
    }
    wlog( "Backing up wallet ${src} to ${dest}",
        ("src", src_path)
        ("dest", dest_path) );

    fc::path dest_parent = fc::absolute(dest_path).parent_path();
    try
    {
      enable_umask_protection();
      if( !fc::exists( dest_parent ) )
        fc::create_directories( dest_parent );
      fc::copy( src_path, dest_path );
      disable_umask_protection();
    }
    catch(...)
    {
      disable_umask_protection();
      throw;
    }
    return true;
  }

  bool is_locked()const
  {
    return _checksum == fc::sha512();
  }

  wallet_serializer_wrapper<database_api::api_dynamic_global_property_object> get_dynamic_global_properties() const
  {
    require_online();
    return { _remote_wallet_bridge_api->get_dynamic_global_properties({}, LOCK) };
  }

  variant info() const
  {
    auto dynamic_props = get_dynamic_global_properties();

    fc::variant var;
    to_variant( dynamic_props, var );
    fc::mutable_variant_object result( var.get_object() );

    result["witness_majority_version"]  = fc::string( _remote_wallet_bridge_api->get_witness_schedule( vector<variant>{ false }, LOCK).majority_version);
    result["hardfork_version"]          = fc::string( _remote_wallet_bridge_api->get_hardfork_version({}, LOCK) );
    result["head_block_num"]            = dynamic_props.value.head_block_number;
    result["head_block_id"]             = dynamic_props.value.head_block_id;
    result["head_block_age"]            = fc::get_approximate_relative_time_string(dynamic_props.value.time, time_point_sec(time_point::now()), " old");
    result["participation"]             = (100*fc::uint128_popcount(dynamic_props.value.recent_slots_filled)) / 128.0;
    result["median_hbd_price"]          = wallet_serializer_wrapper<protocol::price>{ _remote_wallet_bridge_api->get_current_median_history_price({}, LOCK) };
    result["account_creation_fee"]      = wallet_serializer_wrapper<hive::protocol::asset>{ _remote_wallet_bridge_api->get_chain_properties({}, LOCK).account_creation_fee };

    protocol::hardfork_version current_hardfork_version;
    fc::from_variant(result["hardfork_version"], current_hardfork_version);
    if (current_hardfork_version >= HIVE_HARDFORK_0_17_VERSION)
    {
      result["post_reward_fund"] = wallet_serializer_wrapper<database_api::api_reward_fund_object>{ _remote_wallet_bridge_api->get_reward_fund(vector<variant>( {HIVE_POST_REWARD_FUND_NAME} ), LOCK ) };
    }
    else
    {
      result["post_reward_fund"] = fc::variant();
    }

    return result;
  }

  variant_object about() const
  {
    string client_version( hive::utilities::git_revision_description );
    const size_t pos = client_version.find( '/' );
    if( pos != string::npos && client_version.size() > pos )
      client_version = client_version.substr( pos + 1 );

    fc::mutable_variant_object result;
    result["blockchain_version"]       = HIVE_BLOCKCHAIN_VERSION;
    result["client_version"]           = client_version;
    result["hive_revision"]            = hive::utilities::git_revision_sha;
    result["hive_revision_age"]        = fc::get_approximate_relative_time_string( fc::time_point_sec( hive::utilities::git_revision_unix_timestamp ) );
    result["fc_revision"]              = fc::git_revision_sha;
    result["fc_revision_age"]          = fc::get_approximate_relative_time_string( fc::time_point_sec( fc::git_revision_unix_timestamp ) );
    result["chain_id"]                 = _hive_chain_id;
    result["compile_date"]             = "compiled on " __DATE__ " at " __TIME__;
    result["boost_version"]            = boost::replace_all_copy(std::string(BOOST_LIB_VERSION), "_", ".");
    result["openssl_version"]          = OPENSSL_VERSION_TEXT;

    std::string bitness = boost::lexical_cast<std::string>(8 * sizeof(int*)) + "-bit";
#if defined(__APPLE__)
    std::string os = "osx";
#elif defined(__linux__)
    std::string os = "linux";
#elif defined(_MSC_VER)
    std::string os = "win32";
#else
    std::string os = "other";
#endif
    result["build"] = os + " " + bitness;

    try
    {
      FC_ASSERT( !_wallet.offline ); // Throw fc::exception if not online
      auto v = _remote_wallet_bridge_api->get_version({}, LOCK);
      result["server_blockchain_version"] = v["blockchain_version"];
      result["server_hive_revision"] = v["hive_revision"];
      result["server_fc_revision"] = v["fc_revision"];
      result["server_chain_id"] = v["chain_id"];
      result["server_url"] = _wallet.ws_server;
    }
    catch( fc::exception& )
    {
      result["server"] = "Could not retrieve server version information";
    }

    return result;
  }

  database_api::api_account_object get_account( fc::variant account_name ) const
  {
    require_online();
    vector<variant> args{std::move(account_name)};
    auto account = _remote_wallet_bridge_api->get_account( {args}, LOCK );
    FC_ASSERT( account.valid(), "Account does not exist" );
    return *account;
  }

  vector<database_api::api_account_object> get_accounts( fc::variant account_names ) const
  {
    require_online();
    vector<variant> args{std::move(account_names)};
    return _remote_wallet_bridge_api->get_accounts( {args}, LOCK );
  }

  string get_wallet_filename() const { return _wallet_filename; }

  optional<fc::ecc::private_key>  try_get_private_key(const public_key_type& id)const
  {
    auto it = _keys.find(id);
    if( it != _keys.end() )
      return fc::ecc::private_key::wif_to_key( it->second );
    return optional<fc::ecc::private_key>();
  }

  fc::ecc::private_key              get_private_key(const public_key_type& id)const
  {
    auto has_key = try_get_private_key( id );
    FC_ASSERT( has_key );
    return *has_key;
  }


  fc::ecc::private_key get_private_key_for_account(const database_api::api_account_object& account)const
  {
    vector<public_key_type> active_keys = account.active.get_keys();
    if( active_keys.size() != 1 )
      FC_THROW("Expecting a simple authority with one active key");
    return get_private_key(active_keys.front());
  }

  // imports the private key into the wallet, and associate it in some way (?) with the
  // given account name.
  // @returns either true or false if the key has been successfully added to the storage
  bool import_key(const string& wif_key)
  {
    fc::optional<fc::ecc::private_key> optional_private_key = fc::ecc::private_key::wif_to_key(wif_key);
    if( !optional_private_key )
      FC_THROW("Invalid private key");
    hive::chain::public_key_type wif_pub_key = optional_private_key->get_public_key();

    return _keys.emplace( wif_pub_key, wif_key ).second;
  }

  bool load_wallet_file(string wallet_filename = "")
  {
    // TODO:  Merge imported wallet with existing wallet,
    //        instead of replacing it
    if( wallet_filename == "" )
      wallet_filename = _wallet_filename;

    if( ! fc::exists( wallet_filename ) )
      return false;

    _wallet = fc::json::from_file( wallet_filename, fc::json::format_validation_mode::full ).as< wallet_data >();

    return true;
  }

  void save_wallet_file(string wallet_filename = "")
  {
    //
    // Serialize in memory, then save to disk
    //
    // This approach lessens the risk of a partially written wallet
    // if exceptions are thrown in serialization
    //

    encrypt_keys();

    if( wallet_filename == "" )
      wallet_filename = _wallet_filename;

    wlog( "Saving wallet to file ${fn}", ("fn", wallet_filename) );

    string data = fc::json::to_pretty_string( _wallet );
    try
    {
      enable_umask_protection();
      //
      // Parentheses on the following declaration fails to compile,
      // due to the Most Vexing Parse.  Thanks, C++
      //
      // http://en.wikipedia.org/wiki/Most_vexing_parse
      //
      fc::ofstream outfile{ fc::path( wallet_filename ) };
      outfile.write( data.c_str(), data.length() );
      outfile.flush();
      outfile.close();
      disable_umask_protection();
    }
    catch(...)
    {
      disable_umask_protection();
      throw;
    }
  }

  annotated_signed_transaction_ex set_voting_proxy(const string& account_to_modify, const string& proxy, bool broadcast /* = false */)
  { try {
    account_witness_proxy_operation op;
    op.account = account_to_modify;
    op.proxy = proxy;

    signed_transaction tx;
    tx.operations.push_back( op );
    tx.validate();

    return sign_transaction( tx, broadcast );
  } FC_CAPTURE_AND_RETHROW( (account_to_modify)(proxy)(broadcast) ) }

  optional< database_api::api_witness_object > get_witness( const string& owner_account )
  {
    require_online();
    vector<variant> args{owner_account};
    return _remote_wallet_bridge_api->get_witness( {args}, LOCK );
  }

    /// Common body for claim_account_creation and claim_account_creation_nonblocking
  wallet_signed_transaction build_claim_account_creation(const string& creator, const hive::protocol::asset& fee,
    const std::function<annotated_signed_transaction_ex(signed_transaction)>& tx_signer);

  void set_transaction_expiration( uint32_t tx_expiration_seconds, bool is_hive_hardfork_1_28 )
  {
    FC_ASSERT( tx_expiration_seconds < ( is_hive_hardfork_1_28 ? HIVE_MAX_TIME_UNTIL_SIGNATURE_EXPIRATION : HIVE_MAX_TIME_UNTIL_EXPIRATION ) );
    _tx_expiration_seconds = tx_expiration_seconds;
  }

  void use_authority( authority_type type, const account_name_type& account_name )
  {
    switch( type )
    {
    case authority_type::active: _key_collector.use_active_authority( account_name ); break;
    case authority_type::owner: _key_collector.use_owner_authority( account_name ); break;
    case authority_type::posting: _key_collector.use_posting_authority( account_name ); break;
    }
  }

  void use_automatic_authority()
  {
    _key_collector.use_automatic_authority();
  }

  // sets the expiration time and reference block
  void initialize_transaction_header(transaction& tx)
  {
    auto dyn_props = get_dynamic_global_properties();
    tx.set_reference_block( dyn_props.value.head_block_id );
    tx.set_expiration( dyn_props.value.time + fc::seconds(_tx_expiration_seconds) );
  }

  // if the user rapidly sends two identical transactions (within the same block),
  // the second one will fail because it will have the same transaction id.  Waiting
  // a few seconds before sending the second transaction will allow it to succeed,
  // because the second transaction will be assigned a later expiration time.
  // This isn't a good solution for scripts, so we provide this method which
  // adds a do-nothing custom operation to the transaction which contains a
  // random 64-bit number, which will change the transaction's hash to prevent
  // collisions.
  full_transaction_ptr make_transaction_unique(const full_transaction_ptr& tx, const std::string& auth)
  {
    require_online();
    
    vector<variant> args{variant(tx->get_transaction_id())};
    if (_remote_wallet_bridge_api->is_known_transaction({args},LOCK))
    {
      signed_transaction tx_copy(tx->get_transaction());
      /// Reinitialize a header again, to potentially use fresh head block as reference block.
      initialize_transaction_header(tx_copy);

      // create a custom operation with a random 64-bit integer which will give this
      // transaction a new id
      custom_operation custom_op;
      custom_op.data.resize(8);
      fc::rand_bytes(custom_op.data.data(), custom_op.data.size());
      custom_op.required_auths.insert(auth);
      tx_copy.operations.push_back(custom_op);
      tx_copy.validate();

      auto new_tx = hive::chain::full_transaction_type::create_from_transaction(tx_copy, _chosen_transaction_serialization);
      return new_tx;
    }

    return tx;
  }

  annotated_signed_transaction_ex sign_transaction(
    signed_transaction tx,
    bool broadcast = false )
  {
    return sign_and_broadcast_transaction(std::move(tx), broadcast, true);
  }

  typedef std::function<full_transaction_ptr(full_transaction_ptr source_tx)> unique_tx_builder_t;

  annotated_signed_transaction_ex sign_and_broadcast_transaction(transaction tx, bool broadcast, bool blocking,
    unique_tx_builder_t unique_tx_builder = [](full_transaction_ptr ptr) -> full_transaction_ptr { return ptr; })
  {
    require_online();
    static const authority null_auth( 1, public_key_type(), 0 );

    flat_set<public_key_type> approving_key_set;
    // we need local copy to remain stateless, so other threads making the same call won't cause conflicts
    signing_keys_collector collector = _key_collector;
    collector.collect_signing_keys( &approving_key_set, tx );

    initialize_transaction_header(tx);

    auto new_tx = hive::chain::full_transaction_type::create_from_transaction(tx, _chosen_transaction_serialization);
    new_tx = unique_tx_builder(new_tx);

    //idump((_keys));
    flat_set< public_key_type > available_keys;
    flat_map< public_key_type, fc::ecc::private_key > available_private_keys;
    for( const public_key_type& key : approving_key_set )
    {
      auto it = _keys.find(key);
      if( it != _keys.end() )
      {
        fc::optional<fc::ecc::private_key> privkey = fc::ecc::private_key::wif_to_key( it->second );
        FC_ASSERT( privkey.valid(), "Malformed private key in _keys" );
        available_keys.insert(key);
        available_private_keys[key] = *privkey;
      }
    }

    std::vector<hive::protocol::private_key_type> keys_to_sign;

    if( collector.automatic_detection_enabled() )
    {
      const signed_transaction& signature_source = new_tx->get_transaction();

      protocol::hardfork_version _result = _remote_wallet_bridge_api->get_hardfork_version({}, LOCK);

      bool _allow_strict_and_mixed_authorities = _result.minor_v() >= 28;
      
      auto minimal_signing_keys = signature_source.minimize_required_signatures(
        _allow_strict_and_mixed_authorities,
        _hive_chain_id,
        available_keys,
        [&]( const string& account_name ) { return collector.get_account( account_name ).active; },
        [&]( const string& account_name ) { return collector.get_account( account_name ).owner; },
        [&]( const string& account_name ) { return collector.get_account( account_name ).posting; },
        [&]( const string& witness_name ) -> public_key_type
        {
          auto maybe_witness = get_witness(witness_name);
          return maybe_witness ? maybe_witness->signing_key : public_key_type();
        },
        HIVE_MAX_SIG_CHECK_DEPTH,
        HIVE_MAX_AUTHORITY_MEMBERSHIP,
        HIVE_MAX_SIG_CHECK_ACCOUNTS
        );

      for( const public_key_type& k : minimal_signing_keys )
      {
        auto it = available_private_keys.find(k);
        FC_ASSERT( it != available_private_keys.end() );

        keys_to_sign.push_back(it->second);
      }
    }
    else
    {
      for( const public_key_type& k : available_keys )
      {
        auto it = available_private_keys.find(k);
        FC_ASSERT( it != available_private_keys.end() );

        keys_to_sign.push_back(it->second);
      }
    }

    new_tx->sign_transaction(keys_to_sign, _hive_chain_id, _chosen_transaction_serialization);

    dump_transaction(*new_tx);

    if( broadcast ) {
      try {
        auto v = _remote_wallet_bridge_api->get_version({}, LOCK);
        FC_ASSERT(std::string(_hive_chain_id) == v["chain_id"], "chain id on wallet does not mach chain id on node");

        if( blocking )
        {
          auto result = _remote_wallet_bridge_api->broadcast_transaction_synchronous( vector<variant>{{variant(new_tx->get_transaction())}}, LOCK );
          annotated_signed_transaction_ex rtrx(new_tx->get_transaction(), new_tx->get_transaction_id(), result.block_num, result.trx_num, result.rc_cost);
          return rtrx;
        }
        else
        {
          _remote_wallet_bridge_api->broadcast_transaction( vector<variant>{{variant(new_tx->get_transaction())}}, LOCK );
          annotated_signed_transaction_ex _result(new_tx->get_transaction(), new_tx->get_transaction_id());
          return _result;
        }
      }
      catch (const fc::exception& e)
      {
        elog("Caught exception while broadcasting tx ${id}: ${e}", ("id", new_tx->get_transaction_id().str())("e", e.to_detail_string()) );
        throw;
      }
    }
    
    annotated_signed_transaction_ex _result(new_tx->get_transaction(), new_tx->get_transaction_id());

    return _result;
  }

  operation get_prototype_operation( const string& operation_name )
  {
    auto it = _prototype_ops.find( operation_name );
    if( it == _prototype_ops.end() )
      FC_THROW("Unsupported operation: \"${operation_name}\"", ("operation_name", operation_name));
    return it->second;
  }

  void dump_transaction( const hive::chain::full_transaction_type& full_trx) const
  {
    if( _store_transaction.empty() )
      return;

    try
    {
      const signed_transaction& trx = full_trx.get_transaction();

      fc::variant _v;
      fc::to_variant( wallet_serializer_wrapper<signed_transaction>{ trx }, _v );

      std::ofstream _file_json( _store_transaction + ".json", std::ios_base::out );
      _file_json << fc::json::to_string( _v );
      _file_json.flush();
      _file_json.close();
    }
    FC_CAPTURE_AND_LOG(())

    try
    {
      std::ofstream _file_bin( _store_transaction + ".bin", std::ios_base::out | std::ios_base::binary );
      
      full_trx.dump_serialized_transaction(_file_bin);

      _file_bin.flush();
      _file_bin.close();
    }
    FC_CAPTURE_AND_LOG(())
  }

  class signing_keys_collector : public hive::protocol::signing_keys_collector
  {
  public:
    signing_keys_collector( wallet_api_impl& _self ) : self( _self ) {}
    virtual ~signing_keys_collector() {}

    virtual void collect_signing_keys( flat_set< public_key_type >* keys, const transaction& tx ) override
    {
      approving_account_lut.clear();
      hive::protocol::signing_keys_collector::collect_signing_keys( keys, tx );
      // approving_account_lut remains filled until next call
    }

    virtual void prepare_account_authority_data( const std::vector< account_name_type >& accounts ) override
    {
      auto approving_account_objects = self.get_accounts( variant( accounts ) );
      FC_ASSERT( approving_account_objects.size() == accounts.size(), "Some requested accounts were not found",
        ( "found.size:", approving_account_objects.size() )( "requested.size", accounts.size() ) );
      for( const auto& account : approving_account_objects )
        approving_account_lut[ account.name ] = account;
    }

    const database_api::api_account_object& get_account( const account_name_type& account_name ) const
    {
      auto it = approving_account_lut.find( account_name );
      FC_ASSERT( it != approving_account_lut.end(),
        "Tried to access authority for account ${a} but not cached.", ( "a", account_name ) );
      return it->second;
    }

    virtual const authority& get_active( const account_name_type& account_name ) const override
    {
      return get_account( account_name ).active;
    }
    virtual const authority& get_owner( const account_name_type& account_name ) const override
    {
      return get_account( account_name ).owner;
    }
    virtual const authority& get_posting( const account_name_type& account_name ) const override
    {
      return get_account( account_name ).posting;
    }

  private:
    wallet_api_impl& self;
    flat_map< account_name_type, database_api::api_account_object > approving_account_lut;
  };

  string                                                    _wallet_filename;
  wallet_data                                               _wallet;
  chain_id_type                                             _hive_chain_id;
  transaction_serialization_type                            _chosen_transaction_serialization;
  map<public_key_type,string>                               _keys;
  fc::sha512                                                _checksum;
  fc::api< hive::plugins::wallet_bridge_api::wallet_bridge_api > _remote_wallet_bridge_api;
  uint32_t                                                  _tx_expiration_seconds = 30;

  flat_map<string, operation>                               _prototype_ops;

  static_variant_map _operation_which_map = create_static_variant_map< operation >();

  signing_keys_collector                                    _key_collector;

  std::string                             _store_transaction;

#ifdef __unix__
  mode_t                  _old_umask;
#endif
  const string _wallet_filename_extension = ".wallet";
};

wallet_signed_transaction wallet_api_impl::build_claim_account_creation(const string& creator, const hive::protocol::asset& fee,
  const std::function<annotated_signed_transaction_ex(signed_transaction)>& tx_signer)
{
  try
  {
    FC_ASSERT(!is_locked());

    auto creator_account = get_account(creator);
    claim_account_operation op;
    op.creator = creator;
    op.fee = fee;

    signed_transaction tx;
    tx.operations.push_back(op);
    tx.validate();

    return { tx_signer(tx) };
  } FC_CAPTURE_AND_RETHROW((creator))
}

} } } // hive::wallet::detail



namespace hive { namespace wallet {

wallet_api::wallet_api(const wallet_data& initial_data, const chain_id_type& hive_chain_id,
    const fc::api< hive::plugins::wallet_bridge_api::wallet_bridge_api >& remote_api, fc::promise< int >::ptr& exit_promise, bool is_daemon, output_formatter_type _output_formatter, transaction_serialization_type transaction_serialization, const std::string& store_transaction )
  : my(new detail::wallet_api_impl(*this, initial_data, hive_chain_id, remote_api, transaction_serialization, store_transaction)), exit_promise(exit_promise), is_daemon(is_daemon), output_formatter(_output_formatter)
{
}

wallet_api::~wallet_api(){}

bool wallet_api::copy_wallet_file(const string& destination_filename)
{
  return my->copy_wallet_file(destination_filename);
}

optional<wallet_serializer_wrapper<block_api::api_signed_block_object>> wallet_api::get_block(uint32_t num)
{
  my->require_online();
  vector<variant> args{num};
  block_api::get_block_return res = my->_remote_wallet_bridge_api->get_block( {args}, LOCK );
  if( res.block.valid() )
    return wallet_serializer_wrapper<block_api::api_signed_block_object>{ std::move( *(res.block) ) };
  else
    return wallet_serializer_wrapper<block_api::api_signed_block_object>{ block_api::api_signed_block_object() };
}

wallet_serializer_wrapper<vector< account_history::api_operation_object >> wallet_api::get_ops_in_block(uint32_t block_num, bool only_virtual)
{
  my->require_online();
  vector< variant > args{block_num, only_virtual};
  const auto operations = my->_remote_wallet_bridge_api->get_ops_in_block(args , LOCK ).ops;
  vector< account_history::api_operation_object > result;
  result.reserve(operations.size());
  for (const auto& op : operations)
    result.push_back(op);

  result.shrink_to_fit();
  return { result };
}

variant wallet_api::list_my_accounts()
{
  FC_ASSERT( !is_locked(), "Wallet must be unlocked to list accounts" );
  my->require_online();
  vector<database_api::api_account_object> result;

  vector<public_key_type> pub_keys;
  pub_keys.reserve( my->_keys.size() );

  for( const auto& item : my->_keys )
    pub_keys.push_back(item.first);

  vector<variant> args{ variant{ pub_keys } };

  auto _result = my->_remote_wallet_bridge_api->list_my_accounts( {args}, LOCK );
  return wallet_formatter::list_my_accounts( wallet_serializer_wrapper<vector<database_api::api_account_object>>{ _result }, output_formatter );
}

vector< account_name_type > wallet_api::list_accounts(const string& lowerbound, uint32_t limit)
{
  my->require_online();
  vector<variant> args{lowerbound, limit};
  const auto accounts = my->_remote_wallet_bridge_api->list_accounts( args, LOCK );
  vector<account_name_type> result;
  result.reserve(accounts.size());
  for (const auto& acc : accounts)
    result.push_back(acc);

  result.shrink_to_fit();
  return result;
}

get_active_witnesses_return wallet_api::get_active_witnesses( bool include_future )const
{
  my->require_online();
  vector<variant> args{ include_future };
  return my->_remote_wallet_bridge_api->get_active_witnesses( args, LOCK );
}

brain_key_info wallet_api::suggest_brain_key()const
{
  return hive::protocol::suggest_brain_key();
}

string wallet_api::serialize_transaction( const wallet_serializer_wrapper<signed_transaction>& tx )const
{
  return fc::to_hex(fc::raw::pack_to_vector(tx));
}

string wallet_api::get_wallet_filename() const
{
  return my->get_wallet_filename();
}


wallet_serializer_wrapper<database_api::api_account_object> wallet_api::get_account( const string& account_name ) const
{
  my->require_online();
  return { my->get_account( account_name ) };
}

wallet_serializer_wrapper<vector<database_api::api_account_object>> wallet_api::get_accounts( fc::variant account_names ) const
{
  my->require_online();
  return { my->get_accounts( std::move( account_names ) ) };
}

void wallet_api::import_key(const string& wif_key)
{
  FC_ASSERT(!is_locked());

  if( !my->import_key(wif_key) )
    wlog( "Private key duplication in storage: ${wif}", ("wif",wif_key) );

  save_wallet_file();
}

void wallet_api::import_keys( const vector< string >& wif_keys )
{
  FC_ASSERT(!is_locked());

  for( const auto& wif_key : wif_keys )
    if( !my->import_key(wif_key) )
      wlog( "Private key duplication in storage: ${wif}", ("wif",wif_key) );

  save_wallet_file();
}

string wallet_api::normalize_brain_key(string s) const
{
  return hive::protocol::normalize_brain_key( std::move( s ) );
}

variant wallet_api::info()
{
  my->require_online();
  return my->info();
}

variant_object wallet_api::about() const
{
  return my->about();
}

/*
fc::ecc::private_key wallet_api::derive_private_key(const std::string& prefix_string, int sequence_number) const
{
  return detail::derive_private_key( prefix_string, sequence_number );
}
*/

vector< account_name_type > wallet_api::list_witnesses(const string& lowerbound, uint32_t limit)
{
  my->require_online();
  vector<variant> args{lowerbound, limit};
  const auto& witnesses = my->_remote_wallet_bridge_api->list_witnesses( {args}, LOCK ).witnesses;
  vector<account_name_type> result;
  result.reserve(witnesses.size());
  for (const auto& witness : witnesses)
    result.push_back(witness.owner);

  result.shrink_to_fit();
  return result;
}

optional<wallet_serializer_wrapper<database_api::api_witness_object>> wallet_api::get_witness( const string& owner_account)
{
  my->require_online();
  optional<database_api::api_witness_object> res = my->get_witness(owner_account);
  if( res.valid() )
    return wallet_serializer_wrapper<database_api::api_witness_object>{ std::move( *res ) };
  else
    return wallet_serializer_wrapper<database_api::api_witness_object>{ database_api::api_witness_object() };
}

wallet_signed_transaction wallet_api::set_voting_proxy(const string& account_to_modify, const string& voting_account, bool broadcast /* = false */)
{ return { my->set_voting_proxy(account_to_modify, voting_account, broadcast) }; }

void wallet_api::set_wallet_filename(string wallet_filename) { my->_wallet_filename = std::move(wallet_filename); }

wallet_signed_transaction wallet_api::sign_transaction(
  const wallet_serializer_wrapper<transaction>& tx, bool broadcast /* = false */)
{ try {
  signed_transaction appbase_tx( tx.value );
  annotated_signed_transaction_ex result = my->sign_transaction( appbase_tx, broadcast);
  return { result };
} FC_CAPTURE_AND_RETHROW( (tx) ) }

wallet_serializer_wrapper<operation> wallet_api::get_prototype_operation(const string& operation_name)
{
  return { my->get_prototype_operation( operation_name ) };
}

variant wallet_api::help()const
{
  std::vector<std::string> _result;

  std::vector<std::string> method_names = my->method_documentation.get_method_names();
  for( const std::string& method_name : method_names )
  {
    try
    {
      _result.emplace_back( my->method_documentation.get_brief_description(method_name) );
    }
    catch (const fc::key_not_found_exception&)
    {
      _result.emplace_back( method_name + " (no help available)\n" );
    }
  }

  return wallet_formatter::help( _result, output_formatter );
}

variant wallet_api::gethelp(const string& method)const
{
  std::string _result = "\n";

  fc::api<wallet_api> tmp;

  std::string doxygenHelpString = my->method_documentation.get_detailed_description(method);
  if( !doxygenHelpString.empty() )
    _result += doxygenHelpString;
  else
    _result += "No help defined for method " + method + "\n";

  return wallet_formatter::gethelp( _result, output_formatter );
}

bool wallet_api::load_wallet_file( string wallet_filename )
{
  return my->load_wallet_file( std::move(wallet_filename) );
}

void wallet_api::save_wallet_file( string wallet_filename )
{
  my->save_wallet_file( std::move(wallet_filename) );
}

bool wallet_api::is_locked()const
{
  return my->is_locked();
}
bool wallet_api::is_new()const
{
  return my->_wallet.cipher_keys.size() == 0;
}

void wallet_api::encrypt_keys()
{
  my->encrypt_keys();
}

void wallet_api::lock()
{ try {
  FC_ASSERT( !is_locked() );
  encrypt_keys();
  for( auto& key : my->_keys )
    key.second = fc::ecc::private_key().key_to_wif();
  my->_keys.clear();
  my->_checksum = fc::sha512();
  my->self.lock_changed(true);
} FC_CAPTURE_AND_RETHROW() }

void wallet_api::unlock(const string& password)
{ try {
  FC_ASSERT(password.size() > 0);
  auto pw = fc::sha512::hash(password.c_str(), password.size());
  vector<char> decrypted = fc::aes_decrypt(pw, my->_wallet.cipher_keys);
  plain_keys pk;
  fc::raw::unpack_from_vector(decrypted, pk);
  FC_ASSERT(pk.checksum == pw);
  my->_keys = std::move(pk.keys);
  my->_checksum = pk.checksum;
  my->self.lock_changed(false);
} FC_CAPTURE_AND_RETHROW() }

void wallet_api::set_password( const string& password )
{
  if( !is_new() )
    FC_ASSERT( !is_locked(), "The wallet must be unlocked before the password can be set" );
  my->_checksum = fc::sha512::hash( password.c_str(), password.size() );
  lock();
}

void wallet_api::exit()
{
  if( !is_locked() )
    lock();
  exit_promise->set_value(SIGTERM);
  if( !is_daemon )
    FC_THROW_EXCEPTION( fc::sigint_exception, "Exit function invoked" ); // needed for the destruction of interactive cli mode
}

map<public_key_type, string> wallet_api::list_keys()
{
  FC_ASSERT(!is_locked());
  return my->_keys;
}

string wallet_api::get_private_key( public_key_type pubkey )const
{
  return my->get_private_key( pubkey ).key_to_wif();
}

pair<public_key_type,string> wallet_api::get_private_key_from_password( const string& account, const string& role, const string& password )const {
  auto seed = account + role + password;
  FC_ASSERT( seed.size() );
  auto secret = fc::sha256::hash( seed.c_str(), seed.size() );
  auto priv = fc::ecc::private_key::regenerate( secret );
  return std::make_pair( public_key_type( priv.get_public_key() ), priv.key_to_wif() );
}

wallet_serializer_wrapper<database_api::api_feed_history_object> wallet_api::get_feed_history()const
{
  my->require_online();
  return { my->_remote_wallet_bridge_api->get_feed_history({}, LOCK) };
}

wallet_signed_transaction wallet_api::claim_account_creation(const string& creator,
                                                                            const wallet_serializer_wrapper<hive::protocol::asset>& fee,
                                                                            bool broadcast )const
{
  return { my->build_claim_account_creation(creator, fee.value,
    [this, broadcast](signed_transaction tx) -> annotated_signed_transaction_ex
    {
      return my->sign_transaction(tx, broadcast);
    }
    ) };
}

wallet_signed_transaction wallet_api::claim_account_creation_nonblocking(const string& creator,
                                                                                        const wallet_serializer_wrapper<hive::protocol::asset>& fee,
                                                                                        bool broadcast )const
{
  return { my->build_claim_account_creation(creator, fee.value,
    [this, broadcast](signed_transaction tx) ->annotated_signed_transaction_ex
    {
    return my->sign_and_broadcast_transaction(tx, broadcast, false);
    }
  ) };
}

/**
  * This method is used by faucets to create new accounts for other users which must
  * provide their desired keys. The resulting account may not be controllable by this
  * wallet.
  */
wallet_signed_transaction wallet_api::create_account_with_keys(
  const string& creator,
  const string& new_account_name,
  const string& json_meta,
  public_key_type owner,
  public_key_type active,
  public_key_type posting,
  public_key_type memo,
  bool broadcast )const
{
  wallet_serializer_wrapper<hive::protocol::asset> no_funds;
  return { create_funded_account_with_keys(creator, new_account_name, no_funds, "", json_meta, owner,
    active, posting, memo, broadcast) };
}

/**
 * This method is used by faucets to create new accounts for other users which must
 * provide their desired keys. The resulting account may not be controllable by this
 * wallet.
 */
wallet_signed_transaction wallet_api::create_funded_account_with_keys(
  const string& creator,
  const string& new_account_name,
  const wallet_serializer_wrapper<hive::protocol::asset>& initial_amount,
  const string& memo,
  const string& json_meta,
  public_key_type owner_key,
  public_key_type active_key,
  public_key_type posting_key,
  public_key_type memo_key,
  bool broadcast )const
{ try {
   FC_ASSERT( !is_locked() );
   my->require_online();

   hive::protocol::validate_account_name( new_account_name );

   auto creator_account = get_account(creator).value;
   signed_transaction tx;
   if (creator_account.pending_claimed_accounts > 0)
   {
      create_claimed_account_operation op;
      op.creator = creator;
      op.new_account_name = new_account_name;
      op.owner = authority( 1, owner_key, 1 );
      op.active = authority( 1, active_key, 1 );
      op.posting = authority( 1, posting_key, 1 );
      op.memo_key = memo_key;
      op.json_metadata = json_meta;

      tx.operations.push_back(op);
   }
   else
   {
      account_create_operation op;
      op.creator = creator;
      op.new_account_name = new_account_name;
      op.owner = authority( 1, owner_key, 1 );
      op.active = authority( 1, active_key, 1 );
      op.posting = authority( 1, posting_key, 1 );
      op.memo_key = memo_key;
      op.json_metadata = json_meta;
      op.fee = my->_remote_wallet_bridge_api->get_chain_properties({}, LOCK).account_creation_fee;

      tx.operations.push_back(op);
   }
   idump((tx.operations[0]));

   if (initial_amount.value.amount.value > 0)
   {
      transfer_operation transfer_op;
      transfer_op.from = creator;
      transfer_op.to = new_account_name;
      transfer_op.amount = initial_amount.value;

      if( memo.size() > 0 && memo[0] == '#' )
      {
         auto from_account = get_account( creator ).value;
         protocol::crypto_memo _cm;
         transfer_op.memo = _cm.encrypt( my->get_private_key( from_account.memo_key ), memo_key, memo );
      }
      else
      {
        transfer_op.memo = memo;
      }

      tx.operations.push_back(transfer_op);
      idump((tx.operations[1]));
   }

  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
} FC_CAPTURE_AND_RETHROW( (creator)(new_account_name)(json_meta)(owner_key)(active_key)(posting_key)(memo_key)(broadcast) ) }


/**
  * This method is used by faucets to create new accounts for other users which must
  * provide their desired keys. The resulting account may not be controllable by this
  * wallet.
  */
wallet_signed_transaction wallet_api::create_account_with_keys_delegated(
  const string& creator,
  const wallet_serializer_wrapper<hive::protocol::asset>& hive_fee,
  const wallet_serializer_wrapper<hive::protocol::asset>& delegated_vests,
  const string& new_account_name,
  const string& json_meta,
  public_key_type owner,
  public_key_type active,
  public_key_type posting,
  public_key_type memo,
  bool broadcast )const
{ try {
  FC_ASSERT( !is_locked() );

   hive::protocol::validate_account_name( new_account_name );

  account_create_with_delegation_operation op;
  op.creator = creator;
  op.new_account_name = new_account_name;
  op.owner = authority( 1, owner, 1 );
  op.active = authority( 1, active, 1 );
  op.posting = authority( 1, posting, 1 );
  op.memo_key = memo;
  op.json_metadata = json_meta;
  op.fee = hive_fee.value;
  op.delegation = delegated_vests.value;

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
} FC_CAPTURE_AND_RETHROW( (creator)(new_account_name)(json_meta)(owner)(active)(memo)(broadcast) ) }

wallet_signed_transaction wallet_api::request_account_recovery( const string& recovery_account, const string& account_to_recover, authority new_authority, bool broadcast )
{
  FC_ASSERT( !is_locked() );
  request_account_recovery_operation op;
  op.recovery_account = recovery_account;
  op.account_to_recover = account_to_recover;
  op.new_owner_authority = std::move(new_authority);

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::recover_account( const string& account_to_recover, authority recent_authority, authority new_authority, bool broadcast ) {
  FC_ASSERT( !is_locked() );

  recover_account_operation op;
  op.account_to_recover = account_to_recover;
  op.new_owner_authority = std::move(new_authority);
  op.recent_owner_authority = std::move(recent_authority);

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::change_recovery_account( const string& owner, const string& new_recovery_account, bool broadcast ) {
  FC_ASSERT( !is_locked() );

  change_recovery_account_operation op;
  op.account_to_recover = owner;
  op.new_recovery_account = new_recovery_account;

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

vector< database_api::api_owner_authority_history_object > wallet_api::get_owner_history( fc::variant account )const
{
  my->require_online();
  vector<variant> args{std::move(account)};
  return my->_remote_wallet_bridge_api->get_owner_history( {args}, LOCK ).owner_auths;
}

wallet_signed_transaction wallet_api::update_account(
  const string& account_name,
  const string& json_meta,
  public_key_type owner,
  public_key_type active,
  public_key_type posting,
  public_key_type memo,
  bool broadcast )const
{
  try
  {
    FC_ASSERT( !is_locked() );

    account_update_operation op;
    op.account = account_name;
    op.owner  = authority( 1, owner, 1 );
    op.active = authority( 1, active, 1);
    op.posting = authority( 1, posting, 1);
    op.memo_key = memo;
    op.json_metadata = json_meta;

    signed_transaction tx;
    tx.operations.push_back(op);
    tx.validate();

    return { my->sign_transaction( tx, broadcast ) };
  }
  FC_CAPTURE_AND_RETHROW( (account_name)(json_meta)(owner)(active)(memo)(broadcast) )
}

wallet_signed_transaction wallet_api::update_account_auth_key(
  const string& account_name,
  authority_type type,
  public_key_type key,
  weight_type weight,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  auto account = my->get_account( { account_name } );
  FC_ASSERT( account_name == account.name, "Account name doesn't match?" );

  account_update_operation op;
  op.account = account_name;
  op.memo_key = account.memo_key;
  op.json_metadata = account.json_metadata;

  authority new_auth;

  switch( type )
  {
  case authority_type::owner:
    new_auth = account.owner;
    break;
  case authority_type::active:
    new_auth = account.active;
    break;
  case authority_type::posting:
    new_auth = account.posting;
    break;
  }

  if( weight == 0 ) // Remove the key
  {
    new_auth.key_auths.erase( key );
  }
  else
  {
    new_auth.add_authority( key, weight );
  }

  if( new_auth.is_impossible() )
  {
    if( type == authority_type::owner )
    {
      FC_ASSERT( false, "Owner authority change would render account irrecoverable." );
    }

    wlog( "Authority is now impossible." );
  }

  switch( type )
  {
  case authority_type::owner:
    op.owner = new_auth;
    break;
  case authority_type::active:
    op.active = new_auth;
    break;
  case authority_type::posting:
    op.posting = new_auth;
    break;
  }

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::update_account_auth_account(
  const string& account_name,
  authority_type type,
  const string& auth_account,
  weight_type weight,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  auto account = my->get_account( { account_name } );
  FC_ASSERT( account_name == account.name, "Account name doesn't match?" );

  account_update_operation op;
  op.account = account_name;
  op.memo_key = account.memo_key;
  op.json_metadata = account.json_metadata;

  authority new_auth;

  switch( type )
  {
  case authority_type::owner:
    new_auth = account.owner;
    break;
  case authority_type::active:
    new_auth = account.active;
    break;
  case authority_type::posting:
    new_auth = account.posting;
    break;
  }

  if( weight == 0 ) // Remove the key
  {
    new_auth.account_auths.erase( auth_account );
  }
  else
  {
    new_auth.add_authority( auth_account, weight );
  }

  if( new_auth.is_impossible() )
  {
    if( type == authority_type::owner )
    {
      FC_ASSERT( false, "Owner authority change would render account irrecoverable." );
    }

    wlog( "Authority is now impossible." );
  }

  switch( type )
  {
  case authority_type::owner:
    op.owner = new_auth;
    break;
  case authority_type::active:
    op.active = new_auth;
    break;
  case authority_type::posting:
    op.posting = new_auth;
    break;
  }

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::update_account_auth_threshold(
  const string& account_name,
  authority_type type,
  uint32_t threshold,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  auto account = my->get_account( { account_name } );
  FC_ASSERT( account_name == account.name, "Account name doesn't match?" );
  FC_ASSERT( threshold != 0, "Authority is implicitly satisfied" );

  account_update_operation op;
  op.account = account_name;
  op.memo_key = account.memo_key;
  op.json_metadata = account.json_metadata;

  authority new_auth;

  switch( type )
  {
  case authority_type::owner:
    new_auth = account.owner;
    break;
  case authority_type::active:
    new_auth = account.active;
    break;
  case authority_type::posting:
    new_auth = account.posting;
    break;
  }

  new_auth.weight_threshold = threshold;

  if( new_auth.is_impossible() )
  {
    if( type == authority_type::owner )
    {
      FC_ASSERT( false, "Owner authority change would render account irrecoverable." );
    }

    wlog( "Authority is now impossible." );
  }

  switch( type )
  {
  case authority_type::owner:
    op.owner = new_auth;
    break;
  case authority_type::active:
    op.active = new_auth;
    break;
  case authority_type::posting:
    op.posting = new_auth;
    break;
  }

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::update_account_meta(
  const string& account_name,
  const string& json_meta,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  auto account = my->get_account( { account_name } );
  FC_ASSERT( account_name == account.name, "Account name doesn't match?" );

  account_update_operation op;
  op.account = account_name;
  op.memo_key = account.memo_key;
  op.json_metadata = json_meta;

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::update_account_memo_key(
  const string& account_name,
  public_key_type key,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  auto account = my->get_account( { account_name } );
  FC_ASSERT( account_name == account.name, "Account name doesn't match?" );

  account_update_operation op;
  op.account = account_name;
  op.memo_key = key;
  op.json_metadata = account.json_metadata;

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::delegate_vesting_shares_and_transfer_and_broadcast(
  const string& delegator,
  const string& delegatee,
  const hive::protocol::asset& vesting_shares,
  optional<hive::protocol::asset> transfer_amount,
  optional<string> transfer_memo,
  bool broadcast,
  bool blocking )
{
  FC_ASSERT( !is_locked() );
  my->require_online();
  vector<variant> args{delegator, delegatee};
  auto accounts = my->get_accounts( variant{ args } );
  FC_ASSERT( accounts.size() == 2 , "One or more of the accounts specified do not exist." );
  FC_ASSERT( delegator == accounts[0].name, "Delegator account is not right?" );
  FC_ASSERT( delegatee == accounts[1].name, "Delegator account is not right?" );

  delegate_vesting_shares_operation op;
  op.delegator = delegator;
  op.delegatee = delegatee;
  op.vesting_shares = vesting_shares;

  signed_transaction tx;
  tx.operations.push_back( op );

  if( transfer_amount && transfer_amount->amount.value > 0 )
  {
    transfer_operation transfer_op;
    transfer_op.from = delegator;
    transfer_op.to = delegatee;
    transfer_op.amount = *transfer_amount;
    transfer_op.memo = transfer_memo ? get_encrypted_memo(delegator, delegatee, *transfer_memo) : "";

    tx.operations.push_back(transfer_op);
  }

  tx.validate();

  return { my->sign_and_broadcast_transaction( tx, broadcast, blocking ) };
}

wallet_signed_transaction wallet_api::delegate_vesting_shares(
  const string& delegator,
  const string& delegatee,
  const wallet_serializer_wrapper<hive::protocol::asset>& vesting_shares,
  bool broadcast )
{
  return { delegate_vesting_shares_and_transfer_and_broadcast(delegator, delegatee, vesting_shares.value,
    optional<hive::protocol::asset>(), optional<string>(), broadcast, true) };
}

wallet_signed_transaction wallet_api::delegate_vesting_shares_nonblocking(
  const string& delegator,
  const string& delegatee,
  const wallet_serializer_wrapper<hive::protocol::asset>& vesting_shares,
  bool broadcast )
{
  return { delegate_vesting_shares_and_transfer_and_broadcast(delegator, delegatee, vesting_shares.value,
    optional<hive::protocol::asset>(), optional<string>(), broadcast, false) };
}

wallet_signed_transaction wallet_api::delegate_vesting_shares_and_transfer(
  const string& delegator,
  const string& delegatee,
  const wallet_serializer_wrapper<hive::protocol::asset>& vesting_shares,
  const wallet_serializer_wrapper<hive::protocol::asset>& transfer_amount,
  optional<string> transfer_memo,
  bool broadcast )
{
  return { delegate_vesting_shares_and_transfer_and_broadcast(delegator, delegatee, vesting_shares.value,
    transfer_amount.value, transfer_memo, broadcast, true) };
}

wallet_signed_transaction wallet_api::delegate_vesting_shares_and_transfer_nonblocking(
  const string& delegator,
  const string& delegatee,
  const wallet_serializer_wrapper<hive::protocol::asset>& vesting_shares,
  const wallet_serializer_wrapper<hive::protocol::asset>& transfer_amount,
  optional<string> transfer_memo,
  bool broadcast )
{
  return { delegate_vesting_shares_and_transfer_and_broadcast( delegator, delegatee, vesting_shares.value,
    transfer_amount.value, transfer_memo, broadcast, false) };
}


/**
  *  This method will genrate new owner, active, and memo keys for the new account which
  *  will be controlable by this wallet.
  */
wallet_signed_transaction wallet_api::create_account(
  const string& creator,
  const string& new_account_name,
  const string& json_meta,
  bool broadcast )
{ try {
  FC_ASSERT( !is_locked() );
  auto owner = suggest_brain_key();
  auto active = suggest_brain_key();
  auto posting = suggest_brain_key();
  auto memo = suggest_brain_key();
  import_key( owner.wif_priv_key );
  import_key( active.wif_priv_key );
  import_key( posting.wif_priv_key );
  import_key( memo.wif_priv_key );
  return { create_account_with_keys( creator, new_account_name, json_meta, owner.pub_key, active.pub_key, posting.pub_key, memo.pub_key, broadcast ) };
} FC_CAPTURE_AND_RETHROW( (creator)(new_account_name)(json_meta) ) }

/**
  *  This method will genrate new owner, active, and memo keys for the new account which
  *  will be controlable by this wallet.
  */
wallet_signed_transaction wallet_api::create_account_delegated(
  const string& creator,
  const wallet_serializer_wrapper<hive::protocol::asset>& hive_fee,
  const wallet_serializer_wrapper<hive::protocol::asset>& delegated_vests,
  const string& new_account_name,
  const string& json_meta,
  bool broadcast )
{ try {
  FC_ASSERT( !is_locked() );
  auto owner = suggest_brain_key();
  auto active = suggest_brain_key();
  auto posting = suggest_brain_key();
  auto memo = suggest_brain_key();
  import_key( owner.wif_priv_key );
  import_key( active.wif_priv_key );
  import_key( posting.wif_priv_key );
  import_key( memo.wif_priv_key );
  return { create_account_with_keys_delegated( creator, hive_fee, delegated_vests, new_account_name, json_meta,  owner.pub_key, active.pub_key, posting.pub_key, memo.pub_key, broadcast ) };
} FC_CAPTURE_AND_RETHROW( (creator)(new_account_name)(json_meta) ) }


wallet_signed_transaction wallet_api::update_witness(
  const string& witness_account_name,
  const string& url,
  public_key_type block_signing_key,
  const wallet_serializer_wrapper<legacy_chain_properties>& props,
  bool broadcast  )
{
  FC_ASSERT( !is_locked() );
  my->require_online();

  witness_update_operation op;

  vector<variant> args{witness_account_name};
  optional< database_api::api_witness_object > wit = my->_remote_wallet_bridge_api->get_witness( {args}, LOCK );
  if( !wit.valid() )
  {
    op.url = url;
  }
  else
  {
    FC_ASSERT( wit->owner == witness_account_name );
    if( url != "" )
      op.url = url;
    else
      op.url = wit->url;
  }
  op.owner = witness_account_name;
  op.block_signing_key = block_signing_key;
  op.props = props.value;

  signed_transaction tx;
  tx.operations.push_back(op);
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::vote_for_witness(
  const string& voting_account,
  const string& witness_to_vote_for,
  bool approve,
  bool broadcast )
{ try {
  FC_ASSERT( !is_locked() );

  account_witness_vote_operation op;
  op.account = voting_account;
  op.witness = witness_to_vote_for;
  op.approve = approve;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
} FC_CAPTURE_AND_RETHROW( (voting_account)(witness_to_vote_for)(approve)(broadcast) ) }

void wallet_api::check_memo(
  const string& memo,
  const database_api::api_account_object& account )const
{
  vector< public_key_type > keys;

  collect_potential_keys( &keys, account.name, memo );

  // Check keys against public keys in authorites
  for( auto& key_weight_pair : account.owner.key_auths )
  {
    for( auto& key : keys )
      FC_ASSERT( key_weight_pair.first != key, "Detected private owner key in memo field. Cancelling transaction." );
  }

  for( auto& key_weight_pair : account.active.key_auths )
  {
    for( auto& key : keys )
      FC_ASSERT( key_weight_pair.first != key, "Detected private active key in memo field. Cancelling transaction." );
  }

  for( auto& key_weight_pair : account.posting.key_auths )
  {
    for( auto& key : keys )
      FC_ASSERT( key_weight_pair.first != key, "Detected private posting key in memo field. Cancelling transaction." );
  }

  const auto& memo_key = account.memo_key;
  for( auto& key : keys )
    FC_ASSERT( memo_key != key, "Detected private memo key in memo field. Cancelling transaction." );

  // Check against imported keys
  for( auto& key_pair : my->_keys )
  {
    for( auto& key : keys )
      FC_ASSERT( key != key_pair.first, "Detected imported private key in memo field. Cancelling transaction." );
  }
}

string wallet_api::get_encrypted_memo( const string& from, const string& to, const string& memo )
{
  if( memo.size() > 0 && memo[0] == '#' )
  {
    auto from_account = get_account( from ).value;
    auto to_account   = get_account( to ).value;

    protocol::crypto_memo _cm;
    return _cm.encrypt( my->get_private_key( from_account.memo_key ), to_account.memo_key, memo );
  }
  else
  {
    return memo;
  }
}

wallet_signed_transaction wallet_api::transfer(const string& from, const string& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount, const string& memo, bool broadcast )
{
  return { transfer_and_broadcast(from, to, amount.value, memo, broadcast, true) };
}

wallet_signed_transaction wallet_api::transfer_nonblocking(const string& from, const string& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount, const string& memo, bool broadcast )
{
  return { transfer_and_broadcast(from, to, amount.value, memo, broadcast, false) };
}

wallet_signed_transaction wallet_api::transfer_and_broadcast(
  const string& from,
  const string& to,
  const hive::protocol::asset& amount,
  const string& memo,
  bool broadcast,
  bool blocking )
{ try {
  FC_ASSERT( !is_locked() );

  check_memo( memo, get_account( from ).value );
  transfer_operation op;
  op.from = from;
  op.to = to;
  op.amount = amount;

  op.memo = get_encrypted_memo( from, to, memo );

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  auto unique_tx_builder = [this, from](full_transaction_ptr tx) ->full_transaction_ptr
  {
    return my->make_transaction_unique(tx, from);
  };

  return { my->sign_and_broadcast_transaction( tx, broadcast, blocking, std::move(unique_tx_builder)) };
} FC_CAPTURE_AND_RETHROW( (from)(to)(amount)(memo)(broadcast) ) }

wallet_signed_transaction wallet_api::escrow_transfer(
  const string& from,
  const string& to,
  const string& agent,
  uint32_t escrow_id,
  const wallet_serializer_wrapper<hive::protocol::asset>& hbd_amount,
  const wallet_serializer_wrapper<hive::protocol::asset>& hive_amount,
  const wallet_serializer_wrapper<hive::protocol::asset>& fee,
  const time_point_sec& ratification_deadline,
  const time_point_sec& escrow_expiration,
  const string& json_meta,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  escrow_transfer_operation op;
  op.from = from;
  op.to = to;
  op.agent = agent;
  op.escrow_id = escrow_id;
  op.hbd_amount = hbd_amount.value;
  op.hive_amount = hive_amount.value;
  op.fee = fee.value;
  op.ratification_deadline = ratification_deadline;
  op.escrow_expiration = escrow_expiration;
  op.json_meta = json_meta;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::escrow_approve(
  const string& from,
  const string& to,
  const string& agent,
  const string& who,
  uint32_t escrow_id,
  bool approve,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  escrow_approve_operation op;
  op.from = from;
  op.to = to;
  op.agent = agent;
  op.who = who;
  op.escrow_id = escrow_id;
  op.approve = approve;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::escrow_dispute(
  const string& from,
  const string& to,
  const string& agent,
  const string& who,
  uint32_t escrow_id,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  escrow_dispute_operation op;
  op.from = from;
  op.to = to;
  op.agent = agent;
  op.who = who;
  op.escrow_id = escrow_id;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::escrow_release(
  const string& from,
  const string& to,
  const string& agent,
  const string& who,
  const string& receiver,
  uint32_t escrow_id,
  const wallet_serializer_wrapper<hive::protocol::asset>& hbd_amount,
  const wallet_serializer_wrapper<hive::protocol::asset>& hive_amount,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  escrow_release_operation op;
  op.from = from;
  op.to = to;
  op.agent = agent;
  op.who = who;
  op.receiver = receiver;
  op.escrow_id = escrow_id;
  op.hbd_amount = hbd_amount.value;
  op.hive_amount = hive_amount.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();
  return { my->sign_transaction( tx, broadcast ) };
}

/**
  *  Transfers into savings happen immediately, transfers from savings take 72 hours
  */
wallet_signed_transaction wallet_api::transfer_to_savings(
  const string& from,
  const string& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount,
  const string& memo,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  check_memo( memo, get_account( from ).value );
  transfer_to_savings_operation op;
  op.from = from;
  op.to   = to;
  op.memo = get_encrypted_memo( from, to, memo );
  op.amount = amount.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

/**
  * @param request_id - an unique ID assigned by from account, the id is used to cancel the operation and can be reused after the transfer completes
  */
wallet_signed_transaction wallet_api::transfer_from_savings(
  const string& from,
  uint32_t request_id,
  const string& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount,
  const string& memo,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  check_memo( memo, get_account( from ).value );
  transfer_from_savings_operation op;
  op.from = from;
  op.request_id = request_id;
  op.to = to;
  op.amount = amount.value;
  op.memo = get_encrypted_memo( from, to, memo );

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

/**
  *  @param request_id the id used in transfer_from_savings
  *  @param from the account that initiated the transfer
  */
wallet_signed_transaction wallet_api::cancel_transfer_from_savings(
  const string& from,
  uint32_t request_id,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  cancel_transfer_from_savings_operation op;
  op.from = from;
  op.request_id = request_id;
  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::transfer_to_vesting(
  const string& from, const string& to, const wallet_serializer_wrapper<hive::protocol::asset>& amount, bool broadcast )
{
  return { transfer_to_vesting_and_broadcast(from, to, amount.value, broadcast, true) };
}

wallet_signed_transaction wallet_api::transfer_to_vesting_nonblocking(
  const string& from, const string& to, const wallet_serializer_wrapper<hive::protocol::asset>& amount, bool broadcast )
{
  return { transfer_to_vesting_and_broadcast(from, to, amount.value, broadcast, false) };
}

wallet_signed_transaction wallet_api::transfer_to_vesting_and_broadcast(
  const string& from, const string& to, const hive::protocol::asset& amount, bool broadcast, bool blocking )
{
  FC_ASSERT( !is_locked() );
  transfer_to_vesting_operation op;
  op.from = from;
  op.to = (to == from ? "" : to);
  op.amount = amount;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  auto unique_tx_builder = [this, from](full_transaction_ptr tx) ->full_transaction_ptr
  {
    return my->make_transaction_unique(tx, from);
  };

  return { my->sign_and_broadcast_transaction( tx, broadcast, blocking, std::move(unique_tx_builder)) };
}

wallet_signed_transaction wallet_api::withdraw_vesting(
  const string& from,
  const wallet_serializer_wrapper<hive::protocol::asset>& vesting_shares,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  withdraw_vesting_operation op;
  op.account = from;
  op.vesting_shares = vesting_shares.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::set_withdraw_vesting_route(
  const string& from,
  const string& to,
  uint16_t percent,
  bool auto_vest,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  set_withdraw_vesting_route_operation op;
  op.from_account = from;
  op.to_account = to;
  op.percent = percent;
  op.auto_vest = auto_vest;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::convert_hbd(
  const string& from,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  convert_operation op;
  op.owner = from;
  op.requestid = fc::time_point::now().sec_since_epoch();
  op.amount = amount.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::convert_hive_with_collateral(
  const string& from,
  const wallet_serializer_wrapper<hive::protocol::asset>& collateral_amount,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  collateralized_convert_operation op;
  op.owner = from;
  op.requestid = fc::time_point::now().sec_since_epoch();
  op.amount = collateral_amount.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_serializer_wrapper<hive::protocol::asset> wallet_api::estimate_hive_collateral(
  const wallet_serializer_wrapper<hive::protocol::asset>& hbd_amount_to_get )
{
  //must reflect calculations from collateralized_convert_evaluator::do_apply

  auto fhistory = get_feed_history().value;

  return { hive::protocol::hive_collateral::estimate_hive_collateral(
      fhistory.current_median_history,
      fhistory.current_min_history,
      hbd_amount_to_get.value ) };
}

wallet_signed_transaction wallet_api::publish_feed(
  const string& witness,
  const wallet_serializer_wrapper<price>& exchange_rate,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  feed_publish_operation op;
  op.publisher     = witness;
  op.exchange_rate = price( exchange_rate.value );

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_serializer_wrapper<vector< database_api::api_convert_request_object >> wallet_api::get_conversion_requests( fc::variant owner_account )
{
  my->require_online();
  vector<variant> args{std::move(owner_account)};
  return { my->_remote_wallet_bridge_api->get_conversion_requests( {args}, LOCK ) };
}

wallet_serializer_wrapper<vector< database_api::api_collateralized_convert_request_object >> wallet_api::get_collateralized_conversion_requests( fc::variant owner_account )
{
  my->require_online();
  vector<variant> args{std::move(owner_account)};
  return { my->_remote_wallet_bridge_api->get_collateralized_conversion_requests( {args}, LOCK ) };
}

string wallet_api::decrypt_memo( string encrypted_memo )
{
  if( !is_locked() && encrypted_memo.size() && encrypted_memo[0] == '#' )
  {
    protocol::crypto_memo _cm;
    return _cm.decrypt(
      [&]( const public_key_type& key ) { return my->try_get_private_key( key ); },
      encrypted_memo
    );
  }
  return encrypted_memo;
}

wallet_signed_transaction wallet_api::decline_voting_rights(
  const string& account,
  bool decline,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  decline_voting_rights_operation op;
  op.account = account;
  op.decline = decline;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::claim_reward_balance(
  const string& account,
  const wallet_serializer_wrapper<hive::protocol::asset>& reward_hive,
  const wallet_serializer_wrapper<hive::protocol::asset>& reward_hbd,
  const wallet_serializer_wrapper<hive::protocol::asset>& reward_vests,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  claim_reward_balance_operation op;
  op.account = account;
  op.reward_hive = reward_hive.value;
  op.reward_hbd = reward_hbd.value;
  op.reward_vests = reward_vests.value;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

variant wallet_api::get_account_history( const string& account, uint32_t from, uint32_t limit )
{
  my->require_online();
  vector<variant> args{account, from, limit, variant{ output_formatter_type::none } };

  auto _result = my->_remote_wallet_bridge_api->get_account_history( {args}, LOCK );

  if( !is_locked() )
  {
    for( auto& item : _result )
    {
      if( item.second.op.which() == operation::tag<transfer_operation>::value )
      {
        auto& top = item.second.op.get<transfer_operation>();
        top.memo = decrypt_memo( top.memo );
      }
      else if( item.second.op.which() == operation::tag<transfer_from_savings_operation>::value )
      {
        auto& top = item.second.op.get<transfer_from_savings_operation>();
        top.memo = decrypt_memo( top.memo );
      }
      else if( item.second.op.which() == operation::tag<transfer_to_savings_operation>::value )
      {
        auto& top = item.second.op.get<transfer_to_savings_operation>();
        top.memo = decrypt_memo( top.memo );
      }
    }

  }

  return wallet_formatter::get_account_history( wallet_serializer_wrapper<std::map<uint32_t, account_history::api_operation_object>>{ _result }, output_formatter );
}

variant wallet_api::get_withdraw_routes( const string& account, database_api::withdraw_route_type type )const
{
  my->require_online();
  vector<variant> args{ account, variant{ type } };
  auto _result = my->_remote_wallet_bridge_api->get_withdraw_routes( {args} , LOCK );

  return wallet_formatter::get_withdraw_routes( wallet_serializer_wrapper<vector<database_api::api_withdraw_vesting_route_object>>{ _result }, output_formatter );
}

variant wallet_api::get_order_book( uint32_t limit )
{
  my->require_online();
  FC_ASSERT( limit <= 1000 );

  vector<variant> args{ limit };
  auto _result = my->_remote_wallet_bridge_api->get_order_book( {args}, LOCK );

  return wallet_formatter::get_order_book( wallet_serializer_wrapper<market_history::get_order_book_return>{ _result }, output_formatter );
}

variant wallet_api::get_open_orders( const string& accountname )
{
  my->require_online();

  vector<variant> args{ accountname };
  auto _result = my->_remote_wallet_bridge_api->get_open_orders( {args}, LOCK );

  return wallet_formatter::get_open_orders( wallet_serializer_wrapper<vector<database_api::api_limit_order_object>>{ _result }, output_formatter );
}

wallet_signed_transaction wallet_api::create_order(
  const string& owner,
  uint32_t order_id,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount_to_sell,
  const wallet_serializer_wrapper<hive::protocol::asset>& min_to_receive,
  bool fill_or_kill,
  uint32_t expiration_sec,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  limit_order_create_operation op;
  op.owner = owner;
  op.orderid = order_id;
  op.amount_to_sell = amount_to_sell.value;
  op.min_to_receive = min_to_receive.value;
  op.fill_or_kill = fill_or_kill;
  op.expiration = expiration_sec ? ( my->get_dynamic_global_properties().value.time + fc::seconds(expiration_sec)) : fc::time_point::maximum();

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::cancel_order(
  const string& owner,
  uint32_t orderid,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  limit_order_cancel_operation op;
  op.owner = owner;
  op.orderid = orderid;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::post_comment(
  const string& author,
  const string& permlink,
  const string& parent_author,
  const string& parent_permlink,
  const string& title,
  const string& body,
  const string& json,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  comment_operation op;
  op.parent_author =  parent_author;
  op.parent_permlink = parent_permlink;
  op.author = author;
  op.permlink = permlink;
  op.title = title;
  op.body = body;
  op.json_metadata = json;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

wallet_signed_transaction wallet_api::vote(
  const string& voter,
  const string& author,
  const string& permlink,
  int16_t weight,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );
  FC_ASSERT( abs(weight) <= 100, "Weight must be between -100 and 100 and not 0" );

  vote_operation op;
  op.voter = voter;
  op.author = author;
  op.permlink = permlink;
  op.weight = weight * HIVE_1_PERCENT;

  signed_transaction tx;
  tx.operations.push_back( op );
  tx.validate();

  return { my->sign_transaction( tx, broadcast ) };
}

void wallet_api::set_transaction_expiration(uint32_t seconds)
{
  protocol::hardfork_version _result = my->_remote_wallet_bridge_api->get_hardfork_version({}, LOCK);

  bool _is_hive_hardfork_1_28 = _result.minor_v() >= 28;
  my->set_transaction_expiration(seconds, _is_hive_hardfork_1_28 );
}

wallet_serializer_wrapper<hive::plugins::account_history::annotated_signed_transaction> wallet_api::get_transaction( fc::variant id )const
{
  my->require_online();
  vector<variant> args{std::move(id)};
  return { my->_remote_wallet_bridge_api->get_transaction( {args}, LOCK ) };
}

void wallet_api::use_authority( authority_type type, const account_name_type& account_name )
{
  my->use_authority( type, account_name );
}

void wallet_api::use_automatic_authority()
{
  my->use_automatic_authority();
}

wallet_signed_transaction wallet_api::follow( const string& follower, const string& following, set<string> what, bool broadcast )
{
  get_account( follower );
  FC_ASSERT( !following.empty() );
  get_account( following );

  follow_operation op;
  op.follower = follower;
  op.following = '@' + following;
  op.what = std::move( what );
  follow_operation_type fop = op;

  custom_json_operation jop;
  jop.id = "follow";
  jop.json = fc::json::to_string(fop);
  jop.required_posting_auths.insert(follower);

  signed_transaction trx;
  trx.operations.push_back( jop );
  trx.validate();

  return { my->sign_transaction( trx, broadcast ) };
}

  wallet_signed_transaction  wallet_api::create_proposal(
    const account_name_type& creator,
    const account_name_type& receiver,
    time_point_sec start_date,
    time_point_sec end_date,
    const wallet_serializer_wrapper<hive::protocol::asset>& daily_pay,
    string subject,
    string permlink,
    bool broadcast )
  {
    FC_ASSERT( !is_locked() );

    create_proposal_operation cp;
    cp.creator = creator;
    cp.receiver = receiver;
    cp.start_date = start_date;
    cp.end_date = end_date;
    cp.daily_pay = daily_pay.value;
    cp.subject = std::move( subject );
    cp.permlink = std::move( permlink );

    signed_transaction trx;
    trx.operations.push_back( cp );
    trx.validate();
    return { my->sign_transaction( trx, broadcast ) };
  }

 wallet_signed_transaction wallet_api::update_proposal(
  int64_t proposal_id,
  const account_name_type& creator,
  const wallet_serializer_wrapper<hive::protocol::asset>& daily_pay,
  string subject,
  string permlink,
  optional<time_point_sec> end_date,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );

  update_proposal_operation up;

  up.proposal_id = proposal_id;
  up.creator = creator;
  up.daily_pay = daily_pay.value;
  up.subject = std::move(subject);
  up.permlink = std::move(permlink);
  if( end_date )
  {
    update_proposal_end_date ped;
    ped.end_date = *end_date;
    up.extensions.insert(ped);
  }

  signed_transaction trx;
  trx.operations.push_back( up );
  trx.validate();
  return { my->sign_transaction( trx, broadcast ) };
}

wallet_signed_transaction wallet_api::update_proposal_votes(
  const account_name_type& voter,
  const flat_set< int64_t >& proposals,
  bool approve,
  bool broadcast )
{
  FC_ASSERT( !is_locked() );

  update_proposal_votes_operation upv;

  upv.voter = voter;
  upv.proposal_ids = proposals;
  upv.approve = approve;

  signed_transaction trx;
  trx.operations.push_back( upv );
  trx.validate();
  return { my->sign_transaction( trx, broadcast ) };
}

wallet_serializer_wrapper<vector< database_api::api_proposal_object >> wallet_api::list_proposals( fc::variant start,
                                  uint32_t limit,
                                  database_api::sort_order_type order_by,
                                  database_api::order_direction_type order_type,
                                  database_api::proposal_status status )
{
  my->require_online();
  vector<variant> args{std::move(start), limit, order_by, order_type, status};
  return { my->_remote_wallet_bridge_api->list_proposals( {args}, LOCK ).proposals };
}

wallet_serializer_wrapper<vector< database_api::api_proposal_object >> wallet_api::find_proposals( fc::variant proposal_ids )
{
  my->require_online();
  vector<variant> args{std::move(proposal_ids)};
  return { my->_remote_wallet_bridge_api->find_proposals( {args}, LOCK ).proposals };
}

wallet_serializer_wrapper<vector< database_api::api_proposal_vote_object >> wallet_api::list_proposal_votes(
  fc::variant start,
  uint32_t limit,
  database_api::sort_order_type order_by,
  database_api::order_direction_type order_type,
  database_api::proposal_status status )
{
  my->require_online();
  vector<variant> args{std::move( start ), limit, order_by, order_type, status};
  return { my->_remote_wallet_bridge_api->list_proposal_votes( {args}, LOCK ).proposal_votes };
}

wallet_signed_transaction wallet_api::remove_proposal(const account_name_type& deleter,
                                            const flat_set< int64_t >& ids, bool broadcast )
{
  FC_ASSERT( !is_locked() );

  remove_proposal_operation rp;
  rp.proposal_owner = deleter;
  rp.proposal_ids   = ids;

  signed_transaction trx;
  trx.operations.push_back( rp );
  trx.validate();
  return { my->sign_transaction( trx, broadcast ) };
}

wallet_signed_transaction wallet_api::recurrent_transfer_with_id(
  const account_name_type& from,
  const account_name_type& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount,
  const string& memo,
  uint16_t recurrence,
  uint16_t executions,
  uint8_t pair_id,
  bool broadcast )
{
  try {
    FC_ASSERT( !is_locked() );
    check_memo( memo, get_account( from ).value );
    recurrent_transfer_operation op;
    op.from = from;
    op.to = to;
    op.amount = amount.value;
    op.memo = get_encrypted_memo( from, to, memo );
    op.recurrence = recurrence;
    op.executions = executions;
    if( pair_id != 0 )
    {
      recurrent_transfer_pair_id rtpi;
      rtpi.pair_id = pair_id;
      op.extensions.insert(rtpi);
    }

    signed_transaction tx;
    tx.operations.push_back( op );
    tx.validate();

    return { my->sign_transaction( tx, broadcast ) };
  } FC_CAPTURE_AND_RETHROW( (from)(to)(amount)(memo)(recurrence)(executions)(pair_id)(broadcast) )
}

wallet_signed_transaction wallet_api::recurrent_transfer(
  const account_name_type& from,
  const account_name_type& to,
  const wallet_serializer_wrapper<hive::protocol::asset>& amount,
  const string& memo,
  uint16_t recurrence,
  uint16_t executions,
  bool broadcast )
{
  return recurrent_transfer_with_id( from, to, amount, memo, recurrence, executions, 0, broadcast );
}

wallet_serializer_wrapper<vector< database_api::api_recurrent_transfer_object >> wallet_api::find_recurrent_transfers( fc::variant from )
{
  my->require_online();
  vector<variant> args{std::move(from)};
  return { my->_remote_wallet_bridge_api->find_recurrent_transfers( {args}, LOCK ) };
}

wallet_signed_transaction wallet_api::delegate_rc(
            const account_name_type& from,
            const flat_set<account_name_type>& delegatees,
            int64_t max_rc,
            bool broadcast )
{
  using namespace plugins::rc;
  FC_ASSERT( !is_locked() );

  delegate_rc_operation dro;
  dro.from    = from;
  dro.delegatees = delegatees;
  dro.max_rc  = max_rc;

  custom_json_operation op;
  op.json = fc::json::to_string( hive::protocol::rc_custom_operation( dro ) );
  op.id = HIVE_RC_CUSTOM_OPERATION_ID;

  flat_set< account_name_type > required_auths;
  dro.get_required_posting_authorities( required_auths );
  op.required_posting_auths = required_auths;

  signed_transaction trx;
  trx.operations.push_back( op );
  trx.validate();
  return { my->sign_transaction( trx, broadcast ) };
}

wallet_serializer_wrapper<vector< rc::rc_account_api_object >> wallet_api::find_rc_accounts( fc::variant accounts )
{
  vector<variant> args{std::move(accounts)};
  return { my->_remote_wallet_bridge_api->find_rc_accounts( {args}, LOCK ) };
}

wallet_serializer_wrapper<vector< rc::rc_account_api_object >> wallet_api::list_rc_accounts(
            const string& start,
            uint32_t limit)
{
  vector<variant> args{start , limit};
  return { my->_remote_wallet_bridge_api->list_rc_accounts( {args}, LOCK ) };
}

wallet_serializer_wrapper<vector< rc::rc_direct_delegation_api_object >> wallet_api::list_rc_direct_delegations(
            fc::variant start,
            uint32_t limit)
{
  vector<variant> args{std::move( start ), limit};
  return { my->_remote_wallet_bridge_api->list_rc_direct_delegations( {args}, LOCK ) };
}

} } // hive::wallet
