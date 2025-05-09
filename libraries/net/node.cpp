/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <atomic>
#include <sstream>
#include <iomanip>
#include <deque>
#include <unordered_set>
#include <list>
#include <forward_list>
#include <iostream>
#include <algorithm>
#include <tuple>
#include <boost/tuple/tuple.hpp>
#include <boost/circular_buffer.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/range/numeric.hpp>
#include <boost/scope_exit.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/rolling_mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>
#include <boost/accumulators/statistics/count.hpp>

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>

#include <fc/thread/thread.hpp>
#include <fc/thread/future.hpp>
#include <fc/thread/non_preemptable_scope_check.hpp>
#include <fc/thread/mutex.hpp>
#include <fc/thread/scoped_lock.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/json.hpp>
#include <fc/io/enum_type.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/network/rate_limiting.hpp>
#include <fc/network/ip.hpp>
#include <fc/smart_ref_impl.hpp>
#include <fc/bitutil.hpp>

#include <graphene/net/node_configuration.hpp>
#include <graphene/net/node.hpp>
#include <graphene/net/peer_database.hpp>
#include <graphene/net/peer_connection.hpp>
#include <graphene/net/stcp_socket.hpp>
#include <graphene/net/config.hpp>
#include <graphene/net/exceptions.hpp>

#include <hive/protocol/config.hpp>
#include <hive/plugins/statsd/utility.hpp>
#include <hive/chain/block_compression_dictionaries.hpp>
#include <hive/chain/blockchain_worker_thread_pool.hpp>

#include <fc/git_revision.hpp>

//#define ENABLE_DEBUG_ULOGS

#ifdef DEFAULT_LOGGER
# undef DEFAULT_LOGGER
#endif
#define DEFAULT_LOGGER "p2p"

#define P2P_IN_DEDICATED_THREAD 1

#define INVOCATION_COUNTER(name) \
    static unsigned total_ ## name ## _counter = 0; \
    static unsigned active_ ## name ## _counter = 0; \
    struct name ## _invocation_logger { \
      unsigned *total; \
      unsigned *active; \
      name ## _invocation_logger(unsigned *total, unsigned *active) : \
        total(total), active(active) \
      { \
        ++*total; \
        ++*active; \
        dlog("NEWDEBUG: Entering " #name ", now ${total} total calls, ${active} active calls", ("total", *total)("active", *active)); \
      } \
      ~name ## _invocation_logger() \
      { \
        --*active; \
        dlog("NEWDEBUG: Leaving " #name ", now ${total} total calls, ${active} active calls", ("total", *total)("active", *active)); \
      } \
    } invocation_logger(&total_ ## name ## _counter, &active_ ## name ## _counter)

//log these messages even at warn level when operating on the test network
#ifdef GRAPHENE_TEST_NETWORK
#define testnetlog wlog
#else
#define testnetlog(...) do {} while (0)
#endif

namespace graphene { namespace net {

  namespace detail
  {
    namespace bmi = boost::multi_index;

    struct message_hash_index{};
    struct message_contents_hash_index{};
    struct block_clock_index{};


    template <typename smart_ptr_type>
    class blockchain_tied_message_cache
    {
    private:
      static const uint32_t cache_duration_in_blocks = GRAPHENE_NET_MESSAGE_CACHE_DURATION_IN_BLOCKS;

      struct message_info
      {
        smart_ptr_type item;
        uint32_t block_clock_when_received;

        // for network performance stats
        message_propagation_data propagation_data;

        message_info(const smart_ptr_type& item,
                     uint32_t block_clock_when_received,
                     const message_propagation_data& propagation_data) :
          item(item),
          block_clock_when_received(block_clock_when_received),
          propagation_data(propagation_data)
        {}
      };
      struct get_message_contents_hash
      {
        typedef item_hash_t result_type;
        const item_hash_t& get_hash_from_item(const std::shared_ptr<full_block_type>& full_block) const
        {
          return full_block->get_block_id();
        }
        const item_hash_t& get_hash_from_item(const std::shared_ptr<full_transaction_type>& full_transaction) const
        {
          return full_transaction->get_transaction_id();
        }
        const item_hash_t& operator()(const message_info& info) const
        {
          return get_hash_from_item(info.item);
        }
      };
      struct get_message_hash
      {
        typedef item_hash_t result_type;
        const item_hash_t& get_message_hash_from_item(const std::shared_ptr<full_block_type>& full_block) const
        {
          return full_block->get_legacy_block_message_hash();
        }
        const item_hash_t& get_message_hash_from_item(const std::shared_ptr<full_transaction_type>& full_transaction) const
        {
          return full_transaction->get_legacy_transaction_message_hash();
        }
        const item_hash_t& operator()(const message_info& info) const
        {
          return get_message_hash_from_item(info.item);
        }
      };
      typedef boost::multi_index_container
        <message_info,
         bmi::indexed_by<bmi::ordered_unique<bmi::tag<message_hash_index>, get_message_hash>, 
                         bmi::ordered_non_unique<bmi::tag<message_contents_hash_index>, get_message_contents_hash>,
                         bmi::ordered_non_unique<bmi::tag<block_clock_index>,
                                                 bmi::member<message_info, uint32_t, &message_info::block_clock_when_received>>>> message_cache_container;

      message_cache_container _message_cache;
      uint32_t block_clock = 0;

    public:
      void block_accepted();
      void cache_message(const smart_ptr_type& item, const message_propagation_data& propagation_data);
      smart_ptr_type get_item(const message_hash_type& message_hash) const;
      bool contains_item(const message_hash_type& message_hash) const;
      smart_ptr_type get_item_by_contents_hash(const message_hash_type& message_hash) const;
      bool contains_item_by_contents_hash(const message_hash_type& message_hash) const;
      message_propagation_data get_message_propagation_data(const fc::uint160_t& message_contents_hash) const;
      size_t size() const { return _message_cache.size(); }
    };

    template <typename smart_ptr_type>
    void blockchain_tied_message_cache<smart_ptr_type>::block_accepted()
    {
      ++block_clock;
      if (block_clock > cache_duration_in_blocks)
      {
        auto& idx = _message_cache.template get<block_clock_index>();
        idx.erase(idx.begin(), idx.lower_bound(block_clock - cache_duration_in_blocks));
      }
    }

    template <typename smart_ptr_type>
    void blockchain_tied_message_cache<smart_ptr_type>::cache_message(const smart_ptr_type& item, const message_propagation_data& propagation_data)
    {
      _message_cache.insert(message_info(item, block_clock, propagation_data));
    }

    template <typename smart_ptr_type>
    smart_ptr_type blockchain_tied_message_cache<smart_ptr_type>::get_item(const message_hash_type& message_hash) const
    {
      const auto& idx = _message_cache.template get<message_hash_index>();
      if (const auto iter = idx.find(message_hash); iter != idx.end())
        return iter->item;
      FC_THROW_EXCEPTION(fc::key_not_found_exception, "Requested message not in cache");
    }

    template <typename smart_ptr_type>
    bool blockchain_tied_message_cache<smart_ptr_type>::contains_item(const message_hash_type& message_hash) const
    {
      const auto& idx = _message_cache.template get<message_hash_index>();
      return idx.find(message_hash) != idx.end();
    }

    template <typename smart_ptr_type>
    smart_ptr_type blockchain_tied_message_cache<smart_ptr_type>::get_item_by_contents_hash(const message_hash_type& message_contents_hash) const
    {
      const auto& idx = _message_cache.template get<message_contents_hash_index>();
      if (const auto iter = idx.find(message_contents_hash); iter != idx.end())
        return iter->item;
      FC_THROW_EXCEPTION(fc::key_not_found_exception, "Requested message not in cache");
    }

    template <typename smart_ptr_type>
    bool blockchain_tied_message_cache<smart_ptr_type>::contains_item_by_contents_hash(const message_hash_type& message_contents_hash) const
    {
      const auto& idx = _message_cache.template get<message_contents_hash_index>();
      return idx.find(message_contents_hash) != idx.end();
    }

    template <typename smart_ptr_type>
    message_propagation_data blockchain_tied_message_cache<smart_ptr_type>::get_message_propagation_data(const fc::uint160_t& hash_of_message_contents_to_lookup) const
    {
      const auto& idx = _message_cache.template get<message_contents_hash_index>();
      if (hash_of_message_contents_to_lookup != fc::uint160_t())
      {
        if (const auto iter = idx.find(hash_of_message_contents_to_lookup); iter != idx.end())
          return iter->propagation_data;
      }
      FC_THROW_EXCEPTION(fc::key_not_found_exception, "Requested message not in cache");
    }

    // when requesting items from peers, we want to prioritize any blocks before
    // transactions, but otherwise request items in the order we heard about them
    struct prioritized_item_id
    {
      item_id item;
      unsigned sequence_number;
      fc::time_point timestamp; // the time we last heard about this item in an inventory message

      prioritized_item_id(const item_id& item, unsigned sequence_number) :
        item(item),
        sequence_number(sequence_number),
        timestamp(fc::time_point::now())
      {}
      bool operator<(const prioritized_item_id& rhs) const
      {
        static_assert(graphene::net::block_message_type > graphene::net::trx_message_type,
                      "block_message_type must be greater than trx_message_type for prioritized_item_ids to sort correctly");
        if (item.item_type != rhs.item.item_type)
          return item.item_type > rhs.item.item_type;
        return (signed)(rhs.sequence_number - sequence_number) > 0;
      }
    };

/////////////////////////////////////////////////////////////////////////////////////////////////////////
    class statistics_gathering_node_delegate_wrapper : public node_delegate
    {
    private:
      node_delegate *_node_delegate;
      fc::thread *_thread;

      typedef boost::accumulators::accumulator_set<int64_t, boost::accumulators::stats<boost::accumulators::tag::min,
                                                                                       boost::accumulators::tag::rolling_mean,
                                                                                       boost::accumulators::tag::max,
                                                                                       boost::accumulators::tag::sum,
                                                                                       boost::accumulators::tag::count> > call_stats_accumulator;
#define NODE_DELEGATE_METHOD_NAMES (has_item) \
                                   (handle_message) \
                                   (handle_block) \
                                   (handle_transaction) \
                                   (get_block_ids) \
                                   (get_item) \
                                   (get_blockchain_synopsis) \
                                   (sync_status) \
                                   (connection_count_changed) \
                                   (get_block_number) \
                                   (get_block_time) \
                                   (get_head_block_id) \
                                   (estimate_last_known_fork_from_git_revision_timestamp) \
                                   (error_encountered) \
                                   (find_first_item_not_in_blockchain) \
                                   (get_chain_id)


#define DECLARE_ACCUMULATOR(r, data, method_name) \
      mutable call_stats_accumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator)); \
      mutable call_stats_accumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator)); \
      mutable call_stats_accumulator BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator));
      BOOST_PP_SEQ_FOR_EACH(DECLARE_ACCUMULATOR, unused, NODE_DELEGATE_METHOD_NAMES)
#undef DECLARE_ACCUMULATOR

      class call_statistics_collector
      {
      private:
        fc::time_point _call_requested_time;
        fc::time_point _begin_execution_time;
        fc::time_point _execution_completed_time;
        const char* _method_name;
        call_stats_accumulator* _execution_accumulator;
        call_stats_accumulator* _delay_before_accumulator;
        call_stats_accumulator* _delay_after_accumulator;
      public:
        class actual_execution_measurement_helper
        {
          call_statistics_collector &_collector;
        public:
          actual_execution_measurement_helper(call_statistics_collector& collector) :
            _collector(collector)
          {
            _collector.starting_execution();
          }
          ~actual_execution_measurement_helper()
          {
            _collector.execution_completed();
          }
        };
        call_statistics_collector(const char* method_name,
                                  call_stats_accumulator* execution_accumulator,
                                  call_stats_accumulator* delay_before_accumulator,
                                  call_stats_accumulator* delay_after_accumulator) :
          _call_requested_time(fc::time_point::now()),
          _method_name(method_name),
          _execution_accumulator(execution_accumulator),
          _delay_before_accumulator(delay_before_accumulator),
          _delay_after_accumulator(delay_after_accumulator)
        {}
        ~call_statistics_collector()
        {
          fc::time_point end_time(fc::time_point::now());
          fc::microseconds actual_execution_time(_execution_completed_time - _begin_execution_time);
          fc::microseconds delay_before(_begin_execution_time - _call_requested_time);
          fc::microseconds delay_after(end_time - _execution_completed_time);
          fc::microseconds total_duration(actual_execution_time + delay_before + delay_after);
          (*_execution_accumulator)(actual_execution_time.count());
          (*_delay_before_accumulator)(delay_before.count());
          (*_delay_after_accumulator)(delay_after.count());
          if (total_duration > fc::milliseconds(500))
          {
            ilog("Call to method node_delegate::${method} took ${total_duration}us, longer than our target maximum of 500ms",
                 ("method", _method_name)
                 ("total_duration", total_duration.count()));
            ilog("Actual execution took ${execution_duration}us, with a ${delegate_delay}us delay before the delegate thread started "
                 "executing the method, and a ${p2p_delay}us delay after it finished before the p2p thread started processing the response",
                 ("execution_duration", actual_execution_time)
                 ("delegate_delay", delay_before)
                 ("p2p_delay", delay_after));
          }
        }
        void starting_execution()
        {
          _begin_execution_time = fc::time_point::now();
        }
        void execution_completed()
        {
          _execution_completed_time = fc::time_point::now();
        }
      };
    public:
      statistics_gathering_node_delegate_wrapper(node_delegate* delegate, fc::thread* thread_for_delegate_calls);

      fc::variant_object get_call_statistics();

      hive::protocol::chain_id_type get_old_chain_id() const override;
      hive::protocol::chain_id_type get_new_chain_id() const override;
      hive::protocol::chain_id_type get_chain_id() const override;
      bool has_item( const net::item_id& id ) override;
      void handle_message( const message& ) override;
      bool handle_block(const std::shared_ptr<full_block_type>& full_block, bool sync_mode) override;
      void handle_transaction(const std::shared_ptr<full_transaction_type>& full_transaction) override;
      std::vector<item_hash_t> get_block_ids(const std::vector<item_hash_t>& blockchain_synopsis,
                                             uint32_t& remaining_item_count,
                                             uint32_t limit = DEFAULT_MAX_BLOCK_IDS_TO_FETCH) override;
      std::shared_ptr<hive::chain::full_block_type> get_full_block(const block_id_type& id) override;
      std::vector<item_hash_t> get_blockchain_synopsis(const item_hash_t& reference_point,
                                                       uint32_t number_of_blocks_after_reference_point) override;
      void     sync_status( uint32_t item_type, uint32_t item_count ) override;
      void     connection_count_changed( uint32_t c ) override;
      uint32_t get_block_number(const item_hash_t& block_id) override;
      fc::time_point_sec get_block_time(const item_hash_t& block_id) override;
      fc::time_point_sec get_blockchain_now() override;
      item_hash_t get_head_block_id() const override;
      uint32_t estimate_last_known_fork_from_git_revision_timestamp(uint32_t unix_timestamp) const override;
      void error_encountered(const std::string& message, const fc::oexception& error) override;
      std::deque<block_id_type>::const_iterator find_first_item_not_in_blockchain(const std::deque<block_id_type>& item_hashes_received) override;
    };

/////////////////////////////////////////////////////////////////////////////////////////////////////////
    class node_impl : public peer_connection_delegate
    {
    public:
#ifdef P2P_IN_DEDICATED_THREAD
      std::shared_ptr<fc::thread> _thread;
#endif // P2P_IN_DEDICATED_THREAD
      std::unique_ptr<statistics_gathering_node_delegate_wrapper> _delegate;

#define NODE_CONFIGURATION_FILENAME      "node_config.json"
#define POTENTIAL_PEER_DATABASE_FILENAME "peers.json"
      fc::path             _node_configuration_directory;
      node_configuration   _node_configuration;

      /// stores the endpoint we're listening on.  This will be the same as
      // _node_configuration.listen_endpoint, unless that endpoint was already
      // in use.
      fc::ip::endpoint     _actual_listening_endpoint;

      /// we determine whether we're firewalled by asking other nodes.  Store the result here:
      firewalled_state     _is_firewalled;
      /// if we're behind NAT, our listening endpoint address will appear different to the rest of the world.  store it here.
      fc::optional<fc::ip::endpoint> _publicly_visible_listening_endpoint;
      fc::time_point       _last_firewall_check_message_sent;

      /// used by the task that manages connecting to peers
      // @{
      std::list<potential_peer_record> _add_once_node_list; /// list of peers we want to connect to as soon as possible

      peer_database             _potential_peer_db;
      fc::promise<void>::ptr    _retrigger_connect_loop_promise;
      bool                      _potential_peer_database_updated;
      fc::future<void>          _p2p_network_connect_loop_done;
      // @}

      /// used by the task that fetches sync items during synchronization
      // @{
      fc::promise<void>::ptr    _retrigger_fetch_sync_items_loop_promise;
      bool                      _sync_items_to_fetch_updated;
      fc::future<void>          _fetch_sync_items_loop_done;

      typedef std::unordered_map<graphene::net::block_id_type, fc::time_point> active_sync_requests_map;

      active_sync_requests_map              _active_sync_requests; /// list of sync blocks we've asked for from peers but have not yet received

      struct compare_full_blocks_by_block_id
      {
        typedef const item_hash_t& result_type;
        const item_hash_t& operator()(const std::shared_ptr<full_block_type>& full_block) const
        {
          return full_block->get_block_id();
        }
      };
      typedef boost::multi_index_container<std::shared_ptr<full_block_type>,
                                           boost::multi_index::indexed_by<boost::multi_index::ordered_unique<compare_full_blocks_by_block_id>>> received_sync_items_by_id;

      std::set<item_hash_t>  _already_received_sync_item_ids; //set for fast lookup of already received sync items
      std::list<std::shared_ptr<full_block_type>> _new_received_sync_items; /// list of sync blocks we've just received but haven't yet tried to process
      received_sync_items_by_id _received_sync_items; /// set of sync blocks we've received, but can't yet process because we are still missing blocks that come earlier in the chain
      // @}

      fc::future<void> _process_backlog_of_sync_blocks_done;
      bool _suspend_fetching_sync_blocks;

      /// used by the task that fetches items during normal operation
      // @{
      fc::promise<void>::ptr _retrigger_fetch_item_loop_promise;
      bool                   _items_to_fetch_updated;
      fc::future<void>       _fetch_item_loop_done;

      struct item_id_index{};
      typedef boost::multi_index_container<prioritized_item_id,
                                           boost::multi_index::indexed_by<boost::multi_index::ordered_unique<boost::multi_index::identity<prioritized_item_id> >,
                                                                          boost::multi_index::hashed_unique<boost::multi_index::tag<item_id_index>,
                                                                                                            boost::multi_index::member<prioritized_item_id, item_id, &prioritized_item_id::item>,
                                                                                                            std::hash<item_id> > >
                                           > items_to_fetch_set_type;
      unsigned _items_to_fetch_sequence_counter;
      items_to_fetch_set_type _items_to_fetch; /// list of items we know another peer has and we want
      peer_connection::timestamped_items_set_type _recently_failed_items; /// list of transactions we've recently pushed and had rejected by the delegate
      // @}

      /// used by the task that advertises inventory during normal operation
      // @{
      fc::promise<void>::ptr        _retrigger_advertise_inventory_loop_promise;
      fc::future<void>              _advertise_inventory_loop_done;
      std::set<std::shared_ptr<full_block_type>> _new_block_inventory;
      std::set<std::shared_ptr<full_transaction_type>> _new_transaction_inventory;
      // @}

      fc::future<void>     _terminate_inactive_connections_loop_done;
      uint8_t _recent_block_interval_in_seconds; // a cached copy of the block interval, to avoid a thread hop to the blockchain to get the current value

      std::string          _user_agent_string;
      /** _node_public_key is a key automatically generated when the client is first run, stored in
       * node_config.json.  It doesn't really have much of a purpose yet, there was just some thought
       * that we might someday have a use for nodes having a private key (sent in hello messages)
       */
      node_id_t            _node_public_key;
      /**
       * _node_id is a random number generated each time the client is launched, used to prevent us
       * from connecting to the same client multiple times (sent in hello messages).
       * Since this was introduced after the hello_message was finalized, this is sent in the
       * user_data field.
       * While this shares the same underlying type as a public key, it is really just a random
       * number.
       */
      node_id_t            _node_id;

      fc::tcp_server       _tcp_server;
      fc::future<void>     _accept_loop_complete;

      /** Stores all connections which have not yet finished key exchange or are still sending initial handshaking messages
       * back and forth (not yet ready to initiate syncing) */
      std::unordered_set<peer_connection_ptr>                     _handshaking_connections;
      /** stores fully established connections we're either syncing with or in normal operation with */
      std::unordered_set<peer_connection_ptr>                     _active_connections;
      /** stores connections we've closed (sent closing message, not actually closed), but are still waiting for the remote end to close before we delete them */
      std::unordered_set<peer_connection_ptr>                     _closing_connections;
      /** stores connections we've closed, but are still waiting for the OS to notify us that the socket is really closed */
      std::unordered_set<peer_connection_ptr>                     _terminating_connections;

      boost::circular_buffer<item_hash_t> _most_recent_blocks_accepted; // the /n/ most recent blocks we've accepted (currently tuned to the max number of connections)

      uint32_t _sync_item_type;
      uint32_t _total_number_of_unfetched_items; /// the number of items we still need to fetch while syncing
      std::vector<uint32_t> _hard_fork_block_numbers; /// list of all block numbers where there are hard forks

      blockchain_tied_message_cache<std::shared_ptr<full_block_type>> _block_message_cache; /// cache blocks we have received and might be required to provide to other peers via inventory requests
      blockchain_tied_message_cache<std::shared_ptr<full_transaction_type>> _transaction_message_cache; /// cache blocks we have received and might be required to provide to other peers via inventory requests

      fc::rate_limiting_group _rate_limiter;

      uint32_t _last_reported_number_of_connections; // number of connections last reported to the client (to avoid sending duplicate messages)

      fc::future<void> _fetch_updated_peer_lists_loop_done;

      boost::circular_buffer<uint32_t> _average_network_read_speed_seconds;
      boost::circular_buffer<uint32_t> _average_network_write_speed_seconds;
      boost::circular_buffer<uint32_t> _average_network_read_speed_minutes;
      boost::circular_buffer<uint32_t> _average_network_write_speed_minutes;
      boost::circular_buffer<uint32_t> _average_network_read_speed_hours;
      boost::circular_buffer<uint32_t> _average_network_write_speed_hours;
      unsigned _average_network_usage_second_counter;
      unsigned _average_network_usage_minute_counter;

      fc::time_point_sec _bandwidth_monitor_last_update_time;
      fc::future<void> _bandwidth_monitor_loop_done;

      fc::future<void> _dump_node_status_task_done;

      /* We have two alternate paths through the schedule_peer_for_deletion code -- one that
       * uses a mutex to prevent one fiber from adding items to the queue while another is deleting
       * items from it, and one that doesn't.  The one that doesn't is simpler and more efficient
       * code, but we're keeping around the version that uses the mutex because it crashes, and
       * this crash probably indicates a bug in our underlying threading code that needs
       * fixing.  To produce the bug, define USE_PEERS_TO_DELETE_MUTEX and then connect up
       * to the network and set your desired/max connection counts high
       */
//#define USE_PEERS_TO_DELETE_MUTEX 1
#ifdef USE_PEERS_TO_DELETE_MUTEX
      fc::mutex _peers_to_delete_mutex;
#endif
      std::list<peer_connection_ptr> _peers_to_delete;
      fc::future<void> _delayed_peer_deletion_task_done;

#ifdef ENABLE_P2P_DEBUGGING_API
      std::set<node_id_t> _allowed_peers;
#endif // ENABLE_P2P_DEBUGGING_API

      std::atomic_bool _node_is_shutting_down; // set to true when we begin our destructor, used to prevent us from starting new tasks while we're shutting down

      std::list<fc::future<void>> _handle_message_calls_in_progress;
      std::set<message_hash_type> _message_ids_currently_being_processed;
      std::set<block_id_type> _block_ids_currently_being_processed;

      std::atomic_int        _activeCalls;
      fc::promise<void>::ptr _shutdownNotifier;

      appbase::application& theApp;
      hive::chain::blockchain_worker_thread_pool& _thread_pool;

      node_impl(const std::string& user_agent, appbase::application& app, hive::chain::blockchain_worker_thread_pool& thread_pool);
      virtual ~node_impl();
      void save_active_peers_to_peer_database();

      void save_node_configuration();

      void p2p_network_connect_loop();
      void trigger_p2p_network_connect_loop();

      bool have_already_received_sync_item( const item_hash_t& item_hash );
      void request_sync_item_from_peer( const peer_connection_ptr& peer, const item_hash_t& item_to_request );
      void request_sync_items_from_peer( const peer_connection_ptr& peer, const std::vector<item_hash_t>& items_to_request );
      void update_last_requested_block_number_for_peers_on_this_fork(uint32_t last_requested_block_number, const item_hash_t& last_requested_block_id);
      void fetch_sync_items_loop();
      void trigger_fetch_sync_items_loop();

      bool is_item_in_any_peers_inventory(const item_id& item) const;
      void fetch_items_loop();
      void trigger_fetch_items_loop();

      void advertise_inventory_loop();
      void trigger_advertise_inventory_loop();

      void terminate_inactive_connections_loop();

      void fetch_updated_peer_lists_loop();
      void update_bandwidth_data(uint32_t bytes_read_this_second, uint32_t bytes_written_this_second);
      void bandwidth_monitor_loop();
      void dump_node_status_task();

      bool is_accepting_new_connections();
      bool is_wanting_new_connections();
      uint32_t get_number_of_connections();
      peer_connection_ptr get_peer_by_node_id(const node_id_t& id);

      bool is_already_connected_to_id(const node_id_t& node_id);
      bool merge_address_info_with_potential_peer_database( const std::vector<address_info> addresses );
      void display_current_connections();
      uint32_t calculate_unsynced_block_count_from_all_peers();
      std::vector<item_hash_t> create_blockchain_synopsis_for_peer( const peer_connection* peer );
      void fetch_next_batch_of_item_ids_from_peer( peer_connection* peer, bool reset_fork_tracking_data_for_peer = false );

      fc::variant_object generate_hello_user_data();
      void parse_hello_user_data_for_peer( peer_connection* originating_peer, const fc::variant_object& user_data );

      void on_message( peer_connection* originating_peer,
                       const message& received_message ) override;

      void on_hello_message( peer_connection* originating_peer,
                             const hello_message& hello_message_received );

      void on_connection_accepted_message( peer_connection* originating_peer,
                                           const connection_accepted_message& connection_accepted_message_received );

      void on_connection_rejected_message( peer_connection* originating_peer,
                                           const connection_rejected_message& connection_rejected_message_received );

      void on_address_request_message( peer_connection* originating_peer,
                                       const address_request_message& address_request_message_received );

      void on_address_message( peer_connection* originating_peer,
                               const address_message& address_message_received );

      void on_fetch_blockchain_item_ids_message( peer_connection* originating_peer,
                                                 const fetch_blockchain_item_ids_message& fetch_blockchain_item_ids_message_received );
      bool is_item_id_being_processed(const item_hash_t& block_id);

      void on_blockchain_item_ids_inventory_message( peer_connection* originating_peer,
                                                     const blockchain_item_ids_inventory_message& blockchain_item_ids_inventory_message_received );

      void on_fetch_items_message( peer_connection* originating_peer,
                                   const fetch_items_message& fetch_items_message_received );

      void on_item_not_available_message( peer_connection* originating_peer,
                                          const item_not_available_message& item_not_available_message_received );

      void on_item_ids_inventory_message( peer_connection* originating_peer,
                                          const item_ids_inventory_message& item_ids_inventory_message_received );

      void on_closing_connection_message( peer_connection* originating_peer,
                                          const closing_connection_message& closing_connection_message_received );

      void on_current_time_request_message( peer_connection* originating_peer,
                                            const current_time_request_message& current_time_request_message_received );

      void on_current_time_reply_message( peer_connection* originating_peer,
                                          const current_time_reply_message& current_time_reply_message_received );

      void forward_firewall_check_to_next_available_peer(firewall_check_state_data* firewall_check_state);

      void on_check_firewall_message(peer_connection* originating_peer,
                                     const check_firewall_message& check_firewall_message_received);

      void on_check_firewall_reply_message(peer_connection* originating_peer,
                                           const check_firewall_reply_message& check_firewall_reply_message_received);

      void on_get_current_connections_request_message(peer_connection* originating_peer,
                                                      const get_current_connections_request_message& get_current_connections_request_message_received);

      void on_get_current_connections_reply_message(peer_connection* originating_peer,
                                                    const get_current_connections_reply_message& get_current_connections_reply_message_received);

      void on_connection_closed(peer_connection* originating_peer) override;

      void send_sync_block_to_node_delegate(const std::shared_ptr<full_block_type>& full_block);
      uint32_t get_number_of_handle_message_calls_in_progress();
      void discard_abandoned_sync_blocks();
      void process_backlog_of_sync_blocks();
      void trigger_process_backlog_of_sync_blocks();
      void process_block_during_sync(peer_connection* originating_peer, const std::shared_ptr<full_block_type>& full_block);
      void process_block_during_normal_operation(peer_connection* originating_peer, const std::shared_ptr<full_block_type>& full_block);
      void process_block_message(peer_connection* originating_peer, const message& message_to_process, const message_hash_type& message_hash);
      void process_trx_message(peer_connection* originating_peer, const trx_message& transaction_message_to_process);

      void process_ordinary_message(peer_connection* originating_peer, const message& message_to_process, const message_hash_type& message_hash);

      void start_synchronizing();
      void start_synchronizing_with_peer(const peer_connection_ptr& peer);

      void new_peer_just_added(const peer_connection_ptr& peer); /// called after a peer finishes handshaking, kicks off syncing

      void close();

      void accept_connection_task(const peer_connection_ptr& new_peer);
      void accept_loop();
      void send_hello_message(const peer_connection_ptr& peer);
      void connect_to_task(const peer_connection_ptr& new_peer, const fc::ip::endpoint& remote_endpoint);
      bool is_connection_to_endpoint_in_progress(const fc::ip::endpoint& remote_endpoint);

      void move_peer_to_active_list(const peer_connection_ptr& peer);
      void move_peer_to_closing_list(const peer_connection_ptr& peer);
      void move_peer_to_terminating_list(const peer_connection_ptr& peer);

      peer_connection_ptr get_connection_to_endpoint( const fc::ip::endpoint& remote_endpoint );

      void dump_node_status();

      void delayed_peer_deletion_task();
      void schedule_peer_for_deletion(const peer_connection_ptr& peer_to_delete);

      void disconnect_from_peer(peer_connection* originating_peer,
                                const std::string& reason_for_disconnect,
                                bool caused_by_error = false,
                                const fc::oexception& additional_data = fc::oexception());

      // methods implementing node's public interface
      void set_node_delegate(node_delegate* del, fc::thread* thread_for_delegate_calls);
      void load_configuration( const fc::path& configuration_directory );
      void listen_to_p2p_network( std::function<bool()> break_callback );
      void connect_to_p2p_network();
      void add_node( const fc::ip::endpoint& ep );
      void initiate_connect_to(const peer_connection_ptr& peer);
      void connect_to_endpoint(const fc::ip::endpoint& ep);
      void listen_on_endpoint(const fc::ip::endpoint& ep , bool wait_if_not_available);
      void accept_incoming_connections(bool accept);
      void listen_on_port( uint16_t port, bool wait_if_not_available );

      fc::ip::endpoint         get_actual_listening_endpoint() const;
      std::vector<peer_status> get_connected_peers() const;
      uint32_t                 get_connection_count() const;

      void broadcast(const std::shared_ptr<full_block_type>& full_block, const message_propagation_data& propagation_data);
      void broadcast(const std::shared_ptr<full_transaction_type>& full_transaction, const message_propagation_data& propagation_data);
      void broadcast(const std::shared_ptr<full_block_type>& full_block);
      void broadcast(const std::shared_ptr<hive::chain::full_transaction_type>& full_transaction);

      void sync_from(const item_id& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers);
      bool is_connected() const;
      std::vector<potential_peer_record> get_potential_peers() const;
      void set_advanced_node_parameters( const fc::variant_object& params );

      node_configuration         get_advanced_node_parameters()const;
      message_propagation_data   get_transaction_propagation_data( const graphene::net::transaction_id_type& transaction_id );
      message_propagation_data   get_block_propagation_data( const graphene::net::block_id_type& block_id );

      node_id_t                  get_node_id() const;
      void                       set_allowed_peers( const std::vector<node_id_t>& allowed_peers );
      void                       clear_peer_database();
      void                       set_total_bandwidth_limit( uint32_t upload_bytes_per_second, uint32_t download_bytes_per_second );
      fc::variant_object         get_call_statistics() const;
      std::shared_ptr<full_block_type> get_full_block_by_block_id(const block_id_type& block_id) override;

      fc::variant_object         network_get_info() const;
      fc::variant_object         network_get_usage_stats() const;

      bool is_hard_fork_block(uint32_t block_number) const;
      uint32_t get_next_known_hard_fork_block_number(uint32_t block_number) const;

      bool node_is_shutting_down() const;

   private:
      class activity_tracer final
      {
      public:
        activity_tracer(const char* function, node_impl& node) : _node(node), _notify(false)
        {
          if (_node.node_is_shutting_down())
            FC_THROW_EXCEPTION(fc::canceled_exception, "${function} canceled because it is shutting down", (function));

          ++_node._activeCalls;
          /// At this point potential shutdownNotification shall be sent in destructor.
          _notify = true;
        }

        ~activity_tracer()
        {
          if (_notify && --_node._activeCalls == 0 && _node.node_is_shutting_down())
            _node._shutdownNotifier->set_value();
        }
      private:
        node_impl& _node;
        bool       _notify;
      }; /// activity_tracer

#ifdef P2P_IN_DEDICATED_THREAD
# define VERIFY_CORRECT_THREAD() assert(_thread->is_current())
#else
# define VERIFY_CORRECT_THREAD() do {} while (0)
#endif

      template<typename Functor>
      auto async_task(Functor&& f, const char* desc FC_TASK_NAME_DEFAULT_ARG, fc::priority prio = fc::priority()) -> fc::future<decltype(f())>
      {
        auto wrapper = [this, desc, f]() -> decltype(f())
        {
          VERIFY_CORRECT_THREAD();
          activity_tracer aTracer(desc, *this);
          return f();
        };
        return fc::async(wrapper, desc, prio);
      }
      template<typename Functor>
      auto schedule_task(Functor&& f, const fc::time_point& t, const char* desc FC_TASK_NAME_DEFAULT_ARG, fc::priority prio = fc::priority()) -> fc::future<decltype(f())>
      {
        auto wrapper = [this, desc, f]() -> decltype(f())
        {
          VERIFY_CORRECT_THREAD();
          activity_tracer aTracer(desc, *this);
          return f();
        };

        return fc::schedule(wrapper, t, desc, prio);
      }

      void send_message_timing_to_statsd(peer_connection* originating_peer, const message& received_message, const message_hash_type& message_hash);
    }; // end class node_impl

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void node_impl_deleter::operator()(node_impl* impl_to_delete)
    {
#ifdef P2P_IN_DEDICATED_THREAD
      std::weak_ptr<fc::thread> weak_thread;
      if (impl_to_delete)
      {
        std::shared_ptr<fc::thread> impl_thread(impl_to_delete->_thread);
        weak_thread = impl_thread;
        impl_thread->async([impl_to_delete](){ delete impl_to_delete; }, "delete node_impl").wait();
        dlog("deleting the p2p thread");
      }
      if (weak_thread.expired())
        dlog("done deleting the p2p thread");
      else
        dlog("failed to delete the p2p thread, we must be leaking a smart pointer somewhere");
#else // P2P_IN_DEDICATED_THREAD
      delete impl_to_delete;
#endif // P2P_IN_DEDICATED_THREAD
    }

    node_impl::node_impl(const std::string& user_agent, appbase::application& app, hive::chain::blockchain_worker_thread_pool& thread_pool ) :
