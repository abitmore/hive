#include <hive/plugins/account_history_api/account_history_api_plugin.hpp>
#include <hive/plugins/account_history_api/account_history_api.hpp>

namespace hive { namespace plugins { namespace account_history {

namespace detail {

class abstract_account_history_api_impl
{
  public:
    abstract_account_history_api_impl( appbase::application& app )
      : _block_reader( app.get_plugin< hive::plugins::chain::chain_plugin >().block_reader() ) {}
    virtual ~abstract_account_history_api_impl() {}

    virtual get_ops_in_block_return get_ops_in_block( const get_ops_in_block_args& ) = 0;
    virtual get_transaction_return get_transaction( const get_transaction_args& ) = 0;
    virtual get_account_history_return get_account_history( const get_account_history_args& ) = 0;
    virtual enum_virtual_ops_return enum_virtual_ops( const enum_virtual_ops_args& ) = 0;

    const hive::chain::block_read_i& _block_reader;
};

class account_history_api_rocksdb_impl : public abstract_account_history_api_impl
{
  public:
    account_history_api_rocksdb_impl( appbase::application& app ) :
      abstract_account_history_api_impl( app ), _dataSource( app.get_plugin< hive::plugins::account_history_rocksdb::account_history_rocksdb_plugin >() ) {}
    ~account_history_api_rocksdb_impl() {}

    get_ops_in_block_return get_ops_in_block( const get_ops_in_block_args& ) override;
    get_transaction_return get_transaction( const get_transaction_args& ) override;
    get_account_history_return get_account_history( const get_account_history_args& ) override;
    enum_virtual_ops_return enum_virtual_ops( const enum_virtual_ops_args& ) override;

    const account_history_rocksdb::account_history_rocksdb_plugin& _dataSource;
};

DEFINE_API_IMPL( account_history_api_rocksdb_impl, get_ops_in_block )
{
  get_ops_in_block_return result;

  bool include_reversible = args.include_reversible.valid() ? *args.include_reversible : false;
  _dataSource.find_operations_by_block(args.block_num, include_reversible,
    [&result, &args](const account_history_rocksdb::rocksdb_operation_object& op)
    {
      api_operation_object temp(op);
      if( !args.only_virtual || is_virtual_operation( temp.op ) )
        result.ops.emplace(std::move(temp));
    }
  );
  return result;
}

#define CHECK_OPERATION_LOW( r, data, CLASS_NAME ) \
  void operator()( const hive::protocol::CLASS_NAME& ) { \
    _accepted = _filter_low & static_cast< uint64_t >( get_account_history_op_filter_low::CLASS_NAME ); }

#define CHECK_OPERATIONS_LOW( CLASS_NAMES ) \
  BOOST_PP_SEQ_FOR_EACH( CHECK_OPERATION_LOW, _, CLASS_NAMES )

#define CHECK_OPERATION_HIGH( r, data, CLASS_NAME ) \
  void operator()( const hive::protocol::CLASS_NAME& ) { \
    _accepted = _filter_high & static_cast< uint64_t >( get_account_history_op_filter_high::CLASS_NAME ); }

#define CHECK_OPERATIONS_HIGH( CLASS_NAMES ) \
  BOOST_PP_SEQ_FOR_EACH( CHECK_OPERATION_HIGH, _, CLASS_NAMES )

#define CHECK_SMT_OPERATION_HIGH( r, data, CLASS_NAME ) \
  void operator()( const hive::protocol::CLASS_NAME& ) { \
    _accepted = false; }

#define CHECK_SMT_OPERATIONS_HIGH( CLASS_NAMES ) \
  BOOST_PP_SEQ_FOR_EACH( CHECK_SMT_OPERATION_HIGH, _, CLASS_NAMES )


struct operation_filtering_visitor
{
  typedef void result_type;

  bool check(uint64_t filter_low, uint64_t filter_high, const hive::protocol::operation& op)
  {
    _filter_low = filter_low;
    _filter_high = filter_high;
    _accepted = false;
    op.visit(*this);

    return _accepted;
  }

  template< typename T >
  void operator()( const T& op ) = delete; // make new operation break compilation if not added below.

