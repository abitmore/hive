#pragma once
#include <hive/plugins/json_rpc/utility.hpp>
#include <hive/plugins/market_history/market_history_plugin.hpp>

#include <hive/protocol/hive_virtual_operations.hpp>
#include <hive/protocol/types.hpp>

#include <fc/optional.hpp>
#include <fc/variant.hpp>
#include <fc/vector.hpp>


namespace hive { namespace plugins { namespace market_history {


using hive::chain::share_type;
using hive::chain::asset;
using fc::time_point_sec;
using json_rpc::void_type;


typedef void_type get_ticker_args;

struct get_ticker_return
{
  double      latest = 0;
  double      lowest_ask = 0;
  double      highest_bid = 0;
  double      percent_change = 0;
  asset       hive_volume = asset( 0 , HIVE_SYMBOL );
  asset       hbd_volume = asset( 0, HBD_SYMBOL );
};

typedef void_type get_volume_args;

struct get_volume_return
{
  asset       hive_volume = asset( 0, HIVE_SYMBOL );
  asset       hbd_volume = asset( 0, HBD_SYMBOL );
};

struct order
{
  price          order_price;
  double         real_price;
  share_type     hive;
  share_type     hbd;
  time_point_sec created;
};

struct get_order_book_args
{
  uint32_t limit = 500;
};

struct get_order_book_return
{
  vector< order > bids;
  vector< order > asks;
};

struct market_trade
{
  market_trade( const time_point_sec& _date, const protocol::fill_order_operation& op )
    : date( _date ), current_pays( op.current_pays ), open_pays( op.open_pays ),
      taker( op.current_owner ), maker( op.open_owner )
  {}

  time_point_sec    date;
  asset             current_pays;
  asset             open_pays;
  account_name_type taker;
  account_name_type maker;
};

struct get_trade_history_args
{
  time_point_sec start;
  time_point_sec end;
  uint32_t       limit = 1000;
};

struct get_trade_history_return
{
  std::vector< market_trade > trades;
};

struct get_recent_trades_args
{
  uint32_t limit = 1000;
};

typedef get_trade_history_return get_recent_trades_return;

struct get_market_history_args
{
  uint32_t       bucket_seconds = 0;
  time_point_sec start;
  time_point_sec end;
};

struct get_market_history_return
{
  std::vector< market_history::bucket_object > buckets;
};

typedef void_type get_market_history_buckets_args;

struct get_market_history_buckets_return
{
  flat_set< uint32_t > bucket_sizes;
};


namespace detail { class market_history_api_impl; }


class market_history_api
{
  public:
    market_history_api( appbase::application& app );
    ~market_history_api();

    DECLARE_API(
      (get_ticker)
      (get_volume)
      (get_order_book)
      (get_trade_history)
      (get_recent_trades)
      (get_market_history)
      (get_market_history_buckets)
    )

  private:
    std::unique_ptr< detail::market_history_api_impl > my;
};

} } } // hive::plugins::market_history

FC_REFLECT( hive::plugins::market_history::get_ticker_return,
        (latest)(lowest_ask)(highest_bid)(percent_change)(hive_volume)(hbd_volume) )

FC_REFLECT( hive::plugins::market_history::get_volume_return,
        (hive_volume)(hbd_volume) )

FC_REFLECT( hive::plugins::market_history::order,
        (order_price)(real_price)(hive)(hbd)(created) )

FC_REFLECT( hive::plugins::market_history::get_order_book_args,
        (limit) )

FC_REFLECT( hive::plugins::market_history::get_order_book_return,
        (bids)(asks) )

FC_REFLECT( hive::plugins::market_history::market_trade,
        (date)(current_pays)(open_pays)(taker)(maker) )

FC_REFLECT( hive::plugins::market_history::get_trade_history_args,
        (start)(end)(limit) )

FC_REFLECT( hive::plugins::market_history::get_trade_history_return,
        (trades) )

FC_REFLECT( hive::plugins::market_history::get_recent_trades_args,
        (limit) )

FC_REFLECT( hive::plugins::market_history::get_market_history_args,
        (bucket_seconds)(start)(end) )

FC_REFLECT( hive::plugins::market_history::get_market_history_return,
        (buckets) )

FC_REFLECT( hive::plugins::market_history::get_market_history_buckets_return,
        (bucket_sizes) )