#ifdef P2P_IN_DEDICATED_THREAD
      _thread(std::make_shared<fc::thread>("p2p")),
#endif // P2P_IN_DEDICATED_THREAD
      _delegate(nullptr),
      _is_firewalled(firewalled_state::unknown),
      _potential_peer_database_updated(false),
      _sync_items_to_fetch_updated(false),
      _suspend_fetching_sync_blocks(false),
      _items_to_fetch_updated(false),
      _items_to_fetch_sequence_counter(0),
      _recent_block_interval_in_seconds(HIVE_BLOCK_INTERVAL),
      _user_agent_string(user_agent),
      _most_recent_blocks_accepted(GRAPHENE_NET_DEFAULT_MAX_CONNECTIONS),
      _total_number_of_unfetched_items(0),
      _rate_limiter(0, 0),
      _last_reported_number_of_connections(0),
      _average_network_read_speed_seconds(60),
      _average_network_write_speed_seconds(60),
      _average_network_read_speed_minutes(60),
      _average_network_write_speed_minutes(60),
      _average_network_read_speed_hours(72),
      _average_network_write_speed_hours(72),
      _average_network_usage_second_counter(0),
      _average_network_usage_minute_counter(0),
      _node_is_shutting_down(false),
      _activeCalls(0),
      theApp( app ),
      _thread_pool( thread_pool )
    {
      _rate_limiter.set_actual_rate_time_constant(fc::seconds(2));
      fc::rand_bytes(&_node_id.data[0], (int)_node_id.size());

      _shutdownNotifier = fc::promise<void>::create("Node shutdown notifier");
    }

    node_impl::~node_impl()
    {
      VERIFY_CORRECT_THREAD();
      ilog( "cleaning up node" );
      _node_is_shutting_down = true;

      if(_activeCalls.load() != 0)
      {
        ilog("Waiting for node become inactive (on_message/on_connection_closed/get_message_for_item)");
        _shutdownNotifier->wait();
        ilog("Node became inactive");
      }

      try
      {
        ilog( "close" );
        close();
      }
      catch ( const fc::exception& e )
      {
        wlog( "unexpected exception on close ${e}", ("e", e) );
      }
      ilog( "done" );
    }

    void node_impl::save_node_configuration()
    {
      VERIFY_CORRECT_THREAD();
      if( fc::exists(_node_configuration_directory ) )
      {
        fc::path configuration_file_name( _node_configuration_directory / NODE_CONFIGURATION_FILENAME );
        try
        {
          fc::json::save_to_file( _node_configuration, configuration_file_name );
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch ( const fc::exception& except )
        {
          elog( "error writing node configuration to file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", except.to_detail_string() ) );
        }
      }
    }

    void node_impl::save_active_peers_to_peer_database()
    {
      for (const peer_connection_ptr& active_peer : _active_connections)
      {
        fc::optional<fc::ip::endpoint> endpoint_for_db = active_peer->get_endpoint_for_db();
        if (endpoint_for_db)
        {
          fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
          if (updated_peer_record)
          {
            updated_peer_record->last_seen_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_record);
          }
        }
      }
    }

    void node_impl::p2p_network_connect_loop()
    {
      while (!_p2p_network_connect_loop_done.canceled() && !node_is_shutting_down())
      {
        try
        {
          dlog("Starting an iteration of p2p_network_connect_loop().");
          display_current_connections();

          // add-once peers bypass our checks on the maximum/desired number of connections (but they will still be counted against the totals once they're connected)
          if (!_add_once_node_list.empty())
          {
            std::list<potential_peer_record> add_once_node_list;
            add_once_node_list.swap(_add_once_node_list);
            dlog("Processing \"add once\" node list containing ${count} peers:", ("count", add_once_node_list.size()));
            for (const potential_peer_record& add_once_peer : add_once_node_list)
            {
              dlog("    ${peer}", ("peer", add_once_peer.endpoint));
            }
            for (const potential_peer_record& add_once_peer : add_once_node_list)
            {
              // see if we have an existing connection to that peer.  If we do, disconnect them and
              // then try to connect the next time through the loop
              peer_connection_ptr existing_connection_ptr = get_connection_to_endpoint( add_once_peer.endpoint );
              if(!existing_connection_ptr)
                connect_to_endpoint(add_once_peer.endpoint);
            }
            dlog("Done processing \"add once\" node list");
          }

          while (is_wanting_new_connections())
          {
            bool initiated_connection_this_pass = false;
            _potential_peer_database_updated = false;

            for (peer_database::iterator iter = _potential_peer_db.begin();
                 iter != _potential_peer_db.end() && is_wanting_new_connections();
                 ++iter)
            {
              dlog("potential peer to connect to: ${iter}",("iter",*iter));
              fc::microseconds delay_until_retry = fc::seconds((iter->number_of_failed_connection_attempts + 1) * _node_configuration.peer_connection_retry_timeout);

              if (!is_connection_to_endpoint_in_progress(iter->endpoint) &&
                  ((iter->last_connection_disposition != last_connection_failed &&
                    iter->last_connection_disposition != last_connection_rejected &&
                    iter->last_connection_disposition != last_connection_handshaking_failed) ||
                   (fc::time_point::now() - iter->last_connection_attempt_time) > delay_until_retry))
              {
                connect_to_endpoint(iter->endpoint);
                initiated_connection_this_pass = true;
              }
            }

            if (!initiated_connection_this_pass && !_potential_peer_database_updated)
              break;
          } /// while (is_wanting_new_connections())

          display_current_connections();

          if(node_is_shutting_down())
          {
            ilog("Breaking p2p_network_connect_loop loop because node is shutting down");
            break;
          }
          // if we broke out of the while loop, that means either we have connected to enough nodes, or
          // we don't have any good candidates to connect to right now.
#if 0
          try
          {
            _retrigger_connect_loop_promise = fc::promise<void>::ptr( new fc::promise<void>("graphene::net::retrigger_connect_loop") );
            if( is_wanting_new_connections() || !_add_once_node_list.empty() )
            {
              if( is_wanting_new_connections() )
                dlog( "Still want to connect to more nodes, but I don't have any good candidates.  Trying again in 15 seconds" );
              else
                dlog( "I still have some \"add once\" nodes to connect to.  Trying again in 15 seconds" );
              _retrigger_connect_loop_promise->wait_until( fc::time_point::now() + fc::seconds(GRAPHENE_PEER_DATABASE_RETRY_DELAY ) );
            }
            else
            {
              dlog( "I don't need any more connections, waiting forever until something changes" );
              _retrigger_connect_loop_promise->wait();
            }
          }
          catch ( fc::timeout_exception& ) //intentionally not logged
          {
          }  // catch
#else
          fc::usleep(fc::seconds(10));
#endif
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          edump((e));
        }
      }// while(!canceled)
    }

    void node_impl::trigger_p2p_network_connect_loop()
    {
      VERIFY_CORRECT_THREAD();
      _potential_peer_database_updated = true; // if we are looking for peers, this says to retry the loop
#if 0 // disabled because it doesn't trigger the loop any more      
      if( _retrigger_connect_loop_promise )
        _retrigger_connect_loop_promise->set_value();
#endif
    }

    bool node_impl::have_already_received_sync_item(const item_hash_t& item_hash)
    {
      VERIFY_CORRECT_THREAD();
      return (_already_received_sync_item_ids.find(item_hash) != _already_received_sync_item_ids.end());
    }

    void node_impl::request_sync_item_from_peer(const peer_connection_ptr& peer, const item_hash_t& item_to_request)
    {
      VERIFY_CORRECT_THREAD();
      dlog("requesting item ${item_hash} from peer ${endpoint}", ("item_hash", item_to_request)("endpoint", peer->get_remote_endpoint()));
      item_id item_id_to_request(graphene::net::block_message_type, item_to_request);
      _active_sync_requests.insert(active_sync_requests_map::value_type(item_to_request, fc::time_point::now()));
      peer->last_sync_item_received_time = fc::time_point::now();
      peer->sync_items_requested_from_peer.insert(item_to_request);
      peer->send_message(fetch_items_message(item_id_to_request.item_type, std::vector<item_hash_t>{item_id_to_request.item_hash}));
    }

    void node_impl::request_sync_items_from_peer(const peer_connection_ptr& peer, const std::vector<item_hash_t>& items_to_request)
    {
      VERIFY_CORRECT_THREAD();
      dlog("requesting ${item_count} item(s) ${items_to_request} from peer ${endpoint}",
           ("item_count", items_to_request.size())("items_to_request", items_to_request)("endpoint", peer->get_remote_endpoint()));
      for (const item_hash_t& item_to_request : items_to_request)
      {
        _active_sync_requests.insert(active_sync_requests_map::value_type(item_to_request, fc::time_point::now()));
        peer->last_sync_item_received_time = fc::time_point::now();
        peer->sync_items_requested_from_peer.insert(item_to_request);
      }
      peer->send_message(fetch_items_message(graphene::net::block_message_type, items_to_request));
    }

    void node_impl::update_last_requested_block_number_for_peers_on_this_fork(uint32_t last_requested_block_number, const item_hash_t& last_requested_block_id)
    {
      for (const peer_connection_ptr& peer : _active_connections)
      {
        //if peer is on fork with requested_block_id (check if the two peers share the same hash at that block number)
        uint32_t id_index = last_requested_block_number - peer->first_id_block_number;
        if (id_index < peer->ids_of_items_to_get.size() && peer->ids_of_items_to_get[id_index] == last_requested_block_id)
        {
          if (last_requested_block_number > peer->last_requested_block_number_for_peers_on_this_fork)
            peer->last_requested_block_number_for_peers_on_this_fork = last_requested_block_number;
          else
            ddump((last_requested_block_number)(peer->last_requested_block_number_for_peers_on_this_fork));
        }
      }
    }

    void node_impl::fetch_sync_items_loop()
    {
      while (!_fetch_sync_items_loop_done.canceled())
      {
        _sync_items_to_fetch_updated = false;
        dlog("beginning another iteration of the sync items loop");

        uint32_t idle_peer_count = 0;
        if (!_suspend_fetching_sync_blocks)
        {
          std::map<peer_connection_ptr, std::vector<item_hash_t>> sync_item_requests_to_send;
          {
            ASSERT_TASK_NOT_PREEMPTED();
            std::set<item_hash_t> sync_items_to_request;

            // for each idle peer that we're syncing with
            for( const peer_connection_ptr& peer : _active_connections )
            {
              if (peer->inhibit_fetching_sync_blocks)
                dlog("Skipping peer ${peer} because we've inhibited fetching sync blocks from them.  idle: ${idle}, we_need_sync_items_from_peer: ${we_need_sync_items_from_peer}",
                     ("peer", peer->get_remote_endpoint())("idle", peer->idle())("we_need_sync_items_from_peer", peer->we_need_sync_items_from_peer));
              else if (peer->idle())
              {
                dlog("peer ${peer} is idle", ("peer", peer->get_remote_endpoint()));
                ++idle_peer_count;
                if (peer->we_need_sync_items_from_peer && !peer->ids_of_items_to_get.empty())
                {
                  assert(peer->first_id_block_number);
                  if (peer->last_requested_block_number_for_peers_on_this_fork < peer->first_id_block_number - 1)
                    peer->last_requested_block_number_for_peers_on_this_fork = peer->first_id_block_number - 1;
                  uint32_t first_to_get = peer->last_requested_block_number_for_peers_on_this_fork - peer->first_id_block_number + 1;
                  if (first_to_get < peer->ids_of_items_to_get.size())
                  {
                    // loop through the items peer has that we don't yet have on our blockchain
                    unsigned i = first_to_get;
                    for (; i < peer->ids_of_items_to_get.size(); ++i)
                    {
                      const item_hash_t& item_to_potentially_request = peer->ids_of_items_to_get[i];
                      // if we don't already have this item in our temporary storage and we haven't requested from another syncing peer
                      if(_active_sync_requests.find(item_to_potentially_request) == _active_sync_requests.end() && // we've requested it in a previous iteration and we're still waiting for it to arrive
                         !have_already_received_sync_item(item_to_potentially_request) && // already received it, but not yet removed from our list of peer items to get
                         sync_items_to_request.find(item_to_potentially_request) == sync_items_to_request.end()) // we have already decided to request it from another peer during this iteration
                      {
                        // then schedule a request from this peer
                        sync_item_requests_to_send[peer].push_back(item_to_potentially_request);
                        sync_items_to_request.insert(item_to_potentially_request);
                        if (sync_item_requests_to_send[peer].size() >= _node_configuration.maximum_blocks_per_peer_during_syncing)
                          break;
                      }
                    } //for each item to get
                    if (i == peer->ids_of_items_to_get.size()) //if we didn't find any items, we only searched one less than i
                      --i;
                    uint32_t last_searched_block_number = peer->first_id_block_number + i;
                    const item_hash_t& last_searched_item_id = peer->ids_of_items_to_get[i];
                    update_last_requested_block_number_for_peers_on_this_fork(last_searched_block_number, last_searched_item_id);
                    dlog("searched through ${count} ids from ${total_ids} available ids to find ${n} items to request", ("count", i - first_to_get + 1)("total_ids", peer->ids_of_items_to_get.size())("n", sync_item_requests_to_send[peer].size()));

                  } //if this peer has items we aren't currently asking for or already received
                } //if we need sync items from peer
              } //if peer idle
            } //for each active peer
          }// end non-preemptable section

          // make all the requests we scheduled in the loop above
          for( const auto& sync_item_request : sync_item_requests_to_send )
            request_sync_items_from_peer( sync_item_request.first, sync_item_request.second );
          sync_item_requests_to_send.clear();
        }
        else
          dlog("fetch_sync_items_loop is suspended pending backlog processing");

        if( !_sync_items_to_fetch_updated )
        {
          dlog( "idle_peer_count=${idle_peer_count}, can't request more sync items now, going to sleep",(idle_peer_count) );
          _retrigger_fetch_sync_items_loop_promise = fc::promise<void>::create("graphene::net::retrigger_fetch_sync_items_loop");
          _retrigger_fetch_sync_items_loop_promise->wait();
          _retrigger_fetch_sync_items_loop_promise.reset();
        }
      } // while( !canceled )
    }

    void node_impl::trigger_fetch_sync_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      dlog( "Triggering fetch sync items loop now" );
      //we set this so that we will do another iteration of fetch_sync_items even if we are suspended in the middle of a previous run
      _sync_items_to_fetch_updated = true;
      if( _retrigger_fetch_sync_items_loop_promise )
        _retrigger_fetch_sync_items_loop_promise->set_value();
    }

    bool node_impl::is_item_in_any_peers_inventory(const item_id& item) const
    {
      for( const peer_connection_ptr& peer : _active_connections )
      {
        if (peer->inventory_peer_advertised_to_us.find(item) != peer->inventory_peer_advertised_to_us.end() )
          return true;
      }
      return false;
    }

    void node_impl::fetch_items_loop()
    {
      while (!_fetch_item_loop_done.canceled())
      {
        _items_to_fetch_updated = false;
        dlog("beginning an iteration of fetch items (${count} items to fetch)",
             ("count", _items_to_fetch.size()));

        fc::time_point oldest_timestamp_to_fetch = fc::time_point::now() - fc::seconds(_recent_block_interval_in_seconds * GRAPHENE_NET_MESSAGE_CACHE_DURATION_IN_BLOCKS);
        fc::time_point next_peer_unblocked_time = fc::time_point::maximum();

        // we need to construct a list of items to request from each peer first,
        // then send the messages (in two steps, to avoid yielding while iterating)
        // we want to evenly distribute our requests among our peers.
        struct requested_item_count_index {};
        struct peer_and_items_to_fetch
        {
          peer_connection_ptr peer;
          std::vector<item_id> item_ids;
          peer_and_items_to_fetch(const peer_connection_ptr& peer) : peer(peer) {}
          bool operator<(const peer_and_items_to_fetch& rhs) const { return peer < rhs.peer; }
          size_t number_of_items() const { return item_ids.size(); }
        };
        typedef boost::multi_index_container<peer_and_items_to_fetch,
                                             boost::multi_index::indexed_by<boost::multi_index::ordered_unique<boost::multi_index::member<peer_and_items_to_fetch, peer_connection_ptr, &peer_and_items_to_fetch::peer> >,
                                                                            boost::multi_index::ordered_non_unique<boost::multi_index::tag<requested_item_count_index>,
                                                                                                                   boost::multi_index::const_mem_fun<peer_and_items_to_fetch, size_t, &peer_and_items_to_fetch::number_of_items> > > > fetch_messages_to_send_set;
        fetch_messages_to_send_set items_by_peer;

        // initialize the fetch_messages_to_send with an empty set of items for all idle peers
        for (const peer_connection_ptr& peer : _active_connections)
          if (peer->idle())
            items_by_peer.insert(peer_and_items_to_fetch(peer));

        // now loop over all items we want to fetch
        size_t expired_items = 0;
        for (auto item_iter = _items_to_fetch.begin(); item_iter != _items_to_fetch.end();)
        {
          if (item_iter->timestamp < oldest_timestamp_to_fetch)
          {
            // this item has probably already fallen out of our peers' caches, we'll just ignore it.
            // this can happen during flooding, and the _items_to_fetch could otherwise get clogged
            // with a bunch of items that we'll never be able to request from any peer
            dlog("Unable to fetch item ${item} before its likely expiration time, removing it from our list of items to fetch", ("item", item_iter->item));
            item_iter = _items_to_fetch.erase(item_iter);
            ++expired_items;
          }
          else
          {
            // find a peer that has it, we'll use the one who has the least requests going to it to load balance
            bool item_fetched = false;
            for (auto peer_iter = items_by_peer.get<requested_item_count_index>().begin(); peer_iter != items_by_peer.get<requested_item_count_index>().end(); ++peer_iter)
            {
              const peer_connection_ptr& peer = peer_iter->peer;
              // if they have the item and we haven't already decided to ask them for too many other items
              if (peer_iter->item_ids.size() < GRAPHENE_NET_MAX_ITEMS_PER_PEER_DURING_NORMAL_OPERATION &&
                  peer->inventory_peer_advertised_to_us.find(item_iter->item) != peer->inventory_peer_advertised_to_us.end())
              {
                if (item_iter->item.item_type == graphene::net::trx_message_type && peer->is_transaction_fetching_inhibited())
                  next_peer_unblocked_time = std::min(peer->transaction_fetching_inhibited_until, next_peer_unblocked_time);
                else
                {
                  //dlog("requesting item ${hash} from peer ${endpoint}",
                  //     ("hash", iter->item.item_hash)("endpoint", peer->get_remote_endpoint()));
                  item_id item_id_to_fetch = item_iter->item;
                  peer->items_requested_from_peer.insert(peer_connection::item_to_time_map_type::value_type(item_id_to_fetch, fc::time_point::now()));
                  item_iter = _items_to_fetch.erase(item_iter);
                  item_fetched = true;
                  items_by_peer.get<requested_item_count_index>().modify(peer_iter, [&item_id_to_fetch](peer_and_items_to_fetch& peer_and_items) {
                    peer_and_items.item_ids.push_back(item_id_to_fetch);
                  });
                  break;
                }
              }
            }
            if (!item_fetched)
              ++item_iter;
          }
        }
        if (expired_items > 0)
          wlog("Removed ${expired_items} items from the list to fetch due to expiration time.", (expired_items));

        // we've figured out which peer will be providing each item, now send the messages.
        for (const peer_and_items_to_fetch& peer_and_items : items_by_peer)
        {
          // the item lists are heterogenous and
          // the fetch_items_message can only deal with one item type at a time.
          std::map<uint32_t, std::vector<item_hash_t> > items_to_fetch_by_type;
          for (const item_id& item : peer_and_items.item_ids)
            items_to_fetch_by_type[item.item_type].push_back(item.item_hash);
          for (auto& items_by_type : items_to_fetch_by_type)
          {
            dlog("requesting ${count} items of type ${type} from peer ${endpoint}: ${hashes}",
                 ("count", items_by_type.second.size())("type", (uint32_t)items_by_type.first)
                 ("endpoint", peer_and_items.peer->get_remote_endpoint())
                 ("hashes", items_by_type.second));
            if (items_by_type.first == core_message_type_enum::block_message_type)
              for (const item_hash_t& id : items_by_type.second)
              {
                fc_dlog(fc::logger::get("sync"),
                        "requesting a block from peer ${endpoint} (message_id is ${id})",
                        ("endpoint", peer_and_items.peer->get_remote_endpoint())("id", id));
              }

            peer_and_items.peer->send_message(fetch_items_message(items_by_type.first,
                                                                  items_by_type.second));
          }
        }
        items_by_peer.clear();

        if (!_items_to_fetch_updated)
        {
          _retrigger_fetch_item_loop_promise = fc::promise<void>::create("graphene::net::retrigger_fetch_item_loop");
          fc::microseconds time_until_retrigger = fc::microseconds::maximum();
          if (next_peer_unblocked_time != fc::time_point::maximum())
            time_until_retrigger = next_peer_unblocked_time - fc::time_point::now();
          try
          {
            if (time_until_retrigger > fc::microseconds(0))
              _retrigger_fetch_item_loop_promise->wait(time_until_retrigger);
          }
          catch (const fc::timeout_exception&)
          {
            dlog("Resuming fetch_items_loop due to timeout -- one of our peers should no longer be throttled");
          }
          _retrigger_fetch_item_loop_promise.reset();
        }
      } // while (!canceled)
    }

    void node_impl::trigger_fetch_items_loop()
    {
      VERIFY_CORRECT_THREAD();
      _items_to_fetch_updated = true;
      if( _retrigger_fetch_item_loop_promise )
        _retrigger_fetch_item_loop_promise->set_value();
    }

    void node_impl::advertise_inventory_loop()
    {
      while (!_advertise_inventory_loop_done.canceled())
      {
        dlog("beginning an iteration of advertise inventory");
        // swap inventory into local variable, clearing the node's copy
        std::set<std::shared_ptr<full_block_type>> block_inventory_to_advertise;
        std::set<std::shared_ptr<full_transaction_type>> transaction_inventory_to_advertise;

        block_inventory_to_advertise.swap(_new_block_inventory);
        transaction_inventory_to_advertise.swap(_new_transaction_inventory);

        // process all inventory to advertise and construct the inventory messages we'll send
        // first, then send them all in a batch (to avoid any fiber interruption points while
        // we're computing the messages)
        std::list<std::pair<peer_connection_ptr, item_ids_inventory_message>> inventory_messages_to_send;

        for (const peer_connection_ptr& peer : _active_connections)
        {
          // only advertise to peers who are in sync with us
          //wdump((peer->peer_needs_sync_items_from_us));
          if (!peer->peer_needs_sync_items_from_us)
          {
            std::map<uint32_t, std::vector<item_hash_t>> items_to_advertise_by_type;
            // don't send the peer anything we've already advertised to it
            // or anything it has advertised to us
            // group the items we need to send by type, because we'll need to send one inventory message per type
            unsigned total_items_to_send_to_this_peer = 0;
            for (const std::shared_ptr<full_block_type>& full_block_to_advertise : block_inventory_to_advertise)
            {
              fc::ripemd160 item_hash_for_block = peer->advertise_blocks_by_block_id() ? full_block_to_advertise->get_block_id() 
                                                                                       : full_block_to_advertise->get_legacy_block_message_hash();
              item_id item_to_advertise{graphene::net::block_message_type, item_hash_for_block};

              if (peer->inventory_advertised_to_peer.find(item_to_advertise) == peer->inventory_advertised_to_peer.end() &&
                  peer->inventory_peer_advertised_to_us.find(item_to_advertise) == peer->inventory_peer_advertised_to_us.end())
              {
                items_to_advertise_by_type[item_to_advertise.item_type].push_back(item_to_advertise.item_hash);
                peer->inventory_advertised_to_peer.insert(peer_connection::timestamped_item_id(item_to_advertise, fc::time_point::now()));
                ++total_items_to_send_to_this_peer;
                dlog("advertising block ${id} to peer ${endpoint}", ("id", item_to_advertise.item_hash)("endpoint", peer->get_remote_endpoint()));
              }
            }
            for (const std::shared_ptr<full_transaction_type>& full_transaction_to_advertise : transaction_inventory_to_advertise)
            {
              item_id item_to_advertise{graphene::net::trx_message_type, full_transaction_to_advertise->get_legacy_transaction_message_hash()};

              if (peer->inventory_advertised_to_peer.find(item_to_advertise) == peer->inventory_advertised_to_peer.end() &&
                  peer->inventory_peer_advertised_to_us.find(item_to_advertise) == peer->inventory_peer_advertised_to_us.end())
              {
                items_to_advertise_by_type[item_to_advertise.item_type].push_back(item_to_advertise.item_hash);
                peer->inventory_advertised_to_peer.insert(peer_connection::timestamped_item_id(item_to_advertise, fc::time_point::now()));
                ++total_items_to_send_to_this_peer;
                testnetlog("advertising transaction ${id} to peer ${endpoint}", ("id", item_to_advertise.item_hash)("endpoint", peer->get_remote_endpoint()));
                dlog("advertising transaction ${id} to peer ${endpoint}", ("id", item_to_advertise.item_hash)("endpoint", peer->get_remote_endpoint()));
              }
            }
            dlog("advertising ${count} new item(s) of ${types} type(s) to peer ${endpoint}",
                 ("count", total_items_to_send_to_this_peer)("types", items_to_advertise_by_type.size())("endpoint", peer->get_remote_endpoint()));
            for (const auto& items_group : items_to_advertise_by_type)
              inventory_messages_to_send.push_back(std::make_pair(peer, item_ids_inventory_message(items_group.first, items_group.second)));
          }
          peer->clear_old_inventory();
        }

        for (auto iter = inventory_messages_to_send.begin(); iter != inventory_messages_to_send.end(); ++iter)
          iter->first->send_message(iter->second);
        inventory_messages_to_send.clear();

        if (_new_block_inventory.empty() && _new_transaction_inventory.empty())
        {
          _retrigger_advertise_inventory_loop_promise = fc::promise<void>::create("graphene::net::retrigger_advertise_inventory_loop");
          _retrigger_advertise_inventory_loop_promise->wait();
          _retrigger_advertise_inventory_loop_promise.reset();
        }
      } // while(!canceled)
    }

    void node_impl::trigger_advertise_inventory_loop()
    {
      VERIFY_CORRECT_THREAD();
      if( _retrigger_advertise_inventory_loop_promise )
        _retrigger_advertise_inventory_loop_promise->set_value();
    }

    //self-schedules every 1 second to look for peers that aren't responding well
    void node_impl::terminate_inactive_connections_loop()
    {
      dlog("checking if we can terminate inactive connections");
      std::list<peer_connection_ptr> peers_to_disconnect_gently;
      std::list<peer_connection_ptr> peers_to_disconnect_forcibly;
      std::list<peer_connection_ptr> peers_to_send_keep_alive;
      std::list<peer_connection_ptr> peers_to_terminate;
      uint32_t active_disconnect_timeout = 10 * _recent_block_interval_in_seconds; //peer must have sent msg within 10 blocks

      // Disconnect peers that haven't sent us any data recently
      // These numbers are just guesses and we need to think through how this works better.
      // If we and our peers get disconnected from the rest of the network, we will not
      // receive any blocks or transactions from the rest of the world, and that will
      // probably make us disconnect from our peers even though we have working connections to
      // them (but they won't have sent us anything since they aren't getting blocks either).
      // This might not be so bad because it could make us initiate more connections and
      // reconnect with the rest of the network, or it might just futher isolate us.
      {
        // As usual, the first step is to walk through all our peers and figure out which
        // peers need action (disconneting, sending keepalives, etc), then we walk through
        // those lists yielding at our leisure later.
        ASSERT_TASK_NOT_PREEMPTED();

        uint32_t handshaking_timeout = _node_configuration.peer_inactivity_timeout;
        fc::time_point handshaking_disconnect_threshold = fc::time_point::now() - fc::seconds(handshaking_timeout);
        for (const peer_connection_ptr& handshaking_peer : _handshaking_connections)
          if (handshaking_peer->connection_initiation_time < handshaking_disconnect_threshold &&
              handshaking_peer->get_last_message_received_time() < handshaking_disconnect_threshold &&
              handshaking_peer->get_last_message_sent_time() < handshaking_disconnect_threshold)
          {
            wlog("Forcibly disconnecting from handshaking peer ${peer} due to inactivity of at least ${handshaking_timeout} seconds",
                 ("peer", handshaking_peer->get_remote_endpoint())(handshaking_timeout));
            wlog("Peer's negotiating status: ${status}, bytes sent: ${sent}, bytes received: ${received}",
                 ("status", handshaking_peer->negotiation_status)
                 ("sent", handshaking_peer->get_total_bytes_sent())
                 ("received", handshaking_peer->get_total_bytes_received()));
            handshaking_peer->connection_closed_error = fc::exception(FC_LOG_MESSAGE(warn, "Terminating handshaking connection due to inactivity of ${timeout} seconds.  Negotiating status: ${status}, bytes sent: ${sent}, bytes received: ${received}",
                                                                                     ("peer", handshaking_peer->get_remote_endpoint())
                                                                                     ("timeout", handshaking_timeout)
                                                                                     ("status", handshaking_peer->negotiation_status)
                                                                                     ("sent", handshaking_peer->get_total_bytes_sent())
                                                                                     ("received", handshaking_peer->get_total_bytes_received())));
            peers_to_disconnect_forcibly.push_back(handshaking_peer);
          }

        uint32_t active_send_keepalive_timeout = active_disconnect_timeout / 2;

        // set the ignored request time out to 6 second.  When we request a block
        // or transaction from a peer, this timeout determines how long we wait for them
        // to reply before we give up and ask another peer for the item.
        // Ideally this should be significantly shorter than the block interval, because
        // we'd like to realize the block isn't coming and fetch it from a different
        // peer before the next block comes in.
        // Increased to 6 from 1 in #1660 due to heavy load. May need to adjust further
        fc::microseconds active_ignored_request_timeout = fc::microseconds( _node_configuration.active_ignored_request_timeout_microseconds );


        fc::time_point active_disconnect_threshold = fc::time_point::now() - fc::seconds(active_disconnect_timeout);
        fc::time_point active_send_keepalive_threshold = fc::time_point::now() - fc::seconds(active_send_keepalive_timeout);
        fc::time_point active_ignored_request_threshold = fc::time_point::now() - active_ignored_request_timeout;
        for( const peer_connection_ptr& active_peer : _active_connections )
        {
          if( active_peer->connection_initiation_time < active_disconnect_threshold &&
              active_peer->get_last_message_received_time() < active_disconnect_threshold )
          {
            wlog("Closing connection with peer ${peer} due to inactivity of at least ${timeout} seconds",
                 ("peer", active_peer->get_remote_endpoint())("timeout", active_disconnect_timeout));
            peers_to_disconnect_gently.push_back(active_peer);
          }
          else
          {
            bool disconnect_due_to_request_timeout = false;
            if (!active_peer->sync_items_requested_from_peer.empty() &&
                active_peer->last_sync_item_received_time < active_ignored_request_threshold)
            {
              fc_wlog(fc::logger::get("sync"),
                      "disconnecting peer ${peer} because they haven't made any progress on my remaining ${count} sync item requests",
                      ("peer", active_peer->get_remote_endpoint())("count", active_peer->sync_items_requested_from_peer.size()));
              wlog("Disconnecting peer ${peer} because they haven't made any progress on my remaining ${count} sync item requests",
                   ("peer", active_peer->get_remote_endpoint())("count", active_peer->sync_items_requested_from_peer.size()));

              active_peer->connection_closed_error = fc::exception(FC_LOG_MESSAGE(warn, "Disconnecting peer because they haven't made any progress on my remaining ${count} sync item requests", 
                                                                                  ("count", active_peer->sync_items_requested_from_peer.size())));
              disconnect_due_to_request_timeout = true;
            }
            if (!disconnect_due_to_request_timeout &&
                active_peer->item_ids_requested_from_peer &&
                active_peer->item_ids_requested_from_peer->get<1>() < active_ignored_request_threshold)
              {
                fc_wlog(fc::logger::get("sync"), "Disconnecting peer ${peer} because they didn't respond to my request for sync item ids after ${synopsis}",
                      ("peer", active_peer->get_remote_endpoint())
                      ("synopsis", active_peer->item_ids_requested_from_peer->get<0>()));
                wlog("Disconnecting peer ${peer} because they didn't respond to my request for sync item ids after ${synopsis}",
                      ("peer", active_peer->get_remote_endpoint())
                      ("synopsis", active_peer->item_ids_requested_from_peer->get<0>()));
                active_peer->connection_closed_error = fc::exception(FC_LOG_MESSAGE(warn, "Disconnecting peer because they didn't respond to my request for sync item ids after my synopsis"));
                disconnect_due_to_request_timeout = true;
              }
            if (!disconnect_due_to_request_timeout)
              for (const peer_connection::item_to_time_map_type::value_type& item_and_time : active_peer->items_requested_from_peer)
                if (item_and_time.second < active_ignored_request_threshold)
                {
                  fc_wlog(fc::logger::get("sync"), "Disconnecting peer ${peer} because they didn't respond to my request for item ${id}",
                        ("peer", active_peer->get_remote_endpoint())("id", item_and_time.first.item_hash));
                  wlog("Disconnecting peer ${peer} because they didn't respond to my request for item ${id}",
                        ("peer", active_peer->get_remote_endpoint())("id", item_and_time.first.item_hash));
                  active_peer->connection_closed_error = fc::exception(FC_LOG_MESSAGE(warn, "Disconnecting peer because they didn't respond to my request for item ${id}", ("id", item_and_time.first.item_hash)));
                  disconnect_due_to_request_timeout = true;
                  break;
                }
            if (disconnect_due_to_request_timeout)
            {
              // we should probably disconnect nicely and give them a reason, but right now the logic
              // for rescheduling the requests only executes when the connection is fully closed,
              // and we want to get those requests rescheduled as soon as possible
              peers_to_disconnect_forcibly.push_back(active_peer);
            }
            else if (active_peer->connection_initiation_time < active_send_keepalive_threshold) //if we have been connected to this peer for a while
            {
              //if it hasn't sent us a message in a while, send it a keep alive message
              if (active_peer->get_last_message_received_time() < active_send_keepalive_threshold)
              {
                wlog( "Sending a keepalive message to peer ${peer} who hasn't sent us any messages in the last ${timeout} seconds",
                      ( "peer", active_peer->get_remote_endpoint() )("timeout", active_send_keepalive_timeout ) );
                peers_to_send_keep_alive.push_back(active_peer);
              }
              else //it sent us a message recently, but we need sync data from it, yet we aren't processing or asking for new sync data, treat as stalled
                if (active_peer->we_need_sync_items_from_peer &&
                    active_peer->ids_of_items_being_processed.empty() && //we're not processing any blocks from this peer right now
                    !active_peer->is_currently_handling_message() && //we're not processing a message it sent us
                    !active_peer->item_ids_requested_from_peer && //not requesting any new ids
                    active_peer->ids_of_items_to_get.empty()) //all received ids have been sent to the blockchain
              {
                // This is a state we should never get into this condition except in a race, but if we do, we should disconnect the peer
                // to re-establish the connection.
                fc_wlog(fc::logger::get("sync"), "Disconnecting peer ${peer} because we think we need blocks from them but sync has stalled.",
                        ("peer", active_peer->get_remote_endpoint()));
                wlog("Disconnecting peer ${peer} because we think we need blocks from them but sync has stalled.",
                     ("peer", active_peer->get_remote_endpoint()));
                peers_to_disconnect_forcibly.push_back(active_peer);
              }
            }
          }
        }

        fc::time_point closing_disconnect_threshold = fc::time_point::now() - fc::seconds(GRAPHENE_NET_PEER_DISCONNECT_TIMEOUT);
        for (const peer_connection_ptr& closing_peer : _closing_connections)
          if (closing_peer->connection_closed_time < closing_disconnect_threshold)
          {
            // we asked this peer to close their connectoin to us at least GRAPHENE_NET_PEER_DISCONNECT_TIMEOUT
            // seconds ago, but they haven't done it yet.  Terminate the connection now
            wlog("Forcibly disconnecting peer ${peer} who failed to close their connection in a timely manner",
                 ("peer", closing_peer->get_remote_endpoint()));
            peers_to_disconnect_forcibly.push_back(closing_peer);
          }

        fc::time_point failed_terminate_threshold = fc::time_point::now() - fc::seconds(GRAPHENE_NET_FAILED_TERMINATE_TIMEOUT_SECONDS);
        for (const peer_connection_ptr& peer : _terminating_connections )
          if (peer->get_connection_terminated_time() != fc::time_point::min() &&
              peer->get_connection_terminated_time() < failed_terminate_threshold)
          {
            wlog("Terminating connection with peer ${peer}, closing the connection didn't work", ("peer", peer->get_remote_endpoint()));
            peers_to_terminate.push_back(peer);
          }

        // That's the end of the sorting step; now all peers that require further processing are now in one of the
        // lists peers_to_disconnect_gently,  peers_to_disconnect_forcibly, peers_to_send_keep_alive, or peers_to_terminate

        // if we've decided to delete any peers, do it now; in its current implementation this doesn't yield,
        // and once we start yielding, we may find that we've moved that peer to another list (closed or active)
        // and that triggers assertions, maybe even errors
        for (const peer_connection_ptr& peer : peers_to_terminate )
        {
          assert(_terminating_connections.find(peer) != _terminating_connections.end());
          _terminating_connections.erase(peer);
          schedule_peer_for_deletion(peer);
        }
        peers_to_terminate.clear();

        // if we're going to abruptly disconnect anyone, do it here
        // (it doesn't yield).  I don't think there would be any harm if this were
        // moved to the yielding section
        for( const peer_connection_ptr& peer : peers_to_disconnect_forcibly )
        {
          move_peer_to_terminating_list(peer);
          peer->close_connection();
        }
        peers_to_disconnect_forcibly.clear();
      } // end ASSERT_TASK_NOT_PREEMPTED()

      // Now process the peers that we need to do yielding functions with (disconnect sends a message with the
      // disconnect reason, so it may yield)
      for( const peer_connection_ptr& peer : peers_to_disconnect_gently )
      {
        fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Disconnecting due to inactivity",
                                                    ("last_message_received_seconds_ago", (peer->get_last_message_received_time() - fc::time_point::now()).count() / fc::seconds(1).count())
                                                    ("last_message_sent_seconds_ago", (peer->get_last_message_sent_time() - fc::time_point::now()).count() / fc::seconds(1).count())
                                                    (active_disconnect_timeout)));
        disconnect_from_peer(peer.get(), "Disconnecting due to inactivity", false, detailed_error);
      }
      peers_to_disconnect_gently.clear();

      for( const peer_connection_ptr& peer : peers_to_send_keep_alive )
        peer->send_message(current_time_request_message(),
                           offsetof(current_time_request_message, request_sent_time));
      peers_to_send_keep_alive.clear();

      if (!node_is_shutting_down() && !_terminate_inactive_connections_loop_done.canceled())
         _terminate_inactive_connections_loop_done = schedule_task( [this](){ terminate_inactive_connections_loop(); },
                                                                   fc::time_point::now() + fc::seconds(1),
                                                                   "terminate_inactive_connections_loop" );
    }

    void node_impl::fetch_updated_peer_lists_loop()
    {
      dlog("start fetch_updated_peer_lists_loop");
      std::list<peer_connection_ptr> original_active_peers(_active_connections.begin(), _active_connections.end());
      for( const peer_connection_ptr& active_peer : original_active_peers )
      {
        try
        {
          active_peer->send_message(address_request_message());
        }
        catch ( const fc::canceled_exception& )
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          dlog("Caught exception while sending address request message to peer ${peer} : ${e}",
               ("peer", active_peer->get_remote_endpoint())("e", e));
        }
      }

      // this has nothing to do with updating the peer list, but we need to prune this list
      // at regular intervals, this is a fine place to do it.
      fc::time_point_sec oldest_failed_ids_to_keep(fc::time_point::now() - fc::minutes(GRAPHENE_NET_PRUNE_FAILED_IDS_MINUTES));
      auto oldest_failed_ids_to_keep_iter = _recently_failed_items.get<peer_connection::timestamp_index>().lower_bound(oldest_failed_ids_to_keep);
      auto begin_iter = _recently_failed_items.get<peer_connection::timestamp_index>().begin();
      _recently_failed_items.get<peer_connection::timestamp_index>().erase(begin_iter, oldest_failed_ids_to_keep_iter);

      if (!node_is_shutting_down() && !_fetch_updated_peer_lists_loop_done.canceled() )
         _fetch_updated_peer_lists_loop_done = schedule_task( [this](){ fetch_updated_peer_lists_loop(); },
                                                             fc::time_point::now() + fc::minutes(GRAPHENE_NET_FETCH_UPDATED_PEER_LISTS_INTERVAL_MINUTES),
                                                             "fetch_updated_peer_lists_loop" );
      dlog("done fetch_updated_peer_lists_loop (we're not really a loop)");
    }

    void node_impl::update_bandwidth_data(uint32_t bytes_read_this_second, uint32_t bytes_written_this_second)
    {
      VERIFY_CORRECT_THREAD();
      _average_network_read_speed_seconds.push_back(bytes_read_this_second);
      _average_network_write_speed_seconds.push_back(bytes_written_this_second);
      ++_average_network_usage_second_counter;
      if (_average_network_usage_second_counter >= 60)
      {
        _average_network_usage_second_counter = 0;
        ++_average_network_usage_minute_counter;
        uint32_t average_read_this_minute = (uint32_t)boost::accumulate(_average_network_read_speed_seconds, uint64_t(0)) / (uint32_t)_average_network_read_speed_seconds.size();
        _average_network_read_speed_minutes.push_back(average_read_this_minute);
        uint32_t average_written_this_minute = (uint32_t)boost::accumulate(_average_network_write_speed_seconds, uint64_t(0)) / (uint32_t)_average_network_write_speed_seconds.size();
        _average_network_write_speed_minutes.push_back(average_written_this_minute);
        if (_average_network_usage_minute_counter >= 60)
        {
          _average_network_usage_minute_counter = 0;
          uint32_t average_read_this_hour = (uint32_t)boost::accumulate(_average_network_read_speed_minutes, uint64_t(0)) / (uint32_t)_average_network_read_speed_minutes.size();
          _average_network_read_speed_hours.push_back(average_read_this_hour);
          uint32_t average_written_this_hour = (uint32_t)boost::accumulate(_average_network_write_speed_minutes, uint64_t(0)) / (uint32_t)_average_network_write_speed_minutes.size();
          _average_network_write_speed_hours.push_back(average_written_this_hour);
        }
      }
    }
    void node_impl::bandwidth_monitor_loop()
    {
      dlog("start bandwidth_monitor_loop");
      fc::time_point_sec current_time = fc::time_point::now();

      if (_bandwidth_monitor_last_update_time == fc::time_point_sec::min())
        _bandwidth_monitor_last_update_time = current_time;

      uint32_t seconds_since_last_update = current_time.sec_since_epoch() - _bandwidth_monitor_last_update_time.sec_since_epoch();
      seconds_since_last_update = std::max(UINT32_C(1), seconds_since_last_update);
      uint32_t bytes_read_this_second = _rate_limiter.get_actual_download_rate();
      uint32_t bytes_written_this_second = _rate_limiter.get_actual_upload_rate();
      for (uint32_t i = 0; i < seconds_since_last_update - 1; ++i)
        update_bandwidth_data(0, 0);
      update_bandwidth_data(bytes_read_this_second, bytes_written_this_second);
      _bandwidth_monitor_last_update_time = current_time;

      if (!node_is_shutting_down() && !_bandwidth_monitor_loop_done.canceled())
        _bandwidth_monitor_loop_done = schedule_task( [=](){ bandwidth_monitor_loop(); },
                                                     fc::time_point::now() + fc::seconds(GRAPHENE_NET_BANDWIDTH_MONITOR_INTERVAL_SECONDS),
                                                     "bandwidth_monitor_loop" );
      dlog("end bandwidth_monitor_loop (not a loop)");
    }

    void node_impl::dump_node_status_task()
    {
      dump_node_status();
      if (!node_is_shutting_down() && !_dump_node_status_task_done.canceled())
        _dump_node_status_task_done = schedule_task([=](){ dump_node_status_task(); },
                                                   fc::time_point::now() + fc::minutes(GRAPHENE_NET_DUMP_NODE_STATUS_INTERVAL_MINUTES),
                                                   "dump_node_status_task");
    }

    void node_impl::delayed_peer_deletion_task()
    {
      dlog("in delayed_peer_deletion_task with ${count} in queue", ("count", _peers_to_delete.size()));
#ifdef USE_PEERS_TO_DELETE_MUTEX
      fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
      _peers_to_delete.clear();
      dlog("_peers_to_delete cleared");
#else
      while (!_peers_to_delete.empty())
      {
        std::list<peer_connection_ptr> peers_to_delete_copy;
        dlog("beginning an iteration of delayed_peer_deletion_task with ${count} in queue", ("count", _peers_to_delete.size()));
        peers_to_delete_copy.swap(_peers_to_delete);
      }
      dlog("leaving delayed_peer_deletion_task");
#endif
    }

    void node_impl::schedule_peer_for_deletion(const peer_connection_ptr& peer_to_delete)
    {
      VERIFY_CORRECT_THREAD();

      assert(_handshaking_connections.find(peer_to_delete) == _handshaking_connections.end());
      assert(_active_connections.find(peer_to_delete) == _active_connections.end());
      assert(_closing_connections.find(peer_to_delete) == _closing_connections.end());
      assert(_terminating_connections.find(peer_to_delete) == _terminating_connections.end());

#ifdef USE_PEERS_TO_DELETE_MUTEX
      dlog("scheduling peer for deletion: ${peer} (may block on a mutex here)", ("peer", peer_to_delete->get_remote_endpoint()));

      unsigned number_of_peers_to_delete;
      {
        fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
        _peers_to_delete.emplace_back(peer_to_delete);
        number_of_peers_to_delete = _peers_to_delete.size();
      }
      dlog("peer scheduled for deletion: ${peer}", ("peer", peer_to_delete->get_remote_endpoint()));

      if (!node_is_shutting_down() &&
          (!_delayed_peer_deletion_task_done.valid() || _delayed_peer_deletion_task_done.ready()))
      {
        dlog("asyncing delayed_peer_deletion_task to delete ${size} peers", ("size", number_of_peers_to_delete));
        _delayed_peer_deletion_task_done = async_task([this](){ delayed_peer_deletion_task(); }, "delayed_peer_deletion_task" );
    }
      else
        dlog("delayed_peer_deletion_task is already scheduled (current size of _peers_to_delete is ${size})", ("size", number_of_peers_to_delete));
#else
      dlog("scheduling peer for deletion: ${peer} (this will not block)", ("peer", peer_to_delete->get_remote_endpoint()));
      _peers_to_delete.push_back(peer_to_delete);
      if (!node_is_shutting_down() &&
          (!_delayed_peer_deletion_task_done.valid() || _delayed_peer_deletion_task_done.ready()))
      {
        dlog("asyncing delayed_peer_deletion_task to delete ${size} peers", ("size", _peers_to_delete.size()));
        _delayed_peer_deletion_task_done = async_task([this](){ delayed_peer_deletion_task(); }, "delayed_peer_deletion_task" );
      }
      else
        dlog("delayed_peer_deletion_task is already scheduled (current size of _peers_to_delete is ${size})", ("size", _peers_to_delete.size()));

#endif
    }

    bool node_impl::is_accepting_new_connections()
    {
      VERIFY_CORRECT_THREAD();
      return !node_is_shutting_down() && (!_p2p_network_connect_loop_done.valid() || !_p2p_network_connect_loop_done.canceled()) &&
         get_number_of_connections() <= _node_configuration.maximum_number_of_connections;
    }

    bool node_impl::is_wanting_new_connections()
    {
      VERIFY_CORRECT_THREAD();
      return !node_is_shutting_down() && !_p2p_network_connect_loop_done.canceled() &&
         get_number_of_connections() < _node_configuration.desired_number_of_connections;
    }

    uint32_t node_impl::get_number_of_connections()
    {
      VERIFY_CORRECT_THREAD();
      return (uint32_t)(_handshaking_connections.size() + _active_connections.size());
    }

    peer_connection_ptr node_impl::get_peer_by_node_id(const node_id_t& node_id)
    {
      for (const peer_connection_ptr& active_peer : _active_connections)
        if (node_id == active_peer->node_id)
          return active_peer;
      for (const peer_connection_ptr& handshaking_peer : _handshaking_connections)
        if (node_id == handshaking_peer->node_id)
          return handshaking_peer;
      return peer_connection_ptr();
    }

    bool node_impl::is_already_connected_to_id(const node_id_t& node_id)
    {
      VERIFY_CORRECT_THREAD();
      if (node_id == _node_id)
      {
        dlog("is_already_connected_to_id returning true because the peer is us");
        return true;
      }
      for (const peer_connection_ptr& active_peer : _active_connections)
        if (node_id == active_peer->node_id)
        {
          dlog("is_already_connected_to_id returning true because the peer is already in our active list");
          return true;
        }
      for (const peer_connection_ptr& handshaking_peer : _handshaking_connections)
        if (node_id == handshaking_peer->node_id)
        {
          dlog("is_already_connected_to_id returning true because the peer is already in our handshaking list");
          return true;
        }
      return false;
    }

    // merge addresses received from a peer into our database
    bool node_impl::merge_address_info_with_potential_peer_database(const std::vector<address_info> addresses)
    {
      VERIFY_CORRECT_THREAD();
      bool new_information_received = false;
      for (const address_info& address : addresses)
      {
        if (address.firewalled == graphene::net::firewalled_state::not_firewalled)
        {
          potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(address.remote_endpoint);
          if (address.last_seen_time > updated_peer_record.last_seen_time)
            new_information_received = true;
          updated_peer_record.last_seen_time = std::max(address.last_seen_time, updated_peer_record.last_seen_time);
          _potential_peer_db.update_entry(updated_peer_record);
        }
      }
      return new_information_received;
    }

    void node_impl::display_current_connections()
    {
      VERIFY_CORRECT_THREAD();
      dlog("Currently have ${current} of [${desired}/${max}] connections",
           ("current", get_number_of_connections())
           ("desired", _node_configuration.desired_number_of_connections)
           ("max", _node_configuration.maximum_number_of_connections));
      dlog("   my id is ${id}", ("id", _node_id));

      for (const peer_connection_ptr& active_connection : _active_connections)
      {
        dlog("        active: ${endpoint} with ${id}   [${direction}]",
             ("endpoint", active_connection->get_remote_endpoint())
             ("id", active_connection->node_id)
             ("direction", active_connection->direction));
      }
      for (const peer_connection_ptr& handshaking_connection : _handshaking_connections)
      {
        dlog("   handshaking: ${endpoint} with ${id}  [${direction}]",
             ("endpoint", handshaking_connection->get_remote_endpoint())
             ("id", handshaking_connection->node_id)
             ("direction", handshaking_connection->direction));
      }
    }

    void node_impl::on_message(peer_connection* originating_peer, const message& received_message)
    {
      VERIFY_CORRECT_THREAD();

      activity_tracer aTracer(__FUNCTION__, *this);

      message_hash_type message_hash = received_message.id();
      send_message_timing_to_statsd( originating_peer, received_message, message_hash );
      dlog("handling message ${type} ${hash} size ${size} from peer ${endpoint}",
           ("type", graphene::net::core_message_type_enum(received_message.msg_type))("hash", message_hash)
           ("size", received_message.size)
           ("endpoint", originating_peer->get_remote_endpoint()));
      switch (received_message.msg_type)
      {
      case core_message_type_enum::hello_message_type:
        on_hello_message(originating_peer, received_message.as<hello_message>());
        break;
      case core_message_type_enum::connection_accepted_message_type:
        on_connection_accepted_message(originating_peer, received_message.as<connection_accepted_message>());
        break;
      case core_message_type_enum::connection_rejected_message_type:
        on_connection_rejected_message(originating_peer, received_message.as<connection_rejected_message>());
        break;
      case core_message_type_enum::address_request_message_type:
        on_address_request_message(originating_peer, received_message.as<address_request_message>());
        break;
      case core_message_type_enum::address_message_type:
        on_address_message(originating_peer, received_message.as<address_message>());
        break;
      case core_message_type_enum::fetch_blockchain_item_ids_message_type:
        on_fetch_blockchain_item_ids_message(originating_peer, received_message.as<fetch_blockchain_item_ids_message>());
        break;
      case core_message_type_enum::blockchain_item_ids_inventory_message_type:
        on_blockchain_item_ids_inventory_message(originating_peer, received_message.as<blockchain_item_ids_inventory_message>());
        break;
      case core_message_type_enum::fetch_items_message_type:
        on_fetch_items_message(originating_peer, received_message.as<fetch_items_message>());
        break;
      case core_message_type_enum::item_not_available_message_type:
        on_item_not_available_message(originating_peer, received_message.as<item_not_available_message>());
        break;
      case core_message_type_enum::item_ids_inventory_message_type:
        on_item_ids_inventory_message(originating_peer, received_message.as<item_ids_inventory_message>());
        break;
      case core_message_type_enum::closing_connection_message_type:
        on_closing_connection_message(originating_peer, received_message.as<closing_connection_message>());
        break;
      case core_message_type_enum::block_message_type:
      case core_message_type_enum::compressed_block_message_type:
        fc::async( [=]() { process_block_message(originating_peer, received_message, message_hash); }, "process_block_msg");
        break;
      case core_message_type_enum::trx_message_type:
        process_trx_message(originating_peer, received_message.as_trx_message( _thread_pool ));
        break;
      case core_message_type_enum::current_time_request_message_type:
        on_current_time_request_message(originating_peer, received_message.as<current_time_request_message>());
        break;
      case core_message_type_enum::current_time_reply_message_type:
        on_current_time_reply_message(originating_peer, received_message.as<current_time_reply_message>());
        break;
      case core_message_type_enum::check_firewall_message_type:
        on_check_firewall_message(originating_peer, received_message.as<check_firewall_message>());
        break;
      case core_message_type_enum::check_firewall_reply_message_type:
        on_check_firewall_reply_message(originating_peer, received_message.as<check_firewall_reply_message>());
        break;
      case core_message_type_enum::get_current_connections_request_message_type:
        on_get_current_connections_request_message(originating_peer, received_message.as<get_current_connections_request_message>());
        break;
      case core_message_type_enum::get_current_connections_reply_message_type:
        on_get_current_connections_reply_message(originating_peer, received_message.as<get_current_connections_reply_message>());
        break;

      default:
        // ignore any message in between core_message_type_first and _last that we don't handle above
        // to allow us to add messages in the future
        if (received_message.msg_type < core_message_type_enum::core_message_type_first ||
            received_message.msg_type > core_message_type_enum::core_message_type_last)
          fc::async([=](){ process_ordinary_message(originating_peer, received_message, message_hash); }, "process_ord_msg");
        break;
      }
    }

    uint32_t get_block_number(const item_hash_t& id)
    {
      return fc::endian_reverse_u32(id._hash[0]);
    }


    fc::variant_object node_impl::generate_hello_user_data()
    {
      VERIFY_CORRECT_THREAD();
      // for the time being, shoehorn a bunch of properties into the user_data variant object,
      // which lets us add and remove fields without changing the protocol.  Once we
      // settle on what we really want in there, we'll likely promote them to first
      // class fields in the hello message
      fc::mutable_variant_object user_data;
      user_data["fc_git_revision_sha"] = fc::git_revision_sha;
      user_data["fc_git_revision_unix_timestamp"] = fc::git_revision_unix_timestamp;
#if defined( __APPLE__ )
      user_data["platform"] = "osx";
#elif defined( __linux__ )
      user_data["platform"] = "linux";
#elif defined( _MSC_VER )
      user_data["platform"] = "win32";
#else
      user_data["platform"] = "other";
#endif
      user_data["bitness"] = sizeof(void*) * 8;

      user_data["node_id"] = _node_id;

      item_hash_t head_block_id = _delegate->get_head_block_id();
      user_data["last_known_block_hash"] = head_block_id;
      user_data["last_known_block_number"] = _delegate->get_block_number(head_block_id);
      user_data["last_known_block_time"] = _delegate->get_block_time(head_block_id);

      if (!_hard_fork_block_numbers.empty())
        user_data["last_known_fork_block_number"] = _hard_fork_block_numbers.back();

      user_data["chain_id"] = _delegate->get_new_chain_id();
      user_data["last_available_zstd_compression_dictionary_number"] = hive::chain::get_last_available_zstd_compression_dictionary_number();

      return user_data;
    }
    void node_impl::parse_hello_user_data_for_peer(peer_connection* originating_peer, const fc::variant_object& user_data)
    {
      VERIFY_CORRECT_THREAD();
      // try to parse data out of the user_agent string
      if (user_data.contains("graphene_git_revision_sha"))
        originating_peer->graphene_git_revision_sha = user_data["graphene_git_revision_sha"].as_string();
      if (user_data.contains("graphene_git_revision_unix_timestamp"))
        originating_peer->graphene_git_revision_unix_timestamp = fc::time_point_sec(user_data["graphene_git_revision_unix_timestamp"].as<uint32_t>());
      if (user_data.contains("fc_git_revision_sha"))
        originating_peer->fc_git_revision_sha = user_data["fc_git_revision_sha"].as_string();
      if (user_data.contains("fc_git_revision_unix_timestamp"))
        originating_peer->fc_git_revision_unix_timestamp = fc::time_point_sec(user_data["fc_git_revision_unix_timestamp"].as<uint32_t>());
      if (user_data.contains("platform"))
        originating_peer->platform = user_data["platform"].as_string();
      if (user_data.contains("bitness"))
        originating_peer->bitness = user_data["bitness"].as<uint32_t>();
      if (user_data.contains("node_id"))
        originating_peer->node_id = user_data["node_id"].as<node_id_t>();
      if (user_data.contains("last_known_fork_block_number"))
        originating_peer->last_known_fork_block_number = user_data["last_known_fork_block_number"].as<uint32_t>();
      if (user_data.contains("chain_id"))
        originating_peer->chain_id = user_data["chain_id"].as<hive::protocol::chain_id_type>();
      if (user_data.contains("last_available_zstd_compression_dictionary_number"))
        originating_peer->last_available_zstd_compression_dictionary_number = user_data["last_available_zstd_compression_dictionary_number"].as<uint8_t>();
    }

    void node_impl::on_hello_message( peer_connection* originating_peer, const hello_message& hello_message_received )
    {
      VERIFY_CORRECT_THREAD();
      // this already_connected check must come before we fill in peer data below
      node_id_t peer_node_id = hello_message_received.node_public_key;
      try
      {
        peer_node_id = hello_message_received.user_data["node_id"].as<node_id_t>();
      }
      catch (const fc::exception&)
      {
        // either it's not there or it's not a valid session id.  either way, ignore.
      }
      bool already_connected_to_this_peer = is_already_connected_to_id(peer_node_id);

      // validate the node id
      fc::sha256::encoder shared_secret_encoder;
      fc::sha512 shared_secret = originating_peer->get_shared_secret();
      shared_secret_encoder.write(shared_secret.data(), sizeof(shared_secret));
      fc::ecc::public_key expected_node_public_key(hello_message_received.signed_shared_secret, shared_secret_encoder.result());

      // store off the data provided in the hello message
      originating_peer->user_agent = hello_message_received.user_agent;
      originating_peer->node_public_key = hello_message_received.node_public_key;
      originating_peer->node_id = hello_message_received.node_public_key; // will probably be overwritten in parse_hello_user_data_for_peer()
      originating_peer->core_protocol_version = hello_message_received.core_protocol_version;
      originating_peer->inbound_address = hello_message_received.inbound_address;
      originating_peer->inbound_port = hello_message_received.inbound_port;
      originating_peer->outbound_port = hello_message_received.outbound_port;

      parse_hello_user_data_for_peer(originating_peer, hello_message_received.user_data);

      // if they didn't provide a last known fork, try to guess it
      if (originating_peer->last_known_fork_block_number == 0 &&
          originating_peer->graphene_git_revision_unix_timestamp)
      {
        uint32_t unix_timestamp = originating_peer->graphene_git_revision_unix_timestamp->sec_since_epoch();
        originating_peer->last_known_fork_block_number = _delegate->estimate_last_known_fork_from_git_revision_timestamp(unix_timestamp);
      }

      // now decide what to do with it
      if (originating_peer->their_state == peer_connection::their_connection_state::just_connected)
      {
        if (hello_message_received.node_public_key != expected_node_public_key.serialize())
        {
          wlog("Invalid signature in hello message from peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          std::string rejection_message("Invalid signature in hello message");
          connection_rejected_message connection_rejected(_user_agent_string, core_protocol_version,
                                                          originating_peer->get_socket().remote_endpoint(),
                                                          rejection_reason_code::invalid_hello_message,
                                                          rejection_message);

          originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
          originating_peer->send_message( message(connection_rejected ) );
          // for this type of message, we're immediately disconnecting this peer
          fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Invalid signature in hello message"));
          disconnect_from_peer(originating_peer, "Invalid signature in hello message", true, detailed_error);
          return;
        }
        if (originating_peer->last_known_fork_block_number != 0)
        {
          uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(originating_peer->last_known_fork_block_number);
          if (next_fork_block_number != 0)
          {
            // we know about a fork they don't.  See if we've already passed that block.  If we have, don't let them
            // connect because we won't be able to give them anything useful
            uint32_t head_block_num = _delegate->get_block_number(_delegate->get_head_block_id());
            if (next_fork_block_number < head_block_num)
            {
#ifdef ENABLE_DEBUG_ULOGS
              ulog("Rejecting connection from peer because their version is too old.  Their version date: ${date}", ("date", originating_peer->graphene_git_revision_unix_timestamp));
#endif
              wlog("Received hello message from peer running a version of that can only understand blocks up to #${their_hard_fork}, but I'm at head block number #${my_block_number}",
                   ("their_hard_fork", next_fork_block_number)("my_block_number", head_block_num));
              std::ostringstream rejection_message;
              rejection_message << "Your client is outdated -- you can only understand blocks up to #" << next_fork_block_number << ", but I'm already on block #" << head_block_num;
              connection_rejected_message connection_rejected(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              rejection_reason_code::unspecified,
                                                              rejection_message.str() );

              originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
              originating_peer->send_message(message(connection_rejected));
              // for this type of message, we're immediately disconnecting this peer, instead of trying to
              // allowing her to ask us for peers (any of our peers will be on the same chain as us, so there's no
              // benefit of sharing them)
              fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Your client is too old, please upgrade"));
              disconnect_from_peer(originating_peer, "Your client is too old, please upgrade", true, detailed_error);
              return;
            }
          }
        }
        if ( !originating_peer->chain_id ||
              ( ( *originating_peer->chain_id != _delegate->get_old_chain_id() ) && ( *originating_peer->chain_id != _delegate->get_new_chain_id() ) ) )
        {
            wlog("Received hello message from peer running a node for different blockchain. Old chain-id: '${old_chain_id}'. New chain-id: '${new_chain_id}'. Their chain-id: '${their_chain_id}'.",
               ("old_chain_id", _delegate->get_old_chain_id())("new_chain_id", _delegate->get_new_chain_id())("their_chain_id", originating_peer->chain_id) );

            std::ostringstream rejection_message;
            rejection_message << "Your client is running a different chain id";
            connection_rejected_message connection_rejected(_user_agent_string, core_protocol_version,
                                                            originating_peer->get_socket().remote_endpoint(),
                                                            rejection_reason_code::different_chain,
                                                            rejection_message.str() );

            originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
            originating_peer->send_message( message( connection_rejected ) );
            // for this type of message, we're immediately disconnecting this peer, instead of trying to
            // allowing her to ask us for peers (any of our peers will be on the same chain as us, so there's no
            // benefit of sharing them)
            fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Your client is on a different chain, please specify different seed nodes"));
            disconnect_from_peer(originating_peer, "Your client is on a different chain, please specify different seed nodes", true, detailed_error);
            return;
        }
        if (already_connected_to_this_peer)
        {

          connection_rejected_message connection_rejected;
          if (_node_id == originating_peer->node_id)
            connection_rejected = connection_rejected_message(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              rejection_reason_code::connected_to_self,
                                                              "I'm connecting to myself");
          else
            connection_rejected = connection_rejected_message(_user_agent_string, core_protocol_version,
                                                              originating_peer->get_socket().remote_endpoint(),
                                                              rejection_reason_code::already_connected,
                                                              "I'm already connected to you");
          originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
          originating_peer->send_message(message(connection_rejected));
          wlog("Received a hello_message from peer ${peer} that I'm already connected to (with id ${id}), rejection",
               ("peer", originating_peer->get_remote_endpoint())
               ("id", originating_peer->node_id));
        }
#ifdef ENABLE_P2P_DEBUGGING_API
        else if(!_allowed_peers.empty() &&
                _allowed_peers.find(originating_peer->node_id) == _allowed_peers.end())
        {
          connection_rejected_message connection_rejected(_user_agent_string, core_protocol_version,
                                                          originating_peer->get_socket().remote_endpoint(),
                                                          rejection_reason_code::blocked,
                                                          "you are not in my allowed_peers list");
          originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
          originating_peer->send_message( message(connection_rejected ) );
          wlog( "Received a hello_message from peer ${peer} who isn't in my allowed_peers list, rejection", ("peer", originating_peer->get_remote_endpoint() ) );
        }
#endif // ENABLE_P2P_DEBUGGING_API
        else
        {
          // whether we're planning on accepting them as a peer or not, they seem to be a valid node,
          // so add them to our database if they're not firewalled

          // in the hello message, the peer sent us the IP address and port it thought it was connecting from.
          // If they match the IP and port we see, we assume that they're actually on the internet and they're not
          // firewalled.
          fc::ip::endpoint peers_actual_outbound_endpoint = originating_peer->get_socket().remote_endpoint();
          if( peers_actual_outbound_endpoint.get_address() == originating_peer->inbound_address &&
              peers_actual_outbound_endpoint.port() == originating_peer->outbound_port )
          {
            if( originating_peer->inbound_port == 0 )
            {
              dlog( "peer does not appear to be firewalled, but they did not give an inbound port so I'm treating them as if they are." );
              originating_peer->is_firewalled = firewalled_state::firewalled;
            }
            else
            {
              // peer is not firewalled, add it to our database
              fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
              if (endpoint_for_db)
              {
                potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(*endpoint_for_db);
                _potential_peer_db.update_entry(updated_peer_record);
              }
              originating_peer->is_firewalled = firewalled_state::not_firewalled;
            }
          }
          else
          {
            dlog("peer firewall state unknown: they think their outbound endpoint is ${reported_endpoint}, but I see it as ${actual_endpoint}",
                 ("reported_endpoint", fc::ip::endpoint(originating_peer->inbound_address, originating_peer->outbound_port))
                 ("actual_endpoint", peers_actual_outbound_endpoint));
            originating_peer->is_firewalled = firewalled_state::unknown;
          }

          if (!is_accepting_new_connections())
          {
            connection_rejected_message connection_rejected(_user_agent_string, core_protocol_version,
                                                            originating_peer->get_socket().remote_endpoint(),
                                                            rejection_reason_code::not_accepting_connections,
                                                            "not accepting any more incoming connections");
            originating_peer->their_state = peer_connection::their_connection_state::connection_rejected;
            originating_peer->send_message(message(connection_rejected));
            wlog("Received a hello_message from peer ${peer}, but I'm not accepting any more connections, rejection",
                 ("peer", originating_peer->get_remote_endpoint()));
          }
          else
          {
            originating_peer->their_state = peer_connection::their_connection_state::connection_accepted;
            originating_peer->send_message(message(connection_accepted_message()));
            dlog("Received a hello_message from peer ${peer}, sending reply to accept connection",
                 ("peer", originating_peer->get_remote_endpoint()));
          }
        }
      }
      else
      {
        // we can wind up here if we've connected to ourself, and the source and
        // destination endpoints are the same, causing messages we send out
        // to arrive back on the initiating socket instead of the receiving
        // socket.  If we did a complete job of enumerating local addresses,
        // we could avoid directly connecting to ourselves, or at least detect
        // immediately when we did it and disconnect.

        // The only way I know of that we'd get an unexpected hello that we
        //  can't really guard against is if we do a simulatenous open, we
        // probably need to think through that case.  We're not attempting that
        // yet, though, so it's ok to just disconnect here.
        wlog("unexpected hello_message from peer, disconnecting");
        fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Received a unexpected hello_message"));
        disconnect_from_peer(originating_peer, "Received a unexpected hello_message", true, detailed_error);
      }
    }

    void node_impl::on_connection_accepted_message(peer_connection* originating_peer, const connection_accepted_message& connection_accepted_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received a connection_accepted in response to my \"hello\" from ${peer}", ("peer", originating_peer->get_remote_endpoint()));
      originating_peer->negotiation_status = peer_connection::connection_negotiation_status::peer_connection_accepted;
      originating_peer->our_state = peer_connection::our_connection_state::connection_accepted;
      originating_peer->send_message(address_request_message());
      fc::time_point now = fc::time_point::now();
      if (_is_firewalled == firewalled_state::unknown &&
          _last_firewall_check_message_sent < now - fc::minutes(GRAPHENE_NET_FIREWALL_CHECK_MESSAGE_INTERVAL_MINUTES) &&
          originating_peer->core_protocol_version >= GRAPHENE_NET_PROTOCOL_FIREWALL_CHECK_VERSION)
      {
        wlog("I don't know if I'm firewalled.  Sending a firewall check message to peer ${peer}",
             ("peer", originating_peer->get_remote_endpoint()));
        originating_peer->firewall_check_state = new firewall_check_state_data;

        originating_peer->send_message(check_firewall_message());
        _last_firewall_check_message_sent = now;
      }
    }

    void node_impl::on_connection_rejected_message(peer_connection* originating_peer, const connection_rejected_message& connection_rejected_message_received)
    {
      VERIFY_CORRECT_THREAD();
      if (originating_peer->our_state == peer_connection::our_connection_state::just_connected)
      {
        ilog("Received a rejection from ${peer} in response to my \"hello\", reason: \"${reason}\"",
             ("peer", originating_peer->get_remote_endpoint())
             ("reason", connection_rejected_message_received.reason_string));

        if (connection_rejected_message_received.reason_code == rejection_reason_code::connected_to_self)
        {
          _potential_peer_db.erase(originating_peer->get_socket().remote_endpoint());
          move_peer_to_closing_list(originating_peer->shared_from_this());
          originating_peer->close_connection();
        }
        else
        {
          // update our database to record that we were rejected so we won't try to connect again for a while
          // this only happens on connections we originate, so we should already know that peer is not firewalled
          fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
          if (endpoint_for_db)
          {
            fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
            if (updated_peer_record)
            {
              updated_peer_record->last_connection_disposition = last_connection_rejected;
              updated_peer_record->last_connection_attempt_time = fc::time_point::now();
              updated_peer_record->last_failed_time = updated_peer_record->last_connection_attempt_time;
              _potential_peer_db.update_entry(*updated_peer_record);
            }
          }
        }

        originating_peer->negotiation_status = peer_connection::connection_negotiation_status::peer_connection_rejected;
        originating_peer->our_state = peer_connection::our_connection_state::connection_rejected;
        originating_peer->send_message(address_request_message());
      }
      else
        FC_THROW( "unexpected connection_rejected_message from peer" );
    }

    void node_impl::on_address_request_message(peer_connection* originating_peer, const address_request_message& address_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received an address request message");

      address_message reply;
      if (!_node_configuration.peer_advertising_disabled)
      {
        reply.addresses.reserve(_active_connections.size());
        for (const peer_connection_ptr& active_peer : _active_connections)
        {
          fc::optional<fc::ip::endpoint> endpoint_for_db = active_peer->get_endpoint_for_db();
          if (endpoint_for_db)
            reply.addresses.emplace_back(address_info(*endpoint_for_db,
                                                    fc::time_point::now(),
                                                    active_peer->round_trip_delay,
                                                    active_peer->node_id,
                                                    active_peer->direction,
                                                    active_peer->is_firewalled));
        }
      }
      originating_peer->send_message(reply);
    }

    void node_impl::on_address_message(peer_connection* originating_peer, const address_message& address_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("Received an address message containing ${size} addresses", ("size", address_message_received.addresses.size()));
      for (const address_info& address : address_message_received.addresses)
      {
        dlog("    ${endpoint} last seen ${time}", ("endpoint", address.remote_endpoint)("time", address.last_seen_time));
      }
      std::vector<graphene::net::address_info> updated_addresses = address_message_received.addresses;
      for (address_info& address : updated_addresses)
        address.last_seen_time = fc::time_point_sec(fc::time_point::now());
      bool new_information_received = merge_address_info_with_potential_peer_database(updated_addresses);
      if (new_information_received)
        trigger_p2p_network_connect_loop();

      if (_handshaking_connections.find(originating_peer->shared_from_this()) != _handshaking_connections.end())
      {
        // if we were handshaking, we need to continue with the next step in handshaking (which is either
        // ending handshaking and starting synchronization or disconnecting)
        if( originating_peer->our_state == peer_connection::our_connection_state::connection_rejected)
          disconnect_from_peer(originating_peer, "You rejected my connection request (hello message) so I'm disconnecting");
        else if (originating_peer->their_state == peer_connection::their_connection_state::connection_rejected)
          disconnect_from_peer(originating_peer, "I rejected your connection request (hello message) so I'm disconnecting");
        else
        {
          fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
          if (endpoint_for_db)
          {
            // mark the connection as successful in the database
            fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
            if (updated_peer_record)
            {
              updated_peer_record->last_connection_disposition = last_connection_succeeded;
              _potential_peer_db.update_entry(*updated_peer_record);
            }
          }

          originating_peer->negotiation_status = peer_connection::connection_negotiation_status::negotiation_complete;
          move_peer_to_active_list(originating_peer->shared_from_this());
          new_peer_just_added(originating_peer->shared_from_this());
        }
      }
      // else if this was an active connection, then this was just a reply to our periodic address requests.
      // we've processed it, there's nothing else to do
    }

    void node_impl::on_fetch_blockchain_item_ids_message(peer_connection* originating_peer,
                                                         const fetch_blockchain_item_ids_message& fetch_blockchain_item_ids_message_received)
    {
      VERIFY_CORRECT_THREAD();
      item_id peers_last_item_seen = item_id(fetch_blockchain_item_ids_message_received.item_type, item_hash_t());
      if (fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty())
      {
        dlog("sync: received a request for item ids starting at the beginning of the chain from peer ${peer_endpoint} (full request: ${synopsis})",
             ("peer_endpoint", originating_peer->get_remote_endpoint())
             ("synopsis", fetch_blockchain_item_ids_message_received.blockchain_synopsis));
      }
      else
      {
        item_hash_t peers_last_item_hash_seen = fetch_blockchain_item_ids_message_received.blockchain_synopsis.back();
        dlog("sync: received a request for item ids after ${last_item_seen} from peer ${peer_endpoint} (full request: ${synopsis})",
             ("last_item_seen", peers_last_item_hash_seen)
             ("peer_endpoint", originating_peer->get_remote_endpoint())
             ("synopsis", fetch_blockchain_item_ids_message_received.blockchain_synopsis));
        peers_last_item_seen.item_hash = peers_last_item_hash_seen;
      }

      blockchain_item_ids_inventory_message reply_message;
      reply_message.item_type = fetch_blockchain_item_ids_message_received.item_type;
      reply_message.total_remaining_item_count = 0;
      try
      {
        reply_message.item_hashes_available = _delegate->get_block_ids(fetch_blockchain_item_ids_message_received.blockchain_synopsis,
                                                                       reply_message.total_remaining_item_count);
      }
      catch (const peer_is_on_an_unreachable_fork&)
      {
        //This was true when a full block synopsis was sent by the peer, now it's not
        //dlog("Peer is on a fork and there's no set of blocks we can provide to switch them to our fork");
        dlog("We don't have any blocks that fit within the reversible synopsis sent by peer: either it is too far ahead of us (likely case) or it is on a fork that can't be recovered from");
        // we reply with an empty list as if we had an empty blockchain;
        // we don't want to disconnect because they may be able to provide
        // us with blocks on their chain
      }

      bool disconnect_from_inhibited_peer = false;
      // if our client doesn't have any items after the item the peer requested, it will send back
      // a list containing the last item the peer requested
      //wdump((reply_message)(fetch_blockchain_item_ids_message_received.blockchain_synopsis));
      if( reply_message.item_hashes_available.empty() )
        originating_peer->peer_needs_sync_items_from_us = false; /* I have no items in my blockchain */
      else if( !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
               reply_message.item_hashes_available.size() == 1 &&
               std::find(fetch_blockchain_item_ids_message_received.blockchain_synopsis.begin(),
                         fetch_blockchain_item_ids_message_received.blockchain_synopsis.end(),
                         reply_message.item_hashes_available.back() ) != fetch_blockchain_item_ids_message_received.blockchain_synopsis.end() )
      {
        /* the last item in the peer's list matches the last item in our list */
        originating_peer->peer_needs_sync_items_from_us = false;
        if (originating_peer->inhibit_fetching_sync_blocks)
          disconnect_from_inhibited_peer = true; // delay disconnecting until after we send our reply to this fetch_blockchain_item_ids_message
      }
      else
        originating_peer->peer_needs_sync_items_from_us = true;

      if (!originating_peer->peer_needs_sync_items_from_us)
      {
        dlog("sync: peer is already in sync with us");
        // if we thought we had all the items this peer had, but now it turns out that we don't
        // have the last item it requested to send from,
        // we need to kick off another round of synchronization
        if (!originating_peer->we_need_sync_items_from_peer &&
            !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
            !_delegate->has_item(peers_last_item_seen))
        {
          dlog("sync: restarting sync with peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          start_synchronizing_with_peer(originating_peer->shared_from_this());
        }
      }
      else
      {
        dlog("sync: peer is out of sync, sending peer ${count} items ids: first: ${first_item_id}, last: ${last_item_id}",
             ("count", reply_message.item_hashes_available.size())
             ("first_item_id", reply_message.item_hashes_available.front())
             ("last_item_id", reply_message.item_hashes_available.back()));
        if (!originating_peer->we_need_sync_items_from_peer &&
            !fetch_blockchain_item_ids_message_received.blockchain_synopsis.empty() &&
            !_delegate->has_item(peers_last_item_seen))
        {
          dlog("sync: restarting sync with peer ${peer}", ("peer", originating_peer->get_remote_endpoint()));
          start_synchronizing_with_peer(originating_peer->shared_from_this());
        }
      }
      originating_peer->send_message(reply_message);

      if (disconnect_from_inhibited_peer)
      {
        // the peer has all of our blocks, and we don't want any of theirs, so disconnect them
        fc::exception detailed_error(FC_LOG_MESSAGE(warn, "you are on a fork that I'm unable to switch to"));
        disconnect_from_peer(originating_peer, "you are on a fork that I'm unable to switch to", true, detailed_error);
        return;
      }

      //if we are handshaking with a peer that connected to us
      if (originating_peer->direction == peer_connection_direction::inbound &&
          _handshaking_connections.find(originating_peer->shared_from_this()) != _handshaking_connections.end())
      {
        // handshaking is done, move the connection to fully active status and start synchronizing
        dlog("peer ${endpoint} which was handshaking with us has started synchronizing with us, start syncing with it",
             ("endpoint", originating_peer->get_remote_endpoint()));
        fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
        if (endpoint_for_db && originating_peer->is_firewalled != firewalled_state::firewalled)
        {
          // mark the connection as successful in the database
          potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(*endpoint_for_db);
          updated_peer_record.last_connection_disposition = last_connection_succeeded;
          _potential_peer_db.update_entry(updated_peer_record);
        }

        // transition it to our active list
        move_peer_to_active_list(originating_peer->shared_from_this());
        new_peer_just_added(originating_peer->shared_from_this());
      }
    }

    uint32_t node_impl::calculate_unsynced_block_count_from_all_peers()
    {
      VERIFY_CORRECT_THREAD();
      uint32_t max_number_of_unfetched_items = 0;
      for( const peer_connection_ptr& peer : _active_connections )
      {
        uint32_t this_peer_number_of_unfetched_items = (uint32_t)peer->ids_of_items_to_get.size() + peer->number_of_unfetched_item_ids;
        max_number_of_unfetched_items = std::max(max_number_of_unfetched_items,
                                                 this_peer_number_of_unfetched_items);
      }
      return max_number_of_unfetched_items;
    }

    // get a blockchain synopsis that makes sense to send to the given peer.
    // If the peer isn't yet syncing with us, this is just a synopsis of our active blockchain
    // If the peer is syncing with us, it is a synopsis of our active blockchain plus the
    //    blocks the peer has already told us it has
    std::vector<item_hash_t> node_impl::create_blockchain_synopsis_for_peer(const peer_connection* peer)
    {
      VERIFY_CORRECT_THREAD();
      const item_hash_t reference_point = peer->last_block_delegate_has_seen;

      uint32_t number_of_blocks_after_reference_point = peer->ids_of_items_to_get.size();
      std::vector<item_hash_t> synopsis = _delegate->get_blockchain_synopsis(reference_point, number_of_blocks_after_reference_point);
      if (number_of_blocks_after_reference_point)
      {
        // then the synopsis is incomplete, add the missing elements from ids_of_items_to_get
        uint32_t first_block_num_in_ids_to_get = _delegate->get_block_number(peer->ids_of_items_to_get.front());
        uint32_t true_high_block_num = first_block_num_in_ids_to_get + peer->ids_of_items_to_get.size() - 1;

        // in order to generate a seamless synopsis, we need to be using the same low_block_num as the
        // backend code; the first block in the synopsis will be the low block number it used
        uint32_t low_block_num = synopsis.empty() ? 1 : _delegate->get_block_number(synopsis.front());

        do
        {
          if (low_block_num >= first_block_num_in_ids_to_get)
          {
            auto idx = low_block_num - first_block_num_in_ids_to_get;
            FC_ASSERT(idx < peer->ids_of_items_to_get.size());
            synopsis.push_back(peer->ids_of_items_to_get[idx]);
          }
          low_block_num += (true_high_block_num - low_block_num + 2 ) / 2;
        }
        while (low_block_num <= true_high_block_num);
        assert(synopsis.back() == peer->ids_of_items_to_get.back());
      }
      dlog("leaving create_blockchain_synopsis_for_peer");
      return synopsis;
    }

    void node_impl::fetch_next_batch_of_item_ids_from_peer(peer_connection* peer, bool reset_fork_tracking_data_for_peer /* = false */)
    {
      VERIFY_CORRECT_THREAD();
      //don't fetch another batch of ids while waiting for a previously requested batch
      if (peer->item_ids_requested_from_peer)
        return;

      if (reset_fork_tracking_data_for_peer)
      {
        peer->last_block_delegate_has_seen = item_hash_t();
        peer->last_block_time_delegate_has_seen = _delegate->get_block_time(item_hash_t());
      }

      fc::oexception synopsis_exception;
      try
      {
        std::vector<item_hash_t> blockchain_synopsis = create_blockchain_synopsis_for_peer(peer);
        item_hash_t last_item_seen = blockchain_synopsis.empty() ? item_hash_t() : blockchain_synopsis.back();
        dlog("sync: sending a request for the next items after ${last_item_seen} to peer ${peer}, (full request is ${blockchain_synopsis})",
             (last_item_seen)
             ("peer", peer->get_remote_endpoint())
             (blockchain_synopsis));
        peer->item_ids_requested_from_peer = boost::make_tuple(blockchain_synopsis, fc::time_point::now());
        peer->send_message(fetch_blockchain_item_ids_message(_sync_item_type, blockchain_synopsis));
      }
      catch (const block_older_than_undo_history& e)
      {
        synopsis_exception = e;
      }
      if (synopsis_exception)
      {
        fc::exception error_for_peer(FC_LOG_MESSAGE(error, "You are on a fork I'm unable to switch to"));
        disconnect_from_peer(peer, "You are on a fork I'm unable to switch to", true, error_for_peer);
      }
    }

    bool node_impl::is_item_id_being_processed(const item_hash_t& block_id)
    {
      for (const peer_connection_ptr& peer : _active_connections)
        if (peer->ids_of_items_being_processed.find(block_id) != peer->ids_of_items_being_processed.end())
          return true;
      return false;
    }

    void node_impl::on_blockchain_item_ids_inventory_message(peer_connection* originating_peer,
                                                             const blockchain_item_ids_inventory_message& blockchain_item_ids_inventory_message_received)
    {
      VERIFY_CORRECT_THREAD();
      // ignore unless we asked for the data
      if( originating_peer->item_ids_requested_from_peer )
      {
        // verify that the peer's the block ids the peer sent is a valid response to our request;
        // It should either be an empty list of blocks, or a list of blocks that builds off of one of
        // the blocks in the synopsis we sent
        if (!blockchain_item_ids_inventory_message_received.item_hashes_available.empty())
        {
          // what's more, it should be a sequential list of blocks, verify that first
          uint32_t first_block_number_in_reponse = get_block_number(blockchain_item_ids_inventory_message_received.item_hashes_available.front());
          for (unsigned i = 1; i < blockchain_item_ids_inventory_message_received.item_hashes_available.size(); ++i)
          {
            uint32_t actual_num = get_block_number(blockchain_item_ids_inventory_message_received.item_hashes_available[i]);
            uint32_t expected_num = first_block_number_in_reponse + i;
            if (actual_num != expected_num)
            {
              wlog("Invalid response from peer ${peer_endpoint}.  The list of blocks they provided is not sequential, "
                   "the ${position}th block in their reply was block number ${actual_num}, "
                   "but it should have been number ${expected_num}",
                   ("peer_endpoint", originating_peer->get_remote_endpoint())
                   ("position", i)
                   ("actual_num", actual_num)
                   ("expected_num", expected_num));
              fc::exception error_for_peer(FC_LOG_MESSAGE(error,
                                                          "You gave an invalid response to my request for sync blocks.  The list of blocks you provided is not sequential, "
                                                          "the ${position}th block in their reply was block number ${actual_num}, "
                                                          "but it should have been number ${expected_num}",
                                                          ("position", i)
                                                          ("actual_num", actual_num)
                                                          ("expected_num", expected_num)));
              disconnect_from_peer(originating_peer, "You gave an invalid response to my request for sync blocks", true, error_for_peer);
              return;
            }
          }
          const std::vector<item_hash_t>& synopsis_sent_in_request = originating_peer->item_ids_requested_from_peer->get<0>();
          const item_hash_t& first_item_hash = blockchain_item_ids_inventory_message_received.item_hashes_available.front();

          if (synopsis_sent_in_request.empty())
          {
            // if we sent an empty synopsis, we were asking for all blocks, so the first block should be block 1
            if (get_block_number(first_item_hash) != 1)
            {
              wlog("Invalid response from peer ${peer_endpoint}.  We requested a list of sync blocks starting from the beginning of the chain, "
                   "but they provided a list of blocks starting with ${first_block}",
                   ("peer_endpoint", originating_peer->get_remote_endpoint())
                   ("first_block", first_item_hash));
              fc::exception error_for_peer(FC_LOG_MESSAGE(error, "You gave an invalid response for my request for sync blocks.  I asked for blocks starting from the beginning of the chain, "
                                                          "but you returned a list of blocks starting with ${first_item_hash}",
                                                          (first_item_hash)));
              disconnect_from_peer(originating_peer, "You gave an invalid response to my request for sync blocks", true, error_for_peer);
              return;
            }
          }
          else // synopsis was not empty, we expect a response building off one of the blocks we sent
          {
            if (std::find(synopsis_sent_in_request.rbegin(), synopsis_sent_in_request.rend(), first_item_hash) == synopsis_sent_in_request.rend())
            {
              wlog("Invalid response from peer ${peer_endpoint}.  We requested a list of sync blocks based on the synopsis ${synopsis}, but they "
                   "provided a list of blocks starting with ${first_block}",
                   ("peer_endpoint", originating_peer->get_remote_endpoint())
                   ("synopsis", synopsis_sent_in_request)
                   ("first_block", first_item_hash));
              fc::exception error_for_peer(FC_LOG_MESSAGE(error, "You gave an invalid response for my request for sync blocks.  I asked for blocks following something in "
                                                          "${synopsis_sent_in_request}, but you returned a list of blocks starting with ${first_item_hash} which wasn't one of your choices",
                                                          (synopsis_sent_in_request)(first_item_hash)));
              disconnect_from_peer(originating_peer, "You gave an invalid response to my request for sync blocks", true, error_for_peer);
              return;
            }
          }
        }
        originating_peer->item_ids_requested_from_peer.reset();

        // if exceptions are throw after clearing the item_ids_requested_from_peer (above),
        // it could leave our sync in a stalled state.  Wrap a try/catch around the rest
        // of the function so we can log if this ever happens.
        try
        {
          dlog( "sync: received a list of ${count} available items from ${peer_endpoint}",
               ( "count", blockchain_item_ids_inventory_message_received.item_hashes_available.size() )
               ( "peer_endpoint", originating_peer->get_remote_endpoint() ) );
          //for( const item_hash_t& item_hash : blockchain_item_ids_inventory_message_received.item_hashes_available )
          //{
          //  dlog( "sync:     ${hash}", ("hash", item_hash ) );
          //}

          // if the peer doesn't have any items after the one we asked for
          if( blockchain_item_ids_inventory_message_received.total_remaining_item_count == 0 &&
              ( blockchain_item_ids_inventory_message_received.item_hashes_available.empty() || // there are no items in the peer's blockchain.  this should only happen if our blockchain was empty when we requested, might want to verify that.
               ( blockchain_item_ids_inventory_message_received.item_hashes_available.size() == 1 &&
                _delegate->has_item( item_id(blockchain_item_ids_inventory_message_received.item_type,
                                             blockchain_item_ids_inventory_message_received.item_hashes_available.front() ) ) ) ) && // we've already seen the last item in the peer's blockchain
              originating_peer->ids_of_items_to_get.empty() &&
              originating_peer->number_of_unfetched_item_ids == 0 ) // <-- is the last check necessary?
          {
            dlog( "sync: peer said we're up-to-date, entering normal operation with this peer" );
            originating_peer->we_need_sync_items_from_peer = false;

            uint32_t new_number_of_unfetched_items = calculate_unsynced_block_count_from_all_peers();
            _total_number_of_unfetched_items = new_number_of_unfetched_items;
            if( new_number_of_unfetched_items == 0 )
              _delegate->sync_status( blockchain_item_ids_inventory_message_received.item_type, 0 );

            return;
          }

          std::deque<item_hash_t> item_hashes_received(blockchain_item_ids_inventory_message_received.item_hashes_available.begin(),
                                                       blockchain_item_ids_inventory_message_received.item_hashes_available.end());
          originating_peer->number_of_unfetched_item_ids = blockchain_item_ids_inventory_message_received.total_remaining_item_count;

          // flush any items this peer sent us that we've already received and processed from another peer
          if (!item_hashes_received.empty() && originating_peer->ids_of_items_to_get.empty())
          {
            auto first_item_not_in_blockchain_iter = _delegate->find_first_item_not_in_blockchain(item_hashes_received);
            if (first_item_not_in_blockchain_iter == item_hashes_received.end()) //all items in blockchain
            {
              originating_peer->last_block_delegate_has_seen = item_hashes_received.back();
              originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(originating_peer->last_block_delegate_has_seen);
              item_hashes_received.clear();
            }
            else //some items are not in blockchain
            {
              if (first_item_not_in_blockchain_iter != item_hashes_received.begin()) //if some items are in the blockchain
              {
                originating_peer->last_block_delegate_has_seen = *std::prev(first_item_not_in_blockchain_iter);
                originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(originating_peer->last_block_delegate_has_seen);
              }
              auto find_first_item_not_seen_iter = std::partition_point(first_item_not_in_blockchain_iter, item_hashes_received.cend(), [this](const item_hash_t& item) { return is_item_id_being_processed(item); });
              //track for peer all items it has that are already being processed
              std::copy(first_item_not_in_blockchain_iter, find_first_item_not_seen_iter, std::inserter(originating_peer->ids_of_items_being_processed, originating_peer->ids_of_items_being_processed.end()));
              item_hashes_received.erase(item_hashes_received.begin(), find_first_item_not_seen_iter);
            }
            dlog("after removing all items we have already seen or are processing, item_hashes_received.size() = ${size}", ("size", item_hashes_received.size()));

            if (!item_hashes_received.empty())
            {
              originating_peer->first_id_block_number = _delegate->get_block_number(item_hashes_received.front());
              originating_peer->reset_id_search_for_peer();
            }
          }
          else if (!item_hashes_received.empty())
          {
            // we received a list of items and we already have a list of items to fetch from this peer.
            // In the normal case, this list will immediately follow the existing list, meaning the
            // last hash of our existing list will match the first hash of the new list.

            // In the much less likely case, we've received a partial list of items from the peer, then
            // the peer switched forks before sending us the remaining list.  In this case, the first
            // hash in the new list may not be the last hash in the existing list (it may be earlier, or
            // it may not exist at all.

            // In either case, pop items off the back of our existing list until we find our first
            // item, then append our list.
            while (!originating_peer->ids_of_items_to_get.empty())
            {
              if (item_hashes_received.front() != originating_peer->ids_of_items_to_get.back())
              {
                --originating_peer->last_requested_block_number_for_peers_on_this_fork;
                originating_peer->ids_of_items_to_get.pop_back();
              }
              else
                break;
            }
            //ensure we don't go back more than we have item ids to search
            if (originating_peer->last_requested_block_number_for_peers_on_this_fork < originating_peer->first_id_block_number - 1)
              originating_peer->reset_id_search_for_peer();

            if (originating_peer->ids_of_items_to_get.empty())
            {
              // this happens when the peer has switched forks between the last inventory message and
              // this one, and there weren't any unfetched items in common
              // We don't know where in the blockchain the new front() actually falls, all we can
              // expect is that it is a block that we knew about because it should be one of the
              // blocks we sent in the initial synopsis.
              originating_peer->last_block_delegate_has_seen = item_hashes_received.front();
              originating_peer->last_block_time_delegate_has_seen = _delegate->get_block_time(item_hashes_received.front());
              item_hashes_received.pop_front();
              originating_peer->first_id_block_number = _delegate->get_block_number(item_hashes_received.front());
              originating_peer->reset_id_search_for_peer();
            }
            else
            {
              // the common simple case: the new list extends the old.  pop off the duplicate element
              originating_peer->ids_of_items_to_get.pop_back();
            }
          }

          if (!item_hashes_received.empty() && !originating_peer->ids_of_items_to_get.empty())
            assert(item_hashes_received.front() != originating_peer->ids_of_items_to_get.back());

          // append the remaining items to the peer's list
          boost::push_back(originating_peer->ids_of_items_to_get, item_hashes_received);

          originating_peer->number_of_unfetched_item_ids = blockchain_item_ids_inventory_message_received.total_remaining_item_count;

          // at any given time, there's a maximum number of blocks that can possibly be out there
          // [(now - genesis time) / block interval].  If they offer us more blocks than that,
          // they must be an attacker or have a buggy client.
          fc::time_point_sec minimum_time_of_last_offered_block =
              originating_peer->last_block_time_delegate_has_seen + // timestamp of the block immediately before the first unfetched block
              originating_peer->number_of_unfetched_item_ids * HIVE_BLOCK_INTERVAL;
          if (minimum_time_of_last_offered_block > _delegate->get_blockchain_now() + GRAPHENE_NET_FUTURE_SYNC_BLOCKS_GRACE_PERIOD_SEC)
          {
            wlog("Disconnecting from peer ${peer} who offered us an implausible number of blocks, their last block would be in the future (${timestamp})",
                 ("peer", originating_peer->get_remote_endpoint())
                 ("timestamp", minimum_time_of_last_offered_block));
            fc::exception error_for_peer(FC_LOG_MESSAGE(error, "You offered me a list of more sync blocks than could possibly exist.  Total blocks offered: ${blocks}, Minimum time of the last block you offered: ${minimum_time_of_last_offered_block}, Now: ${now}",
                                                        ("blocks", originating_peer->number_of_unfetched_item_ids)
                                                        (minimum_time_of_last_offered_block)
                                                        ("now", _delegate->get_blockchain_now())));
            disconnect_from_peer(originating_peer, "You offered me a list of more sync blocks than could possibly exist", true, error_for_peer);
            return;
          }

          uint32_t new_number_of_unfetched_items = calculate_unsynced_block_count_from_all_peers();
          if (new_number_of_unfetched_items != _total_number_of_unfetched_items)
            _delegate->sync_status(blockchain_item_ids_inventory_message_received.item_type,
                                   new_number_of_unfetched_items);
          _total_number_of_unfetched_items = new_number_of_unfetched_items;

          if (blockchain_item_ids_inventory_message_received.total_remaining_item_count != 0) //node says it has more item ids to supply
          {
            // if we have enough ids from node for now, start fetching blocks
            if (originating_peer->ids_of_items_to_get.size() > GRAPHENE_NET_MIN_BLOCK_IDS_TO_PREFETCH)
            {
              trigger_fetch_sync_items_loop();
            }
            else // keep fetching the peer's list of sync items until we get enough to switch into block-fetchimg mode
              fetch_next_batch_of_item_ids_from_peer(originating_peer);
          }
          else // the peer has told us about all of the item ids it knows
          {
            // if there are block on the list let's try to fetch them
            if (!originating_peer->ids_of_items_to_get.empty())
            {
              trigger_fetch_sync_items_loop();
            }
            else
            {
              // If we get here, the peer has sent us a non-empty list of items, but we have already
              // received all of the items from other peers.  Send a new request to the peer to
              // see if we're really in sync.
              fetch_next_batch_of_item_ids_from_peer(originating_peer);
            }
          }
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          elog("Caught unexpected exception: ${e}", ("e", e));
          assert(false && "exceptions not expected here");
        }
        catch (const std::exception& e)
        {
          elog("Caught unexpected exception: ${e}", ("e", e.what()));
          assert(false && "exceptions not expected here");
        }
        catch (...)
        {
          elog("Caught unexpected exception, could break sync operation");
        }
      }
      else
      {
        wlog("sync: received a list of sync items available, but I didn't ask for any!");
      }
    }

    // this is only called when a peer wants a block from us
    std::shared_ptr<full_block_type> node_impl::get_full_block_by_block_id(const block_id_type& block_id)
    {
      activity_tracer aTracer(__FUNCTION__, *this);

      try
      {
        return _block_message_cache.get_item_by_contents_hash(block_id);
      }
      catch (const fc::key_not_found_exception&)
      {}

      std::shared_ptr<full_block_type> full_block = _delegate->get_full_block(block_id);
      return full_block;
    }

    void node_impl::on_fetch_items_message(peer_connection* originating_peer, const fetch_items_message& fetch_items_message_received)
    {
      VERIFY_CORRECT_THREAD();
      dlog("received items request for ids ${ids} of type ${type} from peer ${endpoint}",
           ("ids", fetch_items_message_received.items_to_fetch)
           ("type", fetch_items_message_received.item_type)
           ("endpoint", originating_peer->get_remote_endpoint()));

      if (fetch_items_message_received.item_type == block_message_type)
      {
        std::vector<std::shared_ptr<full_block_type>> reply_blocks;
        reply_blocks.reserve(fetch_items_message_received.items_to_fetch.size());
        std::shared_ptr<full_block_type> last_full_block_sent;
        for (const item_hash_t& item_hash : fetch_items_message_received.items_to_fetch)
        {
          std::shared_ptr<full_block_type> full_block;
          try
          {
            // first, assume item_hash is the hash of a message.  Search our cache for the matching message
            full_block = _block_message_cache.get_item(item_hash);
            dlog("received item request from peer ${endpoint}, returning the block from cache with id ${id}",
                 ("id", full_block->get_block_id())("endpoint", originating_peer->get_remote_endpoint()));
          }
          catch (const fc::key_not_found_exception&)
          {
            // it wasn't in our local cache
          }

          // if we didn't find it, either it has fallen out of our cache, or it wasn't the hash of a message
          // at all.  If someone was syncing to us, it would be the block_id instead.  We can't tell from
          // just looking at the hash, so check our cache by block_id, and if that fails, try to get it 
          // from the fork_db/block_log
          if (!full_block)
            full_block = get_full_block_by_block_id(item_hash);
          if (full_block)
            last_full_block_sent = full_block;

          /// \warning full_block can be null. if so, we can't touch it, but instead (as previously) push it into reply_blocks, to finally produce `item_not_available_message`
          if (full_block)
          {
            // this block may need to be compressed or decompressed before sending to the peer.  If that's the case,
            // send it over to a worker thread to get started on it
            if (originating_peer->supports_compressed_blocks())
            {
              if (originating_peer->requires_alternate_compression_for_block(full_block))
                _thread_pool.enqueue_work(full_block, hive::chain::blockchain_worker_thread_pool::data_source_type::block_log_destined_for_p2p_alternate_compressed); // trigger alternate compression
              else
                _thread_pool.enqueue_work(full_block, hive::chain::blockchain_worker_thread_pool::data_source_type::block_log_destined_for_p2p_compressed); // trigger default compression
            }
            else
              _thread_pool.enqueue_work(full_block, hive::chain::blockchain_worker_thread_pool::data_source_type::block_log_destined_for_p2p_uncompressed); // decompress if necessary
          }

          reply_blocks.push_back(std::move(full_block));
        }
        assert(reply_blocks.size() == fetch_items_message_received.items_to_fetch.size());
        for (unsigned i = 0; i < reply_blocks.size(); ++i)
          if (reply_blocks[i])
            originating_peer->send_block_message(reply_blocks[i]);
          else
            originating_peer->send_message(item_not_available_message(item_id(block_message_type, fetch_items_message_received.items_to_fetch[i])));

        // if we sent them a block, update our record of the last block they've seen accordingly
        if (last_full_block_sent)
        {
          originating_peer->last_block_delegate_has_seen = last_full_block_sent->get_block_id();
          originating_peer->last_block_time_delegate_has_seen = last_full_block_sent->get_block_header().timestamp;
        }
      }
      else if (fetch_items_message_received.item_type == trx_message_type)
      {
        std::vector<std::shared_ptr<full_transaction_type>> reply_transactions;
        reply_transactions.reserve(fetch_items_message_received.items_to_fetch.size());
        for (const item_hash_t& item_hash : fetch_items_message_received.items_to_fetch)
        {
          std::shared_ptr<full_transaction_type> full_transaction;
          try
          {
            full_transaction = _transaction_message_cache.get_item(item_hash);
            dlog("received item request from peer ${endpoint}, returning the transaction from cache with id ${id}",
                 ("id", full_transaction->get_transaction_id())("endpoint", originating_peer->get_remote_endpoint()));
          }
          catch (const fc::key_not_found_exception&)
          {
            // it wasn't in our local cache
          }
          reply_transactions.push_back(std::move(full_transaction));
        }
        assert(reply_transactions.size() == fetch_items_message_received.items_to_fetch.size());
        for (unsigned i = 0; i < reply_transactions.size(); ++i)
          if (reply_transactions[i])
            originating_peer->send_message(trx_message(reply_transactions[i]));
          else
            originating_peer->send_message(item_not_available_message(item_id(trx_message_type, fetch_items_message_received.items_to_fetch[i])));
      }
      else
        FC_THROW("Peer requested an item that wasn't a block or a transaction");
    }

    void node_impl::on_item_not_available_message( peer_connection* originating_peer, const item_not_available_message& item_not_available_message_received )
    {
      VERIFY_CORRECT_THREAD();
      const item_id& requested_item = item_not_available_message_received.requested_item;
      //if a regular item we asked for is not available
      auto regular_item_iter = originating_peer->items_requested_from_peer.find(requested_item);
      if (regular_item_iter != originating_peer->items_requested_from_peer.end())
      {
        originating_peer->items_requested_from_peer.erase( regular_item_iter );
        originating_peer->inventory_peer_advertised_to_us.erase( requested_item );
        if (is_item_in_any_peers_inventory(requested_item))
          _items_to_fetch.insert(prioritized_item_id(requested_item, _items_to_fetch_sequence_counter++));
        wlog("Peer doesn't have the requested item.");
        trigger_fetch_items_loop();
        return;
      }

      //a sync item was not available
      auto sync_item_iter = originating_peer->sync_items_requested_from_peer.find(requested_item.item_hash);
      if (sync_item_iter != originating_peer->sync_items_requested_from_peer.end())
      {

        if (originating_peer->peer_needs_sync_items_from_us)
        {
          originating_peer->inhibit_fetching_sync_blocks = true;
          wlog("inhibit_fetching_sync_blocks from ${peer} because it didn't have an item it claimed to have",("peer",originating_peer->get_remote_endpoint()));
          //we're keeping this peer, but we need to get the item from someone else
          _active_sync_requests.erase(*sync_item_iter);
          originating_peer->sync_items_requested_from_peer.erase(sync_item_iter);
          for (const peer_connection_ptr& peer : _active_connections)
            peer->reset_id_search_for_peer(); 
        }
        else
          disconnect_from_peer(originating_peer, "You are missing a sync item you claim to have, your database is probably corrupted. Try --rebuild-index.", true,
                               fc::exception(FC_LOG_MESSAGE(error, "You are missing a sync item you claim to have, your database is probably corrupted. Try --rebuild-index.",
                                                            (requested_item))));
        wlog("Peer doesn't have the requested sync item because he must have forked and lost it");
        trigger_fetch_sync_items_loop();
        return;
      }

      dlog("Peer doesn't have an item we're looking for, which is fine because we weren't looking for it");
    }

    void node_impl::on_item_ids_inventory_message(peer_connection* originating_peer, const item_ids_inventory_message& item_ids_inventory_message_received)
    {
      VERIFY_CORRECT_THREAD();

      if (item_ids_inventory_message_received.item_type != block_message_type && 
          item_ids_inventory_message_received.item_type != trx_message_type)
      {
        wlog("received inventory of items of unrecognized type type ${type} from peer ${endpoint}, ignoring",
             ("type", item_ids_inventory_message_received.item_type)("endpoint", originating_peer->get_remote_endpoint()));
        return;
      }

      // expire old inventory so we'll be making decisions our about whether to fetch blocks below based only on recent inventory
      originating_peer->clear_old_inventory();

      dlog("received inventory of ${count} items from peer ${endpoint}",
           ("count", item_ids_inventory_message_received.item_hashes_available.size())("endpoint", originating_peer->get_remote_endpoint()));
      for (const item_hash_t& item_hash : item_ids_inventory_message_received.item_hashes_available)
      {
        if (_message_ids_currently_being_processed.find(item_hash) != _message_ids_currently_being_processed.end())
          // we're in the middle of processing this item, no need to fetch it again
          continue;
        item_id advertised_item_id(item_ids_inventory_message_received.item_type, item_hash);

        // do some more checks to find out if we already have this item.  If it's in our cache of recent
        // messages, there's no reason to get it again.
        if (item_ids_inventory_message_received.item_type == graphene::net::trx_message_type)
        {
          if (_transaction_message_cache.contains_item(item_hash))
            continue;
        }
        else if (item_ids_inventory_message_received.item_type == graphene::net::block_message_type)
        {
          // how we interpret the item_hash depends on the peer's protocol version
          if (originating_peer->advertise_blocks_by_block_id())
          {
            // then it's a block_id
            if (_block_message_cache.contains_item_by_contents_hash(item_hash))
              continue;
          }
          else
          {
            // then it's a legacy_block_message_hash
            if (_block_message_cache.contains_item(item_hash))
              continue;
          }
        }

        bool we_requested_this_item_from_a_peer = false;
        for (const peer_connection_ptr& peer : _active_connections)
          if (peer->items_requested_from_peer.find(advertised_item_id) != peer->items_requested_from_peer.end())
          {
            we_requested_this_item_from_a_peer = true;
            // don't just continue; we want to record the fact that this peer also offered us the item, so if we fail
            // to get it from `peer`, we can know to ask `originating_peer` instead
            break;
          }

        // if the peer has flooded us with transactions, don't add these to the inventory to prevent our
        // inventory list from growing without bound.  We try to allow fetching blocks even when
        // we've stopped fetching transactions.
        if ((item_ids_inventory_message_received.item_type == graphene::net::trx_message_type &&
             originating_peer->is_inventory_advertised_to_us_list_full_for_transactions()) ||
            originating_peer->is_inventory_advertised_to_us_list_full())
          break;

        originating_peer->inventory_peer_advertised_to_us.insert(peer_connection::timestamped_item_id(advertised_item_id, fc::time_point::now()));
        if (!we_requested_this_item_from_a_peer)
        {
          if (_recently_failed_items.find(item_id(item_ids_inventory_message_received.item_type, item_hash)) != _recently_failed_items.end())
          {
            dlog("not adding ${item_hash} to our list of items to fetch because we've recently fetched a copy and it failed to push", (item_hash));
          }
          else
          {
            auto items_to_fetch_iter = _items_to_fetch.get<item_id_index>().find(advertised_item_id);
            if (items_to_fetch_iter == _items_to_fetch.get<item_id_index>().end())
            {
              // it's new to us
              _items_to_fetch.insert(prioritized_item_id(advertised_item_id, _items_to_fetch_sequence_counter++));
              dlog("adding item ${item_hash} from inventory message to our list of items to fetch", (item_hash));
              trigger_fetch_items_loop();
            }
            else
            {
              // another peer has told us about this item already, but this peer just told us it has the item
              // too, we can expect it to be around in this peer's cache for longer, so update its timestamp
              _items_to_fetch.get<item_id_index>().modify(items_to_fetch_iter,
                                                          [](prioritized_item_id& item) { item.timestamp = fc::time_point::now(); });
            }
          }
        }
      }
    }

    void node_impl::on_closing_connection_message( peer_connection* originating_peer, const closing_connection_message& closing_connection_message_received )
    {
      VERIFY_CORRECT_THREAD();
      originating_peer->they_have_requested_close = true;

      if( closing_connection_message_received.closing_due_to_error )
      {
        elog( "Peer ${peer} is disconnecting us because of an error: ${msg}, exception: ${error}",
             ( "peer", originating_peer->get_remote_endpoint() )
             ( "msg", closing_connection_message_received.reason_for_closing )
             ( "error", closing_connection_message_received.error ) );
        std::ostringstream message;
        message << "Peer " << fc::variant( originating_peer->get_remote_endpoint() ).as_string() <<
                  " disconnected us: " << closing_connection_message_received.reason_for_closing;
        fc::exception detailed_error(FC_LOG_MESSAGE(warn, "Peer ${peer} is disconnecting us because of an error: ${msg}, exception: ${error}",
                                                    ( "peer", originating_peer->get_remote_endpoint() )
                                                    ( "msg", closing_connection_message_received.reason_for_closing )
                                                    ( "error", closing_connection_message_received.error ) ));
        _delegate->error_encountered( message.str(),
                                      detailed_error );
      }
      else
      {
        wlog( "Peer ${peer} is disconnecting us because: ${msg}",
             ( "peer", originating_peer->get_remote_endpoint() )
             ( "msg", closing_connection_message_received.reason_for_closing ) );
      }
      if( originating_peer->we_have_requested_close )
        originating_peer->close_connection();
      }

    void node_impl::on_connection_closed(peer_connection* originating_peer)
    {
      VERIFY_CORRECT_THREAD();

      activity_tracer aTracer(__FUNCTION__, *this);

      peer_connection_ptr originating_peer_ptr = originating_peer->shared_from_this();
      _rate_limiter.remove_tcp_socket( &originating_peer->get_socket() );

      //update peer_db record if possible
      fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
      if (endpoint_for_db)
      {
        fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
        if (updated_peer_record)
        {
          // if we closed the connection (due to timeout or handshake failure), we should have recorded an
          // error message to store in the peer database when we closed the connection
          if (originating_peer->connection_closed_error)
            updated_peer_record->last_error = *originating_peer->connection_closed_error;
          else //we didn't close the connection
            updated_peer_record->last_error = fc::exception(FC_LOG_MESSAGE(info, "lost connection to peer"));
          updated_peer_record->last_failed_time = fc::time_point::now();
          _potential_peer_db.update_entry(*updated_peer_record);
        }
      }

      //remove peer from global lists
      _closing_connections.erase(originating_peer_ptr);
      _handshaking_connections.erase(originating_peer_ptr);
      _terminating_connections.erase(originating_peer_ptr);
      if (_active_connections.find(originating_peer_ptr) != _active_connections.end())
      {
        _active_connections.erase(originating_peer_ptr);

        if (endpoint_for_db)
        {
          fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
          if (updated_peer_record)
          {
            updated_peer_record->last_seen_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_record);
          }
        }
      }

      ilog("Remote peer ${endpoint} closed their connection to us", ("endpoint", originating_peer->get_remote_endpoint()));
      display_current_connections();
      trigger_p2p_network_connect_loop();

      // notify the node delegate so it can update the display
      if( _active_connections.size() != _last_reported_number_of_connections )
      {
        _last_reported_number_of_connections = (uint32_t)_active_connections.size();
        _delegate->connection_count_changed( _last_reported_number_of_connections );
      }

      // if we had delegated a firewall check to this peer, send it to another peer
      if (originating_peer->firewall_check_state)
      {
        if (originating_peer->firewall_check_state->requesting_peer != node_id_t())
        {
          // it's a check we're doing for another node
          firewall_check_state_data* firewall_check_state = originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
          forward_firewall_check_to_next_available_peer(firewall_check_state);
        }
        else
        {
          // we were asking them to check whether we're firewalled.  we'll just let it
          // go for now
          delete originating_peer->firewall_check_state;
        }
      }

      // if we had requested any sync or regular items from this peer that we haven't
      // received yet, reschedule them to be fetched from another peer
      if (!originating_peer->sync_items_requested_from_peer.empty())
      {
        for (const auto& sync_item : originating_peer->sync_items_requested_from_peer)
          _active_sync_requests.erase(sync_item);
        for (const peer_connection_ptr& peer : _active_connections)
          peer->reset_id_search_for_peer();
        trigger_fetch_sync_items_loop();
      }

      if (!originating_peer->items_requested_from_peer.empty())
      {
        for (const auto& item_and_time : originating_peer->items_requested_from_peer)
        {
          if (is_item_in_any_peers_inventory(item_and_time.first))
            _items_to_fetch.insert(prioritized_item_id(item_and_time.first, _items_to_fetch_sequence_counter++));
        }
        trigger_fetch_items_loop();
      }

      discard_abandoned_sync_blocks();

      schedule_peer_for_deletion(originating_peer_ptr);
    } //on_connection_closed

    void node_impl::send_sync_block_to_node_delegate(const std::shared_ptr<full_block_type>& full_block)
    {
      dlog("in send_sync_block_to_node_delegate()");

      bool client_accepted_block = false;
      bool discontinue_fetching_blocks_from_peer = false;
      const uint32_t block_number = full_block->get_block_num();
      const block_id_type& block_id = full_block->get_block_id();

      fc::oexception handle_message_exception;

      try
      {
        fc_dlog(fc::logger::get("sync"), "p2p pushing sync block #${block_number} ${block_id}", (block_number)(block_id));
        _delegate->handle_block(full_block, true);

        if (block_number % 1000 == 0)
          ilog("Successfully pushed sync block ${block_number} ${block_id}", (block_number)(block_id));

        _most_recent_blocks_accepted.push_back(block_id);

        client_accepted_block = true;
      }
      catch (const block_older_than_undo_history& e)
      {
        fc_wlog(fc::logger::get("p2p"), "Failed to push sync block #${block_number} ${block_id}: block is on a fork older than our undo history would allow us to switch to: ${e}",
                (block_number)(block_id)("e", (fc::exception)e));
        wlog("Failed to push sync block #${block_number} ${block_id}): block is on a fork older than our undo history would allow us to switch to.",
             (block_number)(block_id));
        handle_message_exception = e;
        discontinue_fetching_blocks_from_peer = true;
      }
      catch (const fc::canceled_exception&)
      {
        throw;
      }
      catch (const p2p_node_shutting_down_exception&)
      {
        // the block wasn't processed because we're shutting down.  The is probably perfectly valid, so we
        // don't want to mark the peer as bad because of the error.
        // Since we're shutting down, I think it's ok to just return out of the function since we won't need
        // to trigger fetching new items ids or other similar tasks
        return;
      }
      catch (const fc::exception& e)
      {
        fc_wlog(fc::logger::get("sync"), "p2p failed to push sync block #${block_number} ${block_id}: client rejected sync block sent by peer.",
                (block_number) (block_id));
        wlog("Failed to push sync block #${block_number} ${block_id}): client rejected sync block sent by peer.",
             (block_number) (block_id));
        handle_message_exception = e;
      }

      // build up lists for any potentially-blocking operations we need to do, then do them at the end of this function
      std::set<peer_connection_ptr> peers_to_fetch_ids_from;
      std::set<peer_connection_ptr> peers_we_need_to_sync_to;
      std::map<peer_connection_ptr, std::pair<std::string, fc::oexception>> peers_to_disconnect; // map peer -> pair<reason_string, exception>
      if (client_accepted_block)
      {
        --_total_number_of_unfetched_items;
        dlog("sync: client accepted the block, we now have only ${count} items left to fetch before we're in sync",
             ("count", _total_number_of_unfetched_items));
        bool is_fork_block = is_hard_fork_block(block_number);
        for (const peer_connection_ptr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections
          bool disconnecting_this_peer = false;
          if (is_fork_block)
          {
            // we just pushed a hard fork block.  Find out if this peer is running a client
            // that will be unable to process future blocks
            if (peer->last_known_fork_block_number != 0)
            {
              uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(peer->last_known_fork_block_number);
              if (next_fork_block_number != 0 &&
                  next_fork_block_number <= block_number)
              {
                std::ostringstream disconnect_reason_stream;
                disconnect_reason_stream << "You need to upgrade your client due to hard fork at block " << block_number;
                peers_to_disconnect[peer] = std::make_pair(disconnect_reason_stream.str(),
                                              fc::oexception(fc::exception(FC_LOG_MESSAGE(error, "You need to upgrade your client due to hard fork at block ${block_number}",
                                                                                          (block_number)))));
#ifdef ENABLE_DEBUG_ULOGS
                ulog("Disconnecting from peer during sync because their version is too old.  Their version date: ${date}", ("date", peer->graphene_git_revision_unix_timestamp));
#endif
                disconnecting_this_peer = true;
              }
            }
          }
          if (!disconnecting_this_peer)
          {
            if (peer->ids_of_items_to_get.empty() && peer->ids_of_items_being_processed.empty())
            {
              if (!peer->peer_needs_sync_items_from_us && !peer->we_need_sync_items_from_peer)
              {
                // We just got a new valid sync block and we don't know for sure that our peers have it.
                // If peer is still syncing to us, we know they will ask us for
                // sync item ids at least one more time and we'll notify them about
                // the item then, so there's no need to do anything.  If we still need items
                // from them, we'll be asking them for more items at some point, and
                // that will clue them in that they are out of sync.  But if we're both in sync,
                // we need to kick off another round of synchronization with them so they can
                // find out about this new item.
                dlog("We will be restarting synchronization with peer ${peer}", ("peer", peer->get_remote_endpoint()));
                peers_we_need_to_sync_to.insert(peer);
              }
            }
            else
            {
              auto items_being_processed_iter = peer->ids_of_items_being_processed.find(block_id);
              if (items_being_processed_iter != peer->ids_of_items_being_processed.end())
              {
                peer->last_block_delegate_has_seen = block_id;
                peer->last_block_time_delegate_has_seen = full_block->get_block_header().timestamp;

                peer->ids_of_items_being_processed.erase(items_being_processed_iter);
                if (peer->idle())
                {
                  // if we know the peer has more block ids and we haven't filled our id buffer, get more ids
                  if (peer->number_of_unfetched_item_ids != 0 && peer->ids_of_items_to_get.size() < GRAPHENE_NET_MIN_BLOCK_IDS_TO_PREFETCH)
                    peers_to_fetch_ids_from.insert(peer);
                  // if we've processed all blocks from peer that we know about, ask again to find out if more and to tell them we may be in sync now
                  else if (peer->ids_of_items_to_get.empty() && peer->ids_of_items_being_processed.empty() && peer->number_of_unfetched_item_ids == 0)
                    peers_to_fetch_ids_from.insert(peer);
                }
              }
            }
          }

        } //for active peers
      }
      else //client did not accept block
      {
        fc::exception disconnect_exception(FC_LOG_MESSAGE(error, "You offered us a block that we reject as invalid.  The invalid block was number: ${block_number} with hash ${block_id}",
                                                          (block_number)(block_id)));
        // invalid message received
        for (const peer_connection_ptr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

          if (peer->ids_of_items_being_processed.find(block_id) != peer->ids_of_items_being_processed.end())
          {
            if (discontinue_fetching_blocks_from_peer)
            {
              wlog("inhibiting fetching sync blocks from peer ${endpoint} because it is on a fork that's too old",
                   ("endpoint", peer->get_remote_endpoint()));
              peer->inhibit_fetching_sync_blocks = true;
            }
            else
              peers_to_disconnect[peer] = std::make_pair(std::string("You offered us a block that we reject as invalid"), disconnect_exception);
          }
        }
      }

      for (auto& peer_to_disconnect : peers_to_disconnect)
      {
        const peer_connection_ptr& peer = peer_to_disconnect.first;
        std::string reason_string;
        fc::oexception reason_exception;
        std::tie(reason_string, reason_exception) = peer_to_disconnect.second;
        wlog("disconnecting client ${endpoint} because it offered us the rejected block", ("endpoint", peer->get_remote_endpoint()));
        disconnect_from_peer(peer.get(), reason_string, true, reason_exception);
      }

      for (const peer_connection_ptr& peer : peers_to_fetch_ids_from)
        fetch_next_batch_of_item_ids_from_peer(peer.get());

      for (const peer_connection_ptr& peer : peers_we_need_to_sync_to)
        start_synchronizing_with_peer(peer);

      dlog("Leaving send_sync_block_to_node_delegate");

      if ( !node_is_shutting_down() &&
          (!_process_backlog_of_sync_blocks_done.valid() || _process_backlog_of_sync_blocks_done.ready()) &&
          get_number_of_handle_message_calls_in_progress() < _node_configuration.maximum_number_of_blocks_to_handle_at_one_time / 2 //DLN add hystersis to reduce number of times we process_backlog_of_sync_blocks
         )
        _process_backlog_of_sync_blocks_done = async_task([=](){ process_backlog_of_sync_blocks(); }, "process_backlog_of_sync_blocks");
    }

    uint32_t node_impl::get_number_of_handle_message_calls_in_progress()
    {
      // garbage-collect the list of async tasks before we count them
      for (auto calls_iter = _handle_message_calls_in_progress.begin();
            calls_iter != _handle_message_calls_in_progress.end();)
      {
        if (calls_iter->ready())
          calls_iter = _handle_message_calls_in_progress.erase(calls_iter);
        else
          ++calls_iter;
      }
      return _handle_message_calls_in_progress.size();
    }
    
    void node_impl::discard_abandoned_sync_blocks()
    {
      std::set<block_id_type> block_ids_still_referenced;
      for (const peer_connection_ptr& peer : _active_connections)
        for (const block_id_type& block_id : peer->ids_of_items_to_get)
          block_ids_still_referenced.insert(block_id);
      unsigned discarded_count = 0;
      for (auto iter = _received_sync_items.begin(); iter != _received_sync_items.end();)
      {
        auto block_id = (*iter)->get_block_id();
        if (block_ids_still_referenced.find(block_id) == block_ids_still_referenced.end())
        {
          _already_received_sync_item_ids.erase(block_id);
          iter = _received_sync_items.erase(iter);
          ++discarded_count;
        }
        else
          ++iter;
      }
      // fc_ilog(fc::logger::get("default"), "Currently discarding ${discarded_count} unreferenced sync blocks", (discarded_count));
    }

    void node_impl::process_backlog_of_sync_blocks()
    {
      dlog("in process_backlog_of_sync_blocks");
      if (get_number_of_handle_message_calls_in_progress() >= _node_configuration.maximum_number_of_blocks_to_handle_at_one_time)
      {
        dlog("leaving process_backlog_of_sync_blocks because we've already sent blockchain enough blocks");
        return; // we will be rescheduled when the next block finishes its processing
      }
      dlog("currently ${count} blocks in the process of being handled", ("count", _handle_message_calls_in_progress.size()));

      if (_suspend_fetching_sync_blocks)
      {
        dlog("resuming processing sync block backlog because we only ${count} blocks in progress",
             ("count", _handle_message_calls_in_progress.size()));
        _suspend_fetching_sync_blocks = false;
      }

      // when syncing with multiple peers, it's possible that we'll have hundreds of blocks ready to push
      // to the client at once.  This can be slow, and we need to limit the number we push at any given
      // time to allow network traffic to continue so we don't end up disconnecting from peers
      //fc::time_point start_time = fc::time_point::now();
      //fc::time_point when_we_should_yield = start_time + fc::seconds(1);

      bool block_processed_this_iteration;
      unsigned blocks_processed = 0;

      std::set<peer_connection_ptr> peers_to_fetch_ids_from;
      std::set<peer_connection_ptr> peers_we_need_to_sync_to;
      std::map<peer_connection_ptr, fc::oexception> peers_with_rejected_block;

      do
      {
        std::copy(std::make_move_iterator(_new_received_sync_items.begin()), std::make_move_iterator(_new_received_sync_items.end()),
                  std::inserter(_received_sync_items, _received_sync_items.end()));
        _new_received_sync_items.clear();
        dlog("currently ${count} sync items to consider", ("count", _received_sync_items.size()));

        block_processed_this_iteration = false;
        for (const peer_connection_ptr& peer : _active_connections)
        {
          activity_tracer aTracer(__FUNCTION__, *this);

          if (!peer->ids_of_items_to_get.empty())
          {
            // check if we have a backlog block to push
            auto received_block_iter = _received_sync_items.find(peer->ids_of_items_to_get.front());
            if (received_block_iter != _received_sync_items.end())
            {
              const block_id_type& received_block_id = (*received_block_iter)->get_block_id();
              // remove from front of any other peer's item to get list
              for (const peer_connection_ptr& peer : _active_connections)
              {
                ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections
                if (!peer->ids_of_items_to_get.empty() &&
                    peer->ids_of_items_to_get.front() == received_block_id)
                {
                  peer->ids_of_items_to_get.pop_front();
                  ++peer->first_id_block_number; //since we popped front, we are at a higher block number
                  peer->ids_of_items_being_processed.insert(received_block_id);
                  //if we've fetched blocks for all ids we have from this peer, lets fetch more if possible (we need this to avoid stall condition)
                  if (peer->ids_of_items_to_get.empty() && 
                      peer->number_of_unfetched_item_ids != 0 &&
                      peer->we_need_sync_items_from_peer)
                    peers_to_fetch_ids_from.insert(peer);
                }
              }  
              // dlog("removed from front of any other peer");

              // we can get into an interesting situation near the end of synchronization.  We can be in
              // sync with one peer who is sending us the last block on the chain via a regular inventory
              // message, while at the same time still be synchronizing with a peer who is sending us the
              // block through the sync mechanism.  Further, we must request both blocks because
              // we don't know they're the same (for the peer in normal operation, it has only told us the
              // message id, for the peer in the sync case we only known the block_id).
              if (std::find(_most_recent_blocks_accepted.begin(), _most_recent_blocks_accepted.end(), received_block_id) == _most_recent_blocks_accepted.end())
              {
                // normal case, schedule this block to be sent to blockchain
                _already_received_sync_item_ids.erase(received_block_id);

                _handle_message_calls_in_progress.push_back(async_task([this, full_block=*received_block_iter](){
                  send_sync_block_to_node_delegate(full_block);
                }, "send_sync_block_to_node_delegate"));
                
                _received_sync_items.erase(received_block_iter);

                ++blocks_processed;
                block_processed_this_iteration = true;
              }
              else
              {
                dlog("Already received and accepted this block through normal inventory mechanism, treating it as accepted");
                _already_received_sync_item_ids.erase(received_block_id);
                _received_sync_items.erase(received_block_iter);
                for (const peer_connection_ptr& peer : _active_connections)
                {
                  auto items_being_processed_iter = peer->ids_of_items_being_processed.find(received_block_id);
                  if (items_being_processed_iter != peer->ids_of_items_being_processed.end())
                  {
                    peer->ids_of_items_being_processed.erase(items_being_processed_iter);
                    dlog("Removed item from ${endpoint}'s list of items being processed, still processing ${len} blocks",
                         ("endpoint", peer->get_remote_endpoint())("len", peer->ids_of_items_being_processed.size()));
                  }
                }
              }
              break; //restart checking front of peers' item lists because we've popped the first item from some of them
            } //if we have a backlog block to push
          } //if peer item list not empty 
        } //for active peers, check if we can push their first block
                
        if (_handle_message_calls_in_progress.size() >= _node_configuration.maximum_number_of_blocks_to_handle_at_one_time)
        {
          dlog("stopping processing sync block backlog because we have ${count} blocks sent to the blockchain",
               ("count", _handle_message_calls_in_progress.size()));
          //ulog("stopping processing sync block backlog because we have ${count} blocks in progress, total on hand: ${received}",
          //     ("count", _handle_message_calls_in_progress.size())("received", _received_sync_items.size()));
          if (_received_sync_items.size() >= _node_configuration.maximum_number_of_sync_blocks_to_prefetch)
          {
            _suspend_fetching_sync_blocks = true;
            dlog("stopped prefetching sync blocks because we have backlog of ${received} blocks to consider", ("received", _received_sync_items.size()));
          }
          break;
        }
      } 
      while (block_processed_this_iteration);

#if 0
      //report if we have any peers that haven't given us the next block we need to process the backlog of received blocks
      if (_handle_message_calls_in_progress.size() < _node_configuration.maximum_number_of_blocks_to_handle_at_one_time &&
          _received_sync_items.size() > 1000)
      {
        std::set<item_hash_t> all_potential_first_items;
        for (const peer_connection_ptr& peer : _active_connections)
          if (!peer->ids_of_items_to_get.empty())
            all_potential_first_items.insert(peer->ids_of_items_to_get.front());
        for (const item_hash_t& id : all_potential_first_items)
          for (const peer_connection_ptr& peer : _active_connections)
            if (peer->sync_items_requested_from_peer.find(id) != peer->sync_items_requested_from_peer.end())
              wlog("We're waiting on a block from ${peer} (requested ${count} blocks from them, last received one ${time_since_last_block_received}ms ago)", 
                   ("peer", peer->get_remote_endpoint())("count", peer->sync_items_requested_from_peer.size())
                   ("time_since_last_block_received", (fc::time_point::now() - peer->last_sync_item_received_time).count() / 1000));
      }
#endif
      for (const peer_connection_ptr& peer : peers_to_fetch_ids_from)
        fetch_next_batch_of_item_ids_from_peer(peer.get());

      if (!_suspend_fetching_sync_blocks)
        trigger_fetch_sync_items_loop();
      dlog("leaving process_backlog_of_sync_blocks, ${count} processed", ("count", blocks_processed));
    }

    void node_impl::trigger_process_backlog_of_sync_blocks()
    {
      if (!node_is_shutting_down() &&
          (!_process_backlog_of_sync_blocks_done.valid() || _process_backlog_of_sync_blocks_done.ready()))
        _process_backlog_of_sync_blocks_done = async_task([=](){ process_backlog_of_sync_blocks(); }, "process_backlog_of_sync_blocks");
    }

    void node_impl::process_block_during_sync(peer_connection* originating_peer, const std::shared_ptr<full_block_type>& full_block)
    {
      dlog("start process_block_during_sync");
      // add it to the front of _received_sync_items, then process _received_sync_items to try to
      // pass as many messages as possible to the client.
      _new_received_sync_items.push_front(full_block);
      _already_received_sync_item_ids.insert(full_block->get_block_id());
      trigger_process_backlog_of_sync_blocks();
    }

    void node_impl::process_block_during_normal_operation(peer_connection *originating_peer, const std::shared_ptr<full_block_type>& full_block)
    {
      // we'll probably want to refer to these a lot
      const uint32_t block_num = full_block->get_block_num();
      const block_id_type& block_id = full_block->get_block_id();

      // TODO: this call to get_legacy_block_message_hash() is forcing the p2p thread to do an expensive (100-200μs) hash computation early
      // in the block processing, before a blockchain worker thread has a chance to offload the work for us. 
      // After the next hf, when we can expect all peers to be on protocol GRAPHENE_NET_PROTOCOL_ADVERTISE_BLOCKS_BY_BLOCK_ID_VERSION,
      // get rid of this to reduce block processing latency
      const fc::ripemd160& legacy_block_message_hash = full_block->get_legacy_block_message_hash();

      fc::time_point message_receive_time = fc::time_point::now();
      dlog("received a block from peer ${endpoint}, passing it to client", ("endpoint", originating_peer->get_remote_endpoint()));
      std::set<peer_connection_ptr> peers_to_disconnect;
      std::string disconnect_reason;
      fc::oexception disconnect_exception;
      fc::oexception restart_sync_exception;
      try
      {
        // we can get into an intersting situation near the end of synchronization.  We can be in
        // sync with one peer who is sending us the last block on the chain via a regular inventory
        // message, while at the same time still be synchronizing with a peer who is sending us the
        // block through the sync mechanism.  Further, we must request both blocks because
        // we don't know they're the same (for the peer in normal operation, it has only told us the
        // message id, for the peer in the sync case we only known the block_id).
        fc::time_point message_validated_time;
        bool block_has_been_accepted = std::find(_most_recent_blocks_accepted.begin(), _most_recent_blocks_accepted.end(), block_id) != _most_recent_blocks_accepted.end();
        if (!block_has_been_accepted &&
            _message_ids_currently_being_processed.find(block_id) == _message_ids_currently_being_processed.end())
        {
          _message_ids_currently_being_processed.insert(legacy_block_message_hash);
          _message_ids_currently_being_processed.insert(block_id);
          BOOST_SCOPE_EXIT(this_, &legacy_block_message_hash, &block_id) {
            this_->_message_ids_currently_being_processed.erase(legacy_block_message_hash);
            this_->_message_ids_currently_being_processed.erase(block_id);
          } BOOST_SCOPE_EXIT_END
          fc_ilog(fc::logger::get("sync"),
                  "p2p pushing block #${block_num} ${block_id} from ${peer} (message_id was ${legacy_block_message_hash})",
                  (block_num)(block_id)("peer", originating_peer->get_remote_endpoint())(legacy_block_message_hash));
          _delegate->handle_block(full_block, false);
          message_validated_time = fc::time_point::now();
          ilog("Successfully pushed block ${block_num} (id:${block_id})", (block_num)(block_id));
          _most_recent_blocks_accepted.push_back(block_id);

          // if we were planning on fetching any of the transactions that we just accepted in the block,
          // well, we already got them, so remove those from our items_to_fetch
          for (const std::shared_ptr<full_transaction_type>& full_transaction : full_block->get_full_transactions())
            _items_to_fetch.get<item_id_index>().erase(item_id(trx_message_type, full_transaction->get_legacy_transaction_message_hash()));
        }
        else
        {
          fc_ilog(fc::logger::get("sync"),
                  "p2p NOT pushing block #${block_num} ${block_hash} from ${peer} because we recently pushed it",
                  ("block_num", block_num)("block_hash", block_id)
                  ("peer", originating_peer->get_remote_endpoint())("id", legacy_block_message_hash));
          dlog("Already received and accepted this block (presumably through sync mechanism), treating it as accepted");
          if (!block_has_been_accepted) //if we haven't finished processing this block yet, don't talk to peers about it
            return;
        }

        dlog("client validated the block, advertising it to other peers");

        item_id legacy_block_message_item_id(core_message_type_enum::block_message_type, legacy_block_message_hash);
        item_id new_block_message_item_id(core_message_type_enum::block_message_type, block_id);
        fc::time_point_sec block_time = full_block->get_block_header().timestamp;

        for (const peer_connection_ptr& peer : _active_connections)
        {
          ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

          auto iter = peer->inventory_peer_advertised_to_us.find(peer->advertise_blocks_by_block_id() ? new_block_message_item_id : legacy_block_message_item_id);
          if (iter != peer->inventory_peer_advertised_to_us.end())
          {
            // this peer offered us the item.  It will eventually expire from the peer's
            // inventory_peer_advertised_to_us list after some time has passed (currently 2 minutes).
            // For now, it will remain there, which will prevent us from offering the peer this
            // block back when we rebroadcast the block below
            peer->last_block_delegate_has_seen = block_id;
            peer->last_block_time_delegate_has_seen = block_time;
          }
          peer->clear_old_inventory();
        }
        message_propagation_data propagation_data{message_receive_time, message_validated_time, originating_peer->node_id};
        broadcast(full_block, propagation_data);

        if (is_hard_fork_block(block_num))
        {
          // we just pushed a hard fork block.  Find out if any of our peers are running clients
          // that will be unable to process future blocks
          for (const peer_connection_ptr& peer : _active_connections)
          {
            if (peer->last_known_fork_block_number != 0)
            {
              uint32_t next_fork_block_number = get_next_known_hard_fork_block_number(peer->last_known_fork_block_number);
              if (next_fork_block_number != 0 && next_fork_block_number <= block_num)
              {
                peers_to_disconnect.insert(peer);
#ifdef ENABLE_DEBUG_ULOGS
                ulog("Disconnecting from peer because their version is too old.  Their version date: ${date}", ("date", peer->graphene_git_revision_unix_timestamp));
#endif
              }
            }
          }
          if (!peers_to_disconnect.empty())
          {
            std::ostringstream disconnect_reason_stream;
            disconnect_reason_stream << "You need to upgrade your client due to hard fork at block " << block_num;
            disconnect_reason = disconnect_reason_stream.str();
            disconnect_exception = fc::exception(FC_LOG_MESSAGE(error, "You need to upgrade your client due to hard fork at block ${block_num}", (block_num)));
          }
        }
      }
      catch (const fc::canceled_exception&)
      {
        throw;
      }
      catch (const unlinkable_block_exception& e)
      {
        restart_sync_exception = e;
      }
      catch (const fc::exception& e)
      {
        // client rejected the block.  Disconnect the client and any other clients that offered us this block
        wlog("Failed to push block ${num} (id:${id}), client rejected block sent by peer",
             ("num", block_num)
             ("id", block_id));

        disconnect_exception = e;
        disconnect_reason = "You offered me a block that I have deemed to be invalid";

        peers_to_disconnect.insert( originating_peer->shared_from_this() );
        for (const peer_connection_ptr& peer : _active_connections)
          if (!peer->ids_of_items_to_get.empty() && peer->ids_of_items_to_get.front() == block_id)
            peers_to_disconnect.insert(peer);
      }

      if (restart_sync_exception)
      {
        wlog("Peer ${peer} sent me a block that didn't link to our blockchain.  Restarting sync mode with them to get the missing block. "
             "Error pushing block was: ${e}",
             ("peer", originating_peer->get_remote_endpoint())
             ("e", *restart_sync_exception));
        start_synchronizing_with_peer(originating_peer->shared_from_this());
      }

      for (const peer_connection_ptr& peer : peers_to_disconnect)
      {
        wlog("disconnecting client ${endpoint} because it offered us the rejected block", ("endpoint", peer->get_remote_endpoint()));
        disconnect_from_peer(peer.get(), disconnect_reason, true, disconnect_exception);
      }
    }

    void node_impl::process_block_message(peer_connection* originating_peer,
                                          const message& message_to_process,
                                          const message_hash_type& actual_message_hash)
    {
      VERIFY_CORRECT_THREAD();
      // keep the peer from being deleted until we're done processing this message
      peer_connection_ptr peer = originating_peer->shared_from_this();

      // the message may be either a uncompressed or a compressed block message.  Either way
      // we just want to convert it to a full_block
      std::shared_ptr<full_block_type> full_block;
      fc::optional<message_hash_type> legacy_message_hash;
      switch (message_to_process.msg_type)
      {
      case core_message_type_enum::block_message_type:
        {
          graphene::net::block_message message(message_to_process.as_block_message( _thread_pool ));
          full_block = message.full_block;
          legacy_message_hash = actual_message_hash;
          // remove this check, just needed during initial development
          ++originating_peer->uncompressed_blocks_received_from_peer;
          break;
        }
      case core_message_type_enum::compressed_block_message_type:
        {
          graphene::net::compressed_block_message message(message_to_process.as_compressed_block_message( _thread_pool ));
          full_block = message.full_block;
          ++originating_peer->compressed_blocks_received_from_peer;
          break;
        }
      default:
        FC_THROW("unrecognized block message type ${type}", ("type", message_to_process.msg_type));
      }

      // find out whether we requested this item while we were synchronizing or during normal operation
      // (it's possible that we request an item during normal operation and then get kicked into sync
      // mode before we receive and process the item.  In that case, we should process the item as a normal
      // item to avoid confusing the sync code)
      auto sync_item_iter = originating_peer->sync_items_requested_from_peer.find(full_block->get_block_id());
      if (sync_item_iter != originating_peer->sync_items_requested_from_peer.end())
      {
        // it's a sync block
        originating_peer->sync_items_requested_from_peer.erase(sync_item_iter);
        // if exceptions are throw here after removing the sync item from the list (above),
        // it could leave our sync in a stalled state.  Wrap a try/catch around the rest
        // of the function so we can log if this ever happens.
        try
        {
          originating_peer->last_sync_item_received_time = fc::time_point::now();
          _active_sync_requests.erase(full_block->get_block_id());
          process_block_during_sync(originating_peer, full_block);
          if (originating_peer->idle())
          {
            // we have finished fetching a batch of items, so we either need to grab another batch of items
            // or we need to get another list of item ids.
            if (originating_peer->number_of_unfetched_item_ids > 0 &&
                originating_peer->ids_of_items_to_get.size() < GRAPHENE_NET_MIN_BLOCK_IDS_TO_PREFETCH)
              fetch_next_batch_of_item_ids_from_peer(originating_peer);
            else
              trigger_fetch_sync_items_loop();
          }
          return;
        }
        catch (const fc::canceled_exception& e)
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          elog("Caught unexpected exception: ${e}", ("e", e));
          assert(false && "exceptions not expected here");
        }
        catch (const std::exception& e)
        {
          elog("Caught unexpected exception: ${e}", ("e", e.what()));
          assert(false && "exceptions not expected here");
        }
        catch (...)
        {
          elog("Caught unexpected exception, could break sync operation");
        }
      }
      else 
      {
        // it wasn't a sync block, check and see if it was sent during live mode.
        
        // old peers will advertise blocks using the message hash during live mode,
        // newer clients will use the block_id instead.
        fc::ripemd160 id_used_for_live_mode_blocks;
        if (originating_peer->advertise_blocks_by_block_id())
          id_used_for_live_mode_blocks = full_block->get_block_id();
        else if (legacy_message_hash)
          id_used_for_live_mode_blocks = *legacy_message_hash;
        else
          id_used_for_live_mode_blocks = full_block->get_legacy_block_message_hash();

        auto item_iter = originating_peer->items_requested_from_peer.find(item_id(graphene::net::block_message_type, id_used_for_live_mode_blocks));
        if (item_iter != originating_peer->items_requested_from_peer.end())
        {
          originating_peer->items_requested_from_peer.erase(item_iter);
          process_block_during_normal_operation(originating_peer, full_block);
          if (originating_peer->idle())
            trigger_fetch_items_loop();
          return;
        }
      }

      // if we get here, we didn't request the message, we must have a misbehaving peer
      wlog("received a block ${block_id} I didn't ask for from peer ${endpoint}, disconnecting from peer",
           ("endpoint", originating_peer->get_remote_endpoint())
           ("block_id", full_block->get_block_id()));
      fc::exception detailed_error(FC_LOG_MESSAGE(error, "You sent me a block that I didn't ask for, block_id: ${block_id}",
                                                  ("block_id", full_block->get_block_id())
                                                  ("graphene_git_revision_sha", originating_peer->graphene_git_revision_sha)
                                                  ("graphene_git_revision_unix_timestamp", originating_peer->graphene_git_revision_unix_timestamp)
                                                  ("fc_git_revision_sha", originating_peer->fc_git_revision_sha)
                                                  ("fc_git_revision_unix_timestamp", originating_peer->fc_git_revision_unix_timestamp)));
      disconnect_from_peer(originating_peer, "You sent me a block that I didn't ask for", true, detailed_error);
    }

    void node_impl::process_trx_message(peer_connection* originating_peer,
                                        const trx_message& transaction_message_to_process)
    {
      VERIFY_CORRECT_THREAD();
      // keep the peer from being deleted until we're done processing this message
      peer_connection_ptr peer = originating_peer->shared_from_this();
      fc::time_point message_receive_time = fc::time_point::now();

      std::shared_ptr<full_transaction_type> full_transaction = transaction_message_to_process.full_transaction;

      // only process it if we asked for it
      const fc::ripemd160& message_hash = full_transaction->get_legacy_transaction_message_hash();
      auto iter = originating_peer->items_requested_from_peer.find(item_id(trx_message_type, message_hash));
      if (iter == originating_peer->items_requested_from_peer.end())
      {
        wlog("received a transaction message I didn't ask for from peer ${endpoint}, disconnecting from peer", ("endpoint", originating_peer->get_remote_endpoint()));
        fc::exception detailed_error(FC_LOG_MESSAGE(error, "You sent me a message that I didn't ask for, message_hash: ${message_hash}", (message_hash)));
        disconnect_from_peer(originating_peer, "You sent me a message that I didn't request", true, detailed_error);
        return;
      }
      else
      {
        originating_peer->items_requested_from_peer.erase(iter);
        if (originating_peer->idle())
          trigger_fetch_items_loop();

        // Next: have the delegate process the message
        fc::time_point message_validated_time;
        try
        {
          dlog("passing message containing transaction ${trx} to client", ("trx", full_transaction->get_transaction_id()));
          _message_ids_currently_being_processed.insert(message_hash);
          _delegate->handle_transaction(full_transaction);
          _message_ids_currently_being_processed.erase(message_hash);
          message_validated_time = fc::time_point::now();
        }
        catch (const fc::canceled_exception&)
        {
          throw;
        }
        catch (const fc::exception& e)
        {
          dlog("client rejected transaction message sent by peer ${peer}, ${e}", ("peer", originating_peer->get_remote_endpoint())(e));
          // record it so we don't try to fetch this item again
          _recently_failed_items.insert(peer_connection::timestamped_item_id(item_id(trx_message_type, message_hash), fc::time_point::now()));
          return;
        }
        catch (...)
        {
          elog("Caught unexpected unknown exception in process_ordinary_message"); //we don't expect this to happen
          _recently_failed_items.insert(peer_connection::timestamped_item_id(item_id(trx_message_type, message_hash), fc::time_point::now()));
          return;
        }

        // finally, if the delegate validated the message, broadcast it to our other peers
        message_propagation_data propagation_data{message_receive_time, message_validated_time, originating_peer->node_id};
        broadcast(full_transaction, propagation_data);
      }
    }

    void node_impl::on_current_time_request_message(peer_connection* originating_peer,
                                                    const current_time_request_message& current_time_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point request_received_time(fc::time_point::now());
      current_time_reply_message reply(current_time_request_message_received.request_sent_time,
                                       request_received_time);
      originating_peer->send_message(reply, offsetof(current_time_reply_message, reply_transmitted_time));
    }

    void node_impl::on_current_time_reply_message(peer_connection* originating_peer,
                                                  const current_time_reply_message& current_time_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();
      fc::time_point reply_received_time = fc::time_point::now();
      originating_peer->clock_offset = fc::microseconds(((current_time_reply_message_received.request_received_time - current_time_reply_message_received.request_sent_time) +
                                                         (current_time_reply_message_received.reply_transmitted_time - reply_received_time)).count() / 2);
      originating_peer->round_trip_delay = (reply_received_time - current_time_reply_message_received.request_sent_time) -
                                           (current_time_reply_message_received.reply_transmitted_time - current_time_reply_message_received.request_received_time);
    }

    void node_impl::forward_firewall_check_to_next_available_peer(firewall_check_state_data* firewall_check_state)
    {
      for (const peer_connection_ptr& peer : _active_connections)
      {
        if (firewall_check_state->expected_node_id != peer->node_id && // it's not the node who is asking us to test
            !peer->firewall_check_state && // the peer isn't already performing a check for another node
            firewall_check_state->nodes_already_tested.find(peer->node_id) == firewall_check_state->nodes_already_tested.end() &&
            peer->core_protocol_version >= GRAPHENE_NET_PROTOCOL_FIREWALL_CHECK_VERSION)
        {
          wlog("forwarding firewall check for node ${to_check} to peer ${checker}",
               ("to_check", firewall_check_state->endpoint_to_test)
               ("checker", peer->get_remote_endpoint()));
          firewall_check_state->nodes_already_tested.insert(peer->node_id);
          peer->firewall_check_state = firewall_check_state;
          check_firewall_message check_request;
          check_request.endpoint_to_check = firewall_check_state->endpoint_to_test;
          check_request.node_id = firewall_check_state->expected_node_id;
          peer->send_message(check_request);
          return;
        }
      }
      wlog("Unable to forward firewall check for node ${to_check} to any other peers, returning 'unable'",
           ("to_check", firewall_check_state->endpoint_to_test));

      peer_connection_ptr originating_peer = get_peer_by_node_id(firewall_check_state->expected_node_id);
      if (originating_peer)
      {
        check_firewall_reply_message reply;
        reply.node_id = firewall_check_state->expected_node_id;
        reply.endpoint_checked = firewall_check_state->endpoint_to_test;
        reply.result = firewall_check_result::unable_to_check;
        originating_peer->send_message(reply);
      }
      delete firewall_check_state;
    }

    void node_impl::on_check_firewall_message(peer_connection* originating_peer,
                                              const check_firewall_message& check_firewall_message_received)
    {
      VERIFY_CORRECT_THREAD();

      if (check_firewall_message_received.node_id == node_id_t() &&
          check_firewall_message_received.endpoint_to_check == fc::ip::endpoint())
      {
        // originating_peer is asking us to test whether it is firewalled
        // we're not going to try to connect back to the originating peer directly,
        // instead, we're going to coordinate requests by asking some of our peers
        // to try to connect to the originating peer, and relay the results back
        wlog("Peer ${peer} wants us to check whether it is firewalled", ("peer", originating_peer->get_remote_endpoint()));
        firewall_check_state_data* firewall_check_state = new firewall_check_state_data;
        // if they are using the same inbound and outbound port, try connecting to their outbound endpoint.
        // if they are using a different inbound port, use their outbound address but the inbound port they reported
        fc::ip::endpoint endpoint_to_check = originating_peer->get_socket().remote_endpoint();
        if (originating_peer->inbound_port != originating_peer->outbound_port)
          endpoint_to_check = fc::ip::endpoint(endpoint_to_check.get_address(), originating_peer->inbound_port);
        firewall_check_state->endpoint_to_test = endpoint_to_check;
        firewall_check_state->expected_node_id = originating_peer->node_id;
        firewall_check_state->requesting_peer = originating_peer->node_id;

        forward_firewall_check_to_next_available_peer(firewall_check_state);
      }
      else
      {
        // we're being asked to check another node
        // first, find out if we're currently connected to that node.  If we are, we
        // can't perform the test
        if (is_already_connected_to_id(check_firewall_message_received.node_id) ||
            is_connection_to_endpoint_in_progress(check_firewall_message_received.endpoint_to_check))
        {
          check_firewall_reply_message reply;
          reply.node_id = check_firewall_message_received.node_id;
          reply.endpoint_checked = check_firewall_message_received.endpoint_to_check;
          reply.result = firewall_check_result::unable_to_check;
        }
        else
        {
          // we're not connected to them, so we need to set up a connection to them
          // to test.
          peer_connection_ptr peer_for_testing(peer_connection::make_shared(this));
          peer_for_testing->firewall_check_state = new firewall_check_state_data;
          peer_for_testing->firewall_check_state->endpoint_to_test = check_firewall_message_received.endpoint_to_check;
          peer_for_testing->firewall_check_state->expected_node_id = check_firewall_message_received.node_id;
          peer_for_testing->firewall_check_state->requesting_peer = originating_peer->node_id;
          peer_for_testing->set_remote_endpoint(check_firewall_message_received.endpoint_to_check);
          initiate_connect_to(peer_for_testing);
        }
      }
    }

    void node_impl::on_check_firewall_reply_message(peer_connection* originating_peer,
                                                    const check_firewall_reply_message& check_firewall_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();

      if (originating_peer->firewall_check_state &&
          originating_peer->firewall_check_state->requesting_peer != node_id_t())
      {
        // then this is a peer that is helping us check the firewalled state of one of our other peers
        // and they're reporting back
        // if they got a result, return it to the original peer.  if they were unable to check,
        // we'll try another peer.
        wlog("Peer ${reporter} reports firewall check status ${status} for ${peer}",
             ("reporter", originating_peer->get_remote_endpoint())
             ("status", check_firewall_reply_message_received.result)
             ("peer", check_firewall_reply_message_received.endpoint_checked));

        if (check_firewall_reply_message_received.result == firewall_check_result::unable_to_connect ||
            check_firewall_reply_message_received.result == firewall_check_result::connection_successful)
        {
          peer_connection_ptr original_peer = get_peer_by_node_id(originating_peer->firewall_check_state->requesting_peer);
          if (original_peer)
          {
            if (check_firewall_reply_message_received.result == firewall_check_result::connection_successful)
            {
              // if we previously thought this peer was firewalled, mark them as not firewalled
              if (original_peer->is_firewalled != firewalled_state::not_firewalled)
              {

                original_peer->is_firewalled = firewalled_state::not_firewalled;
                // there should be no old entry if we thought they were firewalled, so just create a new one
                fc::optional<fc::ip::endpoint> endpoint_for_db = originating_peer->get_endpoint_for_db();
                if (endpoint_for_db)
                {
                  potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(*endpoint_for_db);
                  updated_peer_record.last_seen_time = fc::time_point::now();
                  _potential_peer_db.update_entry(updated_peer_record);
                }
              }
            }
            original_peer->send_message(check_firewall_reply_message_received);
          }
          delete originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
        }
        else
        {
          // they were unable to check for us, ask another peer
          firewall_check_state_data* firewall_check_state = originating_peer->firewall_check_state;
          originating_peer->firewall_check_state = nullptr;
          forward_firewall_check_to_next_available_peer(firewall_check_state);
        }
      }
      else if (originating_peer->firewall_check_state)
      {
        // this is a reply to a firewall check we initiated.
        wlog("Firewall check we initiated has returned with result: ${result}, endpoint = ${endpoint}",
             ("result", check_firewall_reply_message_received.result)
             ("endpoint", check_firewall_reply_message_received.endpoint_checked));
        if (check_firewall_reply_message_received.result == firewall_check_result::connection_successful)
        {
          _is_firewalled = firewalled_state::not_firewalled;
          _publicly_visible_listening_endpoint = check_firewall_reply_message_received.endpoint_checked;
        }
        else if (check_firewall_reply_message_received.result == firewall_check_result::unable_to_connect)
        {
          _is_firewalled = firewalled_state::firewalled;
          _publicly_visible_listening_endpoint = fc::optional<fc::ip::endpoint>();
        }
        delete originating_peer->firewall_check_state;
        originating_peer->firewall_check_state = nullptr;
      }
      else
      {
        wlog("Received a firewall check reply to a request I never sent");
      }

    }

    void node_impl::on_get_current_connections_request_message(peer_connection* originating_peer,
                                                               const get_current_connections_request_message& get_current_connections_request_message_received)
    {
      VERIFY_CORRECT_THREAD();
      get_current_connections_reply_message reply;

      if (!_average_network_read_speed_minutes.empty())
      {
        reply.upload_rate_one_minute = _average_network_write_speed_minutes.back();
        reply.download_rate_one_minute = _average_network_read_speed_minutes.back();

        size_t minutes_to_average = std::min(_average_network_write_speed_minutes.size(), (size_t)15);
        boost::circular_buffer<uint32_t>::iterator start_iter = _average_network_write_speed_minutes.end() - minutes_to_average;
        reply.upload_rate_fifteen_minutes = std::accumulate(start_iter, _average_network_write_speed_minutes.end(), 0u) / (uint32_t)minutes_to_average;
        start_iter = _average_network_read_speed_minutes.end() - minutes_to_average;
        reply.download_rate_fifteen_minutes = std::accumulate(start_iter, _average_network_read_speed_minutes.end(), 0u) / (uint32_t)minutes_to_average;

        minutes_to_average = std::min(_average_network_write_speed_minutes.size(), (size_t)60);
        start_iter = _average_network_write_speed_minutes.end() - minutes_to_average;
        reply.upload_rate_one_hour = std::accumulate(start_iter, _average_network_write_speed_minutes.end(), 0u) / (uint32_t)minutes_to_average;
        start_iter = _average_network_read_speed_minutes.end() - minutes_to_average;
        reply.download_rate_one_hour = std::accumulate(start_iter, _average_network_read_speed_minutes.end(), 0u) / (uint32_t)minutes_to_average;
      }

      fc::time_point now = fc::time_point::now();
      for (const peer_connection_ptr& peer : _active_connections)
      {
        ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

        current_connection_data data_for_this_peer;
        data_for_this_peer.connection_duration = now.sec_since_epoch() - peer->connection_initiation_time.sec_since_epoch();
        if (peer->get_remote_endpoint()) // should always be set for anyone we're actively connected to
          data_for_this_peer.remote_endpoint = *peer->get_remote_endpoint();
        data_for_this_peer.clock_offset = peer->clock_offset;
        data_for_this_peer.round_trip_delay = peer->round_trip_delay;
        data_for_this_peer.node_id = peer->node_id;
        data_for_this_peer.connection_direction = peer->direction;
        data_for_this_peer.firewalled = peer->is_firewalled;
        fc::mutable_variant_object user_data;
        if (peer->graphene_git_revision_sha)
          user_data["graphene_git_revision_sha"] = *peer->graphene_git_revision_sha;
        if (peer->graphene_git_revision_unix_timestamp)
          user_data["graphene_git_revision_unix_timestamp"] = *peer->graphene_git_revision_unix_timestamp;
        if (peer->fc_git_revision_sha)
          user_data["fc_git_revision_sha"] = *peer->fc_git_revision_sha;
        if (peer->fc_git_revision_unix_timestamp)
          user_data["fc_git_revision_unix_timestamp"] = *peer->fc_git_revision_unix_timestamp;
        if (peer->platform)
          user_data["platform"] = *peer->platform;
        if (peer->bitness)
          user_data["bitness"] = *peer->bitness;
        user_data["user_agent"] = peer->user_agent;

        user_data["last_known_block_hash"] = peer->last_block_delegate_has_seen;
        user_data["last_known_block_number"] = _delegate->get_block_number(peer->last_block_delegate_has_seen);
        user_data["last_known_block_time"] = peer->last_block_time_delegate_has_seen;

        data_for_this_peer.user_data = user_data;
        reply.current_connections.emplace_back(data_for_this_peer);
      }
      originating_peer->send_message(reply);
    }

    void node_impl::on_get_current_connections_reply_message(peer_connection* originating_peer,
                                                             const get_current_connections_reply_message& get_current_connections_reply_message_received)
    {
      VERIFY_CORRECT_THREAD();
    }


    // currently, there are no defined messages that would invoke this code, so this must be a message from a future version.
    // we shouldn't request items that we don't expect to be able to handle, so it must be unsolicited.
    void node_impl::process_ordinary_message(peer_connection* originating_peer,
                                             const message& message_to_process, const message_hash_type& message_hash)
    {
      VERIFY_CORRECT_THREAD();
      wlog("Received an unrecognized message type ${type} from peer ${endpoint}, ignoring it",
           ("type", message_to_process.msg_type)("endpoint", originating_peer->get_remote_endpoint()));
    }

    void node_impl::start_synchronizing_with_peer( const peer_connection_ptr& peer )
    {
      VERIFY_CORRECT_THREAD();
      peer->ids_of_items_to_get.clear();
      peer->number_of_unfetched_item_ids = 0;
      peer->we_need_sync_items_from_peer = true;
      peer->last_block_delegate_has_seen = item_hash_t();
      peer->last_block_time_delegate_has_seen = fc::time_point_sec::min();
      peer->inhibit_fetching_sync_blocks = false;
      fetch_next_batch_of_item_ids_from_peer( peer.get() );
    }

    void node_impl::start_synchronizing()
    {
      for( const peer_connection_ptr& peer : _active_connections )
        start_synchronizing_with_peer( peer );
    }

    void node_impl::new_peer_just_added( const peer_connection_ptr& peer )
    {
      VERIFY_CORRECT_THREAD();
      peer->send_message(current_time_request_message(),
                         offsetof(current_time_request_message, request_sent_time));
      start_synchronizing_with_peer( peer );
      if( _active_connections.size() != _last_reported_number_of_connections )
      {
        _last_reported_number_of_connections = (uint32_t)_active_connections.size();
        _delegate->connection_count_changed( _last_reported_number_of_connections );
      }
    }

    void node_impl::close()
    {
      VERIFY_CORRECT_THREAD();

      _node_is_shutting_down = true;

      try
      {
        save_active_peers_to_peer_database();
      }
      catch (const fc::exception &e)
      {
        wlog("Exception thrown while saving active peers to peer database, ignoring exception: ${e}", ("e", e));
      }
      catch (...)
      {
        wlog( "Exception thrown while saving active peers to peer database, ignoring exception" );
      }

      try
      {
        _potential_peer_db.close();
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while closing P2P peer database, ignoring exception: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while closing P2P peer database, ignoring exception" );
      }

      // First, stop accepting incoming network connections
      try
      {
        _tcp_server.close();
        dlog("P2P TCP server closed");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while closing P2P TCP server, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while closing P2P TCP server, ignoring" );
      }

      try
      {
        _accept_loop_complete.cancel_and_wait("node_impl::close()");
        dlog("P2P accept loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating P2P accept loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating P2P accept loop, ignoring" );
      }

      // terminate all of our long-running loops (these run continuously instead of rescheduling themselves)
      try
      {
        _p2p_network_connect_loop_done.cancel_and_wait("node_impl::close()");
        dlog("P2P connect loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("P2P connect loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating P2P connect loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating P2P connect loop, ignoring" );
      }

      try
      {
        _process_backlog_of_sync_blocks_done.cancel_and_wait("node_impl::close()");
        dlog("Process backlog of sync items task terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Process backlog of sync items task terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Process backlog of sync items task, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Process backlog of sync items task, ignoring" );
      }

      unsigned handle_message_call_count = 0;
      while( true )
      {
        auto it = _handle_message_calls_in_progress.begin();
        if( it == _handle_message_calls_in_progress.end() )
           break;
        if( it->ready() || it->error() || it->canceled() )
        {
           _handle_message_calls_in_progress.erase( it );
           continue;
        }
        ++handle_message_call_count;
        try
        {
          it->cancel_and_wait("node_impl::close()");
          dlog("handle_message call #${count} task terminated", ("count", handle_message_call_count));
        }
        catch ( const fc::canceled_exception& )
        {
          dlog("handle_message call #${count} task terminated", ("count", handle_message_call_count));
        }
        catch ( const fc::exception& e )
        {
          wlog("Exception thrown while terminating handle_message call #${count} task, ignoring: ${e}", ("e", e)("count", handle_message_call_count));
        }
        catch (...)
        {
          wlog("Exception thrown while terminating handle_message call #${count} task, ignoring",("count", handle_message_call_count));
        }
      }

      try
      {
        _fetch_sync_items_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_fetch_sync_items_loop();
        _fetch_sync_items_loop_done.wait();
        dlog("Fetch sync items loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Fetch sync items loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch sync items loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch sync items loop, ignoring" );
      }

      try
      {
        _fetch_item_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_fetch_items_loop();
        _fetch_item_loop_done.wait();
        dlog("Fetch items loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Fetch items loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch items loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch items loop, ignoring" );
      }

      try
      {
        _advertise_inventory_loop_done.cancel("node_impl::close()");
        // cancel() is currently broken, so we need to wake up the task to allow it to finish
        trigger_advertise_inventory_loop();
        _advertise_inventory_loop_done.wait();
        dlog("Advertise inventory loop terminated");
      }
      catch ( const fc::canceled_exception& )
      {
        dlog("Advertise inventory loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Advertise inventory loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Advertise inventory loop, ignoring" );
      }


      // Next, terminate our existing connections.  First, close all of the connections nicely.
      // This will close the sockets and may result in calls to our "on_connection_closing"
      // method to inform us that the connection really closed (or may not if we manage to cancel
      // the read loop before it gets an EOF).
      // operate off copies of the lists in case they change during iteration
      std::list<peer_connection_ptr> all_peers;
      boost::push_back(all_peers, _active_connections);
      boost::push_back(all_peers, _handshaking_connections);
      boost::push_back(all_peers, _closing_connections);

      do
      {
         ilog("Attempting to destroy connection on ${c} peers...", ("c", all_peers.size()));

         // and delete all of the peer_connection objects
         _active_connections.clear();
         _handshaking_connections.clear();
         _closing_connections.clear();

         for (const peer_connection_ptr& peer : all_peers)
         {
         try
         {
            peer->destroy_connection(__FUNCTION__);
         }
         catch ( const fc::exception& e )
         {
            wlog( "Exception thrown while closing peer connection, ignoring: ${e}", ("e", e) );
         }
         catch (...)
         {
            wlog( "Exception thrown while closing peer connection, ignoring" );
         }
         }

         all_peers.clear();
         boost::push_back(all_peers, _active_connections);
         boost::push_back(all_peers, _handshaking_connections);
         boost::push_back(all_peers, _closing_connections);
      }
      while(all_peers.empty() == false);

      {
#ifdef USE_PEERS_TO_DELETE_MUTEX
        fc::scoped_lock<fc::mutex> lock(_peers_to_delete_mutex);
#endif
        try
        {
          _delayed_peer_deletion_task_done.cancel_and_wait("node_impl::close()");
          dlog("Delayed peer deletion task terminated");
        }
        catch ( const fc::exception& e )
        {
          wlog( "Exception thrown while terminating Delayed peer deletion task, ignoring: ${e}", ("e", e) );
        }
        catch (...)
        {
          wlog( "Exception thrown while terminating Delayed peer deletion task, ignoring" );
        }
        _peers_to_delete.clear();
      }

      // Now that there are no more peers that can call methods on us, there should be no
      // chance for one of our loops to be rescheduled, so we can safely terminate all of
      // our loops now
      try
      {
        _terminate_inactive_connections_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Terminate inactive connections loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Terminate inactive connections loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Terminate inactive connections loop, ignoring" );
      }
      /// Clear container to immediately deallocate held peer_connection::ptr objects
      _terminating_connections.clear();

      try
      {
        _fetch_updated_peer_lists_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Fetch updated peer lists loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Fetch updated peer lists loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Fetch updated peer lists loop, ignoring" );
      }

      try
      {
        _bandwidth_monitor_loop_done.cancel_and_wait("node_impl::close()");
        dlog("Bandwidth monitor loop terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Bandwidth monitor loop, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Bandwidth monitor loop, ignoring" );
      }

      try
      {
        _dump_node_status_task_done.cancel_and_wait("node_impl::close()");
        dlog("Dump node status task terminated");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while terminating Dump node status task, ignoring: ${e}", ("e", e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while terminating Dump node status task, ignoring" );
      }
    } // node_impl::close()

    void node_impl::accept_connection_task( const peer_connection_ptr& new_peer )
    {
      new_peer->accept_connection(); // this blocks until the secure connection is fully negotiated
      send_hello_message(new_peer);
    }

    void node_impl::accept_loop()
    {
      while ( !_accept_loop_complete.canceled() )
      {
        peer_connection_ptr new_peer(peer_connection::make_shared(this));

        try
        {
          _tcp_server.accept( new_peer->get_socket() );
          new_peer->get_socket().set_receive_buffer_size(MAX_MESSAGE_SIZE);
          new_peer->get_socket().set_send_buffer_size(MAX_MESSAGE_SIZE);
          idump((new_peer->get_socket().get_no_delay()));
          new_peer->get_socket().set_no_delay(true);
          idump((new_peer->get_socket().get_no_delay()));
          ilog( "accepted inbound connection from ${remote_endpoint}", ("remote_endpoint", new_peer->get_socket().remote_endpoint() ) );
          if (node_is_shutting_down())
            return;
          new_peer->connection_initiation_time = fc::time_point::now();
          _handshaking_connections.insert( new_peer );
          _rate_limiter.add_tcp_socket( &new_peer->get_socket() );
          std::weak_ptr<peer_connection> new_weak_peer(new_peer);
          new_peer->accept_or_connect_task_done = async_task( [this, new_weak_peer]() {
            peer_connection_ptr new_peer(new_weak_peer.lock());
            assert(new_peer);
            if (!new_peer)
              return;
            accept_connection_task(new_peer);
          }, "accept_connection_task" );

          // limit the rate at which we accept connections to mitigate DOS attacks
          fc::usleep( fc::milliseconds(10) );
        } FC_CAPTURE_AND_LOG( () )
      }
    } // accept_loop()

    void node_impl::send_hello_message(const peer_connection_ptr& peer)
    {
      VERIFY_CORRECT_THREAD();
      peer->negotiation_status = peer_connection::connection_negotiation_status::hello_sent;

      fc::sha256::encoder shared_secret_encoder;
      fc::sha512 shared_secret = peer->get_shared_secret();
      shared_secret_encoder.write(shared_secret.data(), sizeof(shared_secret));
      fc::ecc::compact_signature signature = _node_configuration.private_key.sign_compact(shared_secret_encoder.result());

      // in the hello messsage, we send three things:
      //  ip address
      //  outbound port
      //  inbound port
      // The peer we're connecting to will assume we're firewalled if the
      // ip address and outbound port we send don't match the values it sees on its remote endpoint
      //
      // if we know that we're behind a NAT that will allow incoming connections because our firewall
      // detection figured it out, send those values instead.

      fc::ip::endpoint local_endpoint(peer->get_socket().local_endpoint());
      uint16_t listening_port = _node_configuration.accept_incoming_connections ? _actual_listening_endpoint.port() : 0;

      if (_is_firewalled == firewalled_state::not_firewalled &&
          _publicly_visible_listening_endpoint)
      {
        local_endpoint = *_publicly_visible_listening_endpoint;
        listening_port = _publicly_visible_listening_endpoint->port();
      }

      hello_message hello(_user_agent_string,
                          core_protocol_version,
                          local_endpoint.get_address(),
                          listening_port,
                          local_endpoint.port(),
                          _node_public_key,
                          signature,
                          generate_hello_user_data());

      peer->send_message(message(hello));
    }

    void node_impl::connect_to_task(const peer_connection_ptr& new_peer,
                                    const fc::ip::endpoint& remote_endpoint)
    {
      VERIFY_CORRECT_THREAD();

      if (!new_peer->performing_firewall_check())
      {
        // create or find the database entry for the new peer
        // if we're connecting to them, we believe they're not firewalled
        potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_record.last_connection_attempt_time = fc::time_point::now();;
        _potential_peer_db.update_entry(updated_peer_record);
      }
      else
      {
        wlog("connecting to peer ${peer} for firewall check", ("peer", new_peer->get_remote_endpoint()));
      }

      fc::oexception connect_failed_exception;

      try
      {
        new_peer->connect_to(remote_endpoint, _actual_listening_endpoint);  // blocks until the connection is established and secure connection is negotiated

        // we connected to the peer.  guess they're not firewalled....
        new_peer->is_firewalled = firewalled_state::not_firewalled;

        // connection succeeded, we've started handshaking.  record that in our database
        potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_record.last_connection_disposition = last_connection_handshaking_failed;
        updated_peer_record.number_of_successful_connection_attempts++;
        updated_peer_record.last_seen_time = fc::time_point::now();
        _potential_peer_db.update_entry(updated_peer_record);
      }
      catch (const fc::exception& except)
      {
        connect_failed_exception = except;
      }

      if (connect_failed_exception && !new_peer->performing_firewall_check())
      {
        // connection failed.  record that in our database
        potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(remote_endpoint);
        updated_peer_record.last_connection_disposition = last_connection_failed;
        updated_peer_record.number_of_failed_connection_attempts++;
        updated_peer_record.last_failed_time = fc::time_point::now();
        if (new_peer->connection_closed_error)
          updated_peer_record.last_error = *new_peer->connection_closed_error;
        else
          updated_peer_record.last_error = *connect_failed_exception;
        _potential_peer_db.update_entry(updated_peer_record);
      }

      if (new_peer->performing_firewall_check())
      {
        // we were connecting to test whether the node is firewalled, and we now know the result.
        // send a message back to the requester
        peer_connection_ptr requesting_peer = get_peer_by_node_id(new_peer->firewall_check_state->requesting_peer);
        if (requesting_peer)
        {
          check_firewall_reply_message reply;
          reply.endpoint_checked = new_peer->firewall_check_state->endpoint_to_test;
          reply.node_id = new_peer->firewall_check_state->expected_node_id;
          reply.result = connect_failed_exception ?
                           firewall_check_result::unable_to_connect :
                           firewall_check_result::connection_successful;
          wlog("firewall check of ${peer_checked} ${success_or_failure}, sending reply to ${requester}",
                ("peer_checked", new_peer->get_remote_endpoint())
                ("success_or_failure", connect_failed_exception ? "failed" : "succeeded" )
                ("requester", requesting_peer->get_remote_endpoint()));

          requesting_peer->send_message(reply);
        }
      }

      if (connect_failed_exception || new_peer->performing_firewall_check())
      {
        // if the connection failed or if this connection was just intended to check
        // whether the peer is firewalled, we want to disconnect now.
        _handshaking_connections.erase(new_peer);
        _terminating_connections.erase(new_peer);
        assert(_active_connections.find(new_peer) == _active_connections.end());
        _active_connections.erase(new_peer);
        assert(_closing_connections.find(new_peer) == _closing_connections.end());
        _closing_connections.erase(new_peer);

        display_current_connections();
        trigger_p2p_network_connect_loop();
        schedule_peer_for_deletion(new_peer);

        if (connect_failed_exception)
          throw *connect_failed_exception;
      }
      else
      {
        // connection was successful and we want to stay connected
        fc::ip::endpoint local_endpoint = new_peer->get_local_endpoint();
        new_peer->inbound_address = local_endpoint.get_address();
        new_peer->inbound_port = _node_configuration.accept_incoming_connections ? _actual_listening_endpoint.port() : 0;
        new_peer->outbound_port = local_endpoint.port();

        new_peer->our_state = peer_connection::our_connection_state::just_connected;
        new_peer->their_state = peer_connection::their_connection_state::just_connected;
        send_hello_message(new_peer);
        dlog("Sent \"hello\" to peer ${peer}", ("peer", new_peer->get_remote_endpoint()));
      }
    }

    // methods implementing node's public interface
    void node_impl::set_node_delegate(node_delegate* del, fc::thread* thread_for_delegate_calls)
    {
      VERIFY_CORRECT_THREAD();
      _delegate.reset();
      if (del)
        _delegate.reset(new statistics_gathering_node_delegate_wrapper(del, thread_for_delegate_calls));
    }

    void node_impl::load_configuration( const fc::path& configuration_directory )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration_directory = configuration_directory;
      fc::path configuration_file_name( _node_configuration_directory / NODE_CONFIGURATION_FILENAME );
      bool node_configuration_loaded = false;
      if( fc::exists(configuration_file_name ) )
      {
        try
        {
          _node_configuration = fc::json::from_file( configuration_file_name, fc::json::format_validation_mode::full ).as<node_configuration>();
          ilog( "Loaded configuration from file ${filename}", ("filename", configuration_file_name ) );

          if( _node_configuration.private_key == fc::ecc::private_key() )
          {
            ilog( "generating new private key for this node" );
            _node_configuration.private_key = fc::ecc::private_key::generate();
          }

          node_configuration_loaded = true;
        }
        catch ( fc::parse_error_exception& parse_error )
        {
          elog( "malformed node configuration file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", parse_error.to_detail_string() ) );
        }
        catch ( fc::exception& except )
        {
          elog( "unexpected exception while reading configuration file ${filename}: ${error}",
               ( "filename", configuration_file_name )("error", except.to_detail_string() ) );
        }
      }

      if( !node_configuration_loaded )
      {
        _node_configuration = node_configuration();

#ifdef GRAPHENE_TEST_NETWORK
        uint32_t port = GRAPHENE_NET_TEST_P2P_PORT + GRAPHENE_TEST_NETWORK_VERSION;
#else
        uint32_t port = GRAPHENE_NET_DEFAULT_P2P_PORT;
#endif
        _node_configuration.listen_endpoint.set_port( port );
        _node_configuration.accept_incoming_connections = true;
        _node_configuration.wait_if_endpoint_is_busy = false;

        ilog( "generating new private key for this node" );
        _node_configuration.private_key = fc::ecc::private_key::generate();
      }

      _node_public_key = _node_configuration.private_key.get_public_key().serialize();

      fc::path potential_peer_database_file_name(_node_configuration_directory / POTENTIAL_PEER_DATABASE_FILENAME);
      try
      {
        _potential_peer_db.open(potential_peer_database_file_name);

        // push back the time on all peers loaded from the database so we will be able to retry them immediately
        for (peer_database::iterator itr = _potential_peer_db.begin(); itr != _potential_peer_db.end(); ++itr)
        {
          potential_peer_record updated_peer_record = *itr;
          ddump((updated_peer_record));
          updated_peer_record.last_connection_attempt_time = std::min<fc::time_point_sec>(updated_peer_record.last_connection_attempt_time,
                                                                                          fc::time_point::now() - fc::seconds(_node_configuration.peer_connection_retry_timeout));
          _potential_peer_db.update_entry(updated_peer_record);
        }
        trigger_p2p_network_connect_loop();
      }
      catch (fc::exception& except)
      {
        elog("unable to open peer database ${filename}: ${error}",
             ("filename", potential_peer_database_file_name)("error", except.to_detail_string()));
        throw;
      }
    }

    void node_impl::listen_to_p2p_network(std::function<bool()> break_callback)
    {
      VERIFY_CORRECT_THREAD();
      if (!_node_configuration.accept_incoming_connections)
      {
        wlog("accept_incoming_connections is false, p2p network will not accept any incoming connections");
        return;
      }

      assert(_node_public_key != fc::ecc::public_key_data());

      fc::ip::endpoint listen_endpoint = _node_configuration.listen_endpoint;
      if (listen_endpoint.port() != 0)
      {
        // if the user specified a port, we only want to bind to it if it's not already
        // being used by another application.  During normal operation, we set the
        // SO_REUSEADDR/SO_REUSEPORT flags so that we can bind outbound sockets to the
        // same local endpoint as we're listening on here.  On some platforms, setting
        // those flags will prevent us from detecting that other applications are
        // listening on that port.  We'd like to detect that, so we'll set up a temporary
        // tcp server without that flag to see if we can listen on that port.
        bool first = true;
        for (;;)
        {
          if (break_callback())
          {
            std::string break_message = "Node manually closed - probably SIGINT/SIGTERM signal appeared";
            ulog(break_message);
            wlog(break_message);
            return;
          }
          bool listen_failed = false;

          try
          {
            fc::tcp_server temporary_server;
            if (listen_endpoint.get_address() != fc::ip::address())
              temporary_server.listen(listen_endpoint);
            else
              temporary_server.listen(listen_endpoint.port());
            break;
          }
          catch ( const fc::exception&)
          {
            listen_failed = true;
          }

          if (listen_failed)
          {
            if (_node_configuration.wait_if_endpoint_is_busy)
            {
              std::ostringstream error_message_stream;
              if (first)
              {
                error_message_stream << "Unable to listen for connections on port " << listen_endpoint.port()
                                     << ", retrying in a few seconds\n";
                error_message_stream << "You can wait for it to become available, or restart this program using\n";
                error_message_stream << "the --p2p-port option to specify another port\n";
                first = false;
              }
              else
              {
                error_message_stream << "\nStill waiting for port " << listen_endpoint.port() << " to become available\n";
              }
              std::string error_message = error_message_stream.str();
              ulog(error_message);
              wlog(error_message);
              _delegate->error_encountered(error_message, fc::oexception());
              fc::usleep(fc::seconds(GRAPHENE_NET_PORT_WAIT_DELAY_SECONDS));
            }
            else // don't wait, just find a random port
            {
              wlog("unable to bind on the requested endpoint ${listen_endpoint}, which probably means that endpoint is already in use", (listen_endpoint));
              listen_endpoint.set_port(0);
            }
          } // if (listen_failed)
        }   // for(;;)
      } // if (listen_endpoint.port() != 0)
      else // port is 0
      {
        // if they requested a random port, we'll just assume it's available
        // (it may not be due to ip address, but we'll detect that in the next step)
      }

      _tcp_server.set_reuse_address();
      int server_receive_buffer_size = _tcp_server.set_receive_buffer_size(MAX_MESSAGE_SIZE);
      idump((server_receive_buffer_size));
      int server_send_buffer_size = _tcp_server.set_send_buffer_size(MAX_MESSAGE_SIZE);
      idump((server_send_buffer_size));
      idump((_tcp_server.get_no_delay()));
      _tcp_server.set_no_delay(true);
      idump((_tcp_server.get_no_delay()));

      try
      {
        if (listen_endpoint.get_address() != fc::ip::address())
          _tcp_server.listen(listen_endpoint);
        else
          _tcp_server.listen(listen_endpoint.port());
        _actual_listening_endpoint = _tcp_server.get_local_endpoint();
        ilog("listening for connections on endpoint ${_actual_listening_endpoint} (our first choice)", (_actual_listening_endpoint));
      }
      catch (fc::exception& e)
      {
        FC_RETHROW_EXCEPTION(e, error, "unable to listen on ${listen_endpoint}", (listen_endpoint));
      }
    }

    void node_impl::connect_to_p2p_network()
    {
      VERIFY_CORRECT_THREAD();
      assert(_node_public_key != fc::ecc::public_key_data());

      assert(!_accept_loop_complete.valid() &&
             !_p2p_network_connect_loop_done.valid() &&
             !_fetch_sync_items_loop_done.valid() &&
             !_fetch_item_loop_done.valid() &&
             !_advertise_inventory_loop_done.valid() &&
             !_terminate_inactive_connections_loop_done.valid() &&
             !_fetch_updated_peer_lists_loop_done.valid() &&
             !_bandwidth_monitor_loop_done.valid() &&
             !_dump_node_status_task_done.valid());
      if (_node_configuration.accept_incoming_connections)
        _accept_loop_complete = async_task( [=](){ accept_loop(); }, "accept_loop");
      _p2p_network_connect_loop_done = async_task( [=]() { p2p_network_connect_loop(); }, "p2p_network_connect_loop" );
      _fetch_sync_items_loop_done = async_task( [=]() { fetch_sync_items_loop(); }, "fetch_sync_items_loop" );
      _fetch_item_loop_done = async_task( [=]() { fetch_items_loop(); }, "fetch_items_loop" );
      _advertise_inventory_loop_done = async_task( [=]() { advertise_inventory_loop(); }, "advertise_inventory_loop" );
      _terminate_inactive_connections_loop_done = async_task( [=]() { terminate_inactive_connections_loop(); }, "terminate_inactive_connections_loop" );
      _fetch_updated_peer_lists_loop_done = async_task([=](){ fetch_updated_peer_lists_loop(); }, "fetch_updated_peer_lists_loop");
      _bandwidth_monitor_loop_done = async_task([=](){ bandwidth_monitor_loop(); }, "bandwidth_monitor_loop");
      _dump_node_status_task_done = async_task([=](){ dump_node_status_task(); }, "dump_node_status_task");
    }

    void node_impl::add_node(const fc::ip::endpoint& ep)
    {
      VERIFY_CORRECT_THREAD();
      // if we're connecting to them, we believe they're not firewalled
      potential_peer_record updated_peer_record = _potential_peer_db.lookup_or_create_entry_for_endpoint(ep);

      // if we've recently connected to this peer, reset the last_connection_attempt_time to allow
      // us to immediately retry this peer
      updated_peer_record.last_connection_attempt_time = std::min<fc::time_point_sec>(updated_peer_record.last_connection_attempt_time,
                                                                                      fc::time_point::now() - fc::seconds(_node_configuration.peer_connection_retry_timeout));
      _add_once_node_list.push_back(updated_peer_record);
      _potential_peer_db.update_entry(updated_peer_record);
      trigger_p2p_network_connect_loop();
    }

    void node_impl::initiate_connect_to(const peer_connection_ptr& new_peer)
    {
      new_peer->get_socket().open();
      new_peer->get_socket().set_reuse_address();
      int peer_send_buffer_size = new_peer->get_socket().set_send_buffer_size(MAX_MESSAGE_SIZE);
      idump((peer_send_buffer_size));
      int peer_receive_buffer_size = new_peer->get_socket().set_receive_buffer_size(MAX_MESSAGE_SIZE);
      idump((peer_receive_buffer_size));
      idump((new_peer->get_socket().get_no_delay()));
      new_peer->get_socket().set_no_delay(true);
      idump((new_peer->get_socket().get_no_delay()));
  
      new_peer->connection_initiation_time = fc::time_point::now();
      _handshaking_connections.insert(new_peer);
      _rate_limiter.add_tcp_socket(&new_peer->get_socket());

      if (node_is_shutting_down())
        return;

      std::weak_ptr<peer_connection> new_weak_peer(new_peer);
      new_peer->accept_or_connect_task_done = async_task([this, new_weak_peer]()
      {
        peer_connection_ptr new_peer(new_weak_peer.lock());
        assert(new_peer);
        if (!new_peer)
          return;
        connect_to_task(new_peer, *new_peer->get_remote_endpoint());
      }, "connect_to_task");
    }

    void node_impl::connect_to_endpoint(const fc::ip::endpoint& remote_endpoint)
    {
      VERIFY_CORRECT_THREAD();
      if (is_connection_to_endpoint_in_progress(remote_endpoint))
        FC_THROW_EXCEPTION(already_connected_to_requested_peer, "already connected to requested endpoint ${endpoint}",
                           ("endpoint", remote_endpoint));

      dlog("node_impl::connect_to_endpoint(${endpoint})", ("endpoint", remote_endpoint));
      peer_connection_ptr new_peer(peer_connection::make_shared(this));
      new_peer->set_remote_endpoint(remote_endpoint);
      initiate_connect_to(new_peer);
    }

    peer_connection_ptr node_impl::get_connection_to_endpoint( const fc::ip::endpoint& remote_endpoint )
    {
      VERIFY_CORRECT_THREAD();
      for( const peer_connection_ptr& active_peer : _active_connections )
      {
        fc::optional<fc::ip::endpoint> endpoint_for_this_peer( active_peer->get_remote_endpoint() );
        if( endpoint_for_this_peer && *endpoint_for_this_peer == remote_endpoint )
          return active_peer;
      }
      for( const peer_connection_ptr& handshaking_peer : _handshaking_connections )
      {
        fc::optional<fc::ip::endpoint> endpoint_for_this_peer( handshaking_peer->get_remote_endpoint() );
        if( endpoint_for_this_peer && *endpoint_for_this_peer == remote_endpoint )
          return handshaking_peer;
      }
      return peer_connection_ptr();
    }

    bool node_impl::is_connection_to_endpoint_in_progress( const fc::ip::endpoint& remote_endpoint )
    {
      VERIFY_CORRECT_THREAD();
      return get_connection_to_endpoint( remote_endpoint ) != peer_connection_ptr();
    }

    void node_impl::move_peer_to_active_list(const peer_connection_ptr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.insert(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.erase(peer);
      _terminating_connections.erase(peer);
      fc_ilog(fc::logger::get("sync"), "New peer is connected (${peer}), now ${count} active peers",
              ("peer", peer->get_remote_endpoint())
              ("count", _active_connections.size()));
    }

    void node_impl::move_peer_to_closing_list(const peer_connection_ptr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.erase(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.insert(peer);
      _terminating_connections.erase(peer);
      fc_ilog(fc::logger::get("sync"), "Peer connection closing (${peer}), now ${count} active peers",
              ("peer", peer->get_remote_endpoint())
              ("count", _active_connections.size()));
    }

    void node_impl::move_peer_to_terminating_list(const peer_connection_ptr& peer)
    {
      VERIFY_CORRECT_THREAD();
      _active_connections.erase(peer);
      _handshaking_connections.erase(peer);
      _closing_connections.erase(peer);
      _terminating_connections.insert(peer);
      fc_ilog(fc::logger::get("sync"), "Peer connection terminating (${peer}), now ${count} active peers",
              ("peer", peer->get_remote_endpoint())
              ("count", _active_connections.size()));
    }


    // format a byte count like 409869 into "409.8 KiB"
    const unsigned formatted_network_bytes_length = 9; // "123.4 KiB" is 9 chars
    std::string format_network_bytes(uint64_t bytes)
    {
      std::ostringstream bytes_stream;
      bytes_stream << std::right << std::setw(5) << std::fixed << std::setprecision(1);
      if (bytes > 1099511627776)
        bytes_stream << (bytes / 1099511627776.0) << " TiB";
      else if (bytes > 1073741824)
        bytes_stream << (bytes / 1073741824.0) << " GiB";
      else if (bytes > 1048576)
        bytes_stream << (bytes / 1048576.0) << " MiB";
      else if (bytes > 1024)
        bytes_stream << (bytes / 1024.0) << " KiB";
      else
        bytes_stream << bytes << "   B";
      return bytes_stream.str();
    }

    // format a speed like 409869 into "409.8 KiB/s"
    const unsigned formatted_network_speed_length = 11; // "123.4 KiB/s" is 11 chars
    std::string format_network_speed(float speed_bps)
    {
      std::ostringstream speed_stream;
      speed_stream << std::right << std::setw(5) << std::fixed << std::setprecision(1);
      if (speed_bps > 1073741824) // yeah, right
        speed_stream << (speed_bps / 1073741824.0) << " GiB/s";
      else if (speed_bps > 1048576)
        speed_stream << (speed_bps / 1048576.0) << " MiB/s";
      else if (speed_bps > 1024)
        speed_stream << (speed_bps / 1024.0) << " KiB/s";
      else
        speed_stream << speed_bps << "   B/s";
      return speed_stream.str();
    }

    // draw a crude ascii bar chart
    std::vector<std::string> graph_values(const boost::circular_buffer<uint32_t>& read_speeds, 
                                          const boost::circular_buffer<uint32_t>& write_speeds, 
                                          unsigned width, const std::string& title,
                                          const std::string& x_axis_min_label, const std::string& x_axis_max_label)
    {
      const uint32_t height = 20;

      // combine read + write
      std::vector<uint32_t> combined_speeds;
      combined_speeds.reserve(read_speeds.size());
      std::transform(read_speeds.begin(), read_speeds.end(),
                     write_speeds.begin(),
                     std::back_inserter(combined_speeds),
                     std::plus<uint32_t>());

      uint32_t min_value = 0;
      uint32_t max_value = 0;
      if (!combined_speeds.empty())
      {
        auto minmax = std::minmax_element(combined_speeds.begin(), combined_speeds.end());
        min_value = *minmax.first;
        max_value = *minmax.second;
      }

      uint32_t y_min = min_value;
      uint32_t y_max = max_value;
      if (min_value == max_value)
        y_max = min_value ? 2 * min_value : height;

      float y_step = ((float)y_max - (float)y_min) / ((float)height - 1);
      std::vector<std::string> graph;

      // spit out the title
      std::ostringstream title_stream;
      title_stream << std::string(formatted_network_speed_length, ' ') << "  ";
      unsigned left_padding = 0;
      if (title.length() < width)
        left_padding = (width - title.length()) / 2;
      title_stream << std::string(left_padding, ' ');
      title_stream << title;
      if (title.length() + left_padding < width)
        title_stream << std::string(width - title.length() - left_padding, ' ');
      graph.push_back(title_stream.str());

      // then the body of the graph
      for (unsigned row_number = 0; row_number < height; ++row_number)
      {
        std::ostringstream row_stream;
        float threshold = (float)y_min + (y_step * (height - row_number - 1));
        row_stream << format_network_speed(threshold) << " |";
        row_stream << std::string(width - combined_speeds.size(), ' '); // pad to full width
        for (unsigned i = 0; i < combined_speeds.size(); ++i)
          row_stream << (read_speeds[i] >= threshold ? '#' : (combined_speeds[i] >= threshold ? '*' : ' '));
        graph.push_back(row_stream.str());
      }

      // and now the x axis
      std::ostringstream x_axis;
      x_axis << std::string(formatted_network_speed_length, ' ') << " +" << std::string(width, '-');
      graph.push_back(x_axis.str());
      // and its labels
      std::ostringstream x_axis_labels;
      x_axis_labels << std::string(formatted_network_speed_length, ' ') << "  " << x_axis_min_label;
      if (x_axis_min_label.length() + x_axis_max_label.length() < width)
        x_axis_labels << std::string(width - x_axis_min_label.length() - x_axis_max_label.length(), ' ');
      x_axis_labels << x_axis_max_label;
      graph.push_back(x_axis_labels.str());

      return graph;
    }

    void node_impl::dump_node_status()
    {
      VERIFY_CORRECT_THREAD();
      ilog( "----------------- PEER STATUS UPDATE --------------------" );
      ilog( " number of peers: ${active} active, ${handshaking}, ${closing} closing.  attempting to maintain ${desired} - ${maximum} peers",
           ( "active", _active_connections.size() )("handshaking", _handshaking_connections.size() )("closing",_closing_connections.size() )
           ( "desired", _node_configuration.desired_number_of_connections )("maximum", _node_configuration.maximum_number_of_connections ) );
      for( const peer_connection_ptr& peer : _active_connections )
      {
        ilog( "       active peer ${endpoint} peer_is_in_sync_with_us:${in_sync_with_us} we_are_in_sync_with_peer:${in_sync_with_them}",
             ( "endpoint", peer->get_remote_endpoint() )
             ( "in_sync_with_us", !peer->peer_needs_sync_items_from_us )("in_sync_with_them", !peer->we_need_sync_items_from_peer ) );
        if( peer->we_need_sync_items_from_peer )
          ilog( "              above peer has ${count} sync items we might need", ("count", peer->ids_of_items_to_get.size() ) );
        if (peer->inhibit_fetching_sync_blocks)
          ilog( "              we are not fetching sync blocks from the above peer (inhibit_fetching_sync_blocks == true)" );

      }
      for( const peer_connection_ptr& peer : _handshaking_connections )
      {
        ilog( "  handshaking peer ${endpoint} in state ours(${our_state}) theirs(${their_state})",
             ( "endpoint", peer->get_remote_endpoint() )("our_state", peer->our_state )("their_state", peer->their_state ) );
      }

      ilog( "--------- MEMORY USAGE ------------" );
      ilog( "node._active_sync_requests size: ${size}", ("size", _active_sync_requests.size() ) );
      ilog( "node._received_sync_items size: ${size}", ("size", _received_sync_items.size() ) );
      ilog( "node._new_received_sync_items size: ${size}", ("size", _new_received_sync_items.size() ) );
      ilog( "node._items_to_fetch size: ${size}", ("size", _items_to_fetch.size() ) );
      ilog( "node._new_inventory size: ${block_size} blocks, ${transaction_size} transactions",
            ("block_size", _new_block_inventory.size() )("transaction_size", _new_transaction_inventory.size() ) );
      ilog( "node._block_message_cache size: ${size}", ("size", _block_message_cache.size() ) );
      ilog( "node._transaction_message_cache size: ${size}", ("size", _transaction_message_cache.size() ) );
      for( const peer_connection_ptr& peer : _active_connections )
      {
        ilog( "  peer ${endpoint}", ("endpoint", peer->get_remote_endpoint() ) );
        ilog( "    last requested block number for peers on this fork: ${block_num}",("block_num",peer->last_requested_block_number_for_peers_on_this_fork) );
        ilog( "    block number of first id to get: ${block_num}", ("block_num",peer->first_id_block_number) );
        ilog( "    peer.ids_of_items_to_get size: ${size}", ("size", peer->ids_of_items_to_get.size() ) );
        ilog( "    peer.inventory_peer_advertised_to_us size: ${size}", ("size", peer->inventory_peer_advertised_to_us.size() ) );
        ilog( "    peer.inventory_advertised_to_peer size: ${size}", ("size", peer->inventory_advertised_to_peer.size() ) );
        if (!peer->item_ids_requested_from_peer)
          ilog( "    peer.time_since_inventory_requested: no request in progress" );
        else
          ilog( "    peer.time_since_inventory_requested: ${time}ms", ("time", (fc::time_point::now() - peer->item_ids_requested_from_peer->get<1>()).count() / 1000));
        ilog( "    peer.items_requested_from_peer size: ${size}", ("size", peer->items_requested_from_peer.size() ) );
        ilog( "    peer.sync_items_requested_from_peer size: ${size}", ("size", peer->sync_items_requested_from_peer.size() ) );
        ilog( "    peer.time_since_last_sync_item_received: ${time_since_last_sync_item_received}ms", ("time_since_last_sync_item_received", (fc::time_point::now() - peer->last_sync_item_received_time).count() / 1000));
        ilog( "    peer.blocks_received_from_peer: ${compressed} compressed, ${uncompressed} uncompressed",
              ("compressed", peer->compressed_blocks_received_from_peer)("uncompressed", peer->uncompressed_blocks_received_from_peer));
      }
      ilog( "--------- END MEMORY USAGE ------------" );

      ilog( "--------- NETWORK USAGE ---------------" );

      // first, log the stats per peer, most talkative first
      try
      {
        fc::time_point current_time = fc::time_point::now();
        struct peer_network_stats {
          fc::optional<fc::ip::endpoint> endpoint;
          uint64_t total_bytes_sent;
          uint64_t total_bytes_received;
          fc::microseconds connection_duration;
          float bytes_per_second() const { return connection_duration != fc::microseconds() ? ((float)(total_bytes_sent + total_bytes_received) / ((float)connection_duration.count() / fc::seconds(1).count())) : 0.f; }
        };
        std::vector<peer_network_stats> network_stats_by_peer;
        for (const peer_connection_ptr& peer : _active_connections)
          network_stats_by_peer.push_back({peer->get_remote_endpoint(), peer->get_total_bytes_sent(), peer->get_total_bytes_received(), current_time - peer->get_connection_time()});
        std::sort(network_stats_by_peer.begin(), network_stats_by_peer.end(), [](const peer_network_stats& lhs, const peer_network_stats& rhs) { return lhs.bytes_per_second() > rhs.bytes_per_second(); });
        ilog("Per peer (stats over the entire duration of their connection):");
        ilog("peer                 |      speed |        sent |    received | connection time");
        ilog("---------------------+------------+-------------+-------------+----------------");
        for (const peer_network_stats& stat : network_stats_by_peer)
        {
          std::string speed = format_network_speed(stat.bytes_per_second());
          int speed_padding = std::max<int>(formatted_network_speed_length - speed.length(), 0);
          std::string bytes_sent = format_network_bytes(stat.total_bytes_sent);
          int bytes_sent_padding = std::max<int>(3 + formatted_network_bytes_length - bytes_sent.length(), 0);
          std::string bytes_received = format_network_bytes(stat.total_bytes_received);
          int bytes_received_padding = std::max<int>(3 + formatted_network_bytes_length - bytes_received.length(), 0);
          std::string endpoint_string = fc::json::to_string(stat.endpoint);
          int endpoint_padding = std::max<int>(23 - endpoint_string.length(), 0);
          ilog("${endpoint}${endpoint_padding}|${speed_padding}${speed} |${bytes_sent_padding}${bytes_sent} |${bytes_received_padding}${bytes_received} | ${ago}",
               ("endpoint", stat.endpoint)("endpoint_padding", std::string(endpoint_padding, ' '))
               (speed)("speed_padding", std::string(speed_padding, ' '))
               (bytes_sent)("bytes_sent_padding", std::string(bytes_sent_padding, ' '))
               (bytes_received)("bytes_received_padding", std::string(bytes_received_padding, ' '))
               ("ago", get_approximate_relative_time_string(current_time - stat.connection_duration, current_time, "")));
        }
        ilog("                                                                               ");
      }
      catch (const fc::exception& e)
      {
        edump((e));
      }
      catch (...)
      {
      }

      if (!_average_network_read_speed_seconds.empty())
      {
        std::vector<std::string> graph = graph_values(_average_network_read_speed_seconds, _average_network_write_speed_seconds, 
                                                      60, "Network Speed by Second",
                                                      "a minute ago", "now");
        for (const std::string& line : graph)
          ilog("${line}", (line));
      }

      if (!_average_network_read_speed_minutes.empty())
      {
        ilog("${space}", ("space", std::string(formatted_network_speed_length + 2 + 60, ' ')));
        std::vector<std::string> graph = graph_values(_average_network_read_speed_minutes, _average_network_write_speed_minutes, 
                                                      60, "Average Network Speed by Minute",
                                                      "an hour ago", "now");
        for (const std::string& line : graph)
          ilog("${line}", (line));
      }

      if (!_average_network_read_speed_hours.empty())
      {
        ilog("${space}", ("space", std::string(formatted_network_speed_length + 2 + 60, ' ')));
        std::vector<std::string> graph = graph_values(_average_network_read_speed_hours, _average_network_write_speed_hours,
                                                      72, "Average Network Speed by Hour",
                                                      "three days ago", "now");
        for (const std::string& line : graph)
          ilog("${line}", (line));
      }

      ilog("${space}", ("space", std::string(formatted_network_speed_length + 2 + 60, ' ')));
      ilog("# = download, * = upload");

      ilog( "--------- END NETWORK USAGE -----------" );
    }

    void node_impl::disconnect_from_peer(peer_connection* peer_to_disconnect,
                                         const std::string& reason_for_disconnect,
                                         bool caused_by_error /* = false */,
                                         const fc::oexception& error /* = fc::oexception() */)
    {
      VERIFY_CORRECT_THREAD();
      move_peer_to_closing_list(peer_to_disconnect->shared_from_this());

      if (peer_to_disconnect->they_have_requested_close)
      {
        // the peer has already told us that it's ready to close the connection, so just close the connection
        peer_to_disconnect->close_connection();
      }
      else
      {
        // we're the first to try to want to close the connection
        fc::optional<fc::ip::endpoint> endpoint_for_db = peer_to_disconnect->get_endpoint_for_db();
        if (endpoint_for_db)
        {
          fc::optional<potential_peer_record> updated_peer_record = _potential_peer_db.lookup_entry_for_endpoint(*endpoint_for_db);
          if (updated_peer_record)
          {
            updated_peer_record->last_seen_time = fc::time_point::now();
            if (error)
              updated_peer_record->last_error = error;
            else
              updated_peer_record->last_error = fc::exception(FC_LOG_MESSAGE(info, reason_for_disconnect.c_str()));
            if (caused_by_error)
              updated_peer_record->last_failed_time = fc::time_point::now();
            _potential_peer_db.update_entry(*updated_peer_record);
          }
        }
        peer_to_disconnect->we_have_requested_close = true;
        peer_to_disconnect->connection_closed_time = fc::time_point::now();

        closing_connection_message closing_message( reason_for_disconnect, caused_by_error, error );
        peer_to_disconnect->send_message( closing_message );
      }

      // notify the user.  This will be useful in testing, but we might want to remove it later;
      // it makes good sense to notify the user if other nodes think she is behaving badly, but
      // if we're just detecting and dissconnecting other badly-behaving nodes, they don't really care.
      if (caused_by_error)
      {
        std::ostringstream error_message;
        error_message << "I am disconnecting peer " << fc::variant( peer_to_disconnect->get_remote_endpoint() ).as_string() <<
                         " for reason: " << reason_for_disconnect;
        _delegate->error_encountered(error_message.str(), fc::oexception());
        dlog(error_message.str());
      }
      else
        dlog("Disconnecting from ${peer} for ${reason}", ("peer",peer_to_disconnect->get_remote_endpoint()) ("reason",reason_for_disconnect));
      // peer_to_disconnect->close_connection();
    }

    void node_impl::listen_on_endpoint( const fc::ip::endpoint& ep, bool wait_if_not_available )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.listen_endpoint = ep;
      _node_configuration.wait_if_endpoint_is_busy = wait_if_not_available;
      save_node_configuration();
    }

    void node_impl::accept_incoming_connections(bool accept)
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.accept_incoming_connections = accept;
      save_node_configuration();
    }

    void node_impl::listen_on_port( uint16_t port, bool wait_if_not_available )
    {
      VERIFY_CORRECT_THREAD();
      _node_configuration.listen_endpoint = fc::ip::endpoint( fc::ip::address(), port );
      _node_configuration.wait_if_endpoint_is_busy = wait_if_not_available;
      save_node_configuration();
    }

    fc::ip::endpoint node_impl::get_actual_listening_endpoint() const
    {
      VERIFY_CORRECT_THREAD();
      return _actual_listening_endpoint;
    }

    std::vector<peer_status> node_impl::get_connected_peers() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<peer_status> statuses;
      for (const peer_connection_ptr& peer : _active_connections)
      {
        ASSERT_TASK_NOT_PREEMPTED(); // don't yield while iterating over _active_connections

        peer_status this_peer_status;
        this_peer_status.version = 0;
        fc::optional<fc::ip::endpoint> endpoint = peer->get_remote_endpoint();
        if (endpoint)
          this_peer_status.host = *endpoint;
        fc::mutable_variant_object peer_details;
        peer_details["addr"] = endpoint ? (std::string)*endpoint : std::string();
        peer_details["addrlocal"] = (std::string)peer->get_local_endpoint();
        peer_details["services"] = "00000001";
        peer_details["lastsend"] = peer->get_last_message_sent_time().sec_since_epoch();
        peer_details["lastrecv"] = peer->get_last_message_received_time().sec_since_epoch();
        peer_details["bytessent"] = peer->get_total_bytes_sent();
        peer_details["bytesrecv"] = peer->get_total_bytes_received();
        peer_details["conntime"] = peer->get_connection_time();
        peer_details["pingtime"] = "";
        peer_details["pingwait"] = "";
        peer_details["version"] = "";
        peer_details["subver"] = peer->user_agent;
        peer_details["inbound"] = peer->direction == peer_connection_direction::inbound;
        peer_details["firewall_status"] = peer->is_firewalled;
        peer_details["startingheight"] = "";
        peer_details["banscore"] = "";
        peer_details["syncnode"] = "";

        if (peer->fc_git_revision_sha)
        {
          std::string revision_string = *peer->fc_git_revision_sha;
          if (*peer->fc_git_revision_sha == fc::git_revision_sha)
            revision_string += " (same as ours)";
          else
            revision_string += " (different from ours)";
          peer_details["fc_git_revision_sha"] = revision_string;

        }
        if (peer->fc_git_revision_unix_timestamp)
        {
          peer_details["fc_git_revision_unix_timestamp"] = *peer->fc_git_revision_unix_timestamp;
          std::string age_string = fc::get_approximate_relative_time_string( *peer->fc_git_revision_unix_timestamp);
          if (*peer->fc_git_revision_unix_timestamp == fc::time_point_sec(fc::git_revision_unix_timestamp))
            age_string += " (same as ours)";
          else if (*peer->fc_git_revision_unix_timestamp > fc::time_point_sec(fc::git_revision_unix_timestamp))
            age_string += " (newer than ours)";
          else
            age_string += " (older than ours)";
          peer_details["fc_git_revision_age"] = age_string;
        }

        if (peer->platform)
          peer_details["platform"] = *peer->platform;

        // provide these for debugging
        // warning: these are just approximations, if the peer is "downstream" of us, they may
        // have received blocks from other peers that we are unaware of
        peer_details["current_head_block"] = peer->last_block_delegate_has_seen;
        peer_details["current_head_block_number"] = _delegate->get_block_number(peer->last_block_delegate_has_seen);
        peer_details["current_head_block_time"] = peer->last_block_time_delegate_has_seen;

        this_peer_status.info = peer_details;
        statuses.push_back(this_peer_status);
      }
      return statuses;
    }

    uint32_t node_impl::get_connection_count() const
    {
      VERIFY_CORRECT_THREAD();
      return (uint32_t)_active_connections.size();
    }

    void node_impl::broadcast(const std::shared_ptr<full_block_type>& full_block, const message_propagation_data& propagation_data)
    {
      VERIFY_CORRECT_THREAD();
      const block_id_type& block_id = full_block->get_block_id();
      idump((block_id));

      _most_recent_blocks_accepted.push_back(block_id);
      _block_message_cache.cache_message(full_block, propagation_data);
      _new_block_inventory.insert(full_block);
      trigger_advertise_inventory_loop();

      /** Trigger block_accepted methods to potentially flush caches in both cases:
          - when node receives a block,
          - just produce (broadcast) block (as single block producer node in the network)
      */
      _block_message_cache.block_accepted();
      _transaction_message_cache.block_accepted();
    }

    void node_impl::broadcast(const std::shared_ptr<full_transaction_type>& full_transaction, const message_propagation_data& propagation_data)
    {
      VERIFY_CORRECT_THREAD();
      _transaction_message_cache.cache_message(full_transaction, propagation_data);
      _new_transaction_inventory.insert(full_transaction);
      trigger_advertise_inventory_loop();
    }

    void node_impl::broadcast(const std::shared_ptr<full_block_type>& full_block)
    {
      VERIFY_CORRECT_THREAD();
      // this version is called directly from the client
      message_propagation_data propagation_data{fc::time_point::now(), fc::time_point::now(), _node_id};
      broadcast(full_block, propagation_data);
    }

    void node_impl::broadcast(const std::shared_ptr<full_transaction_type>& full_transaction)
    {
      VERIFY_CORRECT_THREAD();
      // this version is called directly from the client
      message_propagation_data propagation_data{fc::time_point::now(), fc::time_point::now(), _node_id};
      broadcast(full_transaction, propagation_data);
    }

    void node_impl::sync_from(const item_id& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers)
    {
      VERIFY_CORRECT_THREAD();
      _most_recent_blocks_accepted.clear();
      _sync_item_type = current_head_block.item_type;
      _most_recent_blocks_accepted.push_back(current_head_block.item_hash);
      _hard_fork_block_numbers = hard_fork_block_numbers;
    }

    bool node_impl::is_connected() const
    {
      VERIFY_CORRECT_THREAD();
      return !_active_connections.empty();
    }

    std::vector<potential_peer_record> node_impl::get_potential_peers() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<potential_peer_record> result;
      // use explicit iterators here, for some reason the mac compiler can't used ranged-based for loops here
      for (peer_database::iterator itr = _potential_peer_db.begin(); itr != _potential_peer_db.end(); ++itr)
        result.push_back(*itr);
      return result;
    }

    void node_impl::set_advanced_node_parameters(const fc::variant_object& params)
    {
      VERIFY_CORRECT_THREAD();
      ilog( "set_advanced_node_parameters ${params}", ("params", params) );

      fc::from_variant( params, _node_configuration );

      if( _node_configuration.private_key == fc::ecc::private_key() )
      {
         ilog( "generating new private key for this node" );
         _node_configuration.private_key = fc::ecc::private_key::generate();
      }

      // Private key could have been overridden at this point. Update public key just in case
      _node_public_key = _node_configuration.private_key.get_public_key().serialize();

      if( _node_configuration.desired_number_of_connections > _node_configuration.maximum_number_of_connections )
      {
         wlog( "Reducing desired_number_of_connections from ${x0} to maximum_number_of_connections=${x1}",
            ("x0", _node_configuration.desired_number_of_connections)
            ("x1", _node_configuration.maximum_number_of_connections)
            );
      }

      while (_active_connections.size() > _node_configuration.maximum_number_of_connections)
        disconnect_from_peer(_active_connections.begin()->get(), "I have too many connections open");
      trigger_p2p_network_connect_loop();
    }

    node_configuration node_impl::get_advanced_node_parameters()const
    {
      VERIFY_CORRECT_THREAD();
      return _node_configuration;
    }

    message_propagation_data node_impl::get_transaction_propagation_data(const graphene::net::transaction_id_type& transaction_id)
    {
      VERIFY_CORRECT_THREAD();
      return _transaction_message_cache.get_message_propagation_data(transaction_id);
    }

    message_propagation_data node_impl::get_block_propagation_data(const graphene::net::block_id_type& block_id)
    {
      VERIFY_CORRECT_THREAD();
      return _block_message_cache.get_message_propagation_data(block_id);
    }

    node_id_t node_impl::get_node_id() const
    {
      VERIFY_CORRECT_THREAD();
      return _node_id;
    }
    void node_impl::set_allowed_peers(const std::vector<node_id_t>& allowed_peers)
    {
      VERIFY_CORRECT_THREAD();
#ifdef ENABLE_P2P_DEBUGGING_API
      _allowed_peers.clear();
      _allowed_peers.insert(allowed_peers.begin(), allowed_peers.end());
      std::list<peer_connection_ptr> peers_to_disconnect;
      if (!_allowed_peers.empty())
        for (const peer_connection_ptr& peer : _active_connections)
          if (_allowed_peers.find(peer->node_id) == _allowed_peers.end())
            peers_to_disconnect.push_back(peer);
      for (const peer_connection_ptr& peer : peers_to_disconnect)
        disconnect_from_peer(peer.get(), "My allowed_peers list has changed, and you're no longer allowed.  Bye.");
#endif // ENABLE_P2P_DEBUGGING_API
    }
    void node_impl::clear_peer_database()
    {
      VERIFY_CORRECT_THREAD();
      _potential_peer_db.clear();
    }

    void node_impl::set_total_bandwidth_limit( uint32_t upload_bytes_per_second, uint32_t download_bytes_per_second )
    {
      VERIFY_CORRECT_THREAD();
      _rate_limiter.set_upload_limit( upload_bytes_per_second );
      _rate_limiter.set_download_limit( download_bytes_per_second );
    }

    fc::variant_object node_impl::get_call_statistics() const
    {
      VERIFY_CORRECT_THREAD();
      return _delegate->get_call_statistics();
    }

    fc::variant_object node_impl::network_get_info() const
    {
      VERIFY_CORRECT_THREAD();
      fc::mutable_variant_object info;
      info["listening_on"] = _actual_listening_endpoint;
      info["node_public_key"] = _node_public_key;
      info["node_id"] = _node_id;
      info["firewalled"] = _is_firewalled;
      return info;
    }
    fc::variant_object node_impl::network_get_usage_stats() const
    {
      VERIFY_CORRECT_THREAD();
      std::vector<uint32_t> network_usage_by_second;
      network_usage_by_second.reserve(_average_network_read_speed_seconds.size());
      std::transform(_average_network_read_speed_seconds.begin(), _average_network_read_speed_seconds.end(),
                     _average_network_write_speed_seconds.begin(),
                     std::back_inserter(network_usage_by_second),
                     std::plus<uint32_t>());

      std::vector<uint32_t> network_usage_by_minute;
      network_usage_by_minute.reserve(_average_network_read_speed_minutes.size());
      std::transform(_average_network_read_speed_minutes.begin(), _average_network_read_speed_minutes.end(),
                     _average_network_write_speed_minutes.begin(),
                     std::back_inserter(network_usage_by_minute),
                     std::plus<uint32_t>());

      std::vector<uint32_t> network_usage_by_hour;
      network_usage_by_hour.reserve(_average_network_read_speed_hours.size());
      std::transform(_average_network_read_speed_hours.begin(), _average_network_read_speed_hours.end(),
                     _average_network_write_speed_hours.begin(),
                     std::back_inserter(network_usage_by_hour),
                     std::plus<uint32_t>());

      fc::mutable_variant_object result;
      result["usage_by_second"] = network_usage_by_second;
      result["usage_by_minute"] = network_usage_by_minute;
      result["usage_by_hour"] = network_usage_by_hour;
      return result;
    }

    bool node_impl::is_hard_fork_block(uint32_t block_number) const
    {
      return std::binary_search(_hard_fork_block_numbers.begin(), _hard_fork_block_numbers.end(), block_number);
    }
    uint32_t node_impl::get_next_known_hard_fork_block_number(uint32_t block_number) const
    {
      auto iter = std::upper_bound(_hard_fork_block_numbers.begin(), _hard_fork_block_numbers.end(),
                                   block_number);
      return iter != _hard_fork_block_numbers.end() ? *iter : 0;
    }

    bool node_impl::node_is_shutting_down() const
    {
      return _node_is_shutting_down || theApp.is_interrupt_request();
    }

    void node_impl::send_message_timing_to_statsd( peer_connection* originating_peer, const message& received_message, const message_hash_type& message_hash )
    {
      if( hive::plugins::statsd::util::statsd_enabled( theApp ) )
      {
        auto iter = originating_peer->items_requested_from_peer.find( item_id( received_message.msg_type, message_hash ) );
        if( iter != originating_peer->items_requested_from_peer.end() )
        {
          hive::plugins::statsd::util::get_statsd( theApp ).timing(
            "p2p",
            "latency",
            fc::variant( core_message_type_enum( received_message.msg_type ) ).as_string(),
            hive::plugins::statsd::util::timing_helper( fc::time_point::now() - iter->second ),
            0.1f
          );
        }
      }
    }

  }  // end namespace detail



  /////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // implement node functions, they call the matching function in to detail::node_impl in the correct thread //