  CHECK_OPERATIONS_LOW( (vote_operation)(comment_operation)(transfer_operation)(transfer_to_vesting_operation)
  (withdraw_vesting_operation)(limit_order_create_operation)(limit_order_cancel_operation)(feed_publish_operation)
  (convert_operation)(account_create_operation)(account_update_operation)(witness_update_operation)
  (account_witness_vote_operation)(account_witness_proxy_operation)(pow_operation)(custom_operation)
  (witness_block_approve_operation)(delete_comment_operation)(custom_json_operation)(comment_options_operation)
  (set_withdraw_vesting_route_operation)(limit_order_create2_operation)(claim_account_operation)
  (create_claimed_account_operation)(request_account_recovery_operation)(recover_account_operation)
  (change_recovery_account_operation)(escrow_transfer_operation)(escrow_dispute_operation)(escrow_release_operation)
  (pow2_operation)(escrow_approve_operation)(transfer_to_savings_operation)(transfer_from_savings_operation)
  (cancel_transfer_from_savings_operation)(custom_binary_operation)(decline_voting_rights_operation)
  (reset_account_operation)(set_reset_account_operation)(claim_reward_balance_operation)
  (delegate_vesting_shares_operation)(account_create_with_delegation_operation)(witness_set_properties_operation)
  (account_update2_operation)(create_proposal_operation)(update_proposal_votes_operation)(remove_proposal_operation)
  (update_proposal_operation)(collateralized_convert_operation)(recurrent_transfer_operation)
  (fill_convert_request_operation)(author_reward_operation)(curation_reward_operation)
  (comment_reward_operation)(liquidity_reward_operation)(interest_operation)
  (fill_vesting_withdraw_operation)(fill_order_operation)(shutdown_witness_operation)
  (fill_transfer_from_savings_operation)(hardfork_operation)(comment_payout_update_operation)
  (return_vesting_delegation_operation)(comment_benefactor_reward_operation) )
  