#ifdef P2P_IN_DEDICATED_THREAD
# define INVOKE_IN_IMPL(method_name, ...) \
    return my->_thread->async([&](){ return my->method_name(__VA_ARGS__); }, "thread invoke for method " BOOST_PP_STRINGIZE(method_name)).wait()
#else
# define INVOKE_IN_IMPL(method_name, ...) \
    return my->method_name(__VA_ARGS__)
#endif // P2P_IN_DEDICATED_THREAD

  node::node(const std::string& user_agent, appbase::application& app, hive::chain::blockchain_worker_thread_pool& thread_pool) :
    my(new detail::node_impl(user_agent, app, thread_pool))
  {
  }

  node::~node()
  {
  }

  void node::set_node_delegate( node_delegate* del )
  {
    fc::thread* delegate_thread = &fc::thread::current();
    INVOKE_IN_IMPL(set_node_delegate, del, delegate_thread);
  }

  void node::load_configuration( const fc::path& configuration_directory )
  {
    INVOKE_IN_IMPL(load_configuration, configuration_directory);
  }

  void node::listen_to_p2p_network( std::function<bool()> break_callback )
  {
    INVOKE_IN_IMPL(listen_to_p2p_network, break_callback);
  }

  void node::connect_to_p2p_network()
  {
    INVOKE_IN_IMPL(connect_to_p2p_network);
  }

  void node::add_node( const fc::ip::endpoint& ep )
  {
    INVOKE_IN_IMPL(add_node, ep);
  }

  void node::connect_to_endpoint( const fc::ip::endpoint& remote_endpoint )
  {
    INVOKE_IN_IMPL(connect_to_endpoint, remote_endpoint);
  }

  void node::listen_on_endpoint(const fc::ip::endpoint& ep , bool wait_if_not_available)
  {
    INVOKE_IN_IMPL(listen_on_endpoint, ep, wait_if_not_available);
  }

  void node::accept_incoming_connections(bool accept)
  {
    INVOKE_IN_IMPL(accept_incoming_connections, accept);
  }

  void node::listen_on_port( uint16_t port, bool wait_if_not_available )
  {
    INVOKE_IN_IMPL(listen_on_port, port, wait_if_not_available);
  }

  fc::ip::endpoint node::get_actual_listening_endpoint() const
  {
    INVOKE_IN_IMPL(get_actual_listening_endpoint);
  }

  std::vector<peer_status> node::get_connected_peers() const
  {
    INVOKE_IN_IMPL(get_connected_peers);
  }

  uint32_t node::get_connection_count() const
  {
    INVOKE_IN_IMPL(get_connection_count);
  }

  void node::broadcast(const std::shared_ptr<full_block_type>& full_block) 
  { 
    INVOKE_IN_IMPL(broadcast, full_block);
  }

  void node::broadcast(const std::shared_ptr<full_transaction_type>& full_transaction) 
  { 
    INVOKE_IN_IMPL(broadcast, full_transaction);
  }

  void node::sync_from(const item_id& current_head_block, const std::vector<uint32_t>& hard_fork_block_numbers)
  {
    INVOKE_IN_IMPL(sync_from, current_head_block, hard_fork_block_numbers);
  }

  bool node::is_connected() const
  {
    INVOKE_IN_IMPL(is_connected);
  }

  std::vector<potential_peer_record> node::get_potential_peers()const
  {
    INVOKE_IN_IMPL(get_potential_peers);
  }

  void node::set_advanced_node_parameters( const fc::variant_object& params )
  {
    INVOKE_IN_IMPL(set_advanced_node_parameters, params);
  }

  node_configuration node::get_advanced_node_parameters()const
  {
    INVOKE_IN_IMPL(get_advanced_node_parameters);
  }

  message_propagation_data node::get_transaction_propagation_data( const graphene::net::transaction_id_type& transaction_id )
  {
    INVOKE_IN_IMPL(get_transaction_propagation_data, transaction_id);
  }

  message_propagation_data node::get_block_propagation_data( const graphene::net::block_id_type& block_id )
  {
    INVOKE_IN_IMPL(get_block_propagation_data, block_id);
  }

  node_id_t node::get_node_id() const
  {
    INVOKE_IN_IMPL(get_node_id);
  }

  void node::set_allowed_peers( const std::vector<node_id_t>& allowed_peers )
  {
    INVOKE_IN_IMPL(set_allowed_peers, allowed_peers);
  }

  void node::clear_peer_database()
  {
    INVOKE_IN_IMPL(clear_peer_database);
  }

  void node::set_total_bandwidth_limit(uint32_t upload_bytes_per_second,
                                       uint32_t download_bytes_per_second)
  {
    INVOKE_IN_IMPL(set_total_bandwidth_limit, upload_bytes_per_second, download_bytes_per_second);
  }

  fc::variant_object node::get_call_statistics() const
  {
    INVOKE_IN_IMPL(get_call_statistics);
  }

  fc::variant_object node::network_get_info() const
  {
    INVOKE_IN_IMPL(network_get_info);
  }

  fc::variant_object node::network_get_usage_stats() const
  {
    INVOKE_IN_IMPL(network_get_usage_stats);
  }

  void node::close()
  {
    INVOKE_IN_IMPL(close);
  }

  struct simulated_network::node_info
  {
    node_delegate* delegate;
    fc::future<void> message_sender_task_done;
    typedef std::variant<std::shared_ptr<full_block_type>, std::shared_ptr<full_transaction_type>> block_or_transaction;
    std::queue<block_or_transaction> messages_to_deliver;
    node_info(node_delegate* delegate) : delegate(delegate) {}
  };

  simulated_network::~simulated_network()
  {
    for (node_info* network_node_info : network_nodes)
    {
      network_node_info->message_sender_task_done.cancel_and_wait("~simulated_network()");
      delete network_node_info;
    }
  }

  template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
  template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

  void simulated_network::message_sender(node_info* destination_node)
  {
    while (!destination_node->messages_to_deliver.empty())
    {
      try
      {
        const node_info::block_or_transaction& message_to_deliver = destination_node->messages_to_deliver.front();
        std::visit(overloaded {
                     [&](const std::shared_ptr<full_block_type>& full_block) {
                       std::vector<fc::uint160_t> contained_transaction_message_ids;
                       destination_node->delegate->handle_block(full_block, false);
                     },
                     [&](const std::shared_ptr<full_transaction_type>& full_transaction) {
                       destination_node->delegate->handle_transaction(full_transaction);
                     }
                   }, message_to_deliver);
      }
      catch (const fc::exception& e)
      {
        elog("${e}", (e));
      }
      destination_node->messages_to_deliver.pop();
    }
  }

  void simulated_network::broadcast(const std::shared_ptr<full_block_type>& full_block)
  {
    for (node_info* network_node_info : network_nodes)
    {
      network_node_info->messages_to_deliver.emplace(full_block);
      if (!network_node_info->message_sender_task_done.valid() || network_node_info->message_sender_task_done.ready())
        network_node_info->message_sender_task_done = fc::async([=](){ message_sender(network_node_info); }, "simulated_network_sender");
    }
  }

  void simulated_network::broadcast(const std::shared_ptr<hive::chain::full_transaction_type>& full_transaction)
  {
    for (node_info* network_node_info : network_nodes)
    {
      network_node_info->messages_to_deliver.emplace(full_transaction);
      if (!network_node_info->message_sender_task_done.valid() || network_node_info->message_sender_task_done.ready())
        network_node_info->message_sender_task_done = fc::async([=](){ message_sender(network_node_info); }, "simulated_network_sender");
    }
  }

  void simulated_network::add_node_delegate(node_delegate* node_delegate_to_add)
  {
    network_nodes.push_back(new node_info(node_delegate_to_add));
  }

  namespace detail
  {
#define ROLLING_WINDOW_SIZE 1000
#define INITIALIZE_ACCUMULATOR(r, data, method_name) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE) \
      , BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))(boost::accumulators::tag::rolling_window::window_size = ROLLING_WINDOW_SIZE)


    statistics_gathering_node_delegate_wrapper::statistics_gathering_node_delegate_wrapper(node_delegate* delegate, fc::thread* thread_for_delegate_calls) :
      _node_delegate(delegate),
      _thread(thread_for_delegate_calls)
      BOOST_PP_SEQ_FOR_EACH(INITIALIZE_ACCUMULATOR, unused, NODE_DELEGATE_METHOD_NAMES)
    {}
#undef INITIALIZE_ACCUMULATOR

    fc::variant_object statistics_gathering_node_delegate_wrapper::get_call_statistics()
    {
      fc::mutable_variant_object statistics;
      std::ostringstream note;
      note << "All times are in microseconds, mean is the average of the last " << ROLLING_WINDOW_SIZE << " call times";
      statistics["_note"] = note.str();

#define ADD_STATISTICS_FOR_METHOD(r, data, method_name) \
      fc::mutable_variant_object BOOST_PP_CAT(method_name, _stats); \
      BOOST_PP_CAT(method_name, _stats)["min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_before_sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_before_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_min"] = boost::accumulators::min(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_mean"] = boost::accumulators::rolling_mean(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_max"] = boost::accumulators::max(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["delay_after_sum"] = boost::accumulators::sum(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _delay_after_accumulator))); \
      BOOST_PP_CAT(method_name, _stats)["count"] = boost::accumulators::count(BOOST_PP_CAT(_, BOOST_PP_CAT(method_name, _execution_accumulator))); \
      statistics[BOOST_PP_STRINGIZE(method_name)] = BOOST_PP_CAT(method_name, _stats);

      BOOST_PP_SEQ_FOR_EACH(ADD_STATISTICS_FOR_METHOD, unused, NODE_DELEGATE_METHOD_NAMES)