  CHECK_OPERATIONS_HIGH( (producer_reward_operation) (clear_null_account_balance_operation)(proposal_pay_operation)
  (dhf_funding_operation)(hardfork_hive_operation)(hardfork_hive_restore_operation)(delayed_voting_operation)
  (consolidate_treasury_balance_operation)(effective_comment_vote_operation)(ineffective_delete_comment_operation)
  (dhf_conversion_operation)(expired_account_notification_operation)(changed_recovery_account_operation)
  (transfer_to_vesting_completed_operation)(pow_reward_operation)(vesting_shares_split_operation)
  (account_created_operation)(fill_collateralized_convert_request_operation)(system_warning_operation)
  (fill_recurrent_transfer_operation)(failed_recurrent_transfer_operation)(limit_order_cancelled_operation)
  (producer_missed_operation)(proposal_fee_operation)(collateralized_convert_immediate_conversion_operation)
  (escrow_approved_operation)(escrow_rejected_operation)(proxy_cleared_operation)(declined_voting_rights_operation)
  )

#ifdef HIVE_ENABLE_SMT
  CHECK_SMT_OPERATIONS_HIGH( (claim_reward_balance2_operation)(smt_setup_operation)(smt_setup_emissions_operation)(smt_set_setup_parameters_operation)
    (smt_set_runtime_parameters_operation)(smt_create_operation)(smt_contribute_operation)
  )
#endif

private:
  uint64_t _filter_low = 0ull;
  uint64_t _filter_high = 0ull;
  bool     _accepted = false;
};

DEFINE_API_IMPL( account_history_api_rocksdb_impl, get_account_history )
{
  FC_ASSERT( args.limit <= 1000, "limit of ${l} is greater than maxmimum allowed", ("l",args.limit) );
  FC_ASSERT( args.start >= args.limit-1, "start must be greater than or equal to limit-1 (start is 0-based index)" );

  get_account_history_return result;

  bool include_reversible = args.include_reversible.valid() ? *args.include_reversible : false;
  unsigned int total_processed_items = 0;
  if(args.operation_filter_low || args.operation_filter_high)
  {
    uint64_t filter_low = args.operation_filter_low ? *args.operation_filter_low : 0;
    uint64_t filter_high = args.operation_filter_high ? *args.operation_filter_high : 0;

    try
    {

    _dataSource.find_account_history_data(args.account, args.start, args.limit, include_reversible,
      [&result, filter_low, filter_high, &total_processed_items](unsigned int sequence, const account_history_rocksdb::rocksdb_operation_object& op) -> bool
      {
        FC_ASSERT(total_processed_items < 2000, "Could not find filtered operation in ${total_processed_items} operations, to continue searching, set start=${sequence}.",("total_processed_items",total_processed_items)("sequence",sequence));

        ++total_processed_items;

        // we want to accept any operations where the corresponding bit is set in {filter_high, filter_low}
        api_operation_object api_op(op);
        operation_filtering_visitor accepting_visitor;
        
        if( accepting_visitor.check( filter_low, filter_high, api_op.op ) )
        {
          result.history.emplace(sequence, std::move(api_op));
          return true;
        }
        else
        {
          return false;
        }
      });
    }
    catch(const fc::exception& e)
    { //if we have some results but not all requested, return what we have
      //but if no results, throw an exception that gives a starting item for further searching via pagination
      if (result.history.empty())
        throw;
    }
  }
  else
  {
    _dataSource.find_account_history_data(args.account, args.start, args.limit, include_reversible,
      [&result](unsigned int sequence, const account_history_rocksdb::rocksdb_operation_object& op) -> bool
      {
        /// Here internal counter (inside find_account_history_data) does the limiting job.
        result.history[sequence] = api_operation_object{op};
        return true;
      });
  }

  return result;
}

DEFINE_API_IMPL( account_history_api_rocksdb_impl, get_transaction )
{
  uint32_t blockNo = 0;
  uint32_t txInBlock = 0;

  hive::protocol::transaction_id_type id(args.id);
  if (args.id.size() != id.data_size() * 2)
    FC_ASSERT(false, "Transaction hash '${t}' has invalid size. Transaction hash should have size of ${s} bits", ("t", args.id)("s", sizeof(id._hash) * 8));

  bool include_reversible = args.include_reversible.valid() ? *args.include_reversible : false;

  if(_dataSource.find_transaction_info(id, include_reversible, &blockNo, &txInBlock))
  {
    std::shared_ptr<hive::chain::full_block_type> blk = 
      _block_reader.get_block_by_number(blockNo, fc::seconds(1));
    
    const auto& full_txs = blk->get_full_transactions();

    FC_ASSERT(full_txs.size() > txInBlock);
    const auto& full_tx = full_txs[txInBlock];

    // until AH is required to keep rc_cost we are not guaranteed to have that info, so the API won't be returning it (full_tx->get_rc_cost() would be -1)
    get_transaction_return result(full_tx->get_transaction(), full_tx->get_transaction_id(), blockNo, txInBlock);

    return result;
  }
  else
  {
    FC_ASSERT(false, "Unknown Transaction ${id}", (id));
  }
}

#define CHECK_VIRTUAL_OPERATION( r, data, CLASS_NAME ) \
  void operator()( const hive::protocol::CLASS_NAME& op ) { _accepted = (_filter & enum_vops_filter::CLASS_NAME) == enum_vops_filter::CLASS_NAME; }

#define CHECK_VIRTUAL_OPERATIONS( CLASS_NAMES ) \
  BOOST_PP_SEQ_FOR_EACH( CHECK_VIRTUAL_OPERATION, _, CLASS_NAMES )

struct virtual_operation_filtering_visitor
{
  typedef void result_type;

  bool check(uint64_t filter, const hive::protocol::operation& op)
  {
    _filter = filter;
    _accepted = false;
    op.visit(*this);

    return _accepted;
  }

  template< typename T >
  void operator()( const T& ) { _accepted = false; }