#undef ADD_STATISTICS_FOR_METHOD

      return statistics;
    }

// define VERBOSE_NODE_DELEGATE_LOGGING to log whenever the node delegate throws exceptions
//#define VERBOSE_NODE_DELEGATE_LOGGING
#ifdef VERBOSE_NODE_DELEGATE_LOGGING
#  define INVOKE_AND_COLLECT_STATISTICS(method_name, ...) \
    try \
    { \
      if (_thread->is_current()) \
      { \
         call_statistics_collector statistics_collector(#method_name, \
            &_ ## method_name ## _execution_accumulator, \
            &_ ## method_name ## _delay_before_accumulator, \
            &_ ## method_name ## _delay_after_accumulator); \
        call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); \
        return _node_delegate->method_name(__VA_ARGS__); \
      } \
      else \
        return _thread->async([&](){ \
         call_statistics_collector statistics_collector(#method_name, \
            &_ ## method_name ## _execution_accumulator, \
            &_ ## method_name ## _delay_before_accumulator, \
            &_ ## method_name ## _delay_after_accumulator); \
          call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); \
          return _node_delegate->method_name(__VA_ARGS__); \
        }, "invoke " BOOST_STRINGIZE(method_name)).wait(); \
    } \
    catch (const fc::exception& e) \
    { \
      dlog("node_delegate threw fc::exception: ${e}", ("e", e)); \
      throw; \
    } \
    catch (const std::exception& e) \
    { \
      dlog("node_delegate threw std::exception: ${e}", ("e", e.what())); \
      throw; \
    } \
    catch (...) \
    { \
      dlog("node_delegate threw unrecognized exception"); \
      throw; \
    }