  CHECK_VIRTUAL_OPERATIONS( (fill_convert_request_operation)(author_reward_operation)(curation_reward_operation)
  (comment_reward_operation)(liquidity_reward_operation)(interest_operation)
  (fill_vesting_withdraw_operation)(fill_order_operation)(shutdown_witness_operation)
  (fill_transfer_from_savings_operation)(hardfork_operation)(comment_payout_update_operation)
  (return_vesting_delegation_operation)(comment_benefactor_reward_operation)(producer_reward_operation)
  (clear_null_account_balance_operation)(proposal_pay_operation)(dhf_funding_operation)
  (hardfork_hive_operation)(hardfork_hive_restore_operation)(delayed_voting_operation)
  (consolidate_treasury_balance_operation)(effective_comment_vote_operation)(ineffective_delete_comment_operation)
  (dhf_conversion_operation)(expired_account_notification_operation)(changed_recovery_account_operation)
  (transfer_to_vesting_completed_operation)(pow_reward_operation)(vesting_shares_split_operation)
  (account_created_operation)(fill_collateralized_convert_request_operation)(system_warning_operation)
  (fill_recurrent_transfer_operation)(failed_recurrent_transfer_operation)(limit_order_cancelled_operation)
  (producer_missed_operation)(proposal_fee_operation)(collateralized_convert_immediate_conversion_operation)
  (escrow_approved_operation)(escrow_rejected_operation)(proxy_cleared_operation)(declined_voting_rights_operation) )

private:
  uint64_t _filter = 0;
  bool     _accepted = false;
};

DEFINE_API_IMPL( account_history_api_rocksdb_impl, enum_virtual_ops)
{
  constexpr int32_t max_limit{ 150'000 };
  enum_virtual_ops_return result;

  bool groupOps = args.group_by_block.valid() && *args.group_by_block;
  bool include_reversible = args.include_reversible.valid() ? *args.include_reversible : false;
  int32_t limit = args.limit.valid() ? *args.limit : max_limit;

  FC_ASSERT( limit > 0, "limit of ${l} is lesser or equal 0", ("l",limit) );
  FC_ASSERT( limit <= max_limit, "limit of ${l} is greater than maxmimum allowed", ("l",limit) );

  std::pair< uint32_t, uint64_t > next_values = _dataSource.enum_operations_from_block_range(args.block_range_begin,
    args.block_range_end, include_reversible, args.operation_begin, limit,
    [groupOps, &result, &args ](const account_history_rocksdb::rocksdb_operation_object& op, uint64_t operation_id, bool irreversible)
    {
      api_operation_object _api_obj(op);

      _api_obj.operation_id = operation_id;

      if( args.filter.valid() )
      {
        virtual_operation_filtering_visitor accepting_visitor;

        if(accepting_visitor.check(*args.filter, _api_obj.op))
        {
          if(groupOps)
          {
            auto ii = result.ops_by_block.emplace(op.block);
            ops_array_wrapper& w = const_cast<ops_array_wrapper&>(*ii.first);

            if(ii.second)
            {
              w.timestamp = op.timestamp;
              w.irreversible = irreversible;
            }

            w.ops.emplace_back(std::move(_api_obj));
          }
          else
          {
            result.ops.emplace_back(std::move(_api_obj));
          }
          return true;
        }
        else
          return false;
      }
      else
      {
        if(groupOps)
        {
          auto ii = result.ops_by_block.emplace(op.block);
          ops_array_wrapper& w = const_cast<ops_array_wrapper&>(*ii.first);

          if(ii.second)
          {
            w.timestamp = op.timestamp;
            w.irreversible = irreversible;
          }

          w.ops.emplace_back(std::move(_api_obj));
        }
        else
        {
          result.ops.emplace_back(std::move(_api_obj));
        }
        return true;
      }
    }
  );

  result.next_block_range_begin = next_values.first;
  result.next_operation_begin = next_values.second;

  return result;
}

} // detail

account_history_api::account_history_api( appbase::application& app )
{
  my = std::make_unique< detail::account_history_api_rocksdb_impl >( app );
  JSON_RPC_REGISTER_API( HIVE_ACCOUNT_HISTORY_API_PLUGIN_NAME );
}

account_history_api::~account_history_api() {}

DEFINE_LOCKLESS_APIS( account_history_api ,
  (get_ops_in_block)
  (get_transaction)
  (get_account_history)
  (enum_virtual_ops)
)

} } } // hive::plugins::account_history