#else
#define INVOKE_AND_COLLECT_STATISTICS( method_name, ... ) \
   FC_UNUSED( _thread ) \
   return _node_delegate->method_name(__VA_ARGS__);
/*
#  define INVOKE_AND_COLLECT_STATISTICS(method_name, ...) \
    if ( _thread->is_current()) \
    { \
      call_statistics_collector statistics_collector(#method_name, \
         &_ ## method_name ## _execution_accumulator, \
         &_ ## method_name ## _delay_before_accumulator, \
         &_ ## method_name ## _delay_after_accumulator); \
      call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); \
      return _node_delegate->method_name(__VA_ARGS__); \
    } \
    else \
      return _thread->async([&](){ \
         call_statistics_collector statistics_collector(#method_name, \
            &_ ## method_name ## _execution_accumulator, \
            &_ ## method_name ## _delay_before_accumulator, \
            &_ ## method_name ## _delay_after_accumulator); \
        call_statistics_collector::actual_execution_measurement_helper helper(statistics_collector); \
        return _node_delegate->method_name(__VA_ARGS__); \
      }, "invoke " BOOST_STRINGIZE(method_name)).wait()
*/
#endif

    hive::protocol::chain_id_type statistics_gathering_node_delegate_wrapper::get_old_chain_id() const
    {
      INVOKE_AND_COLLECT_STATISTICS(get_old_chain_id);
    }

    hive::protocol::chain_id_type statistics_gathering_node_delegate_wrapper::get_new_chain_id() const
    {
      INVOKE_AND_COLLECT_STATISTICS(get_new_chain_id);
    }

    hive::protocol::chain_id_type statistics_gathering_node_delegate_wrapper::get_chain_id() const
    {
      INVOKE_AND_COLLECT_STATISTICS(get_chain_id);
    }

    bool statistics_gathering_node_delegate_wrapper::has_item( const net::item_id& id )
    {
      INVOKE_AND_COLLECT_STATISTICS(has_item, id);
    }

    void statistics_gathering_node_delegate_wrapper::handle_message( const message& message_to_handle )
    {
      INVOKE_AND_COLLECT_STATISTICS(handle_message, message_to_handle);
    }

    bool statistics_gathering_node_delegate_wrapper::handle_block(const std::shared_ptr<full_block_type>& full_block, bool sync_mode)
    {
      INVOKE_AND_COLLECT_STATISTICS(handle_block, full_block, sync_mode);
    }

    void statistics_gathering_node_delegate_wrapper::handle_transaction(const std::shared_ptr<full_transaction_type>& full_transaction)
    {
      INVOKE_AND_COLLECT_STATISTICS(handle_transaction, full_transaction);
    }

    std::vector<item_hash_t> statistics_gathering_node_delegate_wrapper::get_block_ids(const std::vector<item_hash_t>& blockchain_synopsis,
                                                                                       uint32_t& remaining_item_count,
                                                                                       uint32_t limit /* = 2000 */)
    {
      INVOKE_AND_COLLECT_STATISTICS(get_block_ids, blockchain_synopsis, remaining_item_count, limit);
    }

    std::shared_ptr<hive::chain::full_block_type> statistics_gathering_node_delegate_wrapper::get_full_block(const block_id_type& id)
    {
      INVOKE_AND_COLLECT_STATISTICS(get_full_block, id);
    }

    std::vector<item_hash_t> statistics_gathering_node_delegate_wrapper::get_blockchain_synopsis(const item_hash_t& reference_point, uint32_t number_of_blocks_after_reference_point)
    {
      INVOKE_AND_COLLECT_STATISTICS(get_blockchain_synopsis, reference_point, number_of_blocks_after_reference_point);
    }

    void statistics_gathering_node_delegate_wrapper::sync_status( uint32_t item_type, uint32_t item_count )
    {
      INVOKE_AND_COLLECT_STATISTICS(sync_status, item_type, item_count);
    }

    void statistics_gathering_node_delegate_wrapper::connection_count_changed( uint32_t c )
    {
      INVOKE_AND_COLLECT_STATISTICS(connection_count_changed, c);
    }

    uint32_t statistics_gathering_node_delegate_wrapper::get_block_number(const item_hash_t& block_id)
    {
      // this function doesn't need to block,
      ASSERT_TASK_NOT_PREEMPTED();
      return _node_delegate->get_block_number(block_id);
    }

    fc::time_point_sec statistics_gathering_node_delegate_wrapper::get_block_time(const item_hash_t& block_id)
    {
      INVOKE_AND_COLLECT_STATISTICS(get_block_time, block_id);
    }

    /** returns graphene::blockchain::now() */
    fc::time_point_sec statistics_gathering_node_delegate_wrapper::get_blockchain_now()
    {
      // this function doesn't need to block,
      ASSERT_TASK_NOT_PREEMPTED();
      return _node_delegate->get_blockchain_now();
    }

    item_hash_t statistics_gathering_node_delegate_wrapper::get_head_block_id() const
    {
      INVOKE_AND_COLLECT_STATISTICS(get_head_block_id);
    }

    uint32_t statistics_gathering_node_delegate_wrapper::estimate_last_known_fork_from_git_revision_timestamp(uint32_t unix_timestamp) const
    {
      INVOKE_AND_COLLECT_STATISTICS(estimate_last_known_fork_from_git_revision_timestamp, unix_timestamp);
    }

    void statistics_gathering_node_delegate_wrapper::error_encountered(const std::string& message, const fc::oexception& error)
    {
      INVOKE_AND_COLLECT_STATISTICS(error_encountered, message, error);
    }

    std::deque<block_id_type>::const_iterator statistics_gathering_node_delegate_wrapper::find_first_item_not_in_blockchain(const std::deque<block_id_type>& item_hashes_received)
    {
      INVOKE_AND_COLLECT_STATISTICS(find_first_item_not_in_blockchain, item_hashes_received);
    }

#undef INVOKE_AND_COLLECT_STATISTICS

  } // end namespace detail

} } // end namespace graphene::net
